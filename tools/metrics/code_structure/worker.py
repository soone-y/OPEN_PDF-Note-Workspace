"""Own and reap analysis processes, including compiler-probe descendants.

Windows worker waits for an event until attached to a kill-on-close Job Object.
POSIX uses an owned process group. No visible window or filesystem IPC is used.
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes
import os
import math
import signal
import subprocess
import uuid
from time import monotonic

from .model import AnalysisError

DEADLINE_METHOD = "active-worker-deadline/v1"
CLOCK_METHODS = {"windows-unbiased-interrupt-time-precise/v1", "windows-unbiased-interrupt-time/v1", "platform-monotonic/v1"}


class ActiveClock:
    """Windows working-state elapsed time; does not count sleep/hibernation."""
    def __init__(self):
        self.last = None
        self.api = kernel() if os.name == "nt" else None
        if self.api:
            self.query = getattr(self.api, "QueryUnbiasedInterruptTimePrecise", None)
            self.precise = self.query is not None
            if self.query is None:
                self.query = getattr(self.api, "QueryUnbiasedInterruptTime", None)
            if self.query is None:
                raise AnalysisError("sleep-excluding Windows clock unavailable")
            self.query.argtypes = [ctypes.POINTER(ctypes.c_uint64)]
            self.query.restype = None if self.precise else wintypes.BOOL
            self.method = "windows-unbiased-interrupt-time-precise/v1" if self.precise else "windows-unbiased-interrupt-time/v1"
        else:
            # Non-Windows suspend semantics depend on the platform; do not
            # advertise the Windows sleep-exclusion guarantee for this fallback.
            self.method = "platform-monotonic/v1"

    def now(self):
        if self.api:
            count = ctypes.c_uint64()
            success = self.query(ctypes.byref(count))
            if not self.precise and not success:
                raise AnalysisError("sleep-excluding Windows clock read failed")
            value = count.value / 10_000_000
        else:
            value = monotonic()
        if not math.isfinite(value) or value < 0 or (self.last is not None and value < self.last):
            raise AnalysisError("active clock is invalid or moved backwards")
        self.last = value
        return value


def kernel():
    library = ctypes.WinDLL("kernel32", use_last_error=True)
    for name, args, restype in (
        ("CreateJobObjectW", [ctypes.c_void_p, wintypes.LPCWSTR], wintypes.HANDLE),
        ("CreateEventW", [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR], wintypes.HANDLE),
        ("OpenEventW", [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR], wintypes.HANDLE),
        ("SetInformationJobObject", [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD], wintypes.BOOL),
        ("QueryInformationJobObject", [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p], wintypes.BOOL),
        ("AssignProcessToJobObject", [wintypes.HANDLE, wintypes.HANDLE], wintypes.BOOL),
        ("SetEvent", [wintypes.HANDLE], wintypes.BOOL),
        ("WaitForSingleObject", [wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
        ("CloseHandle", [wintypes.HANDLE], wintypes.BOOL),
    ):
        function = getattr(library, name); function.argtypes = args; function.restype = restype
    return library


def wait_start(name):
    if not name:
        return
    if os.name != "nt":
        raise AnalysisError("start-event is only valid on Windows")
    api = kernel()
    handle = api.OpenEventW(0x00100000, False, name)
    if not handle:
        raise AnalysisError("owned worker start event unavailable")
    try:
        if api.WaitForSingleObject(handle, 30000) != 0:
            raise AnalysisError("owned worker was not attached/released within 30 seconds")
    finally:
        api.CloseHandle(handle)


def run_worker(arguments, *, cwd, timeout):
    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or timeout <= 0:
        raise AnalysisError("worker timeout must be finite and positive")
    started = monotonic()
    clock = ActiveClock()
    active_started = clock.now()
    deadline = active_started + timeout
    job = event = process = None
    api = None
    try:
        if os.name == "nt":
            api = kernel()
            class Basic(ctypes.Structure):
                _fields_ = [("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64),
                            ("flags", wintypes.DWORD), ("min_ws", ctypes.c_size_t), ("max_ws", ctypes.c_size_t),
                            ("active_processes", wintypes.DWORD), ("affinity", ctypes.c_size_t),
                            ("priority", wintypes.DWORD), ("scheduling", wintypes.DWORD)]
            class IO(ctypes.Structure):
                _fields_ = [(name, ctypes.c_uint64) for name in ("read_ops", "write_ops", "other_ops", "read_bytes", "write_bytes", "other_bytes")]
            class Limits(ctypes.Structure):
                _fields_ = [("basic", Basic), ("io", IO), ("process_memory", ctypes.c_size_t),
                            ("job_memory", ctypes.c_size_t), ("peak_process_memory", ctypes.c_size_t), ("peak_job_memory", ctypes.c_size_t)]
            job = api.CreateJobObjectW(None, None)
            limits = Limits(); limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            if not job or not api.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
                raise AnalysisError(f"cannot create worker containment job: {ctypes.get_last_error()}")
            name = "Local\\PdfNoteCodeStructure_" + uuid.uuid4().hex
            event = api.CreateEventW(None, True, False, name)
            if not event:
                raise AnalysisError("cannot create worker start event")
            arguments = list(arguments) + ["--_start-event", name]
        process = subprocess.Popen(arguments, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   encoding="utf-8", errors="strict", start_new_session=os.name != "nt",
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if api:
            if not api.AssignProcessToJobObject(job, wintypes.HANDLE(process._handle)):
                raise AnalysisError(f"cannot attach owned worker to job: {ctypes.get_last_error()}")
            if not api.SetEvent(event):
                raise AnalysisError("cannot release attached worker")
        try:
            # Recheck the active deadline between bounded waits and after
            # successful collection. Host sleep may extend wall elapsed, but
            # never the available working-state budget.
            while True:
                remaining = deadline - clock.now()
                if remaining <= 0:
                    raise subprocess.TimeoutExpired(arguments, timeout)
                try:
                    stdout, stderr = process.communicate(timeout=min(remaining, 0.5))
                except subprocess.TimeoutExpired:
                    continue
                if clock.now() >= deadline:
                    raise subprocess.TimeoutExpired(arguments, timeout)
                break
        except subprocess.TimeoutExpired as exc:
            if api:
                api.CloseHandle(job); job = None
            else:
                os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate()
            failure = AnalysisError(f"owned analysis worker timeout after {timeout}s; no COMPLETE receipt allowed")
            failure.worker_output = subprocess.CompletedProcess(arguments, process.returncode, stdout, stderr)
            failure.worker_output.resources = {"status": "UNMEASURED", "method": "job terminated at deadline",
                                               "deadline_method": DEADLINE_METHOD, "timeout_seconds": timeout,
                                               "active_clock_method": clock.method,
                                               "active_elapsed_seconds": clock.now() - active_started,
                                               "elapsed_seconds": monotonic() - started}
            raise failure from exc
        result = subprocess.CompletedProcess(arguments, process.returncode, stdout, stderr)
        result.resources = {"status": "UNMEASURED", "method": "not available on this platform"}
        if api:
            observed = Limits()
            if not api.QueryInformationJobObject(job, 9, ctypes.byref(observed), ctypes.sizeof(observed), None):
                raise AnalysisError("owned worker peak memory measurement failed")
            result.resources = {"status": "MEASURED", "method": "Windows Job peak committed memory",
                                "peak_process_bytes": observed.peak_process_memory,
                                "peak_job_bytes": observed.peak_job_memory}
        active_elapsed = clock.now() - active_started
        elapsed = monotonic() - started
        result.resources.update(deadline_method=DEADLINE_METHOD, timeout_seconds=timeout,
                                active_clock_method=clock.method, active_elapsed_seconds=active_elapsed,
                                elapsed_seconds=elapsed, excluded_inactive_seconds=max(0.0, elapsed-active_elapsed))
        if active_elapsed >= timeout:
            failure = AnalysisError(f"owned analysis worker timeout after {timeout}s; late result rejected")
            failure.worker_output = result
            raise failure
        return result
    finally:
        if job:
            api.CloseHandle(job)
        if event:
            api.CloseHandle(event)
        if process and process.poll() is None:
            if os.name != "nt":
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
            process.wait()
