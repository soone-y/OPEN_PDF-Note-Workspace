#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ctypes
import json
import os
import subprocess
import sys
import time
from ctypes import wintypes
from datetime import datetime
from pathlib import Path


TH32CS_SNAPPROCESS = 0x00000002
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
MAX_PATH = 260


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", wintypes.LONG),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * MAX_PATH),
    ]


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("th32ModuleID", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("GlblcntUsage", wintypes.DWORD),
        ("ProccntUsage", wintypes.DWORD),
        ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)),
        ("modBaseSize", wintypes.DWORD),
        ("hModule", wintypes.HMODULE),
        ("szModule", wintypes.WCHAR * 256),
        ("szExePath", wintypes.WCHAR * MAX_PATH),
    ]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run a command and repeatedly observe DLLs loaded by LibreOffice processes. "
            "An unobserved DLL is only a deletion candidate, never proof that removal is safe."
        )
    )
    parser.add_argument("--runtime-root", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--interval-ms", type=int, default=10)
    parser.add_argument("--grace-ms", type=int, default=500)
    parser.add_argument(
        "--process-name",
        action="append",
        default=[],
        help="Process image name to observe. May be repeated.",
    )
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command and args.command[0] == "--":
        args.command = args.command[1:]
    if not args.command:
        parser.error("a command is required after --")
    if args.interval_ms < 1:
        parser.error("--interval-ms must be at least 1")
    if args.grace_ms < 0:
        parser.error("--grace-ms must not be negative")
    return args


def normalize_path(path: Path | str) -> str:
    return os.path.normcase(os.path.abspath(os.fspath(path)))


def is_inside(path: Path | str, root: Path | str) -> bool:
    try:
        return os.path.commonpath((normalize_path(path), normalize_path(root))) == normalize_path(root)
    except ValueError:
        return False


def runtime_binary_inventory(runtime_root: Path) -> dict[str, int]:
    result: dict[str, int] = {}
    for path in runtime_root.rglob("*"):
        if path.is_file() and path.suffix.lower() in {".dll", ".exe", ".com", ".bin", ".pyd"}:
            result[path.relative_to(runtime_root).as_posix()] = path.stat().st_size
    return result


def configure_kernel32():
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.argtypes = (wintypes.DWORD, wintypes.DWORD)
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.Process32FirstW.argtypes = (wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W))
    kernel32.Process32FirstW.restype = wintypes.BOOL
    kernel32.Process32NextW.argtypes = (wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W))
    kernel32.Process32NextW.restype = wintypes.BOOL
    kernel32.Module32FirstW.argtypes = (wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32W))
    kernel32.Module32FirstW.restype = wintypes.BOOL
    kernel32.Module32NextW.argtypes = (wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32W))
    kernel32.Module32NextW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CloseHandle.restype = wintypes.BOOL
    return kernel32


def matching_processes(kernel32, names: frozenset[str]) -> list[tuple[int, str]]:
    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snapshot == INVALID_HANDLE_VALUE:
        return []
    result: list[tuple[int, str]] = []
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
        while ok:
            image_name = entry.szExeFile.lower()
            if image_name in names:
                result.append((int(entry.th32ProcessID), entry.szExeFile))
            ok = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return result


def process_modules(kernel32, pid: int) -> list[str]:
    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snapshot == INVALID_HANDLE_VALUE:
        return []
    result: list[str] = []
    try:
        entry = MODULEENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = kernel32.Module32FirstW(snapshot, ctypes.byref(entry))
        while ok:
            result.append(entry.szExePath)
            ok = kernel32.Module32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return result


def write_json_atomic(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temporary, path)


def main() -> int:
    args = parse_args()
    if os.name != "nt":
        raise SystemExit("This probe currently requires Windows Toolhelp APIs.")

    runtime_root = Path(args.runtime_root).resolve()
    output_path = Path(args.output).resolve()
    if not (runtime_root / "program" / "soffice.com").is_file():
        raise SystemExit(f"LibreOffice runtime not found: {runtime_root}")
    if is_inside(output_path, runtime_root):
        raise SystemExit("The report must be written outside the runtime tree.")

    names = frozenset(
        name.lower()
        for name in (args.process_name or ["soffice.exe", "soffice.bin", "soffice.com"])
    )
    inventory = runtime_binary_inventory(runtime_root)
    kernel32 = configure_kernel32()
    observed: set[str] = set()
    observed_processes: dict[int, str] = {}
    snapshot_count = 0
    module_snapshot_failures = 0

    started_at = datetime.now().astimezone().isoformat()
    process = subprocess.Popen(args.command)
    grace_deadline: float | None = None
    while True:
        snapshot_count += 1
        for pid, image_name in matching_processes(kernel32, names):
            observed_processes[pid] = image_name
            modules = process_modules(kernel32, pid)
            if not modules:
                module_snapshot_failures += 1
            for module in modules:
                if is_inside(module, runtime_root):
                    rel = Path(module).resolve().relative_to(runtime_root).as_posix()
                    observed.add(rel)

        return_code = process.poll()
        now = time.monotonic()
        if return_code is not None:
            if grace_deadline is None:
                grace_deadline = now + args.grace_ms / 1000.0
            if now >= grace_deadline:
                break
        time.sleep(args.interval_ms / 1000.0)

    unobserved = sorted(set(inventory) - observed)
    report = {
        "tool": "libreoffice_runtime_dynamic_probe",
        "report_version": 1,
        "started_at": started_at,
        "runtime_root": str(runtime_root),
        "command": args.command,
        "command_exit_code": process.returncode,
        "interval_ms": args.interval_ms,
        "grace_ms": args.grace_ms,
        "snapshot_count": snapshot_count,
        "module_snapshot_failures": module_snapshot_failures,
        "observed_processes": [
            {"pid": pid, "image_name": observed_processes[pid]}
            for pid in sorted(observed_processes)
        ],
        "summary": {
            "runtime_binaries": len(inventory),
            "observed_runtime_binaries": len(observed),
            "unobserved_runtime_binaries": len(unobserved),
            "observed_bytes": sum(inventory.get(path, 0) for path in observed),
            "unobserved_bytes": sum(inventory[path] for path in unobserved),
        },
        "observed_runtime_binaries": [
            {"path": path, "size_bytes": inventory.get(path, 0)} for path in sorted(observed)
        ],
        "unobserved_runtime_binaries": [
            {"path": path, "size_bytes": inventory[path]} for path in unobserved
        ],
        "limitations": [
            "Polling can miss modules that are loaded and unloaded between snapshots.",
            "The result covers only the supplied command and its exercised documents.",
            "UNO components and data files may be loaded dynamically without appearing in PE imports.",
            "An unobserved binary must pass an isolated removal experiment before deletion.",
        ],
    }
    write_json_atomic(output_path, report)
    summary = report["summary"]
    print(
        "observed="
        f"{summary['observed_runtime_binaries']}/{summary['runtime_binaries']} "
        f"unobserved_bytes={summary['unobserved_bytes']} report={output_path}"
    )
    return int(process.returncode or 0)


if __name__ == "__main__":
    raise SystemExit(main())
