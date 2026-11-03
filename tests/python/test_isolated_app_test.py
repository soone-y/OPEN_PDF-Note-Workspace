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
    'pid': os.getpid(), 'child': child.pid}), flush=True)
if '--wait' in sys.argv: time.sleep(60)
a.check(a.u.DestroyWindow(window))
'''


class IsolatedAppTests(unittest.TestCase):
    def test_invalid_boundary_input_does_not_launch(self):
        with mock.patch.object(runner, "PrivateDesktop") as create:
            for command, timeout in (([], 1), (["child"], 0), (["child"], float("nan"))):
                with self.assertRaises(ValueError):
                    runner.run(command, ROOT, timeout)
            create.assert_not_called()

    def test_isolation_failure_never_launches_on_input_desktop(self):
        with mock.patch.object(runner, "PrivateDesktop", side_effect=PermissionError("denied")), \
                mock.patch.object(runner, "OwnedProcess") as launch:
            with self.assertRaises(PermissionError):
                runner.run(["child"], ROOT, 1)
            launch.assert_not_called()

    def test_private_clipboard_failure_requires_successful_notice(self):
        for outcome, message in ((2, "interrupted"), (1, "failed"),
                                 (subprocess.TimeoutExpired("notice", 30), "timed out")):
            with self.subTest(outcome=outcome), \
                mock.patch.object(runner, "PrivateDesktop", side_effect=PermissionError("denied")), \
                mock.patch.object(runner, "current_station", return_value="WinSta0"), \
                mock.patch.object(runner, "input_desktop_path", return_value="winsta0\\Default"), \
                mock.patch.object(runner, "OwnedProcess") as launch:
                if isinstance(outcome, Exception): launch.return_value.wait.side_effect = outcome
                else: launch.return_value.wait.return_value = outcome
                with self.assertRaisesRegex(RuntimeError, message):
                    runner.run(["must-not-run"], ROOT, 1, private_clipboard=True)
                self.assertEqual(launch.call_count, 1)
                self.assertIn("show_interactive_test_notice.ps1", str(launch.call_args.args[0]))
                launch.return_value.close.assert_called_once()

    def test_notice_success_is_required_before_interactive_child(self):
        with mock.patch.object(runner, "current_station", return_value="WinSta0"), \
                mock.patch.object(runner, "input_desktop_path", return_value="winsta0\\Default"), \
                mock.patch.object(runner, "OwnedProcess") as launch:
            notice, child = mock.Mock(), mock.Mock()
            notice.wait.return_value = 0; child.wait.return_value = 7
            launch.side_effect = [notice, child]
            self.assertEqual(runner.run(["child"], ROOT, 1, interactive=True), 7)
            self.assertEqual(launch.call_args.args[2], "winsta0\\Default")
            self.assertEqual(launch.call_args.kwargs["env"]["PDF_NOTE_TEST_INTERACTIVE"], "notified")
            notice.close.assert_called_once(); child.close.assert_called_once()


@unittest.skipUnless(os.name == "nt", "Windows desktop/job integration")
class IsolatedAppWindowsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = ROOT / "out/tests" / ("desktop_boundary_" + uuid.uuid4().hex)
        cls.root.mkdir(parents=True)

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
        with self.output("probe") as output:
            self.assertEqual(runner.run([sys.executable, "-B", "-c", PROBE], ROOT, 10, stdout=output, stderr=output), 0)
            output.seek(0); evidence = json.loads(output.read())
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
            output.seek(0); evidence = json.loads(output.read())
        self.require_stopped(evidence["pid"]); self.require_stopped(evidence["child"])

    def test_assignment_failure_never_resumes_child(self):
        marker = self.root / "must_not_execute.txt"
        api = runner.Windows()
        desktop = runner.PrivateDesktop()
        try:
            with self.output("assign") as output, mock.patch.object(runner, "Windows", return_value=api), \
                    mock.patch.object(api.k, "AssignProcessToJobObject", return_value=False):
                c.set_last_error(5)
                with self.assertRaises(OSError):
                    runner.OwnedProcess([sys.executable, "-B", "-c",
                        f"from pathlib import Path; Path({str(marker)!r}).write_text('unexpected')"],
                        ROOT, desktop.path, stdout=output, stderr=output)
            self.assertFalse(marker.exists())
        finally:
            desktop.close()

    def test_launcher_termination_kills_child_tree(self):
        with self.output("launcher_death") as output:
            process = subprocess.Popen([sys.executable, "-B", str(ROOT / "tools/dev/isolated_app_test.py"),
                "--timeout", "30", "--", sys.executable, "-B", "-c", PROBE, "--wait"],
                cwd=ROOT, stdout=output, stderr=output, creationflags=0x08000000)
            evidence = None
            try:
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    output.seek(0)
                    for line in output.read().splitlines():
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

    def test_powershell_reentry_preserves_bound_values(self):
        script = self.root / "bound parameters 日本語.ps1"
        result = self.root / "bound.json"
        helper = str(ROOT / "tests/scripts/app_test_desktop.ps1").replace("'", "''")
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
        helper = str(ROOT / "tests/scripts/app_test_desktop.ps1").replace("'", "''")
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

    def test_passive_notice_completes_without_input_and_early_close_fails(self):
        # Exercise the actual notice on an unswitched fixture desktop. The
        # test never shows the notice on the user's screen or injects input.
        api = runner.Windows()
        api.u.EnumDesktopWindows.argtypes = [w.HANDLE, c.c_void_p, w.LPARAM]
        api.u.EnumChildWindows.argtypes = [w.HWND, c.c_void_p, w.LPARAM]
        api.u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
        api.u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
        api.u.GetWindowLongPtrW.argtypes = [w.HWND, c.c_int]
        api.u.GetWindowLongPtrW.restype = c.c_ssize_t
        api.u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
        callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        desktop = runner.PrivateDesktop()
        try:
            for interrupt in (False, True):
                with self.output("notice") as output:
                    process = runner.OwnedProcess(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        str(ROOT / "tests/scripts/show_interactive_test_notice.ps1")], ROOT, desktop.path,
                        stdout=output, stderr=output)
                    try:
                        found = []
                        @callback_type
                        def top(window, unused):
                            title = c.create_unicode_buffer(256)
                            api.u.GetWindowTextW(window, title, len(title))
                            if title.value == "App test starts soon": found.append(window)
                            return True
                        deadline = time.monotonic() + 15
                        while not found and time.monotonic() < deadline and process.poll() is None:
                            api.u.EnumDesktopWindows(desktop.desktop, top, 0)
                            time.sleep(0.02)
                        output.flush()
                        self.assertTrue(found, "actual notice failed to open; see " + output.name)
                        window = found[0]
                        self.assertTrue(api.u.GetWindowLongPtrW(window, -20) & 0x08000000)
                        buttons = []
                        labels = []
                        @callback_type
                        def child(control, unused):
                            title = c.create_unicode_buffer(512)
                            api.u.GetWindowTextW(control, title, len(title))
                            name = c.create_unicode_buffer(128)
                            api.u.GetClassNameW(control, name, len(name))
                            if "BUTTON" in name.value.upper(): buttons.append(control)
                            if "この通知への操作は不要" in title.value: labels.append(control)
                            return True
                        deadline = time.monotonic() + 3
                        while not labels and time.monotonic() < deadline:
                            api.u.EnumChildWindows(window, child, 0)
                            time.sleep(0.02)
                        self.assertFalse(buttons, "passive notice must not require a button click")
                        self.assertTrue(labels, "Japanese notice explains that no action is required")
                        self.assertIsNone(process.poll())
                        if interrupt:
                            api.check(api.u.PostMessageW(window, 0x10, 0, 0))  # Close only the hidden fixture.
                        self.assertEqual(process.wait(10), 2 if interrupt else 0)
                        output.seek(0); log = output.read().decode("utf-8", errors="replace")
                        if not interrupt:
                            measured = [int(line.split("=", 1)[1]) for line in log.splitlines()
                                        if line.startswith("[NOTICE] grace_elapsed_ms=")]
                            self.assertEqual(len(measured), 1, log)
                            self.assertGreaterEqual(measured[0], 5000)
                            self.assertIn("starting test automatically", log)
                        else:
                            self.assertIn("closed before the grace period completed", log)
                    finally:
                        process.close()
        finally:
            desktop.close()


if __name__ == "__main__":
    unittest.main()
