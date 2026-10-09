from __future__ import annotations

import ctypes
from ctypes import wintypes as w
import importlib.util
import os
from pathlib import Path
import subprocess
import time
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("background_app_runner", Path(__file__).with_name("run_background_app.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class BackgroundAppTests(unittest.TestCase):
    def exercise(self, *, foreground=False, exit_code=0, pump_error=None):
        monitor = mock.Mock(took_foreground=foreground)
        monitor.pump.side_effect = pump_error
        process = mock.Mock(pid=123)
        process.poll.return_value = exit_code
        factory = mock.Mock(return_value=monitor)
        return monitor, process, factory

    def test_observed_success_and_child_failure_are_distinct(self):
        for exit_code in (0, 7):
            monitor, process, factory = self.exercise(exit_code=exit_code)
            with mock.patch.object(runner.subprocess, "Popen", return_value=process):
                self.assertEqual(runner.run(["child"], Path.cwd(), 1, factory), exit_code)
            self.assertGreaterEqual(monitor.pump.call_count, 2)
            monitor.close.assert_called_once()
            process.terminate.assert_not_called()

    def test_foreground_acquisition_fails_and_stops_owned_child(self):
        monitor, process, factory = self.exercise(foreground=True, exit_code=None)
        with mock.patch.object(runner.subprocess, "Popen", return_value=process):
            with self.assertRaisesRegex(RuntimeError, "took the Windows foreground"):
                runner.run(["child"], Path.cwd(), 1, factory)
        process.terminate.assert_called_once()
        process.wait.assert_called_once_with(timeout=5)
        monitor.close.assert_called_once()

    def test_observation_failure_is_not_success(self):
        monitor, process, factory = self.exercise(exit_code=None, pump_error=RuntimeError("unavailable"))
        with mock.patch.object(runner.subprocess, "Popen", return_value=process):
            with self.assertRaisesRegex(RuntimeError, "unavailable"):
                runner.run(["child"], Path.cwd(), 1, factory)
        process.terminate.assert_called_once()
        monitor.close.assert_called_once()

    def test_hook_unavailable_prevents_child_launch(self):
        with mock.patch.object(runner.subprocess, "Popen") as launch:
            with self.assertRaises(OSError):
                runner.run(["child"], Path.cwd(), 1, mock.Mock(side_effect=OSError("hook")))
        launch.assert_not_called()

    def test_launch_failure_releases_hook(self):
        monitor, _, factory = self.exercise()
        with mock.patch.object(runner.subprocess, "Popen", side_effect=OSError("launch")):
            with self.assertRaises(OSError):
                runner.run(["child"], Path.cwd(), 1, factory)
        monitor.close.assert_called_once()

    def test_timeout_and_unresponsive_child_fail_with_cleanup(self):
        monitor, process, factory = self.exercise(exit_code=None)
        process.wait.side_effect = [subprocess.TimeoutExpired("child", 5), 0]
        with mock.patch.object(runner.subprocess, "Popen", return_value=process), \
                mock.patch.object(runner.time, "monotonic", side_effect=[0, 2]):
            with self.assertRaisesRegex(RuntimeError, "timed out"):
                runner.run(["child"], Path.cwd(), 1, factory)
        process.terminate.assert_called_once()
        process.kill.assert_called_once()
        monitor.close.assert_called_once()

    def test_unhook_failure_is_not_success(self):
        monitor, process, factory = self.exercise()
        monitor.close.side_effect = OSError("unhook")
        with mock.patch.object(runner.subprocess, "Popen", return_value=process):
            with self.assertRaises(OSError):
                runner.run(["child"], Path.cwd(), 1, factory)

    @unittest.skipUnless(os.name == "nt", "Windows event-hook integration")
    def test_windows_event_hook_observes_owned_window(self):
        # Publish an event for a hidden fixture: verify hook delivery and PID
        # filtering without actually changing the user's foreground window.
        monitor = runner.ForegroundMonitor()
        user = monitor.user
        user.CreateWindowExW.argtypes = [w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD,
                                         ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                         w.HWND, w.HMENU, w.HINSTANCE, w.LPVOID]
        user.CreateWindowExW.restype = w.HWND
        user.DestroyWindow.argtypes = [w.HWND]
        user.NotifyWinEvent.argtypes = [w.DWORD, w.HWND, w.LONG, w.LONG]
        window = None
        try:
            monitor.pump(0)  # Ignore any existing foreground process.
            window = user.CreateWindowExW(0, "STATIC", "Hidden focus fixture", 0,
                                          0, 0, 10, 10, None, None, None, None)
            self.assertTrue(window, "hidden fixture creation must succeed")
            user.NotifyWinEvent(3, window, 0, 0)
            monitor.pump(0)
            self.assertFalse(monitor.took_foreground, "ignore other process IDs")
            monitor.pump(os.getpid())
            user.NotifyWinEvent(3, window, 0, 0)
            deadline = time.monotonic() + 2
            while not monitor.took_foreground and time.monotonic() < deadline:
                monitor.pump(os.getpid())
                time.sleep(0.01)
            self.assertTrue(monitor.took_foreground, "owned-window event must be delivered")
        finally:
            if window:
                user.DestroyWindow(window)
            monitor.close()


if __name__ == "__main__":
    unittest.main()
