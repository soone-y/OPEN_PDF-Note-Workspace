"""Owned native UI check for the two discovery modes and transient preview."""
from __future__ import annotations
import argparse
import ctypes as ct
from ctypes import wintypes as wt
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid
import readonly_viewer_next_app_smoke as ui


def main():
    root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(root))
    from tools.dev.isolated_app_test import protect_entry_point
    protect_entry_point(interactive=False)
    import fitz
    from PIL import Image
    parser = argparse.ArgumentParser()
    parser.add_argument('--main-exe', required=True, type=Path)
    parser.add_argument('--viewer-exe', required=True, type=Path)
    parser.add_argument('--viewer-locale', choices=('ja', 'en'), default='ja')
    args = parser.parse_args()
    main_exe, viewer_exe = args.main_exe.resolve(strict=True), args.viewer_exe.resolve(strict=True)
    output = root / 'out/tests/pdf_pair_finder_ui' / uuid.uuid4().hex
    runtime = output / 'runtime'; runtime.mkdir(parents=True)
    for name in (main_exe.name, *[path.name for path in main_exe.parent.glob('*.dll')]):
        shutil.copyfile(main_exe.parent / name, runtime / name)
    shutil.copyfile(viewer_exe, runtime / 'readonly_viewer.exe')
    workspace = runtime / 'workspace'; (workspace / 'lecture1/session1').mkdir(parents=True)
    (workspace / 'workspace.json').write_text(json.dumps({'classesDir': '.', 'startupSelectFirstSession': False}), encoding='utf-8')
    (runtime / 'pdf_note_workspace_setup.json').write_text(json.dumps({'workspaceRootMode': 'relative', 'workspaceRoot': 'workspace'}), encoding='utf-8')
    search = output / 'search'; search.mkdir()
    source = output / 'clrop-source'; source.mkdir()
    pdf = search / 'renamed.pdf'
    document = fitz.open(); document.new_page(width=600, height=800); document.save(pdf); document.close()
    edited = search / 'edited.pdf'; edited.write_bytes(pdf.read_bytes() + b'\n% external incremental metadata\n')
    sidecar = search / 'old-name.clrop'
    data = {'version': 2, 'pdf_id': {'path': 'old-name.pdf', 'size': pdf.stat().st_size,
        'page_count': 1, 'page_sizes_pt': [[600, 800]], 'sha256': hashlib.sha256(pdf.read_bytes()).hexdigest()},
        'pages': [{'page': 0, 'items': [{'type': 'shape', 'id': 'candidate-marker',
            'created': '2026-11-07T00:00:00Z', 'updated': '2026-11-07T00:00:00Z',
            'bbox': [150, 550, 200, 150], 'color': '#00FF00', 'alpha': 1,
            'shape_kind': 'rectangle', 'shape_draw_mode': 'fill'}]}]}
    sidecar.write_text(json.dumps(data), encoding='utf-8')
    focused = source / 'selected.clrop'; focused.write_bytes(sidecar.read_bytes())
    broken = source / 'broken.clrop'; broken.write_text('{', encoding='utf-8')
    before = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in (pdf, edited, sidecar, focused, broken)}
    catalog = json.loads((root / 'locales/ja.json').read_text(encoding='utf-8-sig'))
    preview_catalog = json.loads((root / f'locales/{args.viewer_locale}.json').read_text(encoding='utf-8-sig'))
    kernel = ct.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]; kernel.OpenProcess.restype = wt.HANDLE
    kernel.VirtualAllocEx.argtypes = [wt.HANDLE, wt.LPVOID, ct.c_size_t, wt.DWORD, wt.DWORD]; kernel.VirtualAllocEx.restype = wt.LPVOID
    kernel.VirtualFreeEx.argtypes = [wt.HANDLE, wt.LPVOID, ct.c_size_t, wt.DWORD]
    kernel.WriteProcessMemory.argtypes = [wt.HANDLE, wt.LPVOID, wt.LPCVOID, ct.c_size_t, ct.POINTER(ct.c_size_t)]
    kernel.ReadProcessMemory.argtypes = [wt.HANDLE, wt.LPCVOID, wt.LPVOID, ct.c_size_t, ct.POINTER(ct.c_size_t)]
    kernel.CloseHandle.argtypes = [wt.HANDLE]
    kernel.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ct.POINTER(wt.DWORD)]
    ui.u.GetMenuItemCount.argtypes = [wt.HMENU]; ui.u.GetMenuItemID.argtypes = [wt.HMENU, ct.c_int]
    class Item(ct.Structure):
        _fields_ = [('mask', wt.UINT), ('item', ct.c_int), ('sub', ct.c_int), ('state', wt.UINT), ('stateMask', wt.UINT),
                    ('text', wt.LPVOID), ('length', ct.c_int), ('image', ct.c_int), ('param', wt.LPARAM), ('indent', ct.c_int)]

    def find(class_name, pid=None):
        found = []
        @ui.enum_callback
        def visit(hwnd, _):
            owner = wt.DWORD(); ui.u.GetWindowThreadProcessId(hwnd, ct.byref(owner))
            name = ct.create_unicode_buffer(128); ui.u.GetClassNameW(hwnd, name, len(name))
            if name.value == class_name and (pid is None or pid == owner.value):
                if pid is None:
                    handle = kernel.OpenProcess(0x1000, False, owner.value)
                    if not handle: return True
                    try:
                        path = ct.create_unicode_buffer(32768); size = wt.DWORD(len(path))
                        if not kernel.QueryFullProcessImageNameW(handle, 0, path, ct.byref(size)):
                            return True
                        if path.value.casefold() != str(runtime / 'readonly_viewer.exe').casefold():
                            return True
                    finally: kernel.CloseHandle(handle)
                found.append(hwnd)
            return True
        ui.u.EnumWindows(visit, 0)
        return found[0] if found else None

    def commands(menu):
        result = set()
        for index in range(ui.u.GetMenuItemCount(menu)):
            result.add(ui.u.GetMenuItemID(menu, index))
            child = ui.u.GetSubMenu(menu, index)
            if child: result |= commands(child)
        return result

    startup = subprocess.STARTUPINFO(); startup.dwFlags = subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow = 4
    process = subprocess.Popen([str(runtime / main_exe.name)], cwd=runtime, startupinfo=startup)
    remote_process = remote = None; tool = preview = window = None
    evidence = []
    try:
        ui.wait_for(lambda: find('PdfWorkspaceMainWnd', process.pid), 'owned main application')
        window = find('PdfWorkspaceMainWnd', process.pid)
        ui.wait_for(lambda: ui.u.IsWindowVisible(window) and
                    {3332, 3333} <= commands(ui.u.GetMenu(window)), 'both tool menu commands exist after startup')
        remote_process = kernel.OpenProcess(0x0038 | 0x0400, False, process.pid)
        ui.require(remote_process, 'open only the owned app for list observations')
        remote = kernel.VirtualAllocEx(remote_process, None, 4096, 0x3000, 4)
        ui.require(remote, 'allocate owned remote list buffer')

        def item_text(list_window, index, column):
            item = Item(); item.item = index; item.sub = column; item.text = remote + 2048; item.length = 512
            count = ct.c_size_t()
            ui.require(kernel.WriteProcessMemory(remote_process, remote, ct.byref(item), ct.sizeof(item), ct.byref(count)), 'write list request')
            ui.send(list_window, 0x1073, index, remote)
            text = ct.create_unicode_buffer(512)
            ui.require(kernel.ReadProcessMemory(remote_process, remote + 2048, text, ct.sizeof(text), ct.byref(count)), 'read candidate text')
            return text.value

        def select(list_window, index):
            item = Item(); item.state = item.stateMask = 3
            count = ct.c_size_t()
            ui.require(kernel.WriteProcessMemory(remote_process, remote, ct.byref(item), ct.sizeof(item), ct.byref(count)), 'write list selection')
            ui.send(list_window, 0x102B, index, remote)

        def open_tool(command):
            ui.u.PostMessageW(window, 0x0111, command, 0)
            def ready():
                handle = find('PdfNotePairFinderWindow', process.pid)
                return handle and ui.u.IsWindowVisible(handle) and ui.u.GetDlgItem(handle, 4603) and ui.u.GetDlgItem(handle, 4611)
            ui.wait_for(ready, 'discovery tool controls are ready')
            return find('PdfNotePairFinderWindow', process.pid)

        def search_tool(tool_window):
            ui.set_text(ui.u.GetDlgItem(tool_window, 4603), str(search))
            ui.send(tool_window, 0x0111, 4607)
            ui.wait_for(lambda: catalog['pair_finder.complete'] in ui.text(ui.u.GetDlgItem(tool_window, 4613)), 'completed discovery')
            list_window = ui.u.GetDlgItem(tool_window, 4611)
            count = ui.send(list_window, 0x1004)
            ui.require(count == 2, f'exactly two candidate rows: {count}')
            rows = [[item_text(list_window, index, col) for col in range(8)] for index in range(count)]
            return list_window, rows

        tool = open_tool(3332)
        ui.require(not ui.u.IsWindowEnabled(ui.u.GetDlgItem(tool, 4605)), 'directory mode has no focused CLROP input')
        table, rows = search_tool(tool)
        ui.require(rows[0][1] == 'renamed.pdf' and rows[0][4] == catalog['pair_finder.match'], 'hash match is listed first')
        ui.require(rows[1][1] == 'edited.pdf' and rows[1][5] == catalog['pair_finder.different'], 'different-size structure candidate remains listed')
        ui.require(all(row[3] == catalog['pair_finder.orphan'] for row in rows), 'orphan status is separate from evidence')
        ui.screenshot(tool, output / 'directory-results.png'); evidence.append('directory-mode-evidence-table')
        select(table, 1)
        ui.require(ui.u.IsWindowEnabled(ui.u.GetDlgItem(tool, 4609)), 'weaker candidate can be explicitly previewed')
        viewer_settings = {path.name: path.read_bytes() for path in runtime.glob('readonly*.json')}
        ui.send(tool, 0x0111, 4609)
        ui.wait_for(lambda: (handle := find('PdfReadonlyPairPreviewWindow')) and ui.u.IsWindowVisible(handle) and
                    ui.u.GetDlgItem(handle, 1001) and ui.u.GetDlgItem(handle, 1002), 'dedicated candidate preview controls')
        preview = find('PdfReadonlyPairPreviewWindow')
        ui.require(preview_catalog['pair_preview.banner'] == ui.text(ui.u.GetDlgItem(preview, 1001)), 'preview always labels candidate unconfirmed')
        ui.require(not ui.u.GetMenu(preview), 'candidate preview exposes no save or apply menu')
        def green():
            image = output / 'candidate-preview.png'; ui.screenshot(preview, image)
            with Image.open(image) as frame:
                return sum(g > 160 and g > r + 70 and g > b + 70 for r, g, b in frame.convert('RGB').get_flattened_data())
        ui.wait_for(lambda: green() > 500, 'mismatched candidate annotations are visible only in explicit preview')
        evidence.append('explicit-weak-candidate-preview')
        ui.u.PostMessageW(preview, 0x0010, 0, 0); ui.wait_for(lambda: not find('PdfReadonlyPairPreviewWindow'), 'preview closes'); preview = None
        ui.require(viewer_settings == {path.name: path.read_bytes() for path in runtime.glob('readonly*.json')},
                   'candidate preview does not persist normal viewer settings')
        ui.u.PostMessageW(tool, 0x0010, 0, 0); ui.wait_for(lambda: not find('PdfNotePairFinderWindow', process.pid), 'tool closes'); tool = None
        tool = open_tool(3333)
        ui.require(ui.u.IsWindowEnabled(ui.u.GetDlgItem(tool, 4605)), 'CLROP-focused mode has a separate source input')
        ui.set_text(ui.u.GetDlgItem(tool, 4605), str(focused))
        _, rows = search_tool(tool)
        ui.require(all(row[0] == focused.name for row in rows), 'focused mode investigates only the selected external CLROP')
        ui.screenshot(tool, output / 'clrop-focused-results.png'); evidence.append('clrop-focused-directory-search')
        ui.set_text(ui.u.GetDlgItem(tool, 4605), str(broken))
        ui.send(tool, 0x0111, 4607)
        ui.wait_for(lambda: catalog['pair_finder.complete'] in ui.text(ui.u.GetDlgItem(tool, 4613)), 'invalid CLROP remains an explicit result')
        invalid_list = ui.u.GetDlgItem(tool, 4611)
        ui.require(ui.send(invalid_list, 0x1004) == 1 and item_text(invalid_list, 0, 3) == catalog['pair_finder.unknown'],
                   'unreadable CLROP does not claim an adjacent same-name PDF')
        select(invalid_list, 0)
        ui.require(not ui.u.IsWindowEnabled(ui.u.GetDlgItem(tool, 4609)), 'unreadable row has no preview action')
        evidence.append('unreadable-source-has-unknown-evidence')
        ui.set_text(ui.u.GetDlgItem(tool, 4603), str(source))
        ui.require(ui.send(ui.u.GetDlgItem(tool, 4611), 0x1004) == 0 and
                   not ui.u.IsWindowEnabled(ui.u.GetDlgItem(tool, 4609)), 'editing conditions invalidates stale result actions')
        evidence.append('changed-conditions-discard-old-results')
        for path, digest in before.items(): ui.require(hashlib.sha256(path.read_bytes()).hexdigest() == digest, 'scan and preview preserve input bytes')
    finally:
        if remote and remote_process: kernel.VirtualFreeEx(remote_process, remote, 0, 0x8000)
        if remote_process: kernel.CloseHandle(remote_process)
        if preview: ui.u.PostMessageW(preview, 0x0010, 0, 0)
        if tool: ui.u.PostMessageW(tool, 0x0010, 0, 0); time.sleep(0.3)
        if window and process.poll() is None: ui.u.PostMessageW(window, 0x0010, 0, 0)
        try: process.wait(timeout=15)
        except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=5); raise RuntimeError('Owned application failed to close')
    ui.require(process.returncode == 0, 'normal app shutdown')
    for path, digest in before.items(): ui.require(hashlib.sha256(path.read_bytes()).hexdigest() == digest, 'shutdown preserves all discovery inputs')
    (output / 'result.json').write_text(json.dumps({'passed': evidence,
        'main_sha256': hashlib.sha256(main_exe.read_bytes()).hexdigest(), 'viewer_sha256': hashlib.sha256(viewer_exe.read_bytes()).hexdigest(),
        'not_verified': ['physical DPI changes', 'audible silence', 'power loss']}, indent=2), encoding='utf-8')
    print(f'PASS: PDF pair finder UI / {output}')


if __name__ == '__main__': main()
