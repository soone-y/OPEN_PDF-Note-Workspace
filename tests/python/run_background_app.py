"""Run one owned test child and fail if it takes the desktop foreground.

The runner observes Windows foreground events; it never restores, locks or
changes another application's focus. Hook failure, timeout and child failure
are errors, not skipped observations. Only the owned child is stopped on error.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes as w
import math
import os
from pathlib import Path
import subprocess
import sys
import time


class ForegroundMonitor:
    def __init__(self):
        if os.name != "nt":
            raise RuntimeError("Background foreground observation requires Windows.")
        self.user = ctypes.WinDLL("user32", use_last_error=True)
        self.pid = 0
        self.took_foreground = False
        self.error = None
        callback_type = ctypes.WINFUNCTYPE(None, w.HANDLE, w.DWORD, w.HWND,
                                          w.LONG, w.LONG, w.DWORD, w.DWORD)
        self.user.SetWinEventHook.argtypes = [w.DWORD, w.DWORD, w.HMODULE,
                                             callback_type, w.DWORD, w.DWORD, w.DWORD]
        self.user.SetWinEventHook.restype = w.HANDLE
        self.user.UnhookWinEvent.argtypes = [w.HANDLE]
        self.user.UnhookWinEvent.restype = w.BOOL
        self.user.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
        self.user.GetWindowThreadProcessId.restype = w.DWORD
        self.user.GetForegroundWindow.restype = w.HWND
        self.user.PeekMessageW.argtypes = [ctypes.POINTER(w.MSG), w.HWND, w.UINT, w.UINT, w.UINT]
        self.user.PeekMessageW.restype = w.BOOL
        self.user.TranslateMessage.argtypes = [ctypes.POINTER(w.MSG)]
        self.user.DispatchMessageW.argtypes = [ctypes.POINTER(w.MSG)]
        self.user.DispatchMessageW.restype = ctypes.c_ssize_t

        def observe(_hook, _event, window, _object, _child, _thread, _tick):
            try:
                self.observe_window(window)
            except Exception as exc:
                self.error = exc

        self.callback = callback_type(observe)  # Keep callback alive until unhook.
        message = w.MSG()
        self.user.PeekMessageW(ctypes.byref(message), None, 0, 0, 0)
        # EVENT_SYSTEM_FOREGROUND, WINEVENT_OUTOFCONTEXT. The parent pumps this
        # queue while the child executes, including when its UI thread is busy.
        self.hook = self.user.SetWinEventHook(3, 3, None, self.callback, 0, 0, 0)
        if not self.hook:
            raise ctypes.WinError(ctypes.get_last_error())

    def observe_window(self, window):
        if not window:
            return
        pid = w.DWORD()
        if self.user.GetWindowThreadProcessId(window, ctypes.byref(pid)) and pid.value == self.pid:
            self.took_foreground = True

    def pump(self, pid):
        self.pid = pid
        message = w.MSG()
        while self.user.PeekMessageW(ctypes.byref(message), None, 0, 0, 1):
            if message.message == 0x12:  # WM_QUIT means observation was interrupted.
                raise RuntimeError("Foreground observation message loop was interrupted.")
            self.user.TranslateMessage(ctypes.byref(message))
            self.user.DispatchMessageW(ctypes.byref(message))
        self.observe_window(self.user.GetForegroundWindow())
        if self.error is not None:
            raise RuntimeError("Foreground event observation failed.") from self.error

    def close(self):
        if not self.user.UnhookWinEvent(self.hook):
            raise ctypes.WinError(ctypes.get_last_error())


def run(command, cwd: Path, timeout: float, monitor_factory=ForegroundMonitor):
    if not command or not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("A child command and a positive finite timeout are required.")
    monitor = monitor_factory()  # Install before launching; unavailable is failure.
    process = None
    try:
        process = subprocess.Popen(command, cwd=cwd, stdin=subprocess.DEVNULL,
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        deadline = time.monotonic() + timeout
        while True:
            monitor.pump(process.pid)
            if monitor.took_foreground:
                raise RuntimeError("Background test child took the Windows foreground.")
            exit_code = process.poll()
            if exit_code is not None:
                # Drain notifications queued as the child finished as well.
                monitor.pump(process.pid)
                if monitor.took_foreground:
                    raise RuntimeError("Background test child took the Windows foreground.")
                return exit_code
            if time.monotonic() >= deadline:
                raise RuntimeError(f"Background test timed out after {timeout:g} seconds.")
            time.sleep(0.03)
    finally:
        try:
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        finally:
            monitor.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cwd", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    try:
        return run(command, args.cwd, args.timeout)
    except Exception as exc:
        print(f"[FAIL] Background test: {exc}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
