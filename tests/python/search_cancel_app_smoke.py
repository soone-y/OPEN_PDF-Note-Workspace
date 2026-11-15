"""Run the real app on an unswitched desktop; retain an owned fixture.

Uses only the owned process's keyboard queue, not physical input or clipboard
writes. Run after building the selected locale, with --exe when using a named build.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import shutil
import time
import uuid

from output_path_app_smoke import DesktopApp


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--locale", choices=("ja", "en"), required=True)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--presentation-only", action="store_true",
                        help="Check folding, native main-window ownership and focus on a small fixture.")
    parser.add_argument("--groups-only", action="store_true",
                        help="Check external-folder selection, explicit group persistence and size confirmation.")
    parser.add_argument("--scope-close-only", action="store_true",
                        help="Check native close and cancellation routes of the search-scope window.")
    parser.add_argument("--completion-timeout", type=float, default=35,
                        help="Cold 601-file completion deadline; includes durable first-time identity registration.")
    args = parser.parse_args()
    if args.groups_only:
        from readonly_viewer_next_app_smoke import screenshot
    if not 0 < args.completion_timeout <= 180:
        parser.error("--completion-timeout must be greater than 0 and at most 180 seconds")
    repo = Path(__file__).resolve().parents[2]
    source = args.exe.resolve(strict=True)
    root = repo / "out/tests" / ("search_cancel_" + uuid.uuid4().hex)
    root.mkdir(parents=True)
    app_dir = root / "app"
    app_dir.mkdir()
    executable = app_dir / "pdf_note_workspace.exe"
    shutil.copy2(source, executable)
    if args.groups_only:
        shutil.copy2(source.parent / "readonly_viewer.exe", app_dir / "readonly_viewer.exe")
    for dll in source.parent.glob("*.dll"):
        shutil.copy2(dll, app_dir / dll.name)
    workspace = root / "workspace"
    session = workspace / "01_Lecture/01_Session"
    session.mkdir(parents=True)
    # Enough distinct files for multiple timer ticks and observable partial results.
    for index in range(12 if args.presentation_only or args.groups_only or args.scope_close_only else 600):
        (session / f"source_{index:04}.txt").write_bytes(b"needle preserved source\n")
    dotted_dir = workspace / "..draft"
    dotted_dir.mkdir()
    dotted_source = dotted_dir / "inside.txt"
    dotted_source.write_bytes(b"draftneedle\n")
    (workspace / "workspace.json").write_text(json.dumps({
        "classesDir": ".", "startupSelectFirstSession": False,
        "sessionFileLayout": "session_root", "sessionAutoOpenMode": "off"
    }), encoding="utf-8")
    (app_dir / "pdf_note_workspace_setup.json").write_text(json.dumps({
        "workspaceRootMode": "relative", "workspaceRoot": "../workspace"
    }), encoding="utf-8")
    print(f"Owned search fixture: {root}", flush=True)

    u, k = c.WinDLL("user32", use_last_error=True), c.WinDLL("kernel32", use_last_error=True)
    class GuiThreadInfo(c.Structure):
        _fields_ = [("size", w.DWORD), ("flags", w.DWORD),
                    ("active", w.HWND), ("focus", w.HWND), ("capture", w.HWND),
                    ("menu_owner", w.HWND), ("move_size", w.HWND), ("caret", w.HWND),
                    ("caret_rect", w.RECT)]
    callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    signatures = {
        "CreateDesktopW": ([w.LPCWSTR, w.LPCWSTR, c.c_void_p, w.DWORD, w.DWORD, c.c_void_p], w.HANDLE),
        "GetThreadDesktop": ([w.DWORD], w.HANDLE), "SetThreadDesktop": ([w.HANDLE], w.BOOL),
        "CloseDesktop": ([w.HANDLE], w.BOOL), "EnumDesktopWindows": ([w.HANDLE, callback, w.LPARAM], w.BOOL),
        "GetWindowThreadProcessId": ([w.HWND, c.POINTER(w.DWORD)], w.DWORD),
        "GetClassNameW": ([w.HWND, w.LPWSTR, c.c_int], c.c_int),
        "GetDlgItem": ([w.HWND, c.c_int], w.HWND), "GetFocus": ([], w.HWND),
        "FindWindowExW": ([w.HWND, w.HWND, w.LPCWSTR, w.LPCWSTR], w.HWND),
        "GetGUIThreadInfo": ([w.DWORD, c.POINTER(GuiThreadInfo)], w.BOOL),
        "SetFocus": ([w.HWND], w.HWND), "SetActiveWindow": ([w.HWND], w.HWND),
        "SetForegroundWindow": ([w.HWND], w.BOOL),
        "IsWindow": ([w.HWND], w.BOOL), "IsWindowEnabled": ([w.HWND], w.BOOL),
        "IsWindowVisible": ([w.HWND], w.BOOL),
        "GetWindowRect": ([w.HWND, c.POINTER(w.RECT)], w.BOOL),
        "GetWindow": ([w.HWND, w.UINT], w.HWND),
        "GetWindowLongPtrW": ([w.HWND, c.c_int], c.c_ssize_t),
        "GetSystemMenu": ([w.HWND, w.BOOL], w.HMENU),
        "GetMenuState": ([w.HMENU, w.UINT, w.UINT], w.UINT),
        "SetWindowPos": ([w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT], w.BOOL),
        "AttachThreadInput": ([w.DWORD, w.DWORD, w.BOOL], w.BOOL),
        "GetKeyboardState": ([c.c_void_p], w.BOOL), "SetKeyboardState": ([c.c_void_p], w.BOOL),
        "GetKeyState": ([c.c_int], c.c_short),
        "PostMessageW": ([w.HWND, w.UINT, w.WPARAM, w.LPARAM], w.BOOL),
        "PeekMessageW": ([c.POINTER(w.MSG), w.HWND, w.UINT, w.UINT, w.UINT], w.BOOL),
        "TranslateMessage": ([c.POINTER(w.MSG)], w.BOOL),
        "DispatchMessageW": ([c.POINTER(w.MSG)], w.LPARAM),
        "SendMessageTimeoutW": ([w.HWND, w.UINT, w.WPARAM, w.LPARAM, w.UINT, w.UINT,
                                  c.POINTER(c.c_size_t)], w.LPARAM),
    }
    for name, (arguments, result) in signatures.items():
        getattr(u, name).argtypes, getattr(u, name).restype = arguments, result
    thread = k.GetCurrentThreadId()
    previous_desktop = u.GetThreadDesktop(thread)
    desktop_name = "SearchCancel_" + uuid.uuid4().hex
    desktop = u.CreateDesktopW(desktop_name, None, None, 0, 0x01FF, None)
    if not desktop or not u.SetThreadDesktop(desktop):
        error = c.get_last_error()
        if desktop:
            u.CloseDesktop(desktop)
        raise c.WinError(error)
    app = None
    checks = 0
    case_context = ""

    def require(ok, label):
        nonlocal checks
        if not ok:
            raise RuntimeError(f"{case_context}: {label}" if case_context else label)
        checks += 1

    def send(hwnd, message, wp=0, lp=0):
        result = c.c_size_t()
        require(u.SendMessageTimeoutW(hwnd, message, wp, lp, 2, 5000, c.byref(result)),
                f"bounded owned message {message}")
        return result.value

    def text(hwnd):
        buf = c.create_unicode_buffer(65536)
        send(hwnd, 0xD, len(buf), c.addressof(buf))
        return buf.value

    def find(class_name, owner_pid=None):
        matches = []

        @callback
        def visit(hwnd, _):
            pid = w.DWORD()
            u.GetWindowThreadProcessId(hwnd, c.byref(pid))
            name = c.create_unicode_buffer(128)
            u.GetClassNameW(hwnd, name, len(name))
            expected_pid = app.pid if owner_pid is None else owner_pid
            if (expected_pid == -1 or pid.value == expected_pid) and name.value == class_name and u.IsWindowVisible(hwnd):
                matches.append(hwnd)
            return True

        c.set_last_error(0)
        if not u.EnumDesktopWindows(desktop, visit, 0) and c.get_last_error():
            raise c.WinError(c.get_last_error())
        return matches[0] if matches else None

    def wait(predicate, label, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            queued = w.MSG()
            while u.PeekMessageW(c.byref(queued), None, 0, 0, 1):
                u.TranslateMessage(c.byref(queued))
                u.DispatchMessageW(c.byref(queued))
            require(app.poll() is None, "owned app remains alive")
            result = predicate()
            if result:
                return result
            time.sleep(0.02)
        raise RuntimeError(f"Timed out: {case_context}: {label}" if case_context else "Timed out: " + label)

    def focus(window, control):
        target_thread = u.GetWindowThreadProcessId(window, None)
        require(u.AttachThreadInput(thread, target_thread, True), "attach owned queue")
        u.SetForegroundWindow(window)  # isolated desktop only; never switch desktops
        u.SetActiveWindow(window)
        u.SetFocus(control)
        if u.GetFocus() != control:
            actual_name, target_name = c.create_unicode_buffer(128), c.create_unicode_buffer(128)
            u.GetClassNameW(u.GetFocus(), actual_name, 128)
            u.GetClassNameW(control, target_name, 128)
            print(f"Focus diagnostic: actual_class={actual_name.value!r}, target_class={target_name.value!r}, "
                  f"window_enabled={bool(u.IsWindowEnabled(window))}, target_visible={bool(u.IsWindowVisible(control))}, "
                  f"target_enabled={bool(u.IsWindowEnabled(control))}", flush=True)
        require(u.GetFocus() == control, f"actual child focus established: actual={u.GetFocus()}, expected={control}")
        require(u.AttachThreadInput(thread, target_thread, False), "detach owned queue")

    def thread_focus(window):
        info = GuiThreadInfo()
        info.size = c.sizeof(info)
        require(u.GetGUIThreadInfo(u.GetWindowThreadProcessId(window, None), c.byref(info)), "observe owned focus without attaching")
        return info.focus

    def dismiss_startup_notice(notice):
        expected_title = "起動時点検" if args.locale == "ja" else "Startup check"
        require(text(notice) == expected_title, "only expected startup diagnostics may be dismissed")
        # Escape/close means leave the other package running, never request exit.
        send(notice, 0x10)
        wait(lambda: not u.IsWindow(notice), "leave other package untouched")

    def key(window, control, keycode, predicate, label):
        target_thread = u.GetWindowThreadProcessId(window, None)
        require(u.AttachThreadInput(thread, target_thread, True), "attach key queue")
        before = (c.c_ubyte * 256)()
        try:
            require(u.GetKeyboardState(before), "capture queue key state")
            u.SetForegroundWindow(window)
            u.SetActiveWindow(window)
            u.SetFocus(control)
            require(u.GetFocus() == control, "key target has focus")
            keys = (c.c_ubyte * 256)()
            keys[0x11] = 0x80
            require(u.SetKeyboardState(keys), "set queue-only Ctrl")
            require(u.PostMessageW(control, 0x100, keycode, 1), "post real-loop key")
            try:
                wait(predicate, label, 5)
            except RuntimeError:
                print(f"Key failure: focus={thread_focus(window)}, target={control}, Ctrl={u.GetKeyState(0x11)}, label={label}", flush=True)
                if search:
                    print(f"Search state: {text(status)!r}, button={text(run)!r}", flush=True)
                raise
            require(u.PostMessageW(control, 0x101, keycode, 0xC0000001), "release key")
        finally:
            u.SetKeyboardState(before)
            u.AttachThreadInput(thread, target_thread, False)

    try:
        # Do not inherit another test's workspace override or automation mode.
        saved_environment = {key: value for key, value in os.environ.items()
                             if key.startswith("PDF_NOTE_SMALL_")}
        for key in saved_environment:
            os.environ.pop(key)
        os.environ["PDF_NOTE_SMALL_INSTANCE_SUFFIX"] = "_" + desktop_name
        try:
            app = DesktopApp(executable, app_dir, desktop_name)
        finally:
            os.environ.pop("PDF_NOTE_SMALL_INSTANCE_SUFFIX", None)
            os.environ.update(saved_environment)
        main_window = wait(lambda: find("PdfWorkspaceMainWnd"), "main window")
        def ready():
            if notice := find("SilentDialogClass"):
                dismiss_startup_notice(notice)
            return u.IsWindowVisible(main_window) and u.IsWindowEnabled(main_window)
        wait(ready, "startup")
        require(u.PostMessageW(main_window, 0x111, 1190, 0), "queue search after application startup")
        search = wait(lambda: find("SearchWnd"), "search window")
        edit, run, results = (u.GetDlgItem(search, control) for control in (4101, 4102, 4103))
        if args.scope_close_only:
            routes = ("title-bar close", "dialog cancellation", "close button", "Escape")
            completed = []

            def close_via_queue(window, control, route):
                # Sent WM_CLOSE bypasses WH_GETMESSAGE/IsDialogMessage and hid
                # the original defect. Exercise the production message loop.
                menu = u.GetSystemMenu(window, False)
                require(menu and (u.GetMenuState(menu, 0xF060, 0) & 3) == 0,
                        "native title-bar close is enabled")
                if route == "close button":
                    button = u.GetDlgItem(window, 2)
                    require(button and u.IsWindowVisible(button) and u.IsWindowEnabled(button),
                            "visible enabled cancellation button exists")
                    require(u.PostMessageW(button, 0xF5, 0, 0), "queue cancellation button click")
                elif route == "Escape":
                    focus(window, control)
                    require(u.PostMessageW(control, 0x100, 0x1B, 1), "queue Escape from a child control")
                else:
                    message, command = (0x112, 0xF060) if route == "title-bar close" else (0x111, 2)
                    require(u.PostMessageW(window, message, command, 0), "queue " + route)
                wait(lambda: not u.IsWindow(window), route, 3)

            def assert_draft_unchanged(scopes):
                require(send(u.GetDlgItem(scopes, 4150), 0x18B) == 0,
                        "cancelled folder selection does not add a candidate")
                require(not (workspace / "__pdf_note_workspace__/__settings__/search_groups.bin").exists(),
                        "cancelled operation does not save a group")

            for label in routes:
                case_context = "scopes/" + label
                send(u.GetDlgItem(search, 4158), 0xF5)
                scopes = wait(lambda: find("PdfNoteSearchScopes"), "scope editor")
                require(u.IsWindowEnabled(scopes), "scope editor is enabled")
                require(not u.IsWindowEnabled(search), "scope editor disables its search owner")
                close_via_queue(scopes, u.GetDlgItem(scopes, 4150), label)
                require(u.IsWindowEnabled(search), "closing scope restores search")
                completed.append("scopes/" + label)
            for command, popup_class, control_id in ((4151, "LocalPathBrowserDlg", 102),
                                                     (4155, "SimpleInputDlg", 101)):
                send(u.GetDlgItem(search, 4158), 0xF5)
                scopes = wait(lambda: find("PdfNoteSearchScopes"), "scope editor for popup cancellation")
                for label in routes:
                    case_context = popup_class + "/" + label
                    require(u.PostMessageW(scopes, 0x111, command, 0), "open " + popup_class)
                    popup = wait(lambda: find(popup_class), popup_class)
                    require(u.GetWindow(popup, 4) == scopes, "popup has the scope window as native owner")
                    if popup_class == "SimpleInputDlg":
                        draft = c.create_unicode_buffer("cancelled group draft")
                        send(u.GetDlgItem(popup, control_id), 0xC, 0, c.addressof(draft))
                    close_via_queue(popup, u.GetDlgItem(popup, control_id), label)
                    wait(lambda: u.IsWindow(scopes) and u.IsWindowEnabled(scopes), "popup restores scope owner", 3)
                    require(not u.IsWindowEnabled(search), "search remains disabled until scope closes")
                    assert_draft_unchanged(scopes)
                    completed.append(popup_class + "/" + label)
                # A close intent while a nested loop owns stack context must
                # cancel that operation, then destroy scopes and restore search.
                case_context = popup_class + "/deferred parent close"
                require(u.PostMessageW(scopes, 0x111, command, 0), "reopen nested scope operation")
                popup = wait(lambda: find(popup_class), "nested scope popup")
                require(u.PostMessageW(scopes, 0x10, 0, 0), "close scope during nested operation")
                wait(lambda: not u.IsWindow(scopes) and not u.IsWindow(popup), "deferred nested close", 5)
                require(u.IsWindowEnabled(search), "nested close restores search")
                require(not (workspace / "__pdf_note_workspace__/__settings__/search_groups.bin").exists(),
                        "closing an unconfirmed operation does not save a group")
                completed.append(popup_class + "/deferred parent close")
            require(len(completed) == 14, "all 14 close cases completed")
            case_context = "final preservation and exit"
            for index in range(12):
                require((session / f"source_{index:04}.txt").read_bytes() == b"needle preserved source\n",
                        "closing auxiliary windows preserves original source")
            (root / "close_cases.json").write_text(json.dumps(completed, indent=2), encoding="utf-8")
            send(search, 0x10)
            send(main_window, 0x10)
            require(app.wait(timeout=10) == 0, "owned app exits normally")
            print(f"Search scope close ({args.locale}) PASS: {checks} checks; native close, cancellation, button, Escape, nested close, owner restoration.", flush=True)
            print("Search auxiliary close contract PASS: windows=3 routes=4 cases=14", flush=True)
            return
        if args.groups_only:
            external = root / "external_資料"
            deep = external
            for index in range(18):
                deep /= "level"
            deep.mkdir(parents=True)
            external_sources = {external / "root.txt": b"needle external\n",
                                deep / "deep.txt": b"needle deep\n",
                                external / "broken.pdf": b"not a PDF"}
            for path, content in external_sources.items():
                path.write_bytes(content)
            group_path = workspace / "__pdf_note_workspace__/__settings__/search_groups.bin"
            require(not group_path.exists(), "opening search does not save a group")
            send(u.GetDlgItem(search, 4158), 0xF5)
            scopes = wait(lambda: find("PdfNoteSearchScopes"), "scope editor")
            require(not u.IsWindowEnabled(search) and u.IsWindowEnabled(main_window),
                    "scope editor owns search while main remains usable")
            require(u.PostMessageW(scopes, 0x111, 4151, 0), "open folder picker")
            browser = wait(lambda: find("LocalPathBrowserDlg"), "local folder picker")
            require(u.PostMessageW(browser, 0x111, 2, 0), "queue folder-selection cancel")
            wait(lambda: not u.IsWindow(browser), "cancel folder selection")
            require(not group_path.exists(), "canceling folder selection does not save settings")
            require(u.PostMessageW(scopes, 0x111, 4151, 0), "reopen folder picker")
            browser = wait(lambda: find("LocalPathBrowserDlg"), "reopened local folder picker")
            send(browser, 0x111, 201)  # workspace -> owned fixture root
            browser_list = u.GetDlgItem(browser, 102)
            found_index = None
            for index in range(send(browser_list, 0x18B)):
                value = c.create_unicode_buffer(65536)
                send(browser_list, 0x189, index, c.addressof(value))
                if external.name in value.value:
                    found_index = index
                    break
            require(found_index is not None, "owned external folder appears in picker")
            send(browser_list, 0x186, found_index)
            send(browser, 0x111, 102 | (2 << 16))
            require(u.PostMessageW(browser, 0x111, 1, 0), "queue folder-selection accept")
            folders = u.GetDlgItem(scopes, 4150)
            wait(lambda: send(folders, 0x18B) == 1, "selected folder added")
            require(send(folders, 0x187, 0) == 1 and not group_path.exists(),
                    "candidate is enabled and remains unsaved")
            require(send(folders, 0x193) > 600, "long source path can be scrolled horizontally")
            require(send(u.GetDlgItem(search, 4113), 0xF0) == 1, "first added folder selects folder-only scope")
            require(u.PostMessageW(scopes, 0x111, 4155, 0), "save a named group")
            name_dialog = wait(lambda: find("SimpleInputDlg"), "group name")
            require(u.PostMessageW(name_dialog, 0x111, 2, 0), "queue group-name cancel")
            wait(lambda: not u.IsWindow(name_dialog), "cancel group naming")
            require(not group_path.exists(), "canceling naming keeps the draft unsaved")
            require(u.PostMessageW(scopes, 0x111, 4155, 0), "reopen group naming")
            name_dialog = wait(lambda: find("SimpleInputDlg"), "reopened group name")
            name = c.create_unicode_buffer("保持する検索_資料")
            send(u.GetDlgItem(name_dialog, 101), 0xC, 0, c.addressof(name))
            require(u.PostMessageW(name_dialog, 0x111, 1, 0), "queue group-name accept")
            combo = u.GetDlgItem(scopes, 4153)
            wait(lambda: group_path.exists() and send(combo, 0x146) == 1, "group saved explicitly")
            require(u.IsWindowEnabled(scopes), "scope editor remains enabled after naming a group")
            saved_group = group_path.read_bytes()
            send(scopes, 0x111, 4152)
            require(send(folders, 0x18B) == 0 and group_path.read_bytes() == saved_group,
                    "draft removal does not change saved group")
            send(combo, 0x14E, 0)
            send(scopes, 0x111, 4154)
            require(send(folders, 0x18B) == 1, "group reload restores folder")
            send(folders, 0x114, 7)  # Show the distinguishing end of a long candidate path.
            screenshot(scopes, root / f"search_scopes_{args.locale}.png")
            send(u.GetDlgItem(scopes, 2), 0xF5)
            require(not u.IsWindow(scopes), "Close button closes scope editor after saving/loading")
            require(u.IsWindowEnabled(search), "closing scope editor restores search")
            for control in (4132,):
                send(u.GetDlgItem(search, control), 0xF1, 0)
            query = c.create_unicode_buffer("needle")
            send(edit, 0xC, 0, c.addressof(query))
            send(search, 0x111, 4102)
            continue_button = u.GetDlgItem(search, 4157)
            wait(lambda: u.IsWindowVisible(continue_button), "deep scope confirmation")
            require(not u.IsWindowEnabled(edit) and send(results, 0x18B) == 0,
                    "confirmation pauses before searching content")
            screenshot(search, root / f"search_confirmation_{args.locale}.png")
            send(u.GetDlgItem(search, 4144), 0xF5)
            require(u.IsWindowVisible(continue_button), "confirmation remains usable with folded conditions")
            latency = []
            for _ in range(15):
                began = time.monotonic()
                send(main_window, 0)
                latency.append((time.monotonic() - began) * 1000)
            require(max(latency) < 500, "main UI responds while confirmation waits")
            send(continue_button, 0xF5)
            wait(lambda: u.IsWindowEnabled(edit) and send(results, 0x18B) == 4, "external recursive search reports matches and unreadable PDF")
            unreadable_index = note_index = None
            for index in range(send(results, 0x18B)):
                value = c.create_unicode_buffer(65536)
                send(results, 0x189, index, c.addressof(value))
                if "broken.pdf" in value.value:
                    unreadable_index = index
                if ".txt" in value.value:
                    note_index = index
            require(unreadable_index is not None and note_index is not None,
                    "unreadable source is distinct from note matches")
            send(results, 0x186, unreadable_index)
            send(search, 0x111, 4103 | (2 << 16))
            detail = wait(lambda: find("SilentDialogClass"), "unreadable-item details")
            require(text(detail) == ("検索" if args.locale == "ja" else "Search"), "owned search error detail")
            require(u.PostMessageW(detail, 0x10, 0, 0), "queue unreadable-detail close")
            wait(lambda: not u.IsWindow(detail), "close unreadable-item details")
            send(results, 0x186, note_index)
            send(search, 0x111, 4103 | (2 << 16))
            viewer = wait(lambda: find("PdfReadonlyViewerWindow", -1), "external result opens read-only viewer")
            wait(lambda: ".txt" in text(viewer), "external note is displayed by viewer")
            require(u.IsWindowEnabled(main_window), "read-only result leaves main usable")
            send(viewer, 0x10)
            wait(lambda: not u.IsWindow(viewer), "owned read-only viewer closes")
            require(all(path.read_bytes() == content for path, content in external_sources.items()),
                    "external originals unchanged")
            require(set(path for path in external.rglob("*") if path.is_file()) == set(external_sources),
                    "search creates no external metadata")
            send(search, 0x10)
            require(u.PostMessageW(main_window, 0x111, 1190, 0), "reopen search")
            search = wait(lambda: find("SearchWnd"), "fresh search window")
            send(u.GetDlgItem(search, 4158), 0xF5)
            scopes = wait(lambda: find("PdfNoteSearchScopes"), "reopened scope editor")
            combo, folders = u.GetDlgItem(scopes, 4153), u.GetDlgItem(scopes, 4150)
            require(send(combo, 0x146) == 1 and send(folders, 0x18B) == 0,
                    "saved group survives reopening; drafts are separate")
            send(combo, 0x14E, 0)
            send(scopes, 0x111, 4154)
            require(send(folders, 0x18B) == 1, "saved group is reusable")
            send(scopes, 0x10)
            send(search, 0x10)
            send(main_window, 0x10)
            require(app.wait(timeout=10) == 0, "first owned app exits normally")
            app.close()
            os.environ["PDF_NOTE_SMALL_INSTANCE_SUFFIX"] = "_" + desktop_name
            try:
                app = DesktopApp(executable, app_dir, desktop_name)
            finally:
                os.environ.pop("PDF_NOTE_SMALL_INSTANCE_SUFFIX", None)
            main_window = wait(lambda: find("PdfWorkspaceMainWnd"), "restart main window")
            wait(ready, "restart startup")
            require(u.PostMessageW(main_window, 0x111, 1190, 0), "restart search")
            search = wait(lambda: find("SearchWnd"), "restart search window")
            send(u.GetDlgItem(search, 4158), 0xF5)
            scopes = wait(lambda: find("PdfNoteSearchScopes"), "restart scope editor")
            combo = u.GetDlgItem(scopes, 4153)
            require(send(combo, 0x146) == 1 and group_path.read_bytes() == saved_group,
                    "group survives actual process restart")
            send(combo, 0x14E, 0)
            send(scopes, 0x111, 4156)
            require(send(combo, 0x146) == 0 and group_path.read_bytes() != saved_group,
                    "explicit delete persists")
            require(any((workspace / "__pdf_note_workspace__/__escape__").glob("search_groups_*.bin")),
                    "previous group data remains backed up")
            send(scopes, 0x10)
            send(search, 0x10)
            send(main_window, 0x10)
            require(app.wait(timeout=10) == 0, "restarted owned app exits normally")
            require(all(path.read_bytes() == content for path, content in external_sources.items()),
                    "all external sources remain unchanged after restart")
            print(f"Search groups ({args.locale}) PASS: {checks} checks; picker, explicit save/reload/delete, "
                  f"restart, deep-scope pause/continue, max_main_probe_ms={max(latency):.1f}, external originals unchanged.", flush=True)
            print("Physical input, DPI and audible-silence checks were not performed.", flush=True)
            return
        if args.presentation_only:
            details, pin = (u.GetDlgItem(search, control) for control in (4144, 4145))
            require(details and pin, "presentation controls exist")

            def rect(window):
                result = w.RECT()
                require(u.GetWindowRect(window, c.byref(result)), "observe actual control bounds")
                return result

            def resize(width, height):
                require(u.SetWindowPos(search, None, 0, 0, width, height, 0x16),
                        "resize without focus or Z-order change")

            send(u.GetDlgItem(search, 4112), 0xF5)
            for control in (4131, 4132):
                send(u.GetDlgItem(search, control), 0xF1, 0)
            query = c.create_unicode_buffer("needle")
            send(edit, 0xC, 0, c.addressof(query))
            send(search, 0x111, 4102)
            wait(lambda: u.IsWindowEnabled(edit) and send(results, 0x18B) == 13,
                 "small fixture search completes")
            send(results, 0x186, 3)
            for width, height in ((760, 820), (620, 780), (620, 640)):
                resize(width, height)
                expanded = rect(results)
                require(expanded.bottom > expanded.top, "expanded minimum layout retains results")
                before = [send(u.GetDlgItem(search, item), 0xF0) for item in (4112, 4130, 4131, 4132)]
                send(details, 0xF5)
                compact = rect(results)
                require(compact.bottom - compact.top > expanded.bottom - expanded.top + 300,
                        "folding gives the result list substantially more space")
                for item in (4110, 4111, 4112, 4113, 4119, 4130, 4131, 4132, 4140, 4141, 4142, 4143, 4158):
                    require(not u.IsWindowVisible(u.GetDlgItem(search, item)), "detail control is hidden")
                require(all(u.IsWindowVisible(item) for item in (edit, run, results, details, pin)),
                        "query/run/results and restore/pin controls stay visible")
                require(text(edit) == "needle" and send(results, 0x18B) == 13 and send(results, 0x188) == 3,
                        "query, hits and selection survive folding")
                require(before == [send(u.GetDlgItem(search, item), 0xF0) for item in (4112, 4130, 4131, 4132)],
                        "hidden search conditions remain unchanged")
                resize(width, 300)
                compact = rect(results)
                require(compact.bottom - compact.top > 80, "compact window can be made short")
                send(details, 0xF5)
                require(rect(search).bottom - rect(search).top >= 640, "expansion restores usable minimum height")
                require(u.IsWindowVisible(u.GetDlgItem(search, 4112)), "conditions are restored")

            long_query = "長い検索語 " * 40
            long_buffer = c.create_unicode_buffer(long_query)
            send(edit, 0xC, 0, c.addressof(long_buffer))
            send(search, 0x111, 4102)
            send(details, 0xF5)
            require(not u.IsWindowEnabled(edit) and u.IsWindowEnabled(run), "folding keeps an active search stoppable")
            send(search, 0x111, 4102)
            require(u.IsWindowEnabled(edit) and text(edit) == long_query, "compact Stop keeps long query intact")
            send(details, 0xF5)
            focus(search, u.GetDlgItem(search, 4112))
            send(search, 0x111, 4144)
            require(thread_focus(search) == edit, "hiding a focused condition returns focus to query")
            send(search, 0x111, 4144)

            # This fixture deliberately opens no note. Use an active main list
            # rather than its hidden compatibility RichEdit control.
            main_input = u.FindWindowExW(main_window, None, "LISTBOX", None)
            require(main_input and u.IsWindowVisible(main_input), "main input control exists")
            focus(search, edit)
            send(pin, 0xF5)
            require(send(pin, 0xF0) == 1 and u.GetWindow(search, 4) == main_window,
                    "actual checkbox click establishes native main ownership")
            require(not (u.GetWindowLongPtrW(search, -20) & 8), "search is not globally topmost")
            while notice := find("SilentDialogClass"):
                dismiss_startup_notice(notice)
            focus(main_window, main_input)
            settled = time.monotonic()
            wait(lambda: time.monotonic() - settled > 0.25, "observe main focus after activation settles")
            require(thread_focus(main_window) == main_input, "main input keeps focus with pinned search")
            window = search
            above_main = True
            while window := u.GetWindow(window, 3):  # GW_HWNDPREV
                if window == main_window:
                    above_main = False
                    break
            require(above_main, "search stays above the activated main window")
            # Programmatic presentation updates must not take focus from main.
            send(search, 0x111, 4144)
            require(thread_focus(main_window) == main_input, "folding does not take main focus")
            send(pin, 0xF1, 0)
            send(search, 0x111, 4145)
            require(not u.GetWindow(search, 4) and thread_focus(main_window) == main_input,
                    "unpin restores independent ownership without taking focus")
            send(pin, 0xF1, 1)
            send(search, 0x111, 4145)
            require(thread_focus(main_window) == main_input, "pinning does not take main focus")
            require(u.PostMessageW(main_window, 0x111, 1302, 0), "open settings while pinned")
            settings = wait(lambda: find("UnifiedSettingsShellClass"), "settings while pinned")
            require(u.IsWindowEnabled(search), "pinned search remains enabled during settings")
            send(settings, 0x111, 2)
            wait(lambda: not u.IsWindow(settings), "close settings while pinned")
            send(search, 0x10)
            require(not u.IsWindow(search), "pinned search closes independently")
            while notice := find("SilentDialogClass"):
                dismiss_startup_notice(notice)
            send(main_window, 0x10)
            require(app.wait(timeout=10) == 0, "presentation app exits normally")
            require(all(p.read_bytes() == b"needle preserved source\n" for p in session.glob("*.txt")),
                    "presentation checks leave note sources unchanged")
            print(f"Search presentation ({args.locale}) PASS: {checks} checks; fold/resize/conditions/results, "
                  "native ownership/Z-order, main focus, nested settings, independent close.", flush=True)
            print("Physical input, DPI and audible-silence checks were not performed.", flush=True)
            return
        # The status STATIC has no control ID in the production window.
        status = None
        status_prefix = "状態:" if args.locale == "ja" else "Status:"
        while candidate := u.FindWindowExW(search, status, "STATIC", None):
            status = candidate
            if text(status).startswith(status_prefix):
                break
        require(status and text(status).startswith(status_prefix), "localized status control exists")
        send(u.GetDlgItem(search, 4112), 0xF5)  # whole workspace
        for control in (4131, 4132):
            send(u.GetDlgItem(search, control), 0xF1, 0)  # note files only
        query = c.create_unicode_buffer("needle")
        send(edit, 0xC, 0, c.addressof(query))
        canceled = "中断" if args.locale == "ja" else "Canceled."
        running_label = "中止 (Ctrl+C)" if args.locale == "ja" else "Stop (Ctrl+C)"

        # Enter from the query must retain keyboard focus in the search window.
        focus(search, edit)
        require(thread_focus(search) == edit, "query focus survives queue detach")
        send(edit, 0x100, 0xD)
        require(not u.IsWindowEnabled(edit) and text(run) == running_label, "search started from Enter")
        actual_focus = thread_focus(search)
        require(actual_focus == run, f"disabled query transfers focus to Stop: actual={actual_focus}, expected={run}")
        key(search, run, ord("C"), lambda: canceled in text(status), "cancel during enumeration")

        # With no selection, normal idle Copy must be a no-op, without changing
        # the user's clipboard. Nonempty-selection Copy is left to native EDIT.
        send(edit, 0xB1, 0, 0)
        idle_status = text(status)
        started = time.monotonic()
        key(search, edit, ord("C"), lambda: time.monotonic() - started > 0.15, "idle Ctrl+C")
        require(text(status) == idle_status and text(edit) == "needle", "idle copy leaves query and status intact")

        # Every enabled child and the root must reach the same preserving cancel.
        for target in (run, results, u.GetDlgItem(search, 4140), u.GetDlgItem(search, 4143), search):
            send(search, 0x111, 4102)
            wait(lambda: send(results, 0x18B) > 0, "partial results")
            before_results = send(results, 0x18B)
            require(not u.IsWindowEnabled(edit), "job active before Ctrl+C")
            key(search, target, ord("C"), lambda: canceled in text(status), "Ctrl+C cancellation")
            require(u.IsWindow(search) and u.IsWindowEnabled(edit), "cancel keeps window and restores controls")
            require(text(edit) == "needle" and send(results, 0x18B) >= before_results, "query and partial results kept")
            stopped_count = send(results, 0x18B)
            time.sleep(0.12)
            require(send(results, 0x18B) == stopped_count, "canceled job does not append further results")

        # While a different top-level memo has focus, search must keep running.
        send(search, 0x111, 4102)
        send(search, 0x111, 4143)
        memo = wait(lambda: find("PdfNoteWorkspaceMemoWnd"), "memo window")
        memo_edit = u.GetDlgItem(memo, 1)
        require(text(memo_edit) == "", "copy test uses empty owned memo, no clipboard write")
        started = time.monotonic()
        key(memo, memo_edit, ord("C"), lambda: time.monotonic() - started > 0.15, "memo Ctrl+C")
        require(not u.IsWindowEnabled(edit), "memo Ctrl+C does not cancel search")
        send(memo, 0x10)
        require(not u.IsWindow(memo), "memo closes independently")
        while notice := find("SilentDialogClass"):
            dismiss_startup_notice(notice)
        note_edit = u.FindWindowExW(main_window, None, "RICHEDIT50W", None)
        require(note_edit and text(note_edit) == "", "main note is an empty copy target")
        started = time.monotonic()
        key(main_window, note_edit, ord("C"), lambda: time.monotonic() - started > 0.15, "main note Ctrl+C")
        require(not u.IsWindowEnabled(edit), "main note Ctrl+C does not cancel search")
        key(search, results, ord("C"), lambda: canceled in text(status), "return and cancel")

        # The search is unowned and can remain usable while settings runs its
        # nested message loop. This must not bypass the shortcut filter.
        send(search, 0x111, 4102)
        require(u.PostMessageW(main_window, 0x111, 1302, 0), "open settings loop")
        settings = wait(lambda: find("UnifiedSettingsShellClass"), "settings window")
        key(search, results, ord("C"), lambda: canceled in text(status), "cancel through settings loop")
        send(settings, 0x111, 2)
        wait(lambda: not u.IsWindow(settings), "close settings")

        # The existing Stop button continues to use the same cancellation.
        send(search, 0x111, 4102)
        require(text(run) == running_label, "restart after keyboard cancellation")
        send(run, 0xF5)
        wait(lambda: canceled in text(status), "Stop button")
        require(u.IsWindow(search) and text(edit) == "needle", "Stop button preserves search")

        # A directory whose name begins with '..' is still inside the root.
        dotted_query = c.create_unicode_buffer("draftneedle")
        send(edit, 0xC, 0, c.addressof(dotted_query))
        search_started = time.monotonic()
        send(search, 0x111, 4102)
        latency_samples = []

        def completed_with_ui_probe():
            began = time.monotonic()
            send(main_window, 0)  # WM_NULL on the real, shared UI thread.
            latency_samples.append((time.monotonic() - began) * 1000)
            return u.IsWindowEnabled(edit) and send(results, 0x18B) > 0

        try:
            wait(completed_with_ui_probe, "search completes inside ..draft", timeout=args.completion_timeout)
        except RuntimeError:
            print(f"Search timeout: status={text(status)!r}, results={send(results, 0x18B)}, "
                  f"probes={len(latency_samples)}, max_probe_ms={max(latency_samples, default=0):.1f}", flush=True)
            raise
        elapsed_ms = (time.monotonic() - search_started) * 1000
        print(f"Search completion PASS: 601 note files, elapsed_ms={elapsed_ms:.0f}, "
              f"UI probes={len(latency_samples)}, max_probe_ms={max(latency_samples):.1f}; "
              "message latency, not physical input latency.", flush=True)
        result_text = c.create_unicode_buffer(4096)
        require(send(results, 0x189, 0, c.addressof(result_text)) >= 0 and
                "..draft" in result_text.value,
                "root-local ..draft result is retained")
        # Separate normal repeat-search cost from cold durable-ID registration.
        latency_samples.clear()
        search_started = time.monotonic()
        send(search, 0x111, 4102)
        wait(completed_with_ui_probe, "warm repeat search completes", timeout=35)
        print(f"Search warm repeat PASS: elapsed_ms={(time.monotonic() - search_started) * 1000:.0f}, "
              f"UI probes={len(latency_samples)}, max_probe_ms={max(latency_samples):.1f}.", flush=True)
        sources = list(session.glob("*.txt"))
        require(len(sources) == 600 and all(p.read_bytes() == b"needle preserved source\n" for p in sources), "all source files unchanged")
        require(dotted_source.read_bytes() == b"draftneedle\n", "dotted source unchanged")
        send(search, 0x10)
        # A late startup diagnostic about another package can own a nested loop.
        # Acknowledge only notices belonging to this disposable app before exit.
        while notice := find("SilentDialogClass"):
            dismiss_startup_notice(notice)
        send(main_window, 0x10)
        require(app.wait(timeout=10) == 0, "owned app exits normally")
        print(f"Search Ctrl+C ({args.locale}): {checks} checks passed; isolated desktop, no physical keys or clipboard writes.", flush=True)
        print("Manual IME, DPI and audible-silence checks were not performed.", flush=True)
    finally:
        if app is not None:
            if app.poll() is None:
                app.kill()  # exact owned process only; keep all fixture evidence
                app.wait(timeout=10)
            app.close()
        u.SetThreadDesktop(previous_desktop)
        u.CloseDesktop(desktop)


if __name__ == "__main__":
    main()
