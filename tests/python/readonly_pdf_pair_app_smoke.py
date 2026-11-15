"""Exercise PDF protection and CLROP rejection in an owned, isolated viewer copy."""
from __future__ import annotations
import argparse
import copy
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
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--locale', choices=('ja', 'en'), required=True)
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    build = dict(line.split('\t', 1) for line in
                 (exe.parent / (exe.name + '.buildinfo.txt')).read_text(encoding='utf-8-sig').splitlines()
                 if '\t' in line and not line.startswith('artifact\t'))
    ui.require(build['locale'] == args.locale, 'explicit locale matches viewer build')
    output = root / 'out/tests/readonly_pdf_pair' / uuid.uuid4().hex
    output.mkdir(parents=True)
    runtime = output / 'runtime'; runtime.mkdir()
    for name in (exe.name, 'pdfium.dll', 'libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll', 'LICENSE.md'):
        shutil.copyfile(exe.parent / name, runtime / name)
    shutil.copytree(exe.parent / 'licenses', runtime / 'licenses')
    exe = runtime / exe.name
    ui.WINDOW_CLASS = 'PdfReadonlyViewerWindow'
    pdf = output / 'pair.pdf'
    document = fitz.open()
    for _ in range(2):
        page = document.new_page(width=600, height=800)
        page.insert_text((60, 60), 'PAIR IDENTITY FIXTURE')
    document.save(pdf); document.close()
    original = pdf.read_bytes()
    sidecar = pdf.with_suffix('.clrop')
    identity = {'path': pdf.name, 'size': len(original), 'page_count': 2,
                'page_sizes_pt': [[600, 800], [600, 800]], 'sha256': hashlib.sha256(original).hexdigest()}
    base = {'version': 2, 'pdf_id': identity, 'pages': [{'page': 0, 'items': [
        {'type': 'shape', 'id': 'pair-marker', 'created': '2026-11-06T00:00:00Z',
         'updated': '2026-11-06T00:00:00Z', 'bbox': [150, 550, 200, 150],
         'color': '#00FF00', 'alpha': 1, 'shape_kind': 'rectangle', 'shape_draw_mode': 'fill'}]}]}

    def put(value):
        sidecar.write_text(json.dumps(value), encoding='utf-8')

    def marker(hwnd, name):
        panel = ui.u.GetDlgItem(hwnd, 1002)
        ui.require(panel and ui.u.IsWindowVisible(panel), 'PDF panel is visible')
        image_path = output / (name + '.png')
        ui.screenshot(hwnd, image_path)
        window_rect = wt.RECT(); panel_rect = wt.RECT()
        ui.u.GetWindowRect(hwnd, ct.byref(window_rect)); ui.u.GetWindowRect(panel, ct.byref(panel_rect))
        with Image.open(image_path) as image:
            area = image.convert('RGB').crop((panel_rect.left - window_rect.left,
                panel_rect.top - window_rect.top, panel_rect.right - window_rect.left,
                panel_rect.bottom - window_rect.top))
            return sum(g > 160 and g > r + 70 and g > b + 70
                       for r, g, b in area.resize((max(1, area.width // 3), max(1, area.height // 3))).get_flattened_data())

    kernel = ct.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.DWORD, wt.DWORD, wt.HANDLE]
    kernel.CreateFileW.restype = wt.HANDLE
    kernel.CloseHandle.argtypes = [wt.HANDLE]
    invalid = ct.c_void_p(-1).value

    def writer():
        return kernel.CreateFileW(str(pdf), 0x40000000, 7, None, 3, 0x80, None)

    evidence = []
    def run(label, check):
        startup = subprocess.STARTUPINFO(); startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 4
        process = subprocess.Popen([str(exe), str(pdf)], cwd=runtime, startupinfo=startup)
        hwnd = None
        try:
            ui.wait_for(lambda: ui.find_window(process.pid), 'owned viewer window')
            hwnd = ui.find_window(process.pid)
            ui.wait_for(lambda: ui.u.IsWindowVisible(hwnd), 'visible viewer')
            time.sleep(0.5)
            check(hwnd)
            evidence.append(label)
        finally:
            if hwnd and process.poll() is None:
                ui.u.PostMessageW(hwnd, 0x0111, 0xF060, 0)
            try: process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.terminate(); process.wait(timeout=5)
                raise RuntimeError('Owned viewer failed to close')
            ui.require(process.returncode == 0, 'viewer exits normally')
        ui.require(pdf.read_bytes() == original, 'PDF original is unchanged')

    def accepted(hwnd):
        ui.send(hwnd, 0x0111, 1118)  # Fit the entire first page before pixel observations.
        ui.wait_for(lambda: marker(hwnd, 'accepted') > 200, 'matching CLROP is rendered')
        handle = writer()
        if handle != invalid: kernel.CloseHandle(handle)
        ui.require(handle == invalid and ct.get_last_error() == 32, 'viewer denies external PDF writes')

    def rejected(hwnd):
        # Wait through several file-watch ticks: a rejected candidate must not
        # be attached later by a different reload path.
        time.sleep(2.2)
        ui.require(marker(hwnd, 'rejected') < 10, 'mismatched CLROP is never rendered')

    put(base); run('matching-hash-and-write-lock', accepted)
    handle = writer(); ui.require(handle != invalid, 'close releases PDF write lock'); kernel.CloseHandle(handle)
    for field, value in [('size', len(original) + 1), ('page_count', 3),
                         ('page_sizes_pt', [[610, 800], [600, 800]]), ('sha256', '0' * 64)]:
        changed = copy.deepcopy(base); changed['pdf_id'][field] = value
        put(changed); before = sidecar.read_bytes()
        run('mismatch-' + field, rejected)
        ui.require(sidecar.read_bytes() == before, 'viewer never rewrites mismatched sidecar')
    legacy = copy.deepcopy(base); legacy['pdf_id'].pop('sha256'); legacy['pdf_id'].pop('page_sizes_pt')
    put(legacy); run('legacy-fallback', accepted)

    def reload(hwnd):
        accepted(hwnd)
        changed = copy.deepcopy(base); changed['pdf_id']['sha256'] = 'f' * 64
        put(changed)
        ui.wait_for(lambda: marker(hwnd, 'reload-rejected') < 10, 'changed mismatched CLROP is hidden')
        put(base)
        ui.wait_for(lambda: marker(hwnd, 'reload-recovered') > 200, 'corrected CLROP is reattached')
        sidecar.unlink()
        ui.wait_for(lambda: marker(hwnd, 'sidecar-removed') < 10, 'removed CLROP is detached')
        put(base)
        ui.wait_for(lambda: marker(hwnd, 'sidecar-recreated') > 200, 'recreated CLROP is checked and reattached')
        ui.send(hwnd, 0x0111, 1114)  # View-only rotation must not change identity.
        ui.require(marker(hwnd, 'rotated') > 200, 'view rotation preserves validated annotations')
    put(base); run('reload-reject-recover-and-view-rotation', reload)
    initial = pdf, sidecar, original, base
    for turns in (1, 2, 3):
        pdf = output / f'intrinsic-{turns}.pdf'
        document = fitz.open()
        for _ in range(2):
            page = document.new_page(width=600, height=800)
            page.insert_text((60, 60), 'INTRINSIC ROTATION')
            page.set_rotation(turns * 90)
        sizes = [[page.rect.width, page.rect.height] for page in document]
        document.save(pdf); document.close()
        original = pdf.read_bytes(); sidecar = pdf.with_suffix('.clrop')
        base = copy.deepcopy(initial[3])
        base['pdf_id'] = {'path': pdf.name, 'size': len(original), 'page_count': 2,
                         'page_sizes_pt': sizes, 'sha256': hashlib.sha256(original).hexdigest()}
        put(base); run(f'intrinsic-rotation-{turns}', accepted)
    pdf, sidecar, original, base = initial
    external = writer(); ui.require(external != invalid, 'external editor starts before viewer')
    try:
        run('preexisting-writer-rejected', rejected)
    finally:
        kernel.CloseHandle(external)
    ui.require(pdf.read_bytes() == original, 'all cases preserve PDF bytes')
    (output / 'result.json').write_text(json.dumps({'passed': evidence, 'locale': args.locale,
        'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
        'not_verified': ['audible silence', 'power loss', 'full viewer suite']}, indent=2), encoding='utf-8')
    print(f'PASS: read-only PDF pairing / {len(evidence)} cases / {output}')


if __name__ == '__main__':
    main()
