"""Exercise the built app's diagnostic conversion in an owned, retained fixture.

Windows only. Sends scalar Win32 messages solely to the process it launches;
never opens a user's workspace or terminates an unrelated process.
"""
import argparse
import ctypes
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--locale", choices=("ja", "en"), default="ja")
    parser.add_argument("--workspace-length", type=int, default=0,
                        help="UTF-16 units; a fresh Unicode/space-containing workspace tests deep paths")
    args = parser.parse_args()
    if sys.platform != "win32":
        raise RuntimeError("This smoke test requires Windows")
    repo = Path(__file__).resolve().parents[2]
    source = repo / ("out/bin_en" if args.locale == "en" else "out/bin")
    # Two ancestors reach the repository, where the app resolves its approved
    # bundled runtime. No junctions or mutable runtime hardlinks are installed.
    root = repo / "out" / ("wc_" + uuid.uuid4().hex[:8])
    root.mkdir()
    print(f"Owned fixture: {root}", flush=True)
    for name in ("pdf_note_workspace.exe", "pdfium.dll", "zlib1.dll",
                 "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"):
        if not (source / name).is_file():
            raise RuntimeError(f"Missing required build artifact: {name}")
        shutil.copy2(source / name, root / name)
    build_info = source / "pdf_note_workspace.exe.buildinfo.txt"
    if not build_info.is_file() or "edition\tfull" not in build_info.read_text(encoding="utf-8-sig"):
        raise RuntimeError("A Full app build is required")
    workspace = root / "w"
    if args.workspace_length:
        workspace /= "日本語 space"
        while len(str(workspace).encode("utf-16-le")) // 2 < args.workspace_length:
            remaining = args.workspace_length - len(str(workspace).encode("utf-16-le")) // 2 - 1
            if remaining == 0:
                workspace = workspace.with_name(workspace.name + "x")
                break
            workspace /= "x" * min(remaining, 40)
    workspace.mkdir(parents=True)
    host_temp = root / "host_temp"
    host_temp.mkdir()
    (root / "pdf_note_workspace_setup.json").write_text(json.dumps({
        "workspaceRootMode": "relative", "workspaceRoot": str(workspace.relative_to(root))
    }), encoding="utf-8")
    u = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    u.EnumWindows.argtypes = [callback_type, w.LPARAM]
    u.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
    u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
    u.GetDlgItem.argtypes = [w.HWND, ctypes.c_int]
    u.GetDlgItem.restype = w.HWND
    u.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM,
                                    w.UINT, w.UINT, ctypes.POINTER(ctypes.c_size_t)]
    u.SendMessageTimeoutW.restype = w.LPARAM
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    environment = os.environ.copy()
    environment.update(TEMP=str(host_temp), TMP=str(host_temp))
    app = subprocess.Popen([str(root / "pdf_note_workspace.exe")], cwd=root,
                           startupinfo=startup, env=environment)

    def find(cls):
        matches = []

        @callback_type
        def visit(hwnd, _):
            pid = w.DWORD()
            u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == app.pid:
                name = ctypes.create_unicode_buffer(128)
                u.GetClassNameW(hwnd, name, len(name))
                if name.value == cls:
                    matches.append(hwnd)
            return True

        if not u.EnumWindows(visit, 0):
            raise ctypes.WinError(ctypes.get_last_error())
        return matches[0] if matches else None

    def send(hwnd, msg, wp=0, lp=0):
        result = ctypes.c_size_t()
        if not u.SendMessageTimeoutW(hwnd, msg, wp, lp, 2, 2000, ctypes.byref(result)):
            raise RuntimeError(f"Owned app message failed: {msg}")
        return result.value

    def wait(action, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if app.poll() is not None:
                raise RuntimeError(f"Owned app exited early: {app.returncode}")
            result = action()
            if result:
                return result
            time.sleep(0.1)
        raise RuntimeError("Owned app check timed out")

    main_window = None
    try:
        main_window = wait(lambda: find("PdfWorkspaceMainWnd"))
        send(main_window, 0)  # WM_NULL: finish startup before measuring dialog side effects.
        record = workspace / "__pdf_note_workspace__/__log__/write_checks.log"
        before_open = record.read_bytes() if record.exists() else None
        send(main_window, 0x111, 3405)  # Help command
        dialog = wait(lambda: find("PdfNoteWriteChecksWnd"))
        control = u.GetDlgItem(dialog, 4702)
        if send(control, 0x1004) != 16:  # LVM_GETITEMCOUNT
            raise RuntimeError("The app did not display all 16 checks")
        after_open = record.read_bytes() if record.exists() else None
        if after_open != before_open:
            raise RuntimeError("Opening the dialog wrote a result unexpectedly")
        send(control, 0x100, 0x23)  # WM_KEYDOWN / VK_END: select conversion
        send(control, 0x101, 0x23)
        send(dialog, 0x111, 4703)

        def conversion_record():
            if not record.exists():
                return None
            try:
                contents = record.read_text(encoding="utf-8")
            except (PermissionError, FileNotFoundError):
                # Atomic replacement/verification can briefly hold the record.
                # The existing bounded wait must still observe a completed row.
                return None
            for line in contents.splitlines():
                if line.startswith("12 "):
                    return line
            return None

        line = wait(conversion_record, 90)
        if not re.fullmatch(r'12 "[^"\n]+" 0 8 0 [1-9][0-9]* "" 0 ""', line):
            raise RuntimeError(f"Conversion did not finish successfully: {line}")
        managed = workspace / "__pdf_note_workspace__/__tmp__/lo"
        if not managed.is_dir() or any(managed.iterdir()):
            raise RuntimeError("Conversion did not clean its workspace operation directory")
        if any(host_temp.iterdir()):
            raise RuntimeError("The app or conversion wrote into the inherited OS temporary folder")
        if {path.name for path in root.iterdir() if path.is_dir()} != {"w", "host_temp"}:
            raise RuntimeError("The app created an unexpected top-level directory")
        send(dialog, 0x10)  # WM_CLOSE
        wait(lambda: not find("PdfNoteWriteChecksWnd"))
        send(main_window, 0x111, 3405)
        dialog = wait(lambda: find("PdfNoteWriteChecksWnd"))
        if conversion_record() != line:
            raise RuntimeError("The persisted conversion result changed on reopen")
        # Block only our empty, owned conversion root with an existing file.
        # The app must preserve it and report failure without a host fallback.
        managed.relative_to(workspace)
        managed.rmdir()
        sentinel = b"owned conversion-root blocker: preserve this file"
        managed.write_bytes(sentinel)
        control = u.GetDlgItem(dialog, 4702)
        send(control, 0x100, 0x23)
        send(control, 0x101, 0x23)
        send(dialog, 0x111, 4703)

        def changed_record():
            updated = conversion_record()
            return updated if updated and updated != line else None

        failed = wait(changed_record)
        if not re.fullmatch(r'12 "[^"\n]+" 1 0 5 [1-9][0-9]* "" 0 ""', failed):
            raise RuntimeError(f"Blocked conversion did not report the expected failure: {failed}")
        if managed.read_bytes() != sentinel or any(host_temp.iterdir()):
            raise RuntimeError("Blocked conversion changed existing data or used host TEMP")
        print("App: 16 items, PDF validation, workspace-only area, empty host TEMP, cleanup, persistent result and blocked-root preservation passed", flush=True)
    finally:
        if app.poll() is None:
            if main_window:
                try:
                    send(main_window, 0x10)
                except RuntimeError:
                    pass
            try:
                app.wait(timeout=10)
            except subprocess.TimeoutExpired:
                app.kill()  # only our copied, owned application
                app.wait()
                raise RuntimeError("Owned app did not close cleanly")
        # Preserve the fixture and records for inspection; no recursive cleanup.
        if app.returncode != 0:
            raise RuntimeError(f"Owned app closed with a failure exit code: {app.returncode}")


if __name__ == "__main__":
    main()
