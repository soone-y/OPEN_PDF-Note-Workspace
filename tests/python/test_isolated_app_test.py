"""Exercise desktop/process boundaries without activating the input desktop."""
from __future__ import annotations

import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import unittest
from unittest import mock
import uuid

from tools.dev import isolated_app_test as runner
from tests.python.run_background_app import ForegroundMonitor


TestRunRecordClass = runner.TestRunRecord
ROOT = Path(__file__).resolve().parents[2]
PROBE = r'''
import ctypes as c
from ctypes import wintypes as w
import json, os, subprocess, sys, time
from tools.dev.isolated_app_test import Windows, execution_mode
a = Windows()
a.u.CreateWindowExW.argtypes = [w.DWORD,w.LPCWSTR,w.LPCWSTR,w.DWORD,c.c_int,c.c_int,c.c_int,c.c_int,w.HWND,w.HMENU,w.HINSTANCE,c.c_void_p]
a.u.CreateWindowExW.restype = w.HWND
a.u.SetForegroundWindow.argtypes = [w.HWND]
a.u.DestroyWindow.argtypes = [w.HWND]
window = a.check(a.u.CreateWindowExW(0,'STATIC','Owned isolated probe',0x10CF0000,0,0,200,100,None,None,None,None))
# This must target a window on the non-input desktop, even when activation is requested.
a.u.SetForegroundWindow(window)
child = subprocess.Popen([sys.executable,'-B','-c','import time; time.sleep(60)'])
print(json.dumps({'mode': execution_mode(), 'station': a.name(a.u.GetProcessWindowStation()),
    'desktop': a.name(a.u.GetThreadDesktop(a.k.GetCurrentThreadId())),
    'pid': os.getpid(), 'child': child.pid, 'temp': os.environ.get('TEMP'), 'appdata': os.environ.get('APPDATA')}), flush=True)
time.sleep(0.3)  # Keep the activation attempt observable by the parent.
if '--wait' in sys.argv: time.sleep(60)
a.check(a.u.DestroyWindow(window))
'''


def read_output(output):
    # Child stdout/stderr inherit duplicates of output's native file object.
    # Never seek/read that handle while they write: it changes their shared
    # file position and can overwrite earlier evidence. Open a separate reader.
    return Path(output.name).read_bytes()


class IsolatedAppTests(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.object(runner, "TestRunRecord")
        self.record = patcher.start().return_value
        self.addCleanup(patcher.stop)

    def test_explicit_interruption_reaches_cli_and_python_entry_as_130(self):
        with mock.patch.object(runner, "run", side_effect=runner.TestInterrupted("stop")), \
                mock.patch.object(runner, "execution_mode", return_value="unprotected"), \
                mock.patch.object(runner.sys, "stdout") as output, \
                mock.patch.object(runner.sys, "stderr"):
            self.assertEqual(runner.main(["--interactive", "--", "child"]), 130)
            with self.assertRaises(SystemExit) as result:
                runner.protect_entry_point(interactive=True)
            self.assertEqual(result.exception.code, 130)

    def test_invalid_boundary_input_does_not_launch(self):
        with mock.patch.object(runner, "PrivateDesktop") as create:
            for command, timeout in (([], 1), (["child"], 0), (["child"], float("nan"))):
                with self.assertRaises(ValueError):
                    runner.run(command, ROOT, timeout)
            create.assert_not_called()

    def test_isolation_failure_never_launches_on_input_desktop(self):
        with mock.patch.object(runner, "PrivateDesktop", side_effect=PermissionError("denied")), \
                mock.patch.object(runner, "current_station", return_value="WinSta0"), \
                mock.patch.object(runner, "OwnedProcess") as launch:
            with self.assertRaises(PermissionError):
                runner.run(["child"], ROOT, 1)
            launch.assert_not_called()

    def test_notice_success_is_required_before_interactive_child(self):
        for outcome in (runner.TestInterrupted("interrupted"), RuntimeError("notice died"),
                        RuntimeError("timed out")):
            with self.subTest(outcome=outcome), \
                    mock.patch.object(runner, "current_station", return_value="WinSta0"), \
                    mock.patch.object(runner, "input_desktop_path", return_value="winsta0\\Default"), \
                    mock.patch.object(runner, "SupervisedNotice") as notice, \
                    mock.patch.object(runner, "PrivateDesktop") as desktop, \
                    mock.patch.object(runner, "OwnedProcess") as launch:
                notice.return_value.wait_ready.side_effect = outcome
                with self.assertRaises(type(outcome)):
                    runner.run(["must-not-run"], ROOT, 1, uses_clipboard=True)
                launch.assert_not_called(); desktop.assert_not_called()
                notice.return_value.close.assert_called_once()

    def test_notice_stays_open_until_test_cleanup_on_exit_and_cancel(self):
        for interrupted in (False, True):
            events = []
            with self.subTest(interrupted=interrupted), \
                    mock.patch.object(runner, "current_station", return_value="WinSta0"), \
                    mock.patch.object(runner, "input_desktop_path", return_value="winsta0\\Default"), \
                    mock.patch.object(runner, "SupervisedNotice") as notice, \
                    mock.patch.object(runner, "OwnedProcess") as launch:
                child = launch.return_value; child.poll.return_value = 7
                child.close.side_effect = lambda **kwargs: events.append("test_stopped")
                notice.return_value.close.side_effect = lambda **kwargs: events.append("notice_closed")
                if interrupted:
                    notice.return_value.check.side_effect = [None, runner.TestInterrupted("stop")]
                    with self.assertRaises(runner.TestInterrupted):
                        runner.run(["child"], ROOT, 1, interactive=True)
                else:
                    self.assertEqual(runner.run(["child"], ROOT, 1, interactive=True), 7)
                self.assertEqual(events, ["test_stopped", "notice_closed"])
                self.assertEqual(launch.call_args.kwargs["env"]["PDF_NOTE_TEST_INTERACTIVE"], "notified")

    def test_cleanup_failure_never_signals_confirmation_and_still_propagates_cancel(self):
        with mock.patch.object(runner, "current_station", return_value="WinSta0"), \
                mock.patch.object(runner, "input_desktop_path", return_value="winsta0\\Default"), \
                mock.patch.object(runner, "SupervisedNotice") as notice, \
                mock.patch.object(runner, "OwnedProcess") as launch:
            notice.return_value.check.side_effect = [None, runner.TestInterrupted("stop")]
            launch.return_value.close.side_effect = RuntimeError("cleanup denied")
            with self.assertRaisesRegex(runner.TestInterrupted, "cleanup denied"):
                runner.run(["child"], ROOT, 1, interactive=True)
            notice.return_value.close.assert_called_once_with(outcome="interrupted", cleanup_confirmed=False)
            updates = [call.kwargs for call in self.record.update.call_args_list]
            self.assertTrue(any(update.get("phase") == "cleanup_unconfirmed" for update in updates))

    def test_run_record_keeps_previous_bytes_when_final_replace_fails(self):
        # Exercise the actual record in owned out/, not the unit-test mock.
        with mock.patch.object(runner, "TestRunRecord", wraps=TestRunRecordClass) as record_type:
            record = record_type(["test"], ROOT, "isolated", 1)
            before = record.path.read_bytes()
            with mock.patch.object(runner.os, "replace", side_effect=PermissionError("denied")):
                with self.assertRaises(PermissionError):
                    record.update(phase="finished")
            self.assertEqual(record.path.read_bytes(), before)
            self.assertTrue(record.path.with_suffix(".pending.json").exists())


@unittest.skipUnless(os.name == "nt", "Windows desktop/job integration")
class IsolatedAppWindowsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = ROOT / "out/tests" / ("desktop_boundary_" + uuid.uuid4().hex)
        cls.root.mkdir(parents=True)

    def setUp(self):
        # Keep the control GUI itself off the input desktop during integration
        # testing; production always uses input_desktop_path().
        self.control_desktop = runner.PrivateDesktop()
        self.addCleanup(self.control_desktop.close)
        route = mock.patch.object(runner, "input_desktop_path", return_value=self.control_desktop.path)
        route.start()
        self.addCleanup(route.stop)

    def output(self, name):
        return (self.root / (name + "_" + uuid.uuid4().hex + ".log")).open("w+b")

    def test_environment_marker_alone_does_not_bypass_isolation(self):
        with mock.patch.dict(os.environ, {"PDF_NOTE_TEST_STATION": "WinSta0",
                "PDF_NOTE_TEST_DESKTOP": "PdfNoteTest_nonexistent", "PDF_NOTE_TEST_INTERACTIVE": ""}):
            self.assertEqual(runner.execution_mode(), "unprotected")

    def require_stopped(self, pid):
        api = runner.Windows()
        api.k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        api.k.OpenProcess.restype = w.HANDLE
        process = api.k.OpenProcess(0x100000, False, pid)
        if process:
            try:
                self.assertEqual(api.k.WaitForSingleObject(process, 5000), 0, f"owned process remains: {pid}")
            finally:
                api.check(api.k.CloseHandle(process))
        else:
            self.assertEqual(c.get_last_error(), 87)  # Already reaped PID, not access denied.

    def test_visible_child_and_descendants_use_non_input_desktop(self):
        monitor = ForegroundMonitor()
        original_poll = runner.OwnedProcess.poll
        def observed_poll(process):
            monitor.pump(process.pid)
            self.assertFalse(monitor.took_foreground, "isolated child took input-desktop foreground")
            return original_poll(process)
        try:
            with self.output("probe") as output, mock.patch.object(runner.OwnedProcess, "poll", observed_poll):
                self.assertEqual(runner.run([sys.executable, "-B", "-c", PROBE], ROOT, 10, stdout=output, stderr=output), 0)
                evidence = next(json.loads(line) for line in read_output(output).splitlines() if line.startswith(b'{"mode"'))
        finally:
            monitor.close()
        self.assertEqual(evidence["mode"], "isolated")
        self.assertTrue(evidence["desktop"].startswith("PdfNoteTest_"))
        self.assertNotEqual(evidence["desktop"].lower(), runner.input_desktop_path().split("\\")[-1].lower())
        self.require_stopped(evidence["child"])

    def test_child_failure_and_exit_259_are_preserved(self):
        for code in (7, 259):
            with self.subTest(code=code), self.output("exit") as output:
                self.assertEqual(runner.run([sys.executable, "-B", "-c", f"import sys; sys.exit({code})"],
                    ROOT, 10, stdout=output, stderr=output), code)

    def test_timeout_kills_owned_descendants(self):
        with self.output("timeout") as output:
            with self.assertRaises(subprocess.TimeoutExpired):
                runner.run([sys.executable, "-B", "-c", PROBE, "--wait"], ROOT, 2, stdout=output, stderr=output)
            evidence = next(json.loads(line) for line in read_output(output).splitlines() if line.startswith(b'{"mode"'))
        self.require_stopped(evidence["pid"]); self.require_stopped(evidence["child"])

    def test_assignment_failure_never_resumes_child(self):
        marker = self.root / "must_not_execute.txt"
        api = runner.Windows()
        desktop = runner.PrivateDesktop()
        try:
            with self.output("assign") as output, mock.patch.object(runner, "Windows", return_value=api), \
                    mock.patch.object(api.k, "IsProcessInJob", return_value=False):
                c.set_last_error(5)
                with self.assertRaises(OSError):
                    runner.OwnedProcess([sys.executable, "-B", "-c",
                        f"from pathlib import Path; Path({str(marker)!r}).write_text('unexpected')"],
                        ROOT, desktop.path, stdout=output, stderr=output)
            self.assertFalse(marker.exists())
        finally:
            desktop.close()

    def test_job_attribute_failure_creates_no_process(self):
        api = runner.Windows()
        update = api.k.UpdateProcThreadAttribute
        def deny_job(attributes, flags, key, value, size, previous, length):
            if key == 0x2000D:
                c.set_last_error(5)
                return False
            return update(attributes, flags, key, value, size, previous, length)
        with self.output("job_attribute") as output, mock.patch.object(runner, "Windows", return_value=api), \
                mock.patch.object(api.k, "UpdateProcThreadAttribute", side_effect=deny_job), \
                mock.patch.object(api.k, "CreateProcessW") as create:
            with self.assertRaises(OSError):
                runner.OwnedProcess([sys.executable, "-B", "-c", "pass"], ROOT, self.control_desktop.path,
                    stdout=output, stderr=output)
            create.assert_not_called()

    def test_existing_job_is_never_reused_or_terminated(self):
        name = "Local\\PdfNoteExisting_" + uuid.uuid4().hex
        with self.output("existing_job") as output:
            original = runner.OwnedProcess([sys.executable, "-B", "-c", "import time;time.sleep(60)"],
                ROOT, self.control_desktop.path, job_name=name, stdout=output, stderr=output)
            try:
                with self.assertRaisesRegex(RuntimeError, "existing test job"):
                    runner.OwnedProcess([sys.executable, "-B", "-c", "pass"], ROOT, self.control_desktop.path,
                        job_name=name, stdout=output, stderr=output)
                self.assertIsNone(original.poll(), "collision cleanup terminated an unrelated job")
            finally:
                original.close()

    def test_launcher_termination_kills_child_tree(self):
        with self.output("launcher_death") as output:
            controller = ("from tools.dev import isolated_app_test as r\nfrom unittest import mock\n"
                f"with mock.patch.object(r,'input_desktop_path',return_value={self.control_desktop.path!r}):\n"
                f" raise SystemExit(r.main({['--timeout', '30', '--', sys.executable, '-B', '-c', PROBE, '--wait']!r}))\n")
            process = subprocess.Popen([sys.executable, "-B", "-c", controller],
                cwd=ROOT, stdout=output, stderr=output, creationflags=0x08000000)
            evidence = None
            try:
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    for line in read_output(output).splitlines():
                        if line.startswith(b'{"mode"'):
                            evidence = json.loads(line)
                    if evidence: break
                    if process.poll() is not None: break
                    time.sleep(0.05)
                self.assertIsNotNone(evidence, "isolated child did not become ready")
            finally:
                if process.poll() is None: process.kill()
                process.wait(5)
            self.require_stopped(evidence["pid"]); self.require_stopped(evidence["child"])

    def desktop_helper(self):
        # Substitute only launcher paths to keep the genuine PowerShell bridge
        # and its native child/encoding behavior while routing its GUI to the fixture.
        folder = self.root / ("bridge_" + uuid.uuid4().hex)
        folder.mkdir()
        launcher = folder / "launcher.py"
        launcher.write_text("import sys\nfrom unittest import mock\n"
            f"sys.path.insert(0, {str(ROOT)!r})\n"
            "from tools.dev import isolated_app_test as r\n"
            f"with mock.patch.object(r,'input_desktop_path',return_value={self.control_desktop.path!r}):\n"
            " raise SystemExit(r.main())\n", encoding="utf-8")
        helper = folder / "app_test_desktop.ps1"
        text = (ROOT / "tests/scripts/app_test_desktop.ps1").read_text(encoding="utf-8-sig")
        text = text.replace("$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)",
                            "$root = '" + str(ROOT).replace("'", "''") + "'")
        text = text.replace("$launcher = Join-Path $root 'tools/dev/isolated_app_test.py'",
                            "$launcher = '" + str(launcher).replace("'", "''") + "'")
        helper.write_text(text, encoding="utf-8-sig")
        return str(helper).replace("'", "''")

    def test_powershell_reentry_preserves_bound_values(self):
        script = self.root / "bound parameters 日本語.ps1"
        result = self.root / "bound.json"
        helper = self.desktop_helper()
        script.write_text("param([switch]$Selected,[switch]$Disabled,[string]$Value,[int]$Count)\n"
            f". '{helper}'\n"
            "if (Invoke-AppTestDesktop -ScriptPath $PSCommandPath -Parameters $PSBoundParameters) { return }\n"
            f"@{{selected=[bool]$Selected;disabled=[bool]$Disabled;value=$Value;count=$Count;station=$env:PDF_NOTE_TEST_STATION}} | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath '{str(result)}'\n",
            encoding="utf-8-sig")
        value = "space ' quote 日本語"
        # Encoded invocation also preserves -Disabled:$false on Windows PowerShell 5.
        import base64
        command = f"& '{str(script)}' -Selected -Disabled:$false -Value '{value.replace(chr(39), chr(39)*2)}' -Count 13"
        completed = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-EncodedCommand",
            base64.b64encode(command.encode("utf-16-le")).decode("ascii")], cwd=ROOT, capture_output=True, timeout=30)
        self.assertEqual(completed.returncode, 0, completed.stderr.decode(errors="replace"))
        evidence = json.loads(result.read_text(encoding="utf-8-sig"))
        self.assertEqual((evidence["selected"], evidence["disabled"], evidence["value"], evidence["count"]),
                         (True, False, value, 13))
        self.assertIn("Execution: isolated desktop", completed.stdout.decode(errors="replace"))
        # Empty PSBoundParameters is the default for most standalone runners.
        completed = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)],
                                   cwd=ROOT, capture_output=True, timeout=30)
        self.assertEqual(completed.returncode, 0, completed.stderr.decode(errors="replace"))
        evidence = json.loads(result.read_text(encoding="utf-8-sig"))
        self.assertEqual((evidence["selected"], evidence["disabled"], evidence["value"], evidence["count"]),
                         (False, False, "", 0))

    def test_nested_powershell_preserves_native_stderr_without_clixml(self):
        fixture = self.root / "nested_failure"
        fixture.mkdir()
        native = fixture / "native.py"
        native.write_text("import sys\nsys.stdout.reconfigure(encoding='utf-8')\n"
            "sys.stderr.reconfigure(encoding='utf-8')\n"
            "print('native stdout marker 日本語')\nprint('native stderr marker 日本語',file=sys.stderr)\n"
            "print(file=sys.stderr)\n",
            encoding="utf-8")
        child = fixture / "runner.ps1"
        parent = fixture / "parent.ps1"
        helper = self.desktop_helper()
        # Match run_repo_checks: a PowerShell parent merges the Windows
        # PowerShell runner's streams while an isolated native child also writes.
        parent.write_text("[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)\n"
            "$ErrorActionPreference='Continue'\n"
            f"& powershell.exe -NoProfile -ExecutionPolicy Bypass -File '{child}' 2>&1 | ForEach-Object {{ $_.ToString() }}\n"
            "exit $LASTEXITCODE\n", encoding="utf-8-sig")
        hosts = ["powershell.exe"]
        if shutil.which("pwsh.exe"): hosts.append("pwsh.exe")
        for fail in (False, True):
            child.write_text("$ErrorActionPreference='Stop'\n"
                f". '{helper}'\n"
                "if (Invoke-AppTestDesktop -ScriptPath $PSCommandPath -Parameters $PSBoundParameters) { return }\n"
                "Write-Host 'host marker 日本語'\n"
                f"& python -B '{native}'\n" + ("throw 'intended failure marker 日本語'\n" if fail else ""),
                encoding="utf-8-sig")
            for host in hosts:
                with self.subTest(host=host, fail=fail):
                    completed = subprocess.run([host, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(parent)],
                        cwd=ROOT, capture_output=True, timeout=30)
                    combined = (completed.stdout + completed.stderr).decode("utf-8", errors="replace")
                    (fixture / (host + ("_failure" if fail else "_success") + ".log")).write_text(combined, encoding="utf-8")
                    self.assertEqual(completed.returncode, 1 if fail else 0, combined)
                    for marker in ("host marker 日本語", "native stdout marker 日本語", "native stderr marker 日本語"):
                        self.assertEqual(combined.count(marker), 1, combined)
                    self.assertNotIn("XML", combined)
                    self.assertNotIn("System.Management.Automation.RemoteException", combined)
                    self.assertNotIn("\ufffd", combined)
                    if fail:
                        self.assertIn("intended failure marker 日本語", combined)
                        self.assertIn(str(child), combined)

    def notice_windows(self, desktop):
        api = runner.Windows()
        api.u.EnumDesktopWindows.argtypes = [w.HANDLE, c.c_void_p, w.LPARAM]
        api.u.EnumChildWindows.argtypes = [w.HWND, c.c_void_p, w.LPARAM]
        api.u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
        api.u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
        api.u.IsWindowVisible.argtypes = [w.HWND]
        api.u.IsWindowVisible.restype = w.BOOL
        api.u.GetWindowLongPtrW.argtypes = [w.HWND, c.c_int]
        api.u.GetWindowLongPtrW.restype = c.c_ssize_t
        api.u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
        callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        found = []
        @callback_type
        def top(window, unused):
            title = c.create_unicode_buffer(512)
            api.u.GetWindowTextW(window, title, len(title))
            if title.value == "検査コントロール": found.append(window)
            return True
        c.set_last_error(0)
        if not api.u.EnumDesktopWindows(desktop.desktop, top, 0) and c.get_last_error():
            raise c.WinError(c.get_last_error())
        return api, callback_type, found

    def test_supervised_notice_lifetime_and_all_exit_paths(self):
        # All UI runs on an unswitched desktop. Only the routing decision is
        # overridden; the actual notice/events, owned jobs and cleanup are used.
        desktop = runner.PrivateDesktop()
        try:
            for mode in ("success", "failure", "timeout", "cancel_before", "cancel_button", "cancel_close",
                         "notice_crash", "launcher_death", "isolated_cancel", "owner_hang", "owner_hang_cancel", "ui_hang", "cleanup_failure"):
                with self.subTest(mode=mode), self.output("supervised_" + mode) as output:
                    fixture = self.root / ("files_" + mode)
                    fixture.mkdir()
                    original = fixture / "original.txt"
                    original.write_bytes(b"keep original")
                    lock_path = fixture / "owned.lock"
                    partial = fixture / "partial.bin"
                    code = PROBE.replace("a.u.SetForegroundWindow(window)",
                        "a.u.SetForegroundWindow(window)\n"
                        f"from pathlib import Path\npartial=Path({str(partial)!r})\npartial.write_bytes(b'partial evidence')\n"
                        "a.k.CreateFileW.argtypes=[w.LPCWSTR,w.DWORD,w.DWORD,c.c_void_p,w.DWORD,w.DWORD,w.HANDLE]\n"
                        "a.k.CreateFileW.restype=w.HANDLE\n"
                        f"lock=a.k.CreateFileW({str(lock_path)!r},0x40000000,0,None,2,0,None)\n"
                        "assert lock != c.c_void_p(-1).value\n")
                    if mode == "failure": code += "\nsys.exit(7)\n"
                    command = [sys.executable, "-B", "-c", code]
                    if mode not in ("success", "failure", "cleanup_failure"): command += ["--wait"]
                    hang_setup = ""
                    if mode.startswith("owner_hang"):
                        hang_setup = ("import time\noriginal_running=r.SupervisedNotice.running\n"
                            "def hang(self):\n original_running(self)\n time.sleep(60)\n"
                            "r.SupervisedNotice.running=hang\n")
                    if mode == "cleanup_failure":
                        hang_setup = ("original_close=r.OwnedProcess.close\n"
                            "def denied(self, **kwargs):\n original_close(self, **kwargs)\n"
                            " if self.job_name and self.job_name.endswith('_tests'): raise RuntimeError('injected cleanup confirmation failure')\n"
                            "r.OwnedProcess.close=denied\n")
                    controller = (
                        "from tools.dev import isolated_app_test as r\n"
                        "from unittest import mock\nimport sys, subprocess\n" + hang_setup +
                        f"with mock.patch.object(r,'input_desktop_path',return_value={desktop.path!r}):\n"
                        " try:\n"
                        f"  code=r.run({command!r}, {str(ROOT)!r}, {1 if mode == 'timeout' else 20}, interactive={mode != "isolated_cancel"})\n"
                        " except r.TestInterrupted: code=130\n"
                        " except subprocess.TimeoutExpired: code=124\n"
                        " except RuntimeError as e: print(str(e),flush=True); code=3\n"
                        " sys.exit(code)\n")
                    process = runner.OwnedProcess([sys.executable, "-B", "-c", controller],
                        ROOT, desktop.path, stdout=output, stderr=output)
                    notice_pid = None
                    evidence = None
                    observed_visible = False
                    try:
                        deadline = time.monotonic() + 25
                        action_taken = False
                        while time.monotonic() < deadline and process.poll() is None:
                            api, callback_type, windows = self.notice_windows(desktop)
                            if windows:
                                window = windows[0]
                                pid = w.DWORD(); api.u.GetWindowThreadProcessId(window, c.byref(pid))
                                notice_pid = pid.value
                                self.assertTrue(api.u.GetWindowLongPtrW(window, -20) & 0x08000000)
                                buttons = []
                                @callback_type
                                def child(control, unused):
                                    title = c.create_unicode_buffer(512)
                                    api.u.GetWindowTextW(control, title, len(title))
                                    if title.value == "中断": buttons.append(control)
                                    return True
                                api.u.EnumChildWindows(window, child, 0)
                                if not buttons or b"[NOTICE] displayed;" not in read_output(output):
                                    time.sleep(0.02)
                                    continue
                                visible = api.u.IsWindowVisible(window)
                                if not visible:
                                    # Enumeration and visibility queries are not atomic.
                                    # Emergency shutdown may finish between them, before
                                    # the launcher's process handle reports termination.
                                    closing_log = read_output(output)
                                    confirmed = (b"emergency_owned_active=0; cleanup_confirmed=True" in closing_log or
                                        b"terminal; cleanup_confirmed=True; owned_active=0" in closing_log)
                                    if confirmed and observed_visible:
                                        if evidence:
                                            self.require_stopped(evidence["pid"])
                                            self.require_stopped(evidence["child"])
                                        time.sleep(0.02)
                                        continue
                                self.assertTrue(visible, "notice logged displayed but is hidden before confirmed cleanup")
                                observed_visible = True
                            data = read_output(output)
                            for line in data.splitlines():
                                if line.startswith(b'{"mode"'): evidence = json.loads(line)
                            if not action_taken and windows:
                                if mode == "cancel_before" or (evidence and mode in ("cancel_button", "isolated_cancel", "owner_hang_cancel")):
                                    api.check(api.u.PostMessageW(buttons[0], 0xF5, 0, 0))  # BM_CLICK
                                    action_taken = True
                                elif evidence and mode == "cancel_close":
                                    api.check(api.u.PostMessageW(window, 0x10, 0, 0))
                                    action_taken = True
                                elif evidence and mode == "ui_hang":
                                    tid = api.u.GetWindowThreadProcessId(window, None)
                                    api.k.OpenThread.argtypes = [w.DWORD, w.BOOL, w.DWORD]
                                    api.k.OpenThread.restype = w.HANDLE
                                    api.k.SuspendThread.argtypes = [w.HANDLE]
                                    api.k.SuspendThread.restype = w.DWORD
                                    thread = api.check(api.k.OpenThread(2, False, tid))
                                    try: self.assertNotEqual(api.k.SuspendThread(thread), 0xFFFFFFFF)
                                    finally: api.check(api.k.CloseHandle(thread))
                                    action_taken = True
                                elif evidence and mode == "notice_crash":
                                    # Kill just this owned notice; the launcher must stop its test.
                                    api.k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
                                    api.k.OpenProcess.restype = w.HANDLE
                                    handle = api.check(api.k.OpenProcess(1, False, notice_pid))
                                    try: api.check(api.k.TerminateProcess(handle, 9))
                                    finally: api.check(api.k.CloseHandle(handle))
                                    action_taken = True
                                elif evidence and mode == "launcher_death":
                                    api.check(api.k.TerminateProcess(process.handle, 9))
                                    action_taken = True
                            if evidence and mode not in ("success", "failure", "cancel_before"):
                                if not windows and not action_taken:
                                    self.require_stopped(evidence["pid"])
                                    self.require_stopped(evidence["child"])
                            time.sleep(0.02)
                        expected = {"success": 0, "failure": 7, "timeout": 124, "cancel_before": 130,
                            "cancel_button": 130, "cancel_close": 130, "notice_crash": 3, "launcher_death": 9, "isolated_cancel": 130, "owner_hang": 3, "owner_hang_cancel": 130, "ui_hang": 3, "cleanup_failure": 3}[mode]
                        self.assertEqual(process.wait(3), expected, data.decode("utf-8", errors="replace"))
                        self.assertIsNotNone(notice_pid, "notice was not displayed")
                        self.assertTrue(observed_visible, "notice was never observed visible")
                        self.require_stopped(notice_pid)
                        if mode == "cancel_before":
                            self.assertIsNone(evidence, "test started after grace-period cancellation")
                        else:
                            self.assertIsNotNone(evidence, data.decode("utf-8", errors="replace"))
                            self.require_stopped(evidence["pid"]); self.require_stopped(evidence["child"])
                        self.assertFalse(self.notice_windows(desktop)[2], "notice window remains")
                        log = read_output(output).decode("utf-8", errors="replace")
                        if mode != "cancel_before":
                            measured = [int(line.split("=", 1)[1]) for line in log.splitlines()
                                if line.startswith("[NOTICE] grace_elapsed_ms=")]
                            self.assertEqual(len(measured), 1, log)
                            self.assertGreaterEqual(measured[0], 5000)
                        if mode not in ("notice_crash", "launcher_death", "owner_hang", "owner_hang_cancel", "ui_hang", "cleanup_failure"):
                            self.assertIn("test cleanup completed; notice closed", log)
                        record_line = next(line for line in log.splitlines() if line.startswith("[CONTROL]") and "record: " in line)
                        record_path = Path(record_line.split("record: ", 1)[1])
                        receipt = json.loads(record_path.read_text(encoding="utf-8"))
                        if mode.startswith("owner_hang"):
                            emergency = json.loads((record_path.parent / "emergency.json").read_text(encoding="utf-8"))
                            self.assertTrue(emergency["cleanup_confirmed"])
                            self.assertEqual(emergency["owned_active"], 0)
                        elif mode in ("launcher_death", "cleanup_failure"):
                            self.assertFalse(receipt["cleanup_confirmed"], "abrupt death must leave unconfirmed evidence")
                        else:
                            self.assertTrue(receipt["cleanup_confirmed"], receipt)
                            self.assertEqual(receipt["phase"], "finished")
                        self.assertEqual(original.read_bytes(), b"keep original")
                        if mode != "cancel_before":
                            self.assertEqual(partial.read_bytes(), b"partial evidence")
                            # Exclusive file handles held by a test must be released.
                            with lock_path.open("ab") as unlocked:
                                unlocked.write(b"lock released")
                        if mode == "ui_hang":
                            self.assertIn("control window is unresponsive", log)
                        if mode == "cleanup_failure":
                            self.assertIn("cleanup_confirmed=False", log)
                        if evidence:
                            self.assertTrue(Path(evidence["temp"]).is_relative_to(record_path.parent))
                            self.assertTrue(Path(evidence["appdata"]).is_relative_to(record_path.parent))
                        if mode == "isolated_cancel":
                            self.assertEqual(evidence["mode"], "isolated")
                            self.assertNotEqual(evidence["desktop"], desktop.name)
                    finally:
                        process.close()
        finally:
            desktop.close()


if __name__ == "__main__":
    unittest.main()
