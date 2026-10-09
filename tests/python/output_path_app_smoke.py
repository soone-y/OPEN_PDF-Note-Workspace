"""Check output pickers in a retained, owned copy of the built Windows app.

Never opens a user's workspace or stops an unrelated process. Native mode
checks opening/cancellation, not the OS dialog's sound or network behavior.
"""
import argparse
import ctypes
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", action="store_true")
    parser.add_argument("--file-operations", action="store_true", help="Check full-name validation and rename byte preservation")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    root = repo / "out/tests" / ("output_picker_" + uuid.uuid4().hex[:10])
    root.mkdir(parents=True)
    print(f"Owned fixture: {root}", flush=True)
    for name in ("pdf_note_workspace.exe", "pdfium.dll", "zlib1.dll",
                 "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"):
        shutil.copy2(repo / "out/bin" / name, root / name)
    workspace = root / "workspace"
    session = workspace / "lecture1/session1"
    destination = session / "授業 日本語 destination"
    destination.mkdir(parents=True)
    original = session / "original.txt"
    original_bytes = b"output picker source: preserve these bytes\r\n"
    original.write_bytes(original_bytes)
    pdf = session / "sample.pdf"
    if args.file_operations:
        shutil.copy2(repo / "tests/fixtures/ui_automation_session/sample.pdf", pdf)
    (workspace / "workspace.json").write_text(json.dumps({
        "classesDir": ".", "startupSelectFirstSession": True,
        "sessionFileLayout": "session_root", "sessionAutoOpenMode": "edit",
        "useNativeFileDialogs": args.native,
    }), encoding="utf-8")
    (root / "pdf_note_workspace_setup.json").write_text(json.dumps({
        "workspaceRootMode": "relative", "workspaceRoot": "workspace"
    }), encoding="utf-8")
    u = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    u.EnumWindows.argtypes = [callback_type, w.LPARAM]
    u.EnumChildWindows.argtypes = [w.HWND, callback_type, w.LPARAM]
    u.GetParent.argtypes = [w.HWND]
    u.GetParent.restype = w.HWND
    u.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
    u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
    u.GetDlgItem.argtypes = [w.HWND, ctypes.c_int]
    u.GetDlgItem.restype = w.HWND
    u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
    u.PostMessageW.restype = w.BOOL
    u.IsWindow.argtypes = [w.HWND]
    u.IsWindow.restype = w.BOOL
    u.IsWindowVisible.argtypes = [w.HWND]
    u.IsWindowVisible.restype = w.BOOL
    u.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM,
                                    w.UINT, w.UINT, ctypes.POINTER(ctypes.c_size_t)]
    u.SendMessageTimeoutW.restype = w.LPARAM
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    app = subprocess.Popen([str(root / "pdf_note_workspace.exe")], cwd=root,
                           startupinfo=startup, env=os.environ.copy())

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

    def post(hwnd, msg, wp=0, lp=0):
        if not u.PostMessageW(hwnd, msg, wp, lp):
            raise ctypes.WinError(ctypes.get_last_error())

    def wait(action):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if app.poll() is not None:
                raise RuntimeError(f"Owned app exited early: {app.returncode}")
            result = action()
            if result:
                return result
            time.sleep(0.05)
        raise RuntimeError("Owned output picker check timed out")

    def text(control):
        length = send(control, 0xE)  # WM_GETTEXTLENGTH
        buffer = ctypes.create_unicode_buffer(length + 1)
        send(control, 0xD, len(buffer), ctypes.addressof(buffer))
        return buffer.value

    def set_text(control, value):
        buffer = ctypes.create_unicode_buffer(value)
        send(control, 0xC, 0, ctypes.addressof(buffer))

    def picker():
        def ready():
            dialog = find("#32770" if args.native else "LocalPathBrowserDlg")
            # EnumWindows can observe WM_CREATE before the child controls and
            # initial listing exist. Wait for the shown, initialized dialog.
            return dialog if dialog and u.IsWindowVisible(dialog) and u.GetDlgItem(dialog, 2) else None

        dialog = wait(ready)
        if args.native and find("LocalPathBrowserDlg"):
            raise RuntimeError("Native setting still opened the app picker")
        return dialog

    main_window = None
    try:
        main_window = wait(lambda: find("PdfWorkspaceMainWnd"))
        send(main_window, 0)
        # Hidden startup deliberately does not select a session. Select the
        # owned course/session/note through the actual main-window list handlers.
        lists = []

        @callback_type
        def collect(hwnd, _):
            name = ctypes.create_unicode_buffer(128)
            u.GetClassNameW(hwnd, name, len(name))
            if u.GetParent(hwnd) == main_window and name.value.lower() == "listbox":
                lists.append(hwnd)
            return True

        u.EnumChildWindows(main_window, collect, 0)
        if len(lists) < 4:
            raise RuntimeError(f"Expected the four main file lists, found {len(lists)}")
        for control in (lists[0], lists[1], lists[3]):
            wait(lambda: send(control, 0x18B) > 0)
            send(control, 0x186, 0)
            send(main_window, 0x111, 1 << 16, control)
        print("Owned note selected", flush=True)
        if args.file_operations:
            current = original

            def rename(command, source, names, accepted=None):
                before = source.read_bytes()
                post(main_window, 0x111, command)
                dialog = wait(lambda: find("SimpleInputDlg"))
                edit = u.GetDlgItem(dialog, 101)
                if text(edit) != source.name:
                    raise RuntimeError("Rename did not show the full filename and extension")
                for name in names:
                    set_text(edit, name)
                    send(dialog, 0x111, 1)
                    if not u.IsWindow(dialog) or text(edit) != name:
                        raise RuntimeError("Invalid extension closed the rename dialog or changed its input")
                    if source.read_bytes() != before or (source.parent / name).exists():
                        raise RuntimeError("Invalid filename changed a document")
                if accepted is None:
                    post(dialog, 0x10)
                    wait(lambda: not find("SimpleInputDlg"))
                    return source
                set_text(edit, accepted)
                post(dialog, 0x111, 1)
                target = source.parent / accepted
                wait(lambda: target.exists() and not source.exists())
                if target.read_bytes() != before:
                    raise RuntimeError("Extension rename changed original document bytes")
                return target

            current = rename(3313, current, ["bad.exe", "no_extension", "bad.pdf", "bad?.txt"], "renamed.md")
            for extension in (".markdown", ".csv", ".tex", ".note", ".icr", ".clro", ".txt"):
                current = rename(3313, current, [], "renamed" + extension)
            rename(3313, current, ["still-invalid.pdf"])
            # Exercise the same folder picker from an actual move command.
            post(main_window, 0x111, 3315)
            move = wait(lambda: find("LocalPathBrowserDlg"))
            folder_list = u.GetDlgItem(move, 102)
            for index in range(send(folder_list, 0x18B)):
                buffer = ctypes.create_unicode_buffer(send(folder_list, 0x18A, index) + 1)
                send(folder_list, 0x189, index, ctypes.addressof(buffer))
                if buffer.value == "[DIR] " + destination.name:
                    send(folder_list, 0x186, index)
                    post(move, 0x111, 102 | (2 << 16), folder_list)
                    break
            else:
                raise RuntimeError("Move destination was not listed")
            wait(lambda: text(u.GetDlgItem(move, 101)).endswith(destination.name))
            post(move, 0x111, 1)
            moved_note = destination / current.name
            wait(lambda: moved_note.exists() and not current.exists())
            current = moved_note
            send(lists[2], 0x186, 0)
            send(main_window, 0x111, 1 << 16, lists[2])
            renamed_pdf = rename(3312, pdf, ["bad.txt", "without_extension", "bad.pdf.exe"], "renamed.PDF")
            if not renamed_pdf.exists():
                raise RuntimeError("Valid PDF rename failed")
            post(main_window, 0x10)
            app.wait(timeout=15)
            if app.returncode != 0 or current.read_bytes() != original_bytes:
                raise RuntimeError("File-operation fixture exit or source preservation failed")
            print("File operations app smoke passed: invalid extensions stay open; supported renames and move preserve bytes", flush=True)
            return
        # Quick Note first exercises cancellation of the actual output command.
        post(main_window, 0x111, 1008)
        dialog = picker()
        print("Quick output picker opened", flush=True)
        post(dialog, 0x111, 2)
        wait(lambda: not find("#32770" if args.native else "LocalPathBrowserDlg"))
        if any(destination.iterdir()) or original.read_bytes() != original_bytes:
            raise RuntimeError("Cancel changed a document or produced output")
        if not args.native:
            post(main_window, 0x111, 1008)
            dialog = picker()
            normal = u.GetDlgItem(dialog, 102)
            control = u.GetDlgItem(dialog, 104)
            expected = "[DIR] 授業 日本語 destination"
            # The recommendation appears independently above and still occurs
            # in the normal name-order list. Navigate using the upper pane.
            def listed_names(listbox):
                names = []
                for i in range(send(listbox, 0x18B)):
                    buffer = ctypes.create_unicode_buffer(send(listbox, 0x18A, i) + 1)
                    send(listbox, 0x189, i, ctypes.addressof(buffer))
                    names.append(buffer.value)
                return names

            if listed_names(control).count(expected) != 1 or listed_names(normal).count(expected) != 1:
                raise RuntimeError("Suggested folder was not present in both picker panes")
            found = False
            for i in range(send(control, 0x18B)):  # LB_GETCOUNT
                buffer = ctypes.create_unicode_buffer(send(control, 0x18A, i) + 1)
                send(control, 0x189, i, ctypes.addressof(buffer))
                if buffer.value == expected:
                    send(control, 0x186, i)  # LB_SETCURSEL
                    post(dialog, 0x111, 104 | (2 << 16), control)  # LBN_DBLCLK
                    found = True
                    break
            if not found:
                raise RuntimeError("The selected note's destination directory was not listed")
            wait(lambda: text(u.GetDlgItem(dialog, 101)).endswith(destination.name))
            set_text(u.GetDlgItem(dialog, 103), "選んだ output.txt")
            post(dialog, 0x111, 1)
            output = destination / "選んだ output.txt"
            wait(lambda: output.is_file() and output.stat().st_size > 0)
            if original.read_bytes() != original_bytes:
                raise RuntimeError("Quick output modified its source")
            # A file can exist before the result dialog is created. Finish
            # that command before posting a new command into its modal loop.
            results = wait(lambda: find("ExportResultsDialog"))
            post(results, 0x10)
            wait(lambda: not find("ExportResultsDialog"))
        # Browse in the unified dialog selects folder and name together.
        post(main_window, 0x111, 1014)
        export = wait(lambda: find("UnifiedExportDialog"))
        previous = (text(u.GetDlgItem(export, 4111)), text(u.GetDlgItem(export, 4113)))
        post(export, 0x111, 4112)
        dialog = picker()
        if args.native:
            post(dialog, 0x111, 2)
            wait(lambda: not find("#32770"))
            if previous != (text(u.GetDlgItem(export, 4111)), text(u.GetDlgItem(export, 4113))):
                raise RuntimeError("Native cancel changed the output target")
        else:
            set_text(u.GetDlgItem(dialog, 103), "selected-only.txt")
            post(dialog, 0x111, 1)
            wait(lambda: not find("LocalPathBrowserDlg"))
            folder = text(u.GetDlgItem(export, 4111))
            name = text(u.GetDlgItem(export, 4113))
            if name != "selected-only.txt" or (Path(folder) / name).exists():
                raise RuntimeError("Browse did not update the name or performed a write")
        post(export, 0x10)
        wait(lambda: not find("UnifiedExportDialog"))
        post(main_window, 0x10)
        app.wait(timeout=15)
        if app.returncode != 0 or original.read_bytes() != original_bytes:
            raise RuntimeError("Owned app exit failed or changed its source")
        print("Output picker app smoke passed: " + ("native direct/cancel" if args.native else "local cancel/export/browse"))
    finally:
        if app.poll() is None:
            for cls in ("LocalPathBrowserDlg", "#32770", "UnifiedExportDialog", "ExportResultsDialog"):
                dialog = find(cls)
                if dialog:
                    try:
                        post(dialog, 0x10)
                    except OSError as error:
                        print(f"Owned dialog cleanup: {error}", flush=True)
            if main_window and u.IsWindow(main_window):
                try:
                    post(main_window, 0x10)
                except OSError as error:
                    print(f"Owned main-window cleanup: {error}", flush=True)
            try:
                app.wait(timeout=5)
            except subprocess.TimeoutExpired:
                app.kill()
                app.wait()


if __name__ == "__main__":
    main()
