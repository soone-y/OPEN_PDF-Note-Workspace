"""Own a test process tree on a separate, non-input Windows desktop.

No desktop switching or physical input injection. Every run has a visible
control window from the five-second grace period through verified cleanup.
The caller owns logs and assertions; this boundary preserves exit status and
kills the entire owned job on timeout, cancellation or launcher termination.
Clipboard checks use supervised input-desktop execution; creating a private
window station is unsupported in the development environment.
This is UI isolation, not a filesystem or security sandbox.
"""
from __future__ import annotations

import argparse
import ctypes as c
from ctypes import wintypes as w
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid


class StartupInfo(c.Structure):
    _fields_ = [("cb", w.DWORD), ("reserved", w.LPWSTR), ("desktop", w.LPWSTR),
                ("title", w.LPWSTR), ("x", w.DWORD), ("y", w.DWORD),
                ("cx", w.DWORD), ("cy", w.DWORD), ("charsx", w.DWORD),
                ("charsy", w.DWORD), ("fill", w.DWORD), ("flags", w.DWORD),
                ("show", w.WORD), ("reserved_size", w.WORD), ("reserved_ptr", c.c_void_p),
                ("stdin", w.HANDLE), ("stdout", w.HANDLE), ("stderr", w.HANDLE)]


class StartupInfoEx(c.Structure):
    _fields_ = [("info", StartupInfo), ("attributes", c.c_void_p)]


class ProcessInfo(c.Structure):
    _fields_ = [("process", w.HANDLE), ("thread", w.HANDLE), ("pid", w.DWORD), ("tid", w.DWORD)]


class JobLimits(c.Structure):
    _fields_ = [("process_time", c.c_int64), ("job_time", c.c_int64), ("flags", w.DWORD),
                ("min_working_set", c.c_size_t), ("max_working_set", c.c_size_t),
                ("active_limit", w.DWORD), ("affinity", c.c_size_t),
                ("priority", w.DWORD), ("scheduling", w.DWORD)]


class ExtendedJobLimits(c.Structure):
    _fields_ = [("basic", JobLimits), ("io", c.c_uint64 * 6),
                ("process_memory", c.c_size_t), ("job_memory", c.c_size_t),
                ("peak_process_memory", c.c_size_t), ("peak_job_memory", c.c_size_t)]


class JobAccounting(c.Structure):
    _fields_ = [("times", c.c_int64 * 4), ("faults", w.DWORD), ("total", w.DWORD),
                ("active", w.DWORD), ("terminated", w.DWORD)]


class Windows:
    def __init__(self):
        if os.name != "nt":
            raise RuntimeError("App test isolation requires Windows.")
        self.k = c.WinDLL("kernel32", use_last_error=True)
        self.u = c.WinDLL("user32", use_last_error=True)
        declarations = [
            (self.u, "GetProcessWindowStation", [], w.HANDLE),
            (self.u, "CreateDesktopW", [w.LPCWSTR, w.LPCWSTR, c.c_void_p, w.DWORD, w.DWORD, c.c_void_p], w.HANDLE),
            (self.u, "CloseDesktop", [w.HANDLE], w.BOOL),
            (self.u, "GetThreadDesktop", [w.DWORD], w.HANDLE),
            (self.u, "OpenInputDesktop", [w.DWORD, w.BOOL, w.DWORD], w.HANDLE),
            (self.u, "GetUserObjectInformationW", [w.HANDLE, c.c_int, c.c_void_p, w.DWORD, c.POINTER(w.DWORD)], w.BOOL),
            (self.k, "GetCurrentThreadId", [], w.DWORD),
            (self.k, "GetCurrentProcess", [], w.HANDLE),
            (self.k, "CreateJobObjectW", [c.c_void_p, w.LPCWSTR], w.HANDLE),
            (self.k, "SetInformationJobObject", [w.HANDLE, c.c_int, c.c_void_p, w.DWORD], w.BOOL),
            (self.k, "QueryInformationJobObject", [w.HANDLE, c.c_int, c.c_void_p, w.DWORD, c.c_void_p], w.BOOL),
            (self.k, "IsProcessInJob", [w.HANDLE, w.HANDLE, c.POINTER(w.BOOL)], w.BOOL),
            (self.k, "TerminateJobObject", [w.HANDLE, w.UINT], w.BOOL),
            (self.k, "CloseHandle", [w.HANDLE], w.BOOL),
            (self.k, "DuplicateHandle", [w.HANDLE, w.HANDLE, w.HANDLE, c.POINTER(w.HANDLE), w.DWORD, w.BOOL, w.DWORD], w.BOOL),
            (self.k, "InitializeProcThreadAttributeList", [c.c_void_p, w.DWORD, w.DWORD, c.POINTER(c.c_size_t)], w.BOOL),
            (self.k, "UpdateProcThreadAttribute", [c.c_void_p, w.DWORD, c.c_size_t, c.c_void_p, c.c_size_t, c.c_void_p, c.c_void_p], w.BOOL),
            (self.k, "DeleteProcThreadAttributeList", [c.c_void_p], None),
            (self.k, "CreateProcessW", [w.LPCWSTR, w.LPWSTR, c.c_void_p, c.c_void_p, w.BOOL, w.DWORD, c.c_void_p, w.LPCWSTR, c.POINTER(StartupInfoEx), c.POINTER(ProcessInfo)], w.BOOL),
            (self.k, "ResumeThread", [w.HANDLE], w.DWORD),
            (self.k, "TerminateProcess", [w.HANDLE, w.UINT], w.BOOL),
            (self.k, "WaitForSingleObject", [w.HANDLE, w.DWORD], w.DWORD),
            (self.k, "GetExitCodeProcess", [w.HANDLE, c.POINTER(w.DWORD)], w.BOOL),
        ]
        for library, name, arguments, result in declarations:
            function = getattr(library, name)
            function.argtypes, function.restype = arguments, result

    @staticmethod
    def check(result):
        if not result:
            raise c.WinError(c.get_last_error())
        return result

    def name(self, handle):
        size = w.DWORD()
        self.u.GetUserObjectInformationW(handle, 2, None, 0, c.byref(size))
        buffer = c.create_unicode_buffer(max(1, size.value // c.sizeof(c.c_wchar)))
        self.check(self.u.GetUserObjectInformationW(handle, 2, buffer, c.sizeof(buffer), c.byref(size)))
        return buffer.value


def current_station():
    api = Windows()
    return api.name(api.u.GetProcessWindowStation())


def input_desktop_path():
    api = Windows()
    handle = api.check(api.u.OpenInputDesktop(0, False, 1))
    try:
        return "winsta0\\" + api.name(handle)
    finally:
        api.check(api.u.CloseDesktop(handle))


def execution_mode():
    """Verify the OS binding, rather than trusting an environment-only bypass."""
    api = Windows()
    station = api.name(api.u.GetProcessWindowStation())
    desktop = api.name(api.u.GetThreadDesktop(api.k.GetCurrentThreadId()))
    expected = os.environ.get("PDF_NOTE_TEST_STATION", "")
    expected_desktop = os.environ.get("PDF_NOTE_TEST_DESKTOP", "")
    if (expected and station == expected and expected_desktop.startswith("PdfNoteTest_")
            and desktop == expected_desktop):
        return "isolated"
    if os.environ.get("PDF_NOTE_TEST_INTERACTIVE") == "notified" and station.lower() == "winsta0":
        return "interactive"
    return "unprotected"


class PrivateDesktop:
    def __init__(self):
        self.api = Windows()
        self.desktop = None
        self.name = "PdfNoteTest_" + uuid.uuid4().hex
        self.station_name = self.api.name(self.api.u.GetProcessWindowStation())
        # Exclude DESKTOP_SWITCHDESKTOP from the launcher handle.
        self.desktop = self.api.check(self.api.u.CreateDesktopW(self.name, None, None, 0, 0xC7, None))
        self.path = self.station_name + "\\" + self.name

    def close(self):
        if self.desktop:
            self.api.check(self.api.u.CloseDesktop(self.desktop))
            self.desktop = None


class OwnedProcess:
    """Create in a non-inherited kill-on-close job; verify before resuming."""
    def __init__(self, command, cwd, desktop, *, stdout=None, stderr=None, env=None,
                 show_without_activation=False, job_name=None):
        import msvcrt
        self.api = api = Windows()
        self.job = self.handle = None
        self.job_name = job_name
        self.pid = None
        self.returncode = None
        handles = []
        attributes = None
        process = ProcessInfo()
        try:
            c.set_last_error(0)
            created_job = api.check(api.k.CreateJobObjectW(None, job_name))
            if job_name and c.get_last_error() == 183:  # ERROR_ALREADY_EXISTS
                api.check(api.k.CloseHandle(created_job))
                raise RuntimeError("Refusing to reuse an existing test job.")
            self.job = created_job
            limits = ExtendedJobLimits()
            limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; no breakaway.
            api.check(api.k.SetInformationJobObject(self.job, 9, c.byref(limits), c.sizeof(limits)))
            with open(os.devnull, "rb") as null_input:
                for stream in (null_input, stdout or sys.stdout, stderr or sys.stderr):
                    handle = w.HANDLE()
                    api.check(api.k.DuplicateHandle(api.k.GetCurrentProcess(), msvcrt.get_osfhandle(stream.fileno()),
                        api.k.GetCurrentProcess(), c.byref(handle), 0, True, 2))
                    handles.append(handle.value)
            size = c.c_size_t()
            api.k.InitializeProcThreadAttributeList(None, 2, 0, c.byref(size))
            buffer = c.create_string_buffer(size.value)
            api.check(api.k.InitializeProcThreadAttributeList(buffer, 2, 0, c.byref(size)))
            attributes = buffer
            inherited = (w.HANDLE * len(handles))(*handles)
            api.check(api.k.UpdateProcThreadAttribute(buffer, 0, 0x20002, inherited, c.sizeof(inherited), None, None))
            # PROC_THREAD_ATTRIBUTE_JOB_LIST: assignment is part of creation,
            # removing the create-suspended/assign gap if the launcher dies.
            # https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute
            jobs = (w.HANDLE * 1)(self.job)
            api.check(api.k.UpdateProcThreadAttribute(buffer, 0, 0x2000D, jobs, c.sizeof(jobs), None, None))
            startup = StartupInfoEx()
            startup.info.cb = c.sizeof(startup)
            startup.info.desktop = desktop
            startup.info.flags = 0x101  # STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW
            # SW_HIDE overrides the first ShowWindow call in a GUI child.
            # The persistent notice must actually be visible without activation.
            startup.info.show = 4 if show_without_activation else 0  # SW_SHOWNOACTIVATE / SW_HIDE
            startup.info.stdin, startup.info.stdout, startup.info.stderr = handles
            startup.attributes = c.cast(buffer, c.c_void_p)
            executable = shutil.which(str(command[0]))
            if not executable:
                raise FileNotFoundError(str(command[0]))
            text = c.create_unicode_buffer(subprocess.list2cmdline([executable, *map(str, command[1:])]))
            environment = dict(os.environ if env is None else env)
            block = c.create_unicode_buffer("\0".join(f"{key}={value}" for key, value in sorted(environment.items())) + "\0\0")
            api.check(api.k.CreateProcessW(executable, text, None, None, True,
                0x08080404, block, str(cwd), c.byref(startup), c.byref(process)))
            # CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED
            self.handle, self.pid = process.process, process.pid
            assigned = w.BOOL()
            api.check(api.k.IsProcessInJob(self.handle, self.job, c.byref(assigned)))
            if not assigned.value:
                raise RuntimeError("Test process job membership was not established.")
            if api.k.ResumeThread(process.thread) == 0xFFFFFFFF:
                raise c.WinError(c.get_last_error())
        except BaseException:
            try:
                if self.handle:
                    # Also covers assignment failure: the process is still suspended.
                    api.check(api.k.TerminateProcess(self.handle, 3))
                    if api.k.WaitForSingleObject(self.handle, 5000) != 0:
                        raise RuntimeError("Suspended test process cleanup failed.")
            finally:
                self.close()
            raise
        finally:
            if process.thread:
                api.k.CloseHandle(process.thread)
            if attributes is not None:
                api.k.DeleteProcThreadAttributeList(attributes)
            for handle in handles:
                api.k.CloseHandle(handle)

    def poll(self):
        result = self.api.k.WaitForSingleObject(self.handle, 0)
        if result == 258:
            return None
        if result != 0:
            raise c.WinError(c.get_last_error())
        code = w.DWORD()
        self.api.check(self.api.k.GetExitCodeProcess(self.handle, c.byref(code)))
        self.returncode = code.value
        return self.returncode

    def wait(self, timeout=None):
        result = self.api.k.WaitForSingleObject(self.handle, 0xFFFFFFFF if timeout is None else int(timeout * 1000))
        if result == 258:
            raise subprocess.TimeoutExpired("owned isolated test", timeout)
        if result != 0:
            raise c.WinError(c.get_last_error())
        return self.poll()

    def kill(self):
        self.api.check(self.api.k.TerminateJobObject(self.job, 3))

    def close(self, heartbeat=None):
        try:
            if self.job:
                self.kill()  # Include descendants even when the root has already exited.
                deadline = time.monotonic() + 5
                while True:
                    if heartbeat:
                        heartbeat()
                    account = JobAccounting()
                    self.api.check(self.api.k.QueryInformationJobObject(self.job, 1, c.byref(account), c.sizeof(account), None))
                    if not account.active:
                        break
                    if time.monotonic() >= deadline:
                        raise RuntimeError("Owned test job cleanup timed out.")
                    time.sleep(0.02)
        finally:
            if self.handle:
                self.api.check(self.api.k.CloseHandle(self.handle))
                self.handle = None
            if self.job:
                self.api.check(self.api.k.CloseHandle(self.job))
                self.job = None


class NoticeEvents:
    """Launcher owns manual-reset events; only the notice requests cancellation."""
    def __init__(self):
        self.api = Windows()
        self.api.k.CreateEventW.argtypes = [c.c_void_p, w.BOOL, w.BOOL, w.LPCWSTR]
        self.api.k.CreateEventW.restype = w.HANDLE
        self.api.k.SetEvent.argtypes = [w.HANDLE]
        self.api.k.SetEvent.restype = w.BOOL
        self.token = "Local\\PdfNoteTestNotice_" + uuid.uuid4().hex
        self.handles = {}
        try:
            for name in ("ready", "cancel", "finished", "running", "failed", "interrupted", "cleanup_failed",
                         "owner_pulse", "ui_pulse"):
                manual_reset = name not in ("owner_pulse", "ui_pulse")
                self.handles[name] = self.api.check(self.api.k.CreateEventW(None, manual_reset, False, self.token + "_" + name))
        except BaseException:
            self.close()
            raise

    def is_set(self, name):
        status = self.api.k.WaitForSingleObject(self.handles[name], 0)
        if status not in (0, 258):
            raise c.WinError(c.get_last_error())
        return status == 0

    def set(self, name):
        self.api.check(self.api.k.SetEvent(self.handles[name]))

    def close(self):
        for handle in self.handles.values():
            self.api.check(self.api.k.CloseHandle(handle))
        self.handles.clear()


class TestInterrupted(RuntimeError):
    """An explicit stop request, propagated as exit 130 through outer runners."""


class TestRunRecord:
    """Keep evidence in a new owned directory; never delete or roll back test files.

    This records process cleanup, not a guarantee about a command's file writes.
    Missing terminal evidence means unconfirmed, including after launcher death.
    """
    def __init__(self, command, cwd, mode, timeout, label=None):
        self.directory = Path(__file__).resolve().parents[2] / "out/tests/test_control" / uuid.uuid4().hex
        self.directory.mkdir(parents=True, exist_ok=False)
        self.path = self.directory / "result.json"
        self.runtime_environment = {}
        # These affect only the owned test children, never the launcher/user's
        # environment. They reduce incidental caches outside retained evidence;
        # they are not a filesystem/security sandbox.
        for key, relative in (("TEMP", "temp"), ("TMP", "temp"),
                              ("APPDATA", "appdata/roaming"), ("LOCALAPPDATA", "appdata/local")):
            directory = self.directory / relative
            directory.mkdir(parents=True, exist_ok=True)
            self.runtime_environment[key] = str(directory)
        self.data = dict(format="test-control-v1", command=list(map(str, command)), cwd=str(Path(cwd).resolve()),
                         label=label, mode=mode, timeout_seconds=timeout, launcher_pid=os.getpid(),
                         runtime_environment=self.runtime_environment,
                         started_at=datetime.now(timezone.utc).isoformat(), phase="preparing",
                         outcome="pending", cleanup_confirmed=False)
        self.update()

    def update(self, **changes):
        self.data.update(changes, updated_at=datetime.now(timezone.utc).isoformat())
        temporary = self.path.with_suffix(".pending.json")
        with temporary.open("w", encoding="utf-8", newline="\n") as stream:
            json.dump(self.data, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, self.path)


class SupervisedNotice:
    """UI and test have separate owned jobs; finish is sent after test cleanup."""
    def __init__(self, cwd, desktop, command, *, stdout=None, stderr=None, mode="interactive", record=None, label=None):
        self.events = NoticeEvents()
        self.process = None
        notice = Path(__file__).resolve().parents[2] / "tests/scripts/show_interactive_test_notice.ps1"
        label = label or next((Path(str(part)).name for part in command if str(part).lower().endswith((".ps1", ".py"))), str(command[0]))
        self.job_name = self.events.token + "_tests"
        self.last_ui_pulse = time.monotonic()
        self.ui_started = False
        try:
            self.process = OwnedProcess(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(notice),
                "-EventPrefix", self.events.token, "-ParentProcessId", str(os.getpid()), "-TestName", label,
                "-ExecutionMode", mode, "-RecordPath", str(record.path) if record else ""],
                cwd, desktop, stdout=stdout, stderr=stderr, show_without_activation=True,
                env={**os.environ, **(record.runtime_environment if record else {})})
        except BaseException:
            self.events.close()
            raise

    def check(self):
        self.pulse()
        if self.events.is_set("cancel"):
            raise TestInterrupted("UI test interrupted from the notice; owned tests will be stopped.")
        code = self.process.poll()
        if code is not None:
            raise RuntimeError(f"UI test notice exited unexpectedly (exit={code}); owned tests will be stopped.")
        if self.events.is_set("ui_pulse"):
            self.last_ui_pulse = time.monotonic()
            self.ui_started = True
        if self.ui_started and time.monotonic() - self.last_ui_pulse > 5:
            raise RuntimeError("Test control window is unresponsive; owned tests will be stopped.")

    def pulse(self):
        self.events.set("owner_pulse")

    def wait_ready(self):
        deadline = time.monotonic() + 30
        while True:
            self.check()
            if self.events.is_set("ready"):
                return
            if time.monotonic() >= deadline:
                raise RuntimeError("Pre-test notice timed out; UI test was not started.")
            time.sleep(0.02)

    def running(self):
        self.events.set("running")

    def close(self, *, outcome="failed", cleanup_confirmed=False):
        try:
            if outcome == "interrupted":
                self.events.set("interrupted")
            elif outcome != "passed":
                self.events.set("failed")
            if not cleanup_confirmed:
                self.events.set("cleanup_failed")
            self.events.set("finished")
            # Give the UI time to close itself; the job still owns its lifetime.
            code = self.process.poll()
            if code is None:
                code = self.process.wait(8)
            if code != 0:
                raise RuntimeError(f"Test control closed without confirmation (exit={code}).")
        finally:
            try:
                self.process.close()
            finally:
                self.events.close()


def run(command, cwd, timeout, *, stdout=None, stderr=None, interactive=False, uses_clipboard=False, label=None):
    if not command or not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("A command and positive finite timeout are required.")
    desktop = process = notice = None
    environment = dict(os.environ)
    interactive = interactive or uses_clipboard
    mode = "interactive" if interactive else "isolated"
    record = TestRunRecord(command, cwd, mode, timeout, label)
    environment.update(record.runtime_environment)
    outcome = "failed"
    attempted_launch = False
    code = None
    try:
        if current_station().lower() != "winsta0":
            raise RuntimeError("Test controls must be started from the user's window station.")
        if interactive:
            desktop_path = input_desktop_path()
            environment.pop("PDF_NOTE_TEST_STATION", None)
            environment.pop("PDF_NOTE_TEST_DESKTOP", None)
            environment["PDF_NOTE_TEST_INTERACTIVE"] = "notified"
        else:
            desktop = PrivateDesktop()
            desktop_path = desktop.path
            environment.pop("PDF_NOTE_TEST_INTERACTIVE", None)
            environment["PDF_NOTE_TEST_STATION"] = desktop.station_name
            environment["PDF_NOTE_TEST_DESKTOP"] = desktop.name
        # Even a hidden test has a visible, independently responsive stop control.
        print(f"[CONTROL] {mode}; five-second grace; record: {record.path}", flush=True)
        notice = SupervisedNotice(cwd, input_desktop_path(), command, stdout=stdout, stderr=stderr,
                                  mode=mode, record=record, label=label)
        notice.wait_ready()
        notice.check()
        attempted_launch = True
        process = OwnedProcess(command, cwd, desktop_path, stdout=stdout, stderr=stderr, env=environment,
                               job_name=notice.job_name)
        record.update(phase="running", test_pid=process.pid, job_name=notice.job_name)
        notice.running()
        if interactive:
            print("Execution: interactive with persistent interrupt control; foreground and clipboard may be used.", flush=True)
        else:
            print(f"Execution: isolated desktop {desktop.path}; persistent control on input desktop; no desktop switching.", flush=True)
        deadline = time.monotonic() + timeout
        while True:
            notice.check()
            code = process.poll()
            if code is not None:
                outcome = "passed" if code == 0 else "failed"
                return code
            if time.monotonic() >= deadline:
                outcome = "timeout"
                raise subprocess.TimeoutExpired(command, timeout)
            time.sleep(0.02)
    except (TestInterrupted, KeyboardInterrupt):
        outcome = "interrupted"
        raise
    finally:
        failure = sys.exception()
        errors = []
        cleanup_confirmed = not attempted_launch
        try:
            record.update(phase="stopping", outcome=outcome, exit_code=code,
                          error=str(failure) if failure else None)
        except Exception as error:
            errors.append("Stop record: " + str(error))
        if process:
            try:
                process.close(heartbeat=notice.pulse if notice else None)
                cleanup_confirmed = True
            except Exception as error:
                errors.append("Owned process cleanup: " + str(error))
        if desktop:
            try:
                desktop.close()
            except Exception as error:
                cleanup_confirmed = False
                errors.append("Desktop cleanup: " + str(error))
        try:
            record.update(phase="finished" if cleanup_confirmed else "cleanup_unconfirmed", outcome=outcome,
                          cleanup_confirmed=cleanup_confirmed, cleanup_errors=errors)
        except Exception as error:
            errors.append("Final record: " + str(error))
        if notice:
            try:
                notice.close(outcome=outcome, cleanup_confirmed=cleanup_confirmed and not errors)
            except Exception as error:
                errors.append("Test control cleanup: " + str(error))
        if errors:
            try:
                record.update(outcome="interrupted" if outcome == "interrupted" else "failed", cleanup_errors=errors)
            except Exception as error:
                errors.append("Error record: " + str(error))
        final_outcome = "interrupted" if outcome == "interrupted" else "failed" if errors else outcome
        print(f"[CONTROL] outcome={final_outcome}; cleanup_confirmed={cleanup_confirmed}; record: {record.path}", flush=True)
        if errors:
            message = ((str(failure) + "; ") if failure else "") + "; ".join(errors)
            if outcome == "interrupted":
                raise TestInterrupted(message) from failure
            raise RuntimeError(message) from failure


def protect_entry_point(*, timeout=900, interactive=False):
    """Re-enter a Python GUI check before it creates windows or starts children."""
    mode = execution_mode()
    if mode == "interactive" or (mode == "isolated" and not interactive):
        return
    try:
        code = run([sys.executable, "-B", str(Path(sys.argv[0]).resolve()), *sys.argv[1:]],
                   Path.cwd(), timeout, interactive=interactive)
    except (TestInterrupted, KeyboardInterrupt):
        code = 130
    raise SystemExit(code)


def main(argv=None):
    # The PowerShell bridge explicitly decodes UTF-8, including native stderr.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cwd", type=Path, default=Path.cwd())
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--interactive", action="store_true", help="Show a persistent interrupt notice; start automatically after five seconds")
    parser.add_argument("--uses-clipboard", action="store_true", help="Run clipboard checks on the input desktop with a persistent interrupt notice")
    parser.add_argument("--label", help="Readable test name shown by the control window")
    parser.add_argument("--check-context", action="store_true")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    try:
        if args.check_context:
            print(execution_mode())
            return 0
        command = args.command[1:] if args.command[:1] == ["--"] else args.command
        return run(command, args.cwd, args.timeout, interactive=args.interactive, uses_clipboard=args.uses_clipboard, label=args.label)
    except (TestInterrupted, KeyboardInterrupt) as error:
        print(f"[INTERRUPTED] App test: {error}", file=sys.stderr)
        return 130
    except Exception as error:
        print(f"[FAIL] Isolated app test: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
