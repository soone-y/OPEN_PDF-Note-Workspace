"""Capture the real center guide in an owned app/workspace copy (foreground allowed)."""
from pathlib import Path
import argparse
import ctypes as ct
from ctypes import wintypes as wt
import json
import shutil
import subprocess
import uuid

import readonly_viewer_next_app_smoke as native


def main():
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.dev.isolated_app_test import protect_entry_point
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--locale', choices=('ja', 'en'), required=True)
    args = parser.parse_args()
    protect_entry_point(interactive=True)
    source = args.exe.resolve(strict=True)
    metadata = source.with_name(source.name + '.buildinfo.txt').read_text(encoding='utf-8-sig')
    native.require(f'locale\t{args.locale}' in metadata, 'explicit locale matches build-info')
    root = Path(__file__).resolve().parents[2]
    output = root / 'out/tests' / ('central_guide_visual_' + uuid.uuid4().hex)
    runtime = output / 'app'; workspace = output / 'workspace'
    runtime.mkdir(parents=True); workspace.mkdir()
    shutil.copyfile(source, runtime / source.name)
    for dll in source.parent.glob('*.dll'): shutil.copyfile(dll, runtime / dll.name)
    (runtime / 'pdf_note_workspace_setup.json').write_text(json.dumps({
        'workspaceRootMode': 'absolute', 'workspaceRoot': str(workspace)}), encoding='utf-8')
    native.WINDOW_CLASS = 'PdfWorkspaceMainWnd'
    u = native.u
    u.SetForegroundWindow.argtypes = [wt.HWND]
    u.RedrawWindow.argtypes = [wt.HWND, ct.c_void_p, wt.HANDLE, wt.UINT]
    u.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ct.c_int, ct.c_int, ct.c_int, ct.c_int, wt.UINT]
    results = []
    for mode in (0, 1, 2):
        (workspace / 'workspace.json').write_text(json.dumps({
            'classesDir': 'classes', 'cacheDir': 'cache', 'restoreLastSession': False,
            'emptyViewGuideMode': mode, 'topHeight': 520}), encoding='utf-8')
        process = subprocess.Popen([str(runtime / source.name)], cwd=runtime)
        hwnd = None
        try:
            native.wait_for(lambda: native.find_window(process.pid), 'owned app window')
            hwnd = native.find_window(process.pid)
            native.wait_for(lambda: u.IsWindowVisible(hwnd) and u.IsWindowEnabled(hwnd), 'ready main window')
            native.require(u.SetWindowPos(hwnd, None, 20, 20, 1280, 880, 0x0040), 'normal guide viewport')
            u.SetForegroundWindow(hwnd)
            view = []
            @native.enum_callback
            def child(window, unused):
                name = ct.create_unicode_buffer(128); u.GetClassNameW(window, name, 128)
                if name.value == 'PdfWorkspacePdfView': view.append(window)
                return True
            u.EnumChildWindows.argtypes = [wt.HWND, native.enum_callback, wt.LPARAM]
            u.EnumChildWindows(hwnd, child, 0)
            native.require(len(view) == 1, 'one actual PDF pane')
            pane = view[0]
            u.RedrawWindow(hwnd, None, None, 0x0185)
            if mode == 2:
                native.wait_for(lambda: native.text(pane) == '', 'hidden guide has no accessible text')
            else:
                native.wait_for(lambda: 'Ctrl+S' in native.text(pane), 'guide uses current content')
                native.require(('1.' in native.text(pane)) == (mode == 0), 'numbered usage matches mode')
            name = ('full', 'shortcuts', 'hidden')[mode]
            native.screenshot(hwnd, output / f'guide-{name}.png', onscreen=True)
            if mode == 0:
                native.require(u.SetWindowPos(hwnd, None, 20, 20, 940, 680, 0x0040), 'compact guide viewport')
                u.RedrawWindow(hwnd, None, None, 0x0185)
                native.screenshot(hwnd, output / 'guide-compact-top.png', onscreen=True)
                native.send(pane, 0x0115, 7)  # WM_VSCROLL / SB_BOTTOM
                u.RedrawWindow(hwnd, None, None, 0x0185)
                native.screenshot(hwnd, output / 'guide-compact-bottom.png', onscreen=True)
            results.append(name)
        finally:
            if hwnd and process.poll() is None: u.PostMessageW(hwnd, 0x0010, 0, 0)
            try: process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.terminate(); process.wait(timeout=5)
                raise RuntimeError('Owned visual-check app did not close')
            native.require(process.returncode == 0, 'normal owned process exit')
    (output / 'report.json').write_text(json.dumps({'locale': args.locale, 'modes': results,
        'not_verified': ['physical keyboard', 'real DPI switch', 'audible silence']}, indent=2), encoding='utf-8')
    print(f'PASS: central guide visual modes={len(results)}: {output}')


if __name__ == '__main__':
    main()
