"""Own a test process tree on a separate, non-input Windows desktop.

No desktop switching or physical input injection. Input-desktop execution
follows a silent five-second notice without requiring user input.
The caller owns logs and assertions; this boundary preserves exit status and
kills the entire owned job on timeout, cancellation or launcher termination.
Optional private window stations also isolate the clipboard. If Windows denies
that, interactive fallback first displays the same passive notice.
This is UI isolation, not a filesystem or security sandbox.
"""
from __future__ import annotations

import argparse
import ctypes as c
from ctypes import wintypes as w
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
            (self.u, "CreateWindowStationW", [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p], w.HANDLE),
            (self.u, "GetProcessWindowStation", [], w.HANDLE),
            (self.u, "SetProcessWindowStation", [w.HANDLE], w.BOOL),
            (self.u, "CloseWindowStation", [w.HANDLE], w.BOOL),
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
            (self.k, "AssignProcessToJobObject", [w.HANDLE, w.HANDLE], w.BOOL),
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
    def __init__(self, private_clipboard=False):
        self.api = Windows()
        self.station = self.desktop = None
        self.name = "PdfNoteTest_" + uuid.uuid4().hex
        previous = self.api.u.GetProcessWindowStation()
        try:
            if private_clipboard:
                self.station = self.api.check(self.api.u.CreateWindowStationW(self.name, 1, 0x33F, None))
                self.api.check(self.api.u.SetProcessWindowStation(self.station))
            try:
                self.station_name = self.api.name(self.api.u.GetProcessWindowStation())
                # Exclude DESKTOP_SWITCHDESKTOP from the launcher handle.
                self.desktop = self.api.check(self.api.u.CreateDesktopW(self.name, None, None, 0, 0xC7, None))
                self.path = self.station_name + "\\" + self.name
            finally:
                self.api.check(self.api.u.SetProcessWindowStation(previous))
        except BaseException:
            self.close()
            raise

    def close(self):
        try:
            if self.desktop:
                self.api.check(self.api.u.CloseDesktop(self.desktop))
                self.desktop = None
        finally:
            if self.station:
                self.api.check(self.api.u.CloseWindowStation(self.station))
                self.station = None


class OwnedProcess:
    """Launch suspended; assign the non-inherited kill-on-close job before resume."""
    def __init__(self, command, cwd, desktop, *, stdout=None, stderr=None, env=None):
        import msvcrt
        self.api = api = Windows()
        self.job = self.handle = None
        self.pid = None
        self.returncode = None
        handles = []
        attributes = None
        process = ProcessInfo()
        try:
            self.job = api.check(api.k.CreateJobObjectW(None, None))
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
            api.k.InitializeProcThreadAttributeList(None, 1, 0, c.byref(size))
            buffer = c.create_string_buffer(size.value)
            api.check(api.k.InitializeProcThreadAttributeList(buffer, 1, 0, c.byref(size)))
            attributes = buffer
            inherited = (w.HANDLE * len(handles))(*handles)
            api.check(api.k.UpdateProcThreadAttribute(buffer, 0, 0x20002, inherited, c.sizeof(inherited), None, None))
            startup = StartupInfoEx()
            startup.info.cb = c.sizeof(startup)
            startup.info.desktop = desktop
            startup.info.flags = 0x101  # STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW
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
            api.check(api.k.AssignProcessToJobObject(self.job, self.handle))
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

    def close(self):
        try:
            if self.job:
                self.kill()  # Include descendants even when the root has already exited.
                deadline = time.monotonic() + 5
                while True:
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


def run(command, cwd, timeout, *, stdout=None, stderr=None, interactive=False, private_clipboard=False):
    if not command or not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("A command and positive finite timeout are required.")
    desktop = process = None
    environment = dict(os.environ)
    try:
        if not interactive:
            try:
                desktop = PrivateDesktop(private_clipboard)
            except PermissionError:
                if not private_clipboard:
                    raise
                print("Private clipboard isolation is unavailable; a five-second passive notice will precede the UI test.", flush=True)
                interactive = True
        if interactive:
            if current_station().lower() != "winsta0":
                raise RuntimeError("Interactive checks must be started from the user's desktop.")
            notice = Path(__file__).resolve().parents[2] / "tests/scripts/show_interactive_test_notice.ps1"
            desktop_path = input_desktop_path()
            print("[NOTICE] UI test starts automatically after the five-second on-screen warning; pause keyboard/mouse input.", flush=True)
            notification = OwnedProcess(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(notice)],
                                        cwd, desktop_path, stdout=stdout, stderr=stderr)
            try:
                try:
                    notice_exit = notification.wait(30)
                except subprocess.TimeoutExpired as error:
                    raise RuntimeError("Pre-test notice timed out; UI test was not started.") from error
                if notice_exit == 2:
                    raise RuntimeError("Pre-test notice was interrupted before the five-second grace period completed; UI test was not started.")
                if notice_exit != 0:
                    raise RuntimeError(f"Pre-test notice failed (exit={notice_exit}); UI test was not started.")
            finally:
                notification.close()
            environment.pop("PDF_NOTE_TEST_STATION", None)
            environment.pop("PDF_NOTE_TEST_DESKTOP", None)
            environment["PDF_NOTE_TEST_INTERACTIVE"] = "notified"
            print("Execution: interactive after passive notice; foreground and clipboard may be used.", flush=True)
        else:
            desktop_path = desktop.path
            environment.pop("PDF_NOTE_TEST_INTERACTIVE", None)
            environment["PDF_NOTE_TEST_STATION"] = desktop.station_name
            environment["PDF_NOTE_TEST_DESKTOP"] = desktop.name
            clipboard = "private" if desktop.station else "shared; clipboard checks require --private-clipboard"
            print(f"Execution: isolated desktop {desktop.path}; clipboard={clipboard}; no desktop switching.", flush=True)
        process = OwnedProcess(command, cwd, desktop_path, stdout=stdout, stderr=stderr, env=environment)
        return process.wait(timeout)
    finally:
        try:
            if process:
                process.close()
        finally:
            if desktop:
                desktop.close()


def protect_entry_point(*, timeout=900, interactive=False):
    """Re-enter a Python GUI check before it creates windows or starts children."""
    mode = execution_mode()
    if mode == "interactive" or (mode == "isolated" and not interactive):
        return
    raise SystemExit(run([sys.executable, "-B", str(Path(sys.argv[0]).resolve()), *sys.argv[1:]],
                         Path.cwd(), timeout, interactive=interactive))


def main(argv=None):
    # The PowerShell bridge explicitly decodes UTF-8, including native stderr.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cwd", type=Path, default=Path.cwd())
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--interactive", action="store_true", help="Show a silent five-second notice, then start automatically without user input")
    parser.add_argument("--private-clipboard", action="store_true", help="Require a private clipboard; show a passive notice before interactive mode if permission is denied")
    parser.add_argument("--check-context", action="store_true")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    try:
        if args.check_context:
            print(execution_mode())
            return 0
        command = args.command[1:] if args.command[:1] == ["--"] else args.command
        return run(command, args.cwd, args.timeout, interactive=args.interactive, private_clipboard=args.private_clipboard)
    except Exception as error:
        print(f"[FAIL] Isolated app test: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
