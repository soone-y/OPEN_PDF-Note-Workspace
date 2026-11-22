"""Check output pickers in a retained, owned copy of the built Windows app.

Never opens a user's workspace or stops an unrelated process. Native mode
checks opening/cancellation, not the OS dialog's sound or network behavior.
"""
import argparse
import base64
import ctypes
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import uuid


# Keep the historical import used by search_cancel_app_smoke, sharing process
# ownership and cleanup with the other GUI checks.
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.dev.isolated_app_test import OwnedProcess, current_station, protect_entry_point


class DesktopApp(OwnedProcess):
    def __init__(self, executable, root, desktop_name, arguments=()):
        super().__init__([str(executable), *arguments], root, current_station() + "\\" + desktop_name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", action="store_true")
    parser.add_argument("--file-operations", action="store_true", help="Check full-name validation and rename byte preservation")
    parser.add_argument("--export-workflow", action="store_true", help="Check reservations, TXT defaults and page order with independent PDF engines")
    parser.add_argument("--clro-markup-export", action="store_true", help="Check CLRO normalization defaults, queued ON/OFF and HTML/cancel/source preservation")
    parser.add_argument("--executable", default="pdf_note_workspace", help="Built executable name in out/bin, without extension")
    parser.add_argument("--isolated-desktop", action="store_true", help="Use an additional owned desktop (all runs already use desktop protection); no visible UI validation")
    args = parser.parse_args()
    protect_entry_point()
    if Path(args.executable).name != args.executable or not args.executable.replace("_", "").replace("-", "").isalnum():
        parser.error("--executable must be a plain executable name")
    if args.export_workflow and (args.native or args.file_operations):
        parser.error("--export-workflow is independent of native/file-operation modes")
    if args.clro_markup_export and (args.export_workflow or args.native or args.file_operations):
        parser.error("--clro-markup-export is an independent note-only export check")
    repo = Path(__file__).resolve().parents[2]
    root = repo / "out/tests" / ("output_picker_" + uuid.uuid4().hex[:10])
    root.mkdir(parents=True)
    print(f"Owned fixture: {root}", flush=True)
    # A unique executable name isolates package diagnostics from a user's
    # running copy as well as other smoke tests. Never request their shutdown.
    owned_executable = root / ("pdf_note_output_smoke_" + uuid.uuid4().hex + ".exe")
    for name in ("pdf_note_workspace.exe", "pdfium.dll", "zlib1.dll",
                 "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"):
        source_name = args.executable + ".exe" if name == "pdf_note_workspace.exe" else name
        shutil.copy2(repo / "out/bin" / source_name,
                     owned_executable if name == "pdf_note_workspace.exe" else root / name)
    workspace = root / "workspace"
    session = workspace / "lecture1/session1"
    destination = session / "授業 日本語 destination"
    destination.mkdir(parents=True)
    if args.export_workflow:
        destination_alias = root / "destination-alias"
        source_alias = root / "source-alias"
        # Owned local junction: alternate spellings must not reserve two jobs
        # that would successively overwrite the same output file.
        def powershell_literal(value):
            return "'" + str(value).replace("'", "''") + "'"

        for alias, directory in ((destination_alias, destination), (source_alias, session)):
            command = ("$ErrorActionPreference='Stop'; New-Item -ItemType Junction -Path "
                       + powershell_literal(alias) + " -Value "
                       + powershell_literal(str(directory).upper()) + " | Out-Null")
            # PowerShell 5.1's command-line decoding can lose non-ASCII paths.
            encoded = base64.b64encode(command.encode("utf-16-le")).decode("ascii")
            result = subprocess.run(["powershell.exe", "-NoProfile", "-EncodedCommand", encoded],
                                    cwd=root, capture_output=True, timeout=15)
            if result.returncode:
                raise RuntimeError("Junction fixture failed: " + result.stderr.decode(errors="replace"))
    original = session / ("original.clro" if args.clro_markup_export else "original.md" if args.export_workflow else "original.txt")
    original_bytes = (b"# Heading\r\n**bold** and [link](local.txt)\r\n" if args.export_workflow
                      else b"**output picker source**: preserve these bytes\r\n")
    if args.clro_markup_export:
        original_bytes = '<ｃ＝赤>文字</>\r\n＃Heading\r\n\r\n＊＊bold＊＊ ＡＢ１２！\r\n［link］（local（１）.txt）\r\n$ a＊b $\r\n\r\n｀｀｀\r\n＊＊literal＊＊\r\n｀｀｀\r\n'.encode('utf-8')
    original.write_bytes(original_bytes)
    pdf = session / "sample.pdf"
    if args.file_operations or args.export_workflow:
        shutil.copy2(repo / "tests/fixtures/ui_automation_session/sample.pdf", pdf)
    if args.export_workflow:
        import fitz
        second_pdf = session / "z-second.pdf"
        with fitz.open() as second:
            second.new_page().insert_text((72, 72), "Page from a different original PDF")
            second.save(second_pdf)
    (workspace / "workspace.json").write_text(json.dumps({
        "classesDir": ".", "startupSelectFirstSession": True,
        "sessionFileLayout": "session_root", "sessionAutoOpenMode": "edit",
        "useNativeFileDialogs": args.native,
        "autoSaveSeconds": 1 if args.export_workflow else 5,
    }), encoding="utf-8")
    (root / "pdf_note_workspace_setup.json").write_text(json.dumps({
        "workspaceRootMode": "relative", "workspaceRoot": "workspace"
    }), encoding="utf-8")
    u = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    u.EnumWindows.argtypes = [callback_type, w.LPARAM]
    u.EnumDesktopWindows.argtypes = [w.HANDLE, callback_type, w.LPARAM]
    u.EnumChildWindows.argtypes = [w.HWND, callback_type, w.LPARAM]
    u.GetParent.argtypes = [w.HWND]
    u.GetParent.restype = w.HWND
    u.GetWindow.argtypes = [w.HWND, w.UINT]
    u.GetWindow.restype = w.HWND
    u.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
    u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
    u.GetDlgItem.argtypes = [w.HWND, ctypes.c_int]
    u.GetDlgItem.restype = w.HWND
    u.GetDlgCtrlID.argtypes = [w.HWND]
    u.GetDlgCtrlID.restype = ctypes.c_int
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
    desktop = None
    previous_desktop = None
    if args.isolated_desktop:
        u.CreateDesktopW.argtypes = [w.LPCWSTR, w.LPCWSTR, ctypes.c_void_p, w.DWORD, w.DWORD, ctypes.c_void_p]
        u.CreateDesktopW.restype = w.HANDLE
        u.SetThreadDesktop.argtypes = [w.HANDLE]
        u.GetThreadDesktop.argtypes = [w.DWORD]
        u.GetThreadDesktop.restype = w.HANDLE
        u.CloseDesktop.argtypes = [w.HANDLE]
        k = ctypes.WinDLL("kernel32", use_last_error=True)
        previous_desktop = u.GetThreadDesktop(k.GetCurrentThreadId())
        desktop_name = "PdfNoteOutputTest_" + uuid.uuid4().hex
        desktop = u.CreateDesktopW(desktop_name, None, None, 0, 0x1FF, None)
        if not desktop or not u.SetThreadDesktop(desktop):
            if desktop:
                u.CloseDesktop(desktop)
            raise ctypes.WinError(ctypes.get_last_error())
        print("Execution: separate non-interactive desktop; no SwitchDesktop, focus or pointer operations", flush=True)
    try:
        app = (DesktopApp(owned_executable, root, desktop_name) if desktop
               else subprocess.Popen([str(owned_executable)], cwd=root,
                                     startupinfo=startup, env=os.environ.copy()))
    except BaseException:
        if desktop:
            u.SetThreadDesktop(previous_desktop)
            u.CloseDesktop(desktop)
        raise

    def find(cls, owner=None):
        matches = []

        @callback_type
        def visit(hwnd, _):
            pid = w.DWORD()
            u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == app.pid:
                name = ctypes.create_unicode_buffer(128)
                u.GetClassNameW(hwnd, name, len(name))
                if name.value == cls and (owner is None or u.GetWindow(hwnd, 4) == owner):
                    matches.append(hwnd)
            return True

        ctypes.set_last_error(0)
        enumerated = u.EnumDesktopWindows(desktop, visit, 0) if desktop else u.EnumWindows(visit, 0)
        # A newly created desktop can have no windows yet (FALSE, error zero).
        # The required root/dialog still has to appear before the bounded wait.
        if not enumerated and ctypes.get_last_error():
            raise ctypes.WinError(ctypes.get_last_error())
        return matches[0] if matches else None

    def send(hwnd, msg, wp=0, lp=0):
        result = ctypes.c_size_t()
        if not u.SendMessageTimeoutW(hwnd, msg, wp, lp, 2, 2000, ctypes.byref(result)):
            raise RuntimeError(f"Owned app message failed: {msg}; error={ctypes.get_last_error()}")
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
        # Enumeration can see the root while WM_CREATE is still initializing.
        # Wait for its first completed idle message, rather than probing early.
        wait(lambda: u.SendMessageTimeoutW(main_window, 0, 0, 0, 2, 100,
                                          ctypes.byref(ctypes.c_size_t())))
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
        if args.clro_markup_export:
            u.IsWindowEnabled.argtypes = [w.HWND]
            u.IsWindowEnabled.restype = w.BOOL
            def open_markup():
                post(main_window, 0x111, 1015)
                dialog = wait(lambda: find("UnifiedExportDialog"))
                wait(lambda: u.IsWindowVisible(dialog) and u.GetDlgItem(dialog, 4060))
                send(u.GetDlgItem(dialog, 4002), 0xF5)
                send(u.GetDlgItem(dialog, 4022), 0xF5)
                send(u.GetDlgItem(dialog, 4055), 0xF5)
                return dialog

            export = open_markup()
            normalize = u.GetDlgItem(export, 4060)
            if not u.IsWindowVisible(normalize) or not u.IsWindowEnabled(normalize) or send(normalize, 0xF0) != 1:
                raise RuntimeError("CLRO Markdown normalization checkbox must be visible, enabled and ON by default")
            send(u.GetDlgItem(export, 4056), 0xF5)
            if u.IsWindowVisible(normalize):
                raise RuntimeError("Syntax conversion option is visible for HTML")
            send(u.GetDlgItem(export, 4055), 0xF5)
            send(normalize, 0xF5)  # A cancelled OFF draft must not change the retained ON default.
            post(export, 0x111, 2)
            wait(lambda: not find("UnifiedExportDialog"))
            if original.read_bytes() != original_bytes:
                raise RuntimeError("Cancelling normalization mutated the source")
            export = open_markup()
            normalize = u.GetDlgItem(export, 4060)
            if send(normalize, 0xF0) != 1:
                raise RuntimeError("Cancellation changed the default normalization option")
            for name, enabled in (("normalized.md", True), ("preserved.md", False)):
                if bool(send(normalize, 0xF0)) != enabled:
                    send(normalize, 0xF5)
                set_text(u.GetDlgItem(export, 4111), str(destination))
                set_text(u.GetDlgItem(export, 4113), name)
                send(u.GetDlgItem(export, 4201), 0xF5)
            if send(u.GetDlgItem(export, 4210), 0x18B) != 2:
                raise RuntimeError("ON/OFF normalization reservations were not both retained")
            for index, expected_check in ((0, 1), (1, 0)):
                reservations = u.GetDlgItem(export, 4210)
                send(reservations, 0x186, index)
                send(export, 0x111, 4210 | (1 << 16), reservations)
                if send(normalize, 0xF0) != expected_check:
                    raise RuntimeError("Selecting a reservation did not display its normalization choice")
            post(export, 0x111, 4213)
            results = wait(lambda: find("ExportResultsDialog"))
            wait(lambda: u.IsWindowVisible(results) and u.GetDlgItem(results, 4901))
            if send(u.GetDlgItem(results, 4901), 0x18B) != 2:
                raise RuntimeError("Normalization queue did not export both jobs")
            expected = '<c=赤>文字</>\n＃Heading\n\n＊＊bold＊＊ ＡＢ１２！\n［link］（local（１）.txt）\n$ a＊b $\n\n｀｀｀\n＊＊literal＊＊\n｀｀｀\n'
            if (destination / "normalized.md").read_text(encoding="utf-8-sig") != expected:
                raise RuntimeError("Default normalization did not preserve prose, link targets and math")
            if (destination / "preserved.md").read_text(encoding="utf-8-sig") != original_bytes.decode().replace('\r\n', '\n'):
                raise RuntimeError("Queued OFF option did not preserve the CLRO source spelling")
            post(results, 0x10)
            wait(lambda: not find("ExportResultsDialog"))
            post(export, 0x10)
            wait(lambda: not find("UnifiedExportDialog"))
            export = open_markup()
            if send(u.GetDlgItem(export, 4060), 0xF0) != 0:
                raise RuntimeError("Reopening export lost the user's last normalization choice")
            # Only the explicit default action persists this ordinary export
            # preference. A failed replacement keeps both the old file and draft.
            config_path = workspace / "workspace.json"
            before_default = config_path.read_bytes()
            with config_path.open("rb"):
                send(u.GetDlgItem(export, 4061), 0xF5)
                if config_path.read_bytes() != before_default or send(u.GetDlgItem(export, 4060), 0xF0) != 0:
                    raise RuntimeError("Failed default save changed the config or current export choice")
            send(u.GetDlgItem(export, 4061), 0xF5)
            if json.loads(config_path.read_text(encoding="utf-8-sig"))["exportNormalizeClroSyntax"] is not False:
                raise RuntimeError("Explicit normalization default was not persisted")
            post(export, 0x111, 2)
            wait(lambda: not find("UnifiedExportDialog"))
            post(main_window, 0x10)
            app.wait(timeout=15)
            if app.returncode != 0 or original.read_bytes() != original_bytes:
                raise RuntimeError("CLRO export changed source data or failed to exit")
            app = (DesktopApp(owned_executable, root, desktop_name) if desktop
                   else subprocess.Popen([str(owned_executable)], cwd=root, startupinfo=startup, env=os.environ.copy()))
            main_window = wait(lambda: find("PdfWorkspaceMainWnd"))
            wait(lambda: u.SendMessageTimeoutW(main_window, 0, 0, 0, 2, 100, ctypes.byref(ctypes.c_size_t())))
            lists.clear()
            u.EnumChildWindows(main_window, collect, 0)
            if len(lists) < 4:
                raise RuntimeError("Restart did not recreate the four file lists")
            for control in (lists[0], lists[1], lists[3]):
                wait(lambda: send(control, 0x18B) > 0)
                send(control, 0x186, 0)
                send(main_window, 0x111, 1 << 16, control)
            export = open_markup()
            if send(u.GetDlgItem(export, 4060), 0xF0) != 0:
                raise RuntimeError("Restart did not restore the saved normalization default")
            send(u.GetDlgItem(export, 4060), 0xF5)
            post(export, 0x111, 2)
            wait(lambda: not find("UnifiedExportDialog"))
            export = open_markup()
            if send(u.GetDlgItem(export, 4060), 0xF0) != 0:
                raise RuntimeError("Cancel overwrote the persisted normalization default")
            post(export, 0x111, 2)
            wait(lambda: not find("UnifiedExportDialog"))
            post(main_window, 0x10)
            app.wait(timeout=15)
            if app.returncode != 0 or original.read_bytes() != original_bytes:
                raise RuntimeError("Default preference restart changed source data or failed to exit")
            print("CLRO Markdown export smoke passed: default ON; HTML visibility; cancel; queued ON/OFF; last choice; explicit default failure/retry/restart; source/code/math/path preservation", flush=True)
            return
        if args.export_workflow:
            import fitz
            from pypdf import PdfReader

            source_pdf_bytes = pdf.read_bytes()
            second_pdf_bytes = second_pdf.read_bytes()
            wait(lambda: send(lists[2], 0x18B) > 0)
            send(lists[2], 0x186, 0)
            send(main_window, 0x111, 1 << 16, lists[2])
            # Check the missing quick preference before any dialog can save it.
            def quick_pdf(name, expected):
                post(main_window, 0x111, 1007)
                dialog = picker()
                if text(u.GetDlgItem(dialog, 103)) != "sample_annotated.pdf":
                    raise RuntimeError("Quick PDF changed its full-document default filename")
                set_text(u.GetDlgItem(dialog, 103), name)
                post(dialog, 0x111, 1)
                result = wait(lambda: find("ExportResultsDialog"))
                wait(lambda: u.IsWindowVisible(result) and u.GetDlgItem(result, 4901))
                path = session / name
                if not path.exists() or [p.rotation for p in PdfReader(path).pages] != expected:
                    raise RuntimeError("Quick PDF did not honor its rotation preference: " + name)
                post(result, 0x10)
                wait(lambda: not find("ExportResultsDialog"))

            send(main_window, 0x111, 1132)
            quick_pdf("quick-default-on.pdf", [90]*5)
            send(main_window, 0x111, 1133)
            # Select through the real PDF list; the owned fixture has five pages.
            post(main_window, 0x111, 1012)
            export = wait(lambda: find("UnifiedExportDialog"))
            wait(lambda: u.IsWindowVisible(export) and u.GetDlgItem(export, 4210))
            reservations = u.GetDlgItem(export, 4210)

            def count():
                return send(reservations, 0x18B) if u.IsWindowEnabled(reservations) else 0

            def click(control_id):
                send(u.GetDlgItem(export, control_id), 0xF5)  # BM_CLICK

            def assert_action_layout(window, rows):
                u.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
                u.GetClientRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
                u.ClientToScreen.argtypes = [w.HWND, ctypes.POINTER(w.POINT)]
                u.GetDC.argtypes = [w.HWND]
                u.GetDC.restype = w.HDC
                u.ReleaseDC.argtypes = [w.HWND, w.HDC]
                gdi = ctypes.WinDLL("gdi32")
                gdi.SelectObject.argtypes = [w.HDC, w.HGDIOBJ]
                gdi.SelectObject.restype = w.HGDIOBJ
                gdi.GetTextExtentPoint32W.argtypes = [w.HDC, w.LPCWSTR, ctypes.c_int, ctypes.POINTER(w.SIZE)]
                gdi.GetDeviceCaps.argtypes = [w.HDC, ctypes.c_int]
                gdi.DeleteObject.argtypes = [w.HGDIOBJ]

                # The app's HFONT is private to its process. Build the same UI
                # font locally using the installed faces in its preference list.
                class LogFont(ctypes.Structure):
                    _fields_ = [(name, w.LONG) for name in ("height", "width", "escapement", "orientation", "weight")] + [
                        (name, w.BYTE) for name in ("italic", "underline", "strikeout", "charset", "output_precision",
                                                    "clip_precision", "quality", "pitch_and_family")
                    ] + [("face", w.WCHAR * 32)]

                enum_fonts = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, w.DWORD, w.LPARAM)
                gdi.EnumFontFamiliesExW.argtypes = [w.HDC, ctypes.POINTER(LogFont), enum_fonts, w.LPARAM, w.DWORD]
                gdi.CreateFontIndirectW.argtypes = [ctypes.POINTER(LogFont)]
                gdi.CreateFontIndirectW.restype = w.HGDIOBJ
                found = False

                @enum_fonts
                def found_font(*_):
                    nonlocal found
                    found = True
                    return 0

                pref_list = (repo / "src/core/font_list.h").read_text(encoding="utf-8").split("kDefaultFontList[] = {", 1)[1].split("};", 1)[0]
                screen = u.GetDC(None)
                try:
                    spec = LogFont()
                    spec.charset = 1  # DEFAULT_CHARSET for enumeration.
                    for face in re.findall(r'\{\s*L"[^"]+",\s*L"([^"]+)",\s*FontStyle::', pref_list):
                        spec.face = face
                        gdi.EnumFontFamiliesExW(screen, ctypes.byref(spec), found_font, 0, 0)
                        if found:
                            break
                    spec.face = spec.face if found else "Segoe UI"
                    spec.height = -((10 * gdi.GetDeviceCaps(screen, 90) + 36) // 72)  # LOGPIXELSY, 10 pt.
                    spec.weight = 400
                    spec.charset = 0  # Match CreateUIFont's zero-initialized LOGFONTW.
                    ui_font = gdi.CreateFontIndirectW(ctypes.byref(spec))
                finally:
                    u.ReleaseDC(None, screen)
                if not ui_font:
                    raise RuntimeError("Could not recreate the app UI font")
                body = w.RECT()
                origin = w.POINT()
                u.GetClientRect(window, ctypes.byref(body))
                u.ClientToScreen(window, ctypes.byref(origin))
                try:
                    for row in rows:
                        prior = None
                        for control_id in row:
                            control = u.GetDlgItem(window, control_id)
                            bounds = w.RECT()
                            if not control or not u.GetWindowRect(control, ctypes.byref(bounds)):
                                raise RuntimeError(f"Missing output action {control_id}")
                            dc = u.GetDC(control)
                            font = gdi.SelectObject(dc, ui_font)
                            caption = text(control)
                            size = w.SIZE()
                            try:
                                if not font or not gdi.GetTextExtentPoint32W(dc, caption, len(caption), ctypes.byref(size)):
                                    raise RuntimeError("Could not measure export button caption")
                            finally:
                                if font:
                                    gdi.SelectObject(dc, font)
                                u.ReleaseDC(control, dc)
                            if (bounds.right - bounds.left < size.cx + 8 or
                                    bounds.bottom - bounds.top < size.cy + 4 or
                                    bounds.right > origin.x + body.right or bounds.bottom > origin.y + body.bottom or
                                    (prior and (bounds.top != prior.top or bounds.left <= prior.right))):
                                raise RuntimeError(
                                    f"Clipped or misplaced output action {control_id}: {ascii(caption)}; "
                                    f"bounds={bounds.left, bounds.top, bounds.right, bounds.bottom}, "
                                    f"client={origin.x, origin.y, body.right, body.bottom}, text={size.cx, size.cy}"
                                )
                            prior = bounds
                finally:
                    gdi.DeleteObject(ui_font)

            assert_action_layout(export, ((1, 4201), (4202, 4203, 4211), (4205, 4206, 4208, 4207, 4204),
                                          (4209, 4213, 4212, 2)))
            current_caption = text(u.GetDlgItem(export, 1))

            def preserve_rotation(enabled):
                check = u.GetDlgItem(export, 4099)
                if not check:
                    raise RuntimeError("Keep-page-rotation checkbox is missing")
                if bool(send(check, 0xF0)) != enabled:  # BM_GETCHECK
                    click(4099)
                if bool(send(check, 0xF0)) != enabled:
                    raise RuntimeError("Keep-page-rotation selection was not retained")

            if u.GetDlgItem(export, 4012):
                raise RuntimeError("Separate page-selection format must be removed")
            page_spec = u.GetDlgItem(export, 4031)
            if not u.IsWindowVisible(page_spec) or text(page_spec) != "1-5":
                raise RuntimeError("Unified PDF output must show all pages by default")
            if not u.IsWindowVisible(u.GetDlgItem(export, 4042)):
                raise RuntimeError("Unified PDF output must expose annotation selection")
            if send(u.GetDlgItem(export, 4099), 0xF0) != 1:
                raise RuntimeError("PDF output must default to keeping page rotation")

            def target(folder, name):
                set_text(u.GetDlgItem(export, 4111), str(folder))
                set_text(u.GetDlgItem(export, 4113), name)

            def expect_selected_target(folder, name):
                actual = (text(u.GetDlgItem(export, 4111)), text(u.GetDlgItem(export, 4113)))
                expected = (str(folder), name)
                if actual != expected:
                    raise RuntimeError(f"Selected reservation has a stale destination: {ascii(actual)} != {ascii(expected)}")

            u.IsWindowEnabled.argtypes = [w.HWND]
            u.IsWindowEnabled.restype = w.BOOL
            color_mode = u.GetDlgItem(export, 4120)
            color_background = u.GetDlgItem(export, 4121)
            if not color_mode or send(color_mode, 0x146) != 4 or send(color_mode, 0x147) != 0:
                raise RuntimeError("Text-color output choices/default marker are missing")
            if u.IsWindowEnabled(color_background):
                raise RuntimeError("Marker output unexpectedly enables background color")
            send(color_mode, 0x14E, 3)
            send(export, 0x111, 4120 | (1 << 16), color_mode)
            if not u.IsWindowEnabled(color_background):
                raise RuntimeError("Specified color output does not enable the color field")
            set_text(color_background, "#GGHHII")
            click(4201)
            if count():
                raise RuntimeError("Invalid text-overlay background was reserved")
            set_text(color_background, "#508CC8")
            click(4001)
            click(4011)
            click(4202)
            preset = json.loads((workspace / "workspace.json").read_text(encoding="utf-8-sig"))
            if preset.get("quickPdfTextColorMode") != 3 or preset.get("quickPdfTextColorBackground") != 0xC88C50:
                raise RuntimeError("Quick PDF preset did not save color-overlay settings")
            send(color_mode, 0x14E, 0)
            send(export, 0x111, 4120 | (1 << 16), color_mode)
            click(4011)
            set_text(u.GetDlgItem(export, 4031), "1n")
            target(source_alias, "SAMPLE.PDF")
            click(4201)
            if count() or pdf.read_bytes() != source_pdf_bytes:
                raise RuntimeError("A junction/case alias of the original PDF was accepted")
            # Invalid input must neither reserve a different page nor close UI.
            for invalid in ("", "1xyz", "1-2xyz", "1-2147483647", "4294967297", "0", "6", "3-1"):
                set_text(u.GetDlgItem(export, 4031), invalid)
                target(destination, "invalid.pdf")
                click(4201)
                if not u.IsWindow(export) or count() or (destination / "invalid.pdf").exists():
                    raise RuntimeError(f"Invalid page specification accepted: {invalid}")
            set_text(u.GetDlgItem(export, 4031), "3a,1n,3a")
            target(destination, "reordered.pdf")
            click(4201)
            if u.IsWindowEnabled(u.GetDlgItem(export, 4212)):
                raise RuntimeError("PDF merge is enabled with only one reservation")
            click(4001)
            click(4011)
            target(destination_alias, "REORDERED.PDF")
            click(4201)
            if count() != 1:
                raise RuntimeError("A junction/case alias reserved the same output destination twice")
            # Adding switches the category; select PDF output again explicitly.
            click(4001)
            click(4011)
            set_text(u.GetDlgItem(export, 4031), "2[0:0:200:200]n")
            target(destination, "crop.pdf")
            click(4201)
            if not u.IsWindowEnabled(u.GetDlgItem(export, 4212)):
                raise RuntimeError("PDF-only queue cannot be merged")
            # The shared settings window must preserve this export draft and queue.
            export_before = (count(), send(reservations, 0x188), text(u.GetDlgItem(export, 4111)),
                             text(u.GetDlgItem(export, 4113)), send(u.GetDlgItem(export, 4099), 0xF0),
                             text(u.GetDlgItem(export, 4031)), text(u.GetDlgItem(export, 4121)))
            post(export, 0x111, 4211)
            settings = wait(lambda: find("UnifiedSettingsShellClass"))
            wait(lambda: u.IsWindowVisible(settings) and not u.IsWindowEnabled(export))
            send(settings, 0x8000 + 140, 7)
            quick_fields = []

            @callback_type
            def collect_quick_background(hwnd, _):
                if u.GetDlgCtrlID(hwnd) == 4235:
                    quick_fields.append(hwnd)
                return True

            u.EnumChildWindows(settings, collect_quick_background, 0)
            if len(quick_fields) != 1:
                raise RuntimeError("Shared quick-PDF settings editor is missing")
            set_text(quick_fields[0], "#010203")
            post(settings, 0x111, 1)
            wait(lambda: not find("UnifiedSettingsShellClass") and u.IsWindowEnabled(export))
            export_after = (count(), send(reservations, 0x188), text(u.GetDlgItem(export, 4111)),
                            text(u.GetDlgItem(export, 4113)), send(u.GetDlgItem(export, 4099), 0xF0),
                            text(u.GetDlgItem(export, 4031)), text(u.GetDlgItem(export, 4121)))
            if export_after != export_before:
                raise RuntimeError("Quick settings changed the active export draft or reservations")
            if json.loads((workspace / "workspace.json").read_text(encoding="utf-8-sig"))["quickPdfTextColorBackground"] != 0x030201:
                raise RuntimeError("Shared quick-PDF settings did not use the existing persistent defaults")
            click(4002)
            click(4021)
            if send(u.GetDlgItem(export, 4054), 0xF0) != 0:
                raise RuntimeError("TXT markup removal default is not OFF")
            long_destination = destination / ("長い保存先_" * 10) / ("long folder " * 6).rstrip()
            long_destination.mkdir(parents=True)
            target(long_destination, "raw.txt")
            if text(u.GetDlgItem(export, 4111)) != str(long_destination):
                raise RuntimeError("Long output folder was truncated")
            # WM_USER tooltip messages do not marshal pointers across processes.
            # Use a bounded data buffer in this owned child to inspect registered
            # text, without focus, pointer movement, popup activation or clipboard.
            class ToolInfo(ctypes.Structure):
                _fields_ = [("size", w.UINT), ("flags", w.UINT), ("hwnd", w.HWND),
                            ("id", ctypes.c_size_t), ("rect", w.RECT), ("instance", w.HINSTANCE),
                            ("text", ctypes.c_void_p), ("param", w.LPARAM), ("reserved", ctypes.c_void_p)]

            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
            kernel.OpenProcess.restype = w.HANDLE
            kernel.VirtualAllocEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_size_t, w.DWORD, w.DWORD]
            kernel.VirtualAllocEx.restype = ctypes.c_void_p
            kernel.WriteProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
            kernel.ReadProcessMemory.argtypes = kernel.WriteProcessMemory.argtypes
            kernel.VirtualFreeEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_size_t, w.DWORD]
            kernel.CloseHandle.argtypes = [w.HANDLE]
            process_handle = kernel.OpenProcess(0x38, False, app.pid)  # Owned PID, VM read/write/operation only.
            remote = kernel.VirtualAllocEx(process_handle, None, 66000, 0x3000, 4) if process_handle else None
            if not remote:
                if process_handle:
                    kernel.CloseHandle(process_handle)
                raise ctypes.WinError(ctypes.get_last_error())
            try:
                tip = wait(lambda: find("tooltips_class32", export))
                if send(tip, 0x40D) != 2:  # TTM_GETTOOLCOUNT
                    raise RuntimeError("Both output path tooltips must be registered")
                for folder, name in ((long_destination, "raw.txt"), (long_destination, "長い出力名_" * 20 + ".txt"),
                                     (destination, "changed.txt"), (long_destination, "raw.txt")):
                    target(folder, name)
                    for control_id, expected in ((4111, str(folder)), (4113, str(folder / name))):
                        tool = ToolInfo()
                        tool.size = ToolInfo.reserved.offset
                        tool.flags = 1  # TTF_IDISHWND
                        tool.hwnd = export
                        tool.id = u.GetDlgItem(export, control_id)
                        tool.text = remote + 256
                        if not kernel.WriteProcessMemory(process_handle, remote, ctypes.byref(tool), ctypes.sizeof(tool), None):
                            raise ctypes.WinError(ctypes.get_last_error())
                        send(tip, 0x438, 0, remote)  # TTM_GETTEXTW
                        registered = ctypes.create_unicode_buffer(32768)
                        if not kernel.ReadProcessMemory(process_handle, remote + 256, registered, ctypes.sizeof(registered), None):
                            raise ctypes.WinError(ctypes.get_last_error())
                        if registered.value != expected:
                            raise RuntimeError(f"Output tooltip {control_id} did not retain its complete updated path")
            finally:
                kernel.VirtualFreeEx(process_handle, remote, 0, 0x8000)
                kernel.CloseHandle(process_handle)
            click(4201)
            if count() != 3:
                raise RuntimeError("Reservation additions did not retain all three jobs")
            if u.IsWindowEnabled(u.GetDlgItem(export, 4212)):
                raise RuntimeError("PDF merge is enabled for a mixed TXT queue")
            send(export, 0x111, 4212)
            if find("LocalPathBrowserDlg") or count() != 3:
                raise RuntimeError("Mixed queue accepted the merge command")
            # Reorder jobs, update a selected destination, then batch execute.
            send(reservations, 0x186, 2)
            send(export, 0x111, 4210 | (1 << 16), reservations)
            click(4205)
            if send(reservations, 0x188) != 1:
                raise RuntimeError("Moving a reservation up did not keep it selected")
            click(4206)
            if send(reservations, 0x188) != 2:
                raise RuntimeError("Moving a reservation down did not keep it selected")
            send(export, 0x111, 4210 | (1 << 16), reservations)
            target(long_destination, "updated-raw.txt")
            click(4208)
            if text(u.GetDlgItem(export, 1)) != current_caption:
                raise RuntimeError("Adding reservations changed the current-output button")
            post(export, 0x111, 4213)
            results = wait(lambda: find("ExportResultsDialog"))
            wait(lambda: u.IsWindowVisible(results) and u.GetDlgItem(results, 4901))
            result_list = u.GetDlgItem(results, 4901)
            if send(result_list, 0x18B) != 3 or not find("UnifiedExportDialog") or count():
                raise RuntimeError("Batch output did not finish with one complete result list")
            assert_action_layout(results, ((4902, 4903, 4904),))
            pdf_open = text(u.GetDlgItem(results, 4902))
            send(result_list, 0x186, 2)
            send(results, 0x111, 4901 | (1 << 16), result_list)
            if pdf_open == text(u.GetDlgItem(results, 4902)):
                raise RuntimeError("Results did not offer the appropriate open action for TXT")
            assert_action_layout(results, ((4902, 4903, 4904),))
            paths = []
            for i in range(3):
                buffer = ctypes.create_unicode_buffer(send(result_list, 0x18A, i) + 1)
                send(result_list, 0x189, i, ctypes.addressof(buffer))
                paths.append(Path(buffer.value))
            expected_paths = [destination / "reordered.pdf", destination / "crop.pdf", long_destination / "updated-raw.txt"]
            if paths != expected_paths or (long_destination / "raw.txt").exists():
                raise RuntimeError(f"Reservation order/update changed destination: {ascii(paths)} != {ascii(expected_paths)}")
            post(results, 0x10)
            wait(lambda: not find("ExportResultsDialog"))
            post(export, 0x10)
            wait(lambda: not find("UnifiedExportDialog"))
            with fitz.open(pdf) as source, fitz.open(paths[0]) as output:
                if len(output) != 3 or [p.get_text() for p in output] != [source[i].get_text() for i in (2, 0, 2)]:
                    raise RuntimeError("Reordered/repeated PDF pages changed order or content")
            reader = PdfReader(paths[0])
            source_reader = PdfReader(pdf)
            if [page.extract_text() for page in reader.pages] != [source_reader.pages[i].extract_text() for i in (2, 0, 2)]:
                raise RuntimeError("Independent page order verification failed")
            crop = PdfReader(paths[1])
            if len(crop.pages) != 1 or list(crop.pages[0].cropbox) != [0, 0, 200, 200]:
                raise RuntimeError("Page extraction/crop geometry changed")
            exported = paths[2].read_text(encoding="utf-8-sig")
            if "**bold**" not in exported or "[link](local.txt)" not in exported or "# Heading" not in exported:
                raise RuntimeError("TXT default removed markup")
            if original.read_bytes() != original_bytes or pdf.read_bytes() != source_pdf_bytes:
                raise RuntimeError("Batch export changed a source document")
            # Merge preserves page order/crop/scale/rotation independently for each reservation.
            # Mix a page-zero adjustment with a batch turn before freezing the checked page angles.
            send(main_window, 0x111, 1129)
            send(main_window, 0x111, 1132)  # Batch adds a turn, retaining page zero's individual adjustment.
            post(main_window, 0x111, 1014)
            export = wait(lambda: find("UnifiedExportDialog"))
            wait(lambda: u.IsWindowVisible(export) and u.GetDlgItem(export, 4212))
            reservations = u.GetDlgItem(export, 4210)
            click(4001)
            click(4011)
            click(4082)  # Full size.
            preserve_rotation(False)  # OFF ignores all temporary view angles.
            set_text(u.GetDlgItem(export, 4031), "3a,1n,3a")
            target(destination, "merge-segment-a.pdf")
            click(4201)
            click(4001)
            click(4011)
            click(4081)  # Half size for this crop segment only.
            preserve_rotation(True)  # This crop keeps page two's batch turn.
            set_text(u.GetDlgItem(export, 4031), "2[0:0:200:200]n")
            target(destination, "merge-segment-b.pdf")
            click(4201)
            click(4001)
            click(4011)
            set_text(u.GetDlgItem(export, 4031), "1-5")  # Include the first/last page.
            click(4083)  # Double size for this segment only.
            preserve_rotation(True)  # Freeze batch angles plus page zero's individual adjustment.
            target(destination, "merge-segment-all.pdf")
            click(4201)
            click(4001)
            click(4011)
            preserve_rotation(False)  # Already queued angles must remain frozen.
            # PNG is not a PDF reservation even though it is a PDF-pane output.
            click(4001)
            click(4013)
            target(destination, "merge-mixed.png")
            click(4201)
            if u.IsWindowEnabled(u.GetDlgItem(export, 4212)):
                raise RuntimeError("PDF merge is enabled for a mixed PNG queue")
            send(reservations, 0x186, 3)
            send(export, 0x111, 4210 | (1 << 16), reservations)
            click(4207)
            if count() != 3 or not u.IsWindowEnabled(u.GetDlgItem(export, 4212)):
                raise RuntimeError("Removing the non-PDF reservation did not restore merge")
            expect_selected_target(destination, "merge-segment-all.pdf")
            send(reservations, 0x186, 2)
            send(export, 0x111, 4210 | (1 << 16), reservations)
            click(4205)
            click(4205)
            click(4206)  # Final order: A, all pages, B.
            if send(reservations, 0x188) != 1:
                raise RuntimeError("Merge reservation move did not keep the new selection")
            target(destination, "ignored-current-name.txt")
            post(export, 0x111, 4212)
            dialog = picker()
            post(dialog, 0x111, 2)
            wait(lambda: not find("LocalPathBrowserDlg") and u.IsWindowEnabled(export))
            if count() != 3:
                raise RuntimeError("Merge destination cancellation discarded reservations")
            target(session, pdf.name)
            post(export, 0x111, 4212)
            dialog = picker()
            set_text(u.GetDlgItem(dialog, 103), pdf.name)
            post(dialog, 0x111, 1)
            wait(lambda: not find("LocalPathBrowserDlg") and u.IsWindowEnabled(export))
            if count() != 3 or pdf.read_bytes() != source_pdf_bytes or find("SilentDialogClass"):
                raise RuntimeError("Merged output accepted the open original or lost its queue")
            target(destination, "ignored-current-name.txt")

            merged_path = destination / "merged-output.pdf"
            sentinel = b"existing merged output must survive cancellation/failure"
            merged_path.write_bytes(sentinel)

            def choose_merge():
                post(export, 0x111, 4212)
                dialog = picker()
                set_text(u.GetDlgItem(dialog, 103), merged_path.name)
                post(dialog, 0x111, 1)
                wait(lambda: not find("LocalPathBrowserDlg"))
                confirmation = wait(lambda: find("SilentDialogClass"))
                wait(lambda: u.IsWindowVisible(confirmation) and u.GetDlgItem(confirmation, 5103))
                if merged_path.name not in text(u.GetDlgItem(confirmation, 5101)):
                    raise RuntimeError("Merge overwrite confirmation has an unexpected target")
                return confirmation

            confirmation = choose_merge()
            post(confirmation, 0x10)
            wait(lambda: not find("SilentDialogClass") and u.IsWindowEnabled(export))
            if count() != 3 or merged_path.read_bytes() != sentinel:
                raise RuntimeError("Merge overwrite cancellation changed output or queue")
            # An exclusive read handle blocks final replacement while allowing
            # the existing-path check and explicit overwrite confirmation.
            kernel.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
            kernel.CreateFileW.restype = w.HANDLE
            locked = kernel.CreateFileW(str(merged_path), 0x80000000, 0, None, 3, 0, None)
            if locked == ctypes.c_void_p(-1).value:
                raise ctypes.WinError(ctypes.get_last_error())
            try:
                confirmation = choose_merge()
                overwrite_title = text(confirmation)
                post(confirmation, 0x111, 5102)  # Explicit overwrite.
                def merge_failure():
                    dialog = find("SilentDialogClass")
                    if not dialog:
                        return None
                    try:
                        return dialog if text(dialog) != overwrite_title else None
                    except RuntimeError:
                        # The prior confirmation can close between enumeration
                        # and reading its title. Retry only a destroyed HWND.
                        if ctypes.get_last_error() == 1400 and not u.IsWindow(dialog):
                            return None
                        raise
                failure = wait(merge_failure)
                wait(lambda: u.IsWindowVisible(failure) and u.GetDlgItem(failure, 5101))
                post(failure, 0x10)
                wait(lambda: not find("SilentDialogClass") and u.IsWindowEnabled(export))
                if count() != 3:
                    raise RuntimeError("Failed merged output discarded reservations")
            finally:
                kernel.CloseHandle(locked)
            if merged_path.read_bytes() != sentinel:
                raise RuntimeError("Failed merged output damaged its existing destination")
            confirmation = choose_merge()
            post(confirmation, 0x111, 5102)
            results = wait(lambda: find("ExportResultsDialog"))
            wait(lambda: u.IsWindowVisible(results) and u.GetDlgItem(results, 4901))
            if send(u.GetDlgItem(results, 4901), 0x18B) != 1 or count() != 0:
                raise RuntimeError("Merged output did not produce one result and clear the completed queue")
            post(results, 0x10)
            wait(lambda: not find("ExportResultsDialog") and u.IsWindowEnabled(export))
            if count() != 0:
                raise RuntimeError("Completed merged reservations remain queued")
            with fitz.open(pdf) as source, fitz.open(merged_path) as merged:
                order = (2, 0, 2, 0, 1, 2, 3, 4)
                if len(merged) != 9 or [merged[i].get_text() for i in range(8)] != [source[i].get_text() for i in order]:
                    raise RuntimeError("Merged page order/content differs from the visible reservation order")
                for output_index, source_index, scale in ((0, 2, 1), (3, 0, 2), (7, 4, 2)):
                    source_page = source[source_index]
                    original_angle = source_page.rotation
                    source_page.set_rotation(original_angle + {0:0, 3:180, 7:90}[output_index])
                    expected = source_page.get_pixmap(matrix=fitz.Matrix(0.5, 0.5), alpha=False)
                    source_page.set_rotation(original_angle)
                    actual = merged[output_index].get_pixmap(matrix=fitz.Matrix(0.5 / scale, 0.5 / scale), alpha=False)
                    if (expected.width, expected.height, expected.samples) != (actual.width, actual.height, actual.samples):
                        raise RuntimeError("Merged page rendering changed beyond its requested scale")
            merged_reader = PdfReader(merged_path)
            if [page.rotation for page in merged_reader.pages] != [0,0,0,180,90,90,90,90,90]:
                raise RuntimeError("OFF/original and ON/view rotation were not frozen per reservation and page")
            # pypdf infers additional spaces from scaled glyph positions.
            # Check every non-whitespace character independently; PyMuPDF above
            # additionally requires exact text and identical normalized pixels.
            if len(merged_reader.pages) != 9 or ["".join(p.extract_text().split()) for p in merged_reader.pages[:8]] != ["".join(source_reader.pages[i].extract_text().split()) for i in order]:
                raise RuntimeError("Independent merged PDF page order verification failed")
            if list(merged_reader.pages[-1].cropbox) != [0, 0, 100, 100]:
                raise RuntimeError("Merged crop segment lost its own half-scale geometry")
            for output_index, source_index, scale in ((0, 2, 1), (3, 0, 2), (7, 4, 2)):
                source_box = source_reader.pages[source_index].mediabox
                box = merged_reader.pages[output_index].mediabox
                if abs(float(box.width) - float(source_box.width) * scale) > 0.01 or abs(float(box.height) - float(source_box.height) * scale) > 0.01:
                    raise RuntimeError("Merged segment scale was not retained")
            if any((destination / name).exists() for name in ("merge-segment-a.pdf", "merge-segment-b.pdf", "merge-segment-all.pdf", "merge-mixed.png", "ignored-current-name.txt")):
                raise RuntimeError("Merge also wrote individual reservation destinations")
            if original.read_bytes() != original_bytes or pdf.read_bytes() != source_pdf_bytes:
                raise RuntimeError("Merged export changed a source document")
            post(export, 0x10)
            wait(lambda: not find("UnifiedExportDialog"))
            # Type into the owned note before individual output, requiring the
            # asynchronous pre-export integration to publish this edit first.
            editors = []

            @callback_type
            def collect_editor(hwnd, _):
                name = ctypes.create_unicode_buffer(128)
                u.GetClassNameW(hwnd, name, len(name))
                if name.value.lower().startswith("richedit") and "# Heading" in text(hwnd):
                    editors.append(hwnd)
                return True

            u.EnumChildWindows(main_window, collect_editor, 0)
            if len(editors) != 1:
                raise RuntimeError("Could not identify the owned note editor for integration")
            token = "export-integration-token"
            send(editors[0], 0xB1, -1, -1)  # EM_SETSEL, end of note
            for character in token:
                send(editors[0], 0x102, ord(character), 1)  # Actual WM_CHAR input
            if token not in text(editors[0]):
                raise RuntimeError("Owned note input was not accepted")
            stage_dir = workspace / "__pdf_note_workspace__/__tmp__/__stage__/note"
            wait(lambda: stage_dir.exists() and any(stage_dir.iterdir()))
            # Individual output must preserve the remaining queue. Cancel an
            # overwrite and inject an inaccessible destination without losing it.
            post(main_window, 0x111, 1014)
            export = wait(lambda: find("UnifiedExportDialog"))
            wait(lambda: u.IsWindowVisible(export) and u.GetDlgItem(export, 4210))
            reservations = u.GetDlgItem(export, 4210)
            target(destination, "single-raw.txt")
            click(4201)
            click(4002)
            click(4021)
            send(u.GetDlgItem(export, 4054), 0xF1, 1)  # BM_SETCHECK
            target(destination, "single-stripped.txt")
            click(4201)
            if count() != 2:
                raise RuntimeError("Individual output fixture reservations missing")
            send(reservations, 0x186, 0)
            send(export, 0x111, 4210 | (1 << 16), reservations)
            post(export, 0x111, 4209)
            results = wait(lambda: find("ExportResultsDialog"))
            wait(lambda: u.IsWindowVisible(results) and u.GetDlgItem(results, 4901))
            if not u.IsWindow(export) or count() != 1 or send(u.GetDlgItem(results, 4901), 0x18B) != 1:
                raise RuntimeError("Individual output closed the queue or removed remaining jobs")
            expect_selected_target(destination, "single-stripped.txt")
            post(results, 0x10)
            wait(lambda: not find("ExportResultsDialog"))
            if "**bold**" not in (destination / "single-raw.txt").read_text(encoding="utf-8-sig"):
                raise RuntimeError("Individual raw export changed its options")
            expected_note = original_bytes.decode().replace("\r\n", "\n") + token
            if original.read_text(encoding="utf-8-sig") != expected_note or token not in (destination / "single-raw.txt").read_text(encoding="utf-8-sig"):
                raise RuntimeError("Individual export did not integrate the current edit before output")
            original_bytes = original.read_bytes()  # Explicit test edit, now safely integrated.
            blocked = destination / "single-stripped.txt"
            sentinel = b"existing output must survive cancellation"
            blocked.write_bytes(sentinel)
            send(reservations, 0x186, 0)
            post(export, 0x111, 4209)
            overwrite = wait(lambda: find("SilentDialogClass"))
            wait(lambda: u.IsWindowVisible(overwrite) and u.GetDlgItem(overwrite, 5103))
            if "single-stripped.txt" not in text(u.GetDlgItem(overwrite, 5101)):
                raise RuntimeError("Unexpected confirmation instead of the owned output overwrite")
            post(overwrite, 0x10)  # Safe No/escape result, without pointer/focus.
            try:
                wait(lambda: not find("SilentDialogClass") and u.IsWindowEnabled(export))
            except RuntimeError as error:
                remaining = find("SilentDialogClass")
                raise RuntimeError(f"Overwrite cancel incomplete: original={overwrite}, remaining={remaining}, queue_enabled={bool(u.IsWindowEnabled(export))}, title={ascii(text(remaining)) if remaining else ''}, message={ascii(text(u.GetDlgItem(remaining, 5101))) if remaining else ''}") from error
            if count() != 1 or blocked.read_bytes() != sentinel:
                raise RuntimeError("Overwrite cancellation lost a reservation or changed output")
            # Replace the validated folder with an owned regular file, so the
            # normal writer cannot create a parent directory or retry elsewhere.
            vanished = destination / "removed-after-reservation"
            vanished.mkdir()
            target(vanished, "retry.txt")
            click(4208)
            vanished.rmdir()
            vanished.write_bytes(sentinel)
            send(export, 0x111, 4209)
            wait(lambda: u.IsWindowEnabled(export))
            if count() != 1 or vanished.read_bytes() != sentinel:
                raise RuntimeError("Failed output lost its reservation")
            target(destination, "retry-stripped.txt")
            click(4208)
            post(export, 0x111, 4209)
            results = wait(lambda: find("ExportResultsDialog"))
            post(results, 0x10)
            wait(lambda: not find("ExportResultsDialog"))
            stripped = (destination / "retry-stripped.txt").read_text(encoding="utf-8-sig")
            if count() or "**bold**" in stripped or "bold" not in stripped:
                raise RuntimeError("Retry failed or explicit markup removal was ignored")
            click(4002)
            click(4021)
            target(destination, "retained-on-close.txt")
            click(4201)
            post(export, 0x10)
            wait(lambda: not find("UnifiedExportDialog"))
            post(main_window, 0x111, 1014)
            export = wait(lambda: find("UnifiedExportDialog"))
            wait(lambda: u.IsWindowVisible(export) and u.GetDlgItem(export, 4210))
            reservations = u.GetDlgItem(export, 4210)
            if count() != 1 or (destination / "retained-on-close.txt").exists():
                raise RuntimeError("Closing the dialog did not retain the unexecuted reservation")
            click(4204)
            click(4001)
            click(4011)
            click(4082)
            preserve_rotation(False)
            set_text(u.GetDlgItem(export, 4031), "2n")
            target(destination, "cross-source-a.pdf")
            click(4201)
            post(export, 0x10)
            wait(lambda: not find("UnifiedExportDialog"))
            send(lists[2], 0x186, 1)
            send(main_window, 0x111, 1 << 16, lists[2])
            post(main_window, 0x111, 1005)
            export = wait(lambda: find("UnifiedExportDialog"))
            page_spec = wait(lambda: u.IsWindowVisible(export) and u.GetDlgItem(export, 4031))
            reservations = u.GetDlgItem(export, 4210)
            if count() != 1 or text(page_spec) != "1":
                raise RuntimeError("Switching PDF lost the prior reservation or opened the wrong source")
            set_text(page_spec, "1n")
            target(destination, "cross-source-b.pdf")
            click(4201)
            if count() != 2 or not u.IsWindowEnabled(u.GetDlgItem(export, 4212)):
                raise RuntimeError("PDF-only reservations from both sources did not enable merge")
            for index, name in enumerate((pdf.name, second_pdf.name)):
                item = ctypes.create_unicode_buffer(send(reservations, 0x18A, index) + 1)
                send(reservations, 0x189, index, ctypes.addressof(item))
                if name not in item.value:
                    raise RuntimeError("Reservation does not identify its PDF source")

            # Export the second PDF directly while both jobs are still queued.
            click(4001)
            click(4011)
            target(destination, "cross-source-current.pdf")
            post(export, 0x111, 1)
            results = wait(lambda: find("ExportResultsDialog"))
            wait(lambda: u.IsWindowVisible(results) and u.GetDlgItem(results, 4901))
            if send(u.GetDlgItem(results, 4901), 0x18B) != 1:
                raise RuntimeError("Current output executed queued jobs")
            post(results, 0x10)
            wait(lambda: not find("ExportResultsDialog"))
            post(main_window, 0x111, 1005)
            export = wait(lambda: find("UnifiedExportDialog"))
            wait(lambda: u.IsWindowVisible(export) and u.GetDlgItem(export, 4210))
            reservations = u.GetDlgItem(export, 4210)
            if count() != 2:
                raise RuntimeError("Current output consumed queued jobs")
            target(session, pdf.name)
            post(export, 0x111, 4212)
            dialog = picker()
            set_text(u.GetDlgItem(dialog, 103), pdf.name)
            post(dialog, 0x111, 1)
            wait(lambda: not find("LocalPathBrowserDlg") and u.IsWindowEnabled(export))
            if count() != 2 or find("SilentDialogClass") or pdf.read_bytes() != source_pdf_bytes:
                raise RuntimeError("Merge accepted a queued original that was no longer open")

            def cross_merge():
                target(destination, "cross-source.pdf")
                post(export, 0x111, 4212)
                dialog = picker()
                set_text(u.GetDlgItem(dialog, 103), "cross-source.pdf")
                post(dialog, 0x111, 1)

            moved_source = pdf.with_suffix(".hold")
            pdf.rename(moved_source)
            try:
                cross_merge()
                error = wait(lambda: find("SilentDialogClass"))
                wait(lambda: u.IsWindowVisible(error) and pdf.name in text(u.GetDlgItem(error, 5101)))
                post(error, 0x10)
                wait(lambda: u.IsWindowEnabled(export))
                if count() != 2 or (destination / "cross-source.pdf").exists():
                    raise RuntimeError("Missing merge source changed the output or queue")
            finally:
                moved_source.rename(pdf)
            cross_merge()
            results = wait(lambda: find("ExportResultsDialog"))
            wait(lambda: u.IsWindowVisible(results) and u.GetDlgItem(results, 4901))
            post(results, 0x10)
            wait(lambda: not find("ExportResultsDialog"))
            expected = [PdfReader(pdf).pages[1].extract_text(), PdfReader(second_pdf).pages[0].extract_text()]
            actual = [page.extract_text() for page in PdfReader(destination / "cross-source.pdf").pages]
            with fitz.open(pdf) as first, fitz.open(second_pdf) as second, fitz.open(destination / "cross-source.pdf") as merged:
                if [p.get_text() for p in merged] != [first[1].get_text(), second[0].get_text()]:
                    raise RuntimeError("Independent engine found the wrong cross-source merge pages")
            if actual != expected or count() or any((destination / f"cross-source-{part}.pdf").exists() for part in ("a", "b")):
                raise RuntimeError("Cross-source merge changed order, queue, or per-job destinations")
            target(destination, "discard-on-exit.pdf")
            click(4201)
            if count() != 1:
                raise RuntimeError("Restart fixture reservation was not added")
            post(export, 0x10)
            wait(lambda: not find("UnifiedExportDialog"))
            send(lists[2], 0x186, 0)
            send(main_window, 0x111, 1 << 16, lists[2])
            if (original.read_bytes() != original_bytes or pdf.read_bytes() != source_pdf_bytes or
                    second_pdf.read_bytes() != second_pdf_bytes):
                raise RuntimeError("Export workflow changed a source document")
            # Recreate the temporary angles after switching back to this PDF.
            send(main_window, 0x111, 1133)
            send(main_window, 0x111, 1129)
            send(main_window, 0x111, 1132)
            mixed_angles = [180,90,90,90,90]
            for enabled in (False, True):
                post(main_window, 0x111, 1005)
                export = wait(lambda: find("UnifiedExportDialog"))
                wait(lambda: u.IsWindowVisible(export) and u.GetDlgItem(export, 4202))
                if send(u.GetDlgItem(export, 4099), 0xF0) != 1:
                    raise RuntimeError("New export dialogs must default to keeping page rotation")
                click(4001)
                click(4011)
                preserve_rotation(enabled)
                click(4202)
                saved = json.loads((workspace / "workspace.json").read_text(encoding="utf-8-sig"))
                if saved.get("quickPdfKeepPageRotation") is not enabled:
                    raise RuntimeError("Quick rotation preference was not persisted")
                post(export, 0x10)
                wait(lambda: not find("UnifiedExportDialog"))
                quick_pdf("quick-saved-" + str(enabled) + ".pdf", mixed_angles if enabled else [0]*5)
            if pdf.read_bytes() != source_pdf_bytes:
                raise RuntimeError("Quick output changed the original PDF")
            # Save a nondefault TextColor preset, then verify its actual reload.
            post(main_window, 0x111, 1014)
            export = wait(lambda: find("UnifiedExportDialog"))
            wait(lambda: u.IsWindowVisible(export) and u.GetDlgItem(export, 4120))
            click(4001)
            click(4011)
            preserve_rotation(False)
            color_mode = u.GetDlgItem(export, 4120)
            send(color_mode, 0x14E, 3)
            send(export, 0x111, 4120 | (1 << 16), color_mode)
            set_text(u.GetDlgItem(export, 4121), "#508CC8")
            click(4202)
            post(export, 0x10)
            wait(lambda: not find("UnifiedExportDialog"))
            post(main_window, 0x10)
            app.wait(timeout=15)
            if app.returncode != 0:
                raise RuntimeError("Export workflow app exit failed")
            # A new process must reload the TextColor preference from the file.
            restarted = (DesktopApp(owned_executable, root, desktop_name) if desktop
                         else subprocess.Popen([str(owned_executable)], cwd=root,
                                               startupinfo=startup, env=os.environ.copy()))
            if isinstance(app, DesktopApp):
                app.close()
            app = restarted
            main_window = wait(lambda: find("PdfWorkspaceMainWnd"))
            wait(lambda: u.SendMessageTimeoutW(main_window, 0, 0, 0, 2, 100,
                                              ctypes.byref(ctypes.c_size_t())))
            lists = []
            u.EnumChildWindows(main_window, collect, 0)
            if len(lists) < 4:
                raise RuntimeError("Restarted color-preset fixture has no main lists")
            for control in (lists[0], lists[1], lists[3]):
                wait(lambda: send(control, 0x18B) > 0)
                send(control, 0x186, 0)
                send(main_window, 0x111, 1 << 16, control)
            post(main_window, 0x111, 1014)
            export = wait(lambda: find("UnifiedExportDialog"))
            wait(lambda: u.IsWindowVisible(export) and u.GetDlgItem(export, 4211))
            reservations = u.GetDlgItem(export, 4210)
            if count() or (destination / "discard-on-exit.pdf").exists():
                raise RuntimeError("A reservation survived an application restart")
            post(export, 0x111, 4211)
            summary = wait(lambda: find("UnifiedSettingsShellClass"))
            wait(lambda: "出力" in text(u.GetDlgItem(summary, 7103)))
            if "簡易ノート" not in text(u.GetDlgItem(summary, 7103)):
                raise RuntimeError("Note export did not open the quick-note settings page")
            post(summary, 0x10)
            wait(lambda: not find("UnifiedSettingsShellClass"))
            click(4001)  # PDF settings route follows the active output kind.
            post(export, 0x111, 4211)
            summary = wait(lambda: find("UnifiedSettingsShellClass"))
            wait(lambda: u.IsWindowVisible(summary) and u.GetDlgItem(summary, 7100))
            backgrounds = []

            @callback_type
            def quick_background(hwnd, _):
                if u.GetDlgCtrlID(hwnd) == 4235:
                    backgrounds.append(hwnd)
                return True

            u.EnumChildWindows(summary, quick_background, 0)
            if len(backgrounds) != 1 or text(backgrounds[0]) != "#508CC8":
                raise RuntimeError("Quick PDF color preset was not reloaded after restart")
            post(summary, 0x10)
            wait(lambda: not find("UnifiedSettingsShellClass"))
            post(export, 0x10)
            wait(lambda: not find("UnifiedExportDialog"))
            post(main_window, 0x10)
            app.wait(timeout=15)
            if app.returncode != 0 or pdf.read_bytes() != source_pdf_bytes or original.read_bytes() != original_bytes:
                raise RuntimeError("Restarted color-preset fixture failed or changed originals")
            print("Export workflow passed: action layout, current and batch outputs, destination aliases, page validation, PDF-only merge options and cross-source order, missing-source recovery, original protection, overwrite cancellation, reservation retention on close and clearing on restart, quick PDF settings, and note integration", flush=True)
            return
        if args.file_operations:
            current = original

            def rename(command, source, names, accepted=None, confirm=False, save_failure=False):
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
                if confirm:
                    notice = wait(lambda: find("SilentDialogClass"))
                    wait(lambda: u.IsWindowVisible(notice) and u.GetDlgItem(notice, 5102))
                    if "txt" not in text(u.GetDlgItem(notice, 5101)):
                        raise RuntimeError("Nonstandard extension confirmation did not explain TXT treatment")
                    if source.read_bytes() != before or (source.parent / accepted).exists():
                        raise RuntimeError("A rename started before extension confirmation")
                    if save_failure:
                        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                        kernel.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p,
                                                       w.DWORD, w.DWORD, w.HANDLE]
                        kernel.CreateFileW.restype = w.HANDLE
                        kernel.CloseHandle.argtypes = [w.HANDLE]
                        config = workspace / "workspace.json"
                        config_bytes = config.read_bytes()
                        lock = kernel.CreateFileW(str(config), 0x80000000, 1, None, 3, 0, None)
                        if lock == ctypes.c_void_p(-1).value:
                            raise ctypes.WinError(ctypes.get_last_error())
                        try:
                            post(notice, 0x111, 5102)
                            wait(lambda: not find("SimpleInputDlg"))
                            send(main_window, 0)  # Complete the owning UI command.
                            if config.read_bytes() != config_bytes or source.read_bytes() != before or (source.parent / accepted).exists():
                                raise RuntimeError("Failed extension setting save changed the source or configuration")
                        finally:
                            kernel.CloseHandle(lock)
                        return source
                    post(notice, 0x111, 5102)  # Yes
                target = source.parent / accepted
                try:
                    wait(lambda: target.exists() and not source.exists())
                except RuntimeError:
                    notice = find("SilentDialogClass")
                    if notice:
                        print("Rename notice: " + text(u.GetDlgItem(notice, 5101)), flush=True)
                    draft = find("SimpleInputDlg")
                    print("Rename editor still open: " + str(bool(draft)), flush=True)
                    raise
                wait(lambda: not find("SimpleInputDlg"))
                if target.read_bytes() != before:
                    raise RuntimeError("Extension rename changed original document bytes")
                return target

            current = rename(3313, current, ["bad.exe", "no_extension", "bad.pdf", "bad?.txt",
                                            "bad.docx", "bad.xlsx", "bad.pptx", "bad.dll", "bad.png"], "renamed.md")
            for extension in (".markdown", ".csv", ".tex", ".note", ".icr", ".clro", ".txt"):
                current = rename(3313, current, [], "renamed" + extension)
            rename(3313, current, ["still-invalid.pdf"])
            # Rejection keeps the same name editor and does not register a suffix.
            post(main_window, 0x111, 3313)
            dialog = wait(lambda: find("SimpleInputDlg"))
            set_text(u.GetDlgItem(dialog, 101), "cancelled.memo")
            post(dialog, 0x111, 1)
            notice = wait(lambda: find("SilentDialogClass"))
            post(notice, 0x111, 5104)  # No; 5103 is the shared dialog's keep-open choice.
            wait(lambda: not find("SilentDialogClass"))
            if not u.IsWindow(dialog) or text(u.GetDlgItem(dialog, 101)) != "cancelled.memo":
                raise RuntimeError("Declining the extension confirmation discarded the draft")
            if not current.exists() or (session / "cancelled.memo").exists():
                raise RuntimeError("Declining the confirmation changed a document")
            if json.loads((workspace / "workspace.json").read_text(encoding="utf-8-sig")).get("additionalNoteExtensions"):
                raise RuntimeError("Declining the confirmation registered an extension")
            post(dialog, 0x10)
            wait(lambda: not find("SimpleInputDlg"))
            rename(3313, current, [], "save-failed.memo", confirm=True, save_failure=True)
            current = rename(3313, current, [], "renamed.Memo", confirm=True)
            config_path = workspace / "workspace.json"
            if json.loads(config_path.read_text(encoding="utf-8-sig"))["additionalNoteExtensions"] != [".memo"]:
                raise RuntimeError("Confirmed extension was not persisted in normalized form")

            def extension_settings(value=None, save=True, rejected=False):
                post(main_window, 0x111, 1302)
                settings = wait(lambda: find("UnifiedSettingsShellClass"))
                send(settings, 0x8000 + 140, 10)  # Note / Files
                controls = []

                @callback_type
                def control_id(hwnd, _):
                    if u.GetDlgCtrlID(hwnd) == 4222:
                        controls.append(hwnd)
                    return True

                def extensions_ready():
                    controls.clear()
                    u.EnumChildWindows(settings, control_id, 0)
                    return len(controls) == 1

                wait(extensions_ready)
                previous = text(controls[0])
                if value is not None:
                    set_text(controls[0], value)
                if rejected:
                    before = config_path.read_bytes()
                    send(settings, 0x111, 1)
                    if not u.IsWindow(settings) or config_path.read_bytes() != before:
                        raise RuntimeError("Settings accepted a restricted extension or changed saved configuration")
                    post(settings, 0x111, 2)
                else:
                    post(settings, 0x111, 1 if save else 2)
                if rejected or not save:
                    confirmation = wait(lambda: find("SilentDialogClass"))
                    post(confirmation, 0x111, 5103)  # Discard unsaved settings.
                wait(lambda: not find("UnifiedSettingsShellClass"))
                return previous

            if extension_settings(".memo, .log") != ".memo":
                raise RuntimeError("Settings did not display the extension approved by renaming")
            extension_settings(".discarded", save=False)
            extension_settings(".memo, .PDF", rejected=True)
            if json.loads(config_path.read_text(encoding="utf-8-sig"))["additionalNoteExtensions"] != [".memo", ".log"]:
                raise RuntimeError("Settings edit or cancellation did not preserve the saved extension list")

            # A real restart must discover other files with the allowed suffix,
            # while a hand-edited restricted entry must never enable that type.
            sibling = session / "another.memo"
            sibling.write_bytes(b"another plain text note\r\n")
            unknown = session / "hidden.unregistered"
            unknown.write_bytes(b"not opted in\r\n")
            failure_session = session.parent / "session2"
            failure_session.mkdir()
            legacy_note = failure_session / "legacy.txt"
            legacy_text = "再読み込みに失敗しても表示する本文"
            legacy_bytes = b"\xff\xfe" + legacy_text.encode("utf-16-le")
            legacy_note.write_bytes(legacy_bytes)
            post(main_window, 0x10)
            app.wait(timeout=15)
            if app.returncode != 0:
                raise RuntimeError("File operation fixture failed before restart")
            if desktop:
                app.close()
            saved = json.loads(config_path.read_text(encoding="utf-8-sig"))
            saved["additionalNoteExtensions"] += [".pdf", ".docx", ".exe"]
            config_path.write_text(json.dumps(saved), encoding="utf-8")
            app = (DesktopApp(owned_executable, root, desktop_name) if desktop
                   else subprocess.Popen([str(owned_executable)], cwd=root, startupinfo=startup, env=os.environ.copy()))
            main_window = wait(lambda: find("PdfWorkspaceMainWnd"))
            wait(lambda: u.SendMessageTimeoutW(main_window, 0, 0, 0, 2, 100, ctypes.byref(ctypes.c_size_t())))
            lists.clear()
            u.EnumChildWindows(main_window, collect, 0)
            if len(lists) < 4:
                raise RuntimeError("Restart did not create all file lists")
            for control in lists[:2]:
                wait(lambda: send(control, 0x18B) > 0)
                send(control, 0x186, 0)
                send(main_window, 0x111, 1 << 16, control)
            note_names = []
            for index in range(send(lists[3], 0x18B)):
                buffer = ctypes.create_unicode_buffer(send(lists[3], 0x18A, index) + 1)
                send(lists[3], 0x189, index, ctypes.addressof(buffer))
                note_names.append(buffer.value)
            if sorted(note_names) != sorted([current.name, sibling.name]):
                raise RuntimeError(f"Restart note candidates ignored extension policy: {note_names}")
            send(lists[3], 0x186, note_names.index(current.name))
            send(main_window, 0x111, 1 << 16, lists[3])
            # Remove the restricted direct-edit entries through the software UI.
            extension_settings(".memo, .log")
            extension_settings(".log")
            if send(lists[3], 0x18B) != 0 or current.read_bytes() != original_bytes:
                raise RuntimeError("Removing an extension failed to refresh candidates or changed the open note")
            extension_settings(".memo, .log")
            if send(lists[3], 0x18B) != 2:
                raise RuntimeError("Restoring an extension did not restore the note candidates")
            # Plain-text export must preserve literal Markdown punctuation.
            post(main_window, 0x111, 1008)
            output_picker = picker()
            set_text(u.GetDlgItem(output_picker, 103), "plain-extension-output.txt")
            output = current.parent / "plain-extension-output.txt"
            post(output_picker, 0x111, 1)
            wait(lambda: output.is_file() and output.stat().st_size > 0)
            if output.read_text(encoding="utf-8-sig").replace("\r\n", "\n") != original_bytes.decode().replace("\r\n", "\n"):
                raise RuntimeError("Additional extension export did not preserve TXT semantics")
            results = wait(lambda: find("ExportResultsDialog"))
            post(results, 0x10)
            wait(lambda: not find("ExportResultsDialog"))
            # Exercise the same folder picker from an actual move command.
            post(main_window, 0x111, 3315)
            move = picker()
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
            send(main_window, 0x111, 1043)  # Save a clockwise page rotation.
            view_file = workspace / "__pdf_note_workspace__" / "__tmp__" / "pdf_view_positions.txt"

            def saved_pdf_rotations(path):
                for row in view_file.read_text(encoding="utf-8").splitlines():
                    fields = row.split("\t")
                    if os.path.normcase(json.loads(fields[0])) == os.path.normcase(str(path)):
                        return fields[-1].removeprefix("R1:") if fields[-1].startswith("R1:") else None
                return None

            rotations = saved_pdf_rotations(pdf)
            if not rotations or "1" not in rotations:
                raise RuntimeError("Saved rotation fixture was not established")
            renamed_pdf = rename(3312, pdf, ["bad.txt", "without_extension", "bad.pdf.exe"], "renamed.PDF")
            if not renamed_pdf.exists():
                raise RuntimeError("Valid PDF rename failed")

            # TXT can display UTF-16, while CLRO requires UTF-8. The rename
            # preserves bytes, so this exercises a real post-commit load failure.
            if send(lists[1], 0x18B) != 2:
                raise RuntimeError("Expected both file-operation fixture sessions")
            send(lists[1], 0x186, 1)
            send(main_window, 0x111, 1 << 16, lists[1])
            send(lists[3], 0x186, 0)
            send(main_window, 0x111, 1 << 16, lists[3])

            def displayed_legacy_note():
                matches = []

                @callback_type
                def editor_with_text(hwnd, _):
                    cls = ctypes.create_unicode_buffer(128)
                    u.GetClassNameW(hwnd, cls, len(cls))
                    if cls.value.lower().startswith("richedit") and text(hwnd) == legacy_text:
                        matches.append(hwnd)
                    return True

                u.EnumChildWindows(main_window, editor_with_text, 0)
                return matches[0] if len(matches) == 1 else None

            editor = wait(displayed_legacy_note)
            send(editor, 0xB1, 1, 4)  # EM_SETSEL
            clro = rename(3313, legacy_note, [], "legacy.clro")
            notice = wait(lambda: find("SilentDialogClass", main_window))
            wait(lambda: u.IsWindowVisible(notice) and u.GetDlgItem(notice, 5102))
            expected = json.loads((repo / "locales/ja.json").read_text(encoding="utf-8-sig"))[
                "workspace.file_ops.moved_active_note_reload_failed"]
            if expected not in text(u.GetDlgItem(notice, 5101)):
                raise RuntimeError("Committed rename did not report the reload failure")
            if displayed_legacy_note() != editor or send(editor, 0xB0) != (4 << 16 | 1):
                raise RuntimeError("Failed reload discarded the open note or its selection")
            selected = send(lists[3], 0x188)  # LB_GETCURSEL
            selected_name = ctypes.create_unicode_buffer(len(clro.name) + 1)
            if selected != 0 or send(lists[3], 0x18A, selected) != len(clro.name):
                raise RuntimeError("Failed reload did not select the committed destination")
            send(lists[3], 0x189, selected, ctypes.addressof(selected_name))
            if selected_name.value != clro.name:
                raise RuntimeError("Failed reload left an outdated filename in the note list")
            post(notice, 0x111, 5102)
            wait(lambda: not find("SilentDialogClass"))
            recovered = rename(3313, clro, [], legacy_note.name)
            send(main_window, 0)  # Wait for the successful retry and list sync.
            if recovered.read_bytes() != legacy_bytes or displayed_legacy_note() != editor:
                raise RuntimeError("Renaming back did not reopen the retained note")
            # The session switch has saved the live state again. It must agree
            # with the rotation from before the rename, under the new path only.
            if saved_pdf_rotations(renamed_pdf) != rotations or saved_pdf_rotations(pdf) is not None:
                raise RuntimeError("Renamed PDF did not reopen with its saved rotation")
            post(main_window, 0x10)
            app.wait(timeout=15)
            if app.returncode != 0 or current.read_bytes() != original_bytes:
                raise RuntimeError("File-operation fixture exit or source preservation failed")
            print("File operations app smoke passed: extension validation and persistence; TXT export; rename/move byte preservation; failed reload keeps text and selection; retry uses the committed path; PDF rotation survives reopening", flush=True)
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
                try:
                    dialog = find(cls)
                except OSError as error:
                    print(f"Owned dialog enumeration cleanup: {error}", flush=True)
                    continue
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
        if desktop:
            app.close()
            u.SetThreadDesktop(previous_desktop)
            u.CloseDesktop(desktop)


if __name__ == "__main__":
    main()
