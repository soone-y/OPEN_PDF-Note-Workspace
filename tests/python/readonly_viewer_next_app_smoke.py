"""Local native-reader smoke. Never touches existing user documents or processes.

Requires the explicitly built prototype, Pillow and PyMuPDF (development only).
Exercises displayed controls without SendInput; detach/reload UI may take foreground. Real IME, keyboard
focus, system DPI switching and audible silence still need interactive review.
"""
from __future__ import annotations
import argparse
import ctypes as ct
from ctypes import wintypes as wt
import hashlib
import json
from pathlib import Path
import subprocess
import shutil
import time
import uuid
import threading

u = ct.WinDLL('user32', use_last_error=True)
if hasattr(u, 'SetProcessDpiAwarenessContext'):
    u.SetProcessDpiAwarenessContext.argtypes = [wt.HANDLE]
    u.SetProcessDpiAwarenessContext.restype = wt.BOOL
    if not u.SetProcessDpiAwarenessContext(ct.c_void_p(-4)):
        u.SetThreadDpiAwarenessContext.argtypes = [wt.HANDLE]
        u.SetThreadDpiAwarenessContext.restype = wt.HANDLE
        if not u.SetThreadDpiAwarenessContext(ct.c_void_p(-4)):
            raise RuntimeError('Cannot match reader DPI context for native coordinate observations')
WINDOW_CLASS = 'PdfReadonlyViewerNextWindow'
g = ct.WinDLL('gdi32', use_last_error=True)
u.SendMessageTimeoutW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM, wt.UINT, wt.UINT, ct.POINTER(ct.c_size_t)]
u.SendMessageTimeoutW.restype = wt.LPARAM
u.GetDlgItem.argtypes = [wt.HWND, ct.c_int]
u.GetDlgItem.restype = wt.HWND
u.IsWindowVisible.argtypes = [wt.HWND]
u.IsWindowVisible.restype = wt.BOOL
u.IsWindowEnabled.argtypes = [wt.HWND]
u.IsWindowEnabled.restype = wt.BOOL
u.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u.GetWindowThreadProcessId.argtypes = [wt.HWND, ct.POINTER(wt.DWORD)]
u.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ct.c_int]
u.GetWindowRect.argtypes = [wt.HWND, ct.POINTER(wt.RECT)]
u.GetClientRect.argtypes = [wt.HWND, ct.POINTER(wt.RECT)]
u.GetDpiForWindow.argtypes = [wt.HWND]; u.GetDpiForWindow.restype = wt.UINT
u.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ct.c_int, ct.c_int, ct.c_int, ct.c_int, wt.UINT]
u.ShowWindow.argtypes = [wt.HWND, ct.c_int]
enum_callback = ct.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
u.EnumWindows.argtypes = [enum_callback, wt.LPARAM]
u.EnumChildWindows.argtypes = [wt.HWND, enum_callback, wt.LPARAM]
u.GetParent.argtypes = [wt.HWND]; u.GetParent.restype = wt.HWND
u.GetPropW.argtypes = [wt.HWND, wt.LPCWSTR]; u.GetPropW.restype = wt.HANDLE
u.GetSystemMetrics.argtypes = [ct.c_int]; u.GetSystemMetrics.restype = ct.c_int
u.WindowFromPoint.argtypes = [wt.POINT]; u.WindowFromPoint.restype = wt.HWND
u.GetAncestor.argtypes = [wt.HWND, wt.UINT]; u.GetAncestor.restype = wt.HWND
u.GetMenu.argtypes = [wt.HWND]; u.GetMenu.restype = wt.HMENU
u.GetSubMenu.argtypes = [wt.HMENU, ct.c_int]; u.GetSubMenu.restype = wt.HMENU
u.GetMenuState.argtypes = [wt.HMENU, wt.UINT, wt.UINT]; u.GetMenuState.restype = wt.UINT


def send(hwnd, msg, wp=0, lp=0):
    result = ct.c_size_t()
    if not u.SendMessageTimeoutW(hwnd, msg, wp, lp, 2, 5000, ct.byref(result)):
        raise RuntimeError(f'UI message timed out: {msg:x}')
    return result.value

def text(hwnd):
    size = send(hwnd, 0x000E)
    if size > 2_000_000:
        raise RuntimeError('Unexpected UI text length')
    buffer = ct.create_unicode_buffer(size + 1)
    send(hwnd, 0x000D, size + 1, ct.addressof(buffer))
    return buffer.value

def set_text(hwnd, value):
    buffer = ct.create_unicode_buffer(value)
    send(hwnd, 0x000C, 0, ct.addressof(buffer))

def require(condition, label):
    if not condition:
        raise AssertionError(label)

def wait_for(predicate, label):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise RuntimeError(f'Observation missing: {label}')

def find_window(pid):
    found = []
    @enum_callback
    def visit(hwnd, _):
        owner = wt.DWORD()
        u.GetWindowThreadProcessId(hwnd, ct.byref(owner))
        name = ct.create_unicode_buffer(128)
        u.GetClassNameW(hwnd, name, 128)
        if owner.value == pid and name.value == WINDOW_CLASS:
            found.append(hwnd)
        return True
    u.EnumWindows(visit, 0)
    return found[0] if found else None

def screenshot(hwnd, destination, *, onscreen=False):
    from PIL import Image
    u.GetDC.argtypes = [wt.HWND]; u.GetDC.restype = wt.HDC
    u.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
    u.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
    g.CreateCompatibleDC.argtypes = [wt.HDC]; g.CreateCompatibleDC.restype = wt.HDC
    g.CreateCompatibleBitmap.argtypes = [wt.HDC, ct.c_int, ct.c_int]; g.CreateCompatibleBitmap.restype = wt.HBITMAP
    g.SelectObject.argtypes = [wt.HDC, wt.HANDLE]; g.SelectObject.restype = wt.HANDLE
    g.DeleteObject.argtypes = [wt.HANDLE]
    g.DeleteDC.argtypes = [wt.HDC]
    g.BitBlt.argtypes = [wt.HDC, ct.c_int, ct.c_int, ct.c_int, ct.c_int, wt.HDC, ct.c_int, ct.c_int, wt.DWORD]
    class Header(ct.Structure):
        _fields_ = [('size', wt.DWORD), ('width', wt.LONG), ('height', wt.LONG), ('planes', wt.WORD),
                    ('bitCount', wt.WORD), ('compression', wt.DWORD), ('sizeImage', wt.DWORD),
                    ('x', wt.LONG), ('y', wt.LONG), ('used', wt.DWORD), ('important', wt.DWORD)]
    class Info(ct.Structure):
        _fields_ = [('header', Header), ('colors', wt.DWORD * 3)]
    g.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ct.c_void_p, ct.POINTER(Info), wt.UINT]
    rect = wt.RECT(); require(u.GetWindowRect(hwnd, ct.byref(rect)), 'window geometry')
    width, height = rect.right - rect.left, rect.bottom - rect.top
    if onscreen:
        # Observe pixels already presented, without PrintWindow forcing a new paint.
        for x, y in ((rect.left + 24, rect.top + 24), (rect.right - 24, rect.top + 24),
                     (rect.left + 24, rect.bottom - 24), (rect.right - 24, rect.bottom - 24)):
            observed_root = u.GetAncestor(u.WindowFromPoint(wt.POINT(x, y)), 2)
            require(observed_root == u.GetAncestor(hwnd, 2),
                    f'screen capture is unobstructed and belongs to the owned reader: point={x},{y}, '
                    f'observed={observed_root}, expected={u.GetAncestor(hwnd, 2)}, visible={u.IsWindowVisible(hwnd)}')
    source_window = None if onscreen else hwnd
    source = u.GetDC(source_window); dc = g.CreateCompatibleDC(source)
    bitmap = g.CreateCompatibleBitmap(source, width, height); old = g.SelectObject(dc, bitmap)
    try:
        if onscreen:
            require(g.BitBlt(dc, 0, 0, width, height, source, rect.left, rect.top, 0x00CC0020), 'presented pixel capture')
        else:
            require(u.PrintWindow(hwnd, dc, 2), 'native render capture')
        g.SelectObject(dc, old)
        info = Info(); info.header = Header(ct.sizeof(Header), width, -height, 1, 32, 0, 0, 0, 0, 0, 0)
        pixels = ct.create_string_buffer(width * height * 4)
        require(g.GetDIBits(dc, bitmap, 0, height, pixels, ct.byref(info), 0) == height, 'capture pixel count')
        Image.frombuffer('RGB', (width, height), pixels.raw, 'raw', 'BGRX', 0, 1).save(destination)
    finally:
        g.SelectObject(dc, old); g.DeleteObject(bitmap); g.DeleteDC(dc); u.ReleaseDC(source_window, source)

def main():
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.dev.isolated_app_test import protect_entry_point
    import fitz
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--variant', choices=('prototype', 'standard'), default='prototype')
    parser.add_argument('--locale', choices=('ja', 'en'))
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument('--pdf-wheel-zoom-only', action='store_true',
                        help='Run only the isolated PDF wheel-anchor regression; not the full UI suite.')
    scope.add_argument('--pdf-layout-only', action='store_true',
                       help='Run PDF wheel, layout and tab-position regressions; not the full UI suite.')
    scope.add_argument('--pdf-selection-only', action='store_true',
                       help='Check visible PDF frame/character selection without using the clipboard.')
    scope.add_argument('--pdf-grayscale-only', action='store_true',
                       help='Check the formal PDF menu, grayscale pixels, and process-only setting.')
    scope.add_argument('--pdf-rotation-only', action='store_true',
                       help='Check all view/intrinsic angles, tab isolation, selection and transient rotation.')
    scope.add_argument('--search-overview-only', action='store_true',
                       help='Check search-hit scrollbar marks and view/tab cancellation in an isolated copy.')
    args = parser.parse_args()
    protect_entry_point(interactive=True)
    exe = args.exe.resolve(strict=True)
    global WINDOW_CLASS
    WINDOW_CLASS = 'PdfReadonlyViewerWindow' if args.variant == 'standard' else 'PdfReadonlyViewerNextWindow'
    root = Path(__file__).resolve().parents[2]
    output = root / 'out/tests/readonly_viewer_next' / uuid.uuid4().hex
    output.mkdir(parents=True)
    md = output / '読みやすさの確認.md'
    md.write_text('# 読むことに、集中する。\n\n検索対象 alpha。日本語の検索対象。\n\n## 資料を読み比べる\n\n'
                  '左の一覧、上のタブ、右の本文。既存の配置を保ちます。\n\n'
                  '> 原本を変更せずに、必要なところへ移動できます。\n\n'
                  '- 検索対象を見つける\n- 文字を拡大して読む\n- 原文と表示を切り替える\n\n'
                  '## 表を確認する\n\n| 操作 | ショートカット |\n| --- | --- |\n| 本文検索 | Ctrl+F |\n| 集中表示 | Ctrl+B |\n'
                  '| 長い日本語のセル | セル内の全文が折り返されて切り捨てられずに最後まで読めることを確かめます。EndOfLongCell |\n\n'
                  '```mermaid\nflowchart LR\nA[原本を読む] --> B[安全に表示]\n```\n\n'
                  '```mermaid\nflowchart LR\nA[拒否]\nclick A callback\n```\n\n'
                  '```\n安全に、ローカルで読む。\n{\\rtf1 inert}\\object\\objdata\n```\n\n' + '読みやすい行間で、資料の内容を確認します。\n' * 35, encoding='utf-8')
    pdf = output / 'search.pdf'
    document = fitz.open()
    for index in range(3):
        page = document.new_page(); page.insert_text((72, 90), f'Reader page {index + 1}')
        if index >= 1: page.insert_text((72, 150), 'SearchNeedle')
    document.save(pdf); document.close()
    binary = output / 'bytes.bin'; binary.write_bytes(bytes(range(256)))
    originals = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in [md, pdf, binary]}
    evidence = []
    if args.variant == 'standard':
        require(args.locale is not None, 'standard viewer smoke requires explicit --locale')
        build = dict(line.split('\t', 1) for line in (exe.parent / (exe.name + '.buildinfo.txt')).read_text(encoding='utf-8-sig').splitlines() if '\t' in line and not line.startswith('artifact\t'))
    else:
        build = json.loads((exe.parent / 'prototype-build.json').read_text(encoding='utf-8-sig'))
    locale = build['locale']
    require(locale in ('ja', 'en'), 'explicit prototype locale')
    require(args.locale is None or args.locale == locale, 'smoke locale matches build-info')
    subprocess.run(['python', str(root / 'tools/dev/prepare_readonly_viewer_next_licenses.py'),
                    '--locale', locale, '--variant', args.variant, '--output', str(exe.parent), '--check'], check=True, cwd=root)
    # All app-created settings stay in a new owned copy, preserving personal
    # configuration beside the explicitly supplied executable.
    runtime_bin = output.parent / (output.name + '-runtime')
    runtime_bin.mkdir()
    for name in (exe.name, 'pdfium.dll', 'libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll', 'LICENSE.md'):
        shutil.copyfile(exe.parent / name, runtime_bin / name)
    shutil.copytree(exe.parent / 'licenses', runtime_bin / 'licenses')
    exe = runtime_bin / exe.name

    def license_view(hwnd):
        editor = u.GetDlgItem(hwnd, 104)
        send(hwnd, 0x0111, 122)
        require('README.txt' in text(hwnd), 'license menu opens local overview')
        guide = 'readonly_viewer' if args.variant == 'standard' else 'readonly_viewer_next'
        overview = (root / f'src/{guide}/licenses_{locale}.txt').read_text(encoding='utf-8')
        require(text(editor).replace('\r\n', '\n') == overview.replace('\r\n', '\n'),
                'complete localized copyright and runtime-exception overview')
        require(send(u.GetDlgItem(hwnd, 103), 0x1304) == 2, 'license view retains original document tab')
        send(hwnd, 0x0111, 122)
        require(send(u.GetDlgItem(hwnd, 103), 0x1304) == 2, 'reopening license menu reuses tab')
        tree = u.GetDlgItem(hwnd, 101)
        root_item = send(tree, 0x110A, 0)
        item = send(tree, 0x110A, 4, root_item)
        # Parent, md4c, mingw-w64, pdfium, project, manifest, README.
        for _ in range(4): item = send(tree, 0x110A, 1, item)
        require(item, 'project license folder present in native list')
        send(tree, 0x110B, 9, item)
        root_item = send(tree, 0x110A, 0)
        item = send(tree, 0x110A, 4, root_item)
        item = send(tree, 0x110A, 1, item)
        send(tree, 0x110B, 9, item)
        require('Soone-Y' in text(editor) and 'Permission' in text(editor), 'original project license readable')
        screenshot(hwnd, output / 'licenses.png')
        send(hwnd, 0x8000 + 4, 401, 0)
        require('検索対象 alpha' in text(editor), 'original reading survives license navigation')

    def run(path, check, executable=exe, restore_session=False, extra_arguments=()):
        startup = subprocess.STARTUPINFO(); startup.dwFlags = subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow = 4
        arguments = [str(executable)] if restore_session else ([str(executable), str(path)] if path else [str(executable), '--folder', str(output)])
        arguments.extend(extra_arguments)
        process = subprocess.Popen(arguments, cwd=executable.parent, startupinfo=startup)
        hwnd = None
        try:
            wait_for(lambda: find_window(process.pid), 'prototype root')
            hwnd = find_window(process.pid)
            wait_for(lambda: u.IsWindowVisible(hwnd), 'displayed prototype')
            time.sleep(0.3)
            check(hwnd)
            evidence.append(path.name if path else 'empty')
        finally:
            if hwnd and process.poll() is None and find_window(process.pid) == hwnd:
                u.PostMessageW(hwnd, 0x0111, 0xF060, 0)
            try: process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.terminate(); process.wait(timeout=5)
                raise RuntimeError('Owned test process did not close')
            require(process.returncode == 0, f'prototype exit {process.returncode}')

    if args.pdf_rotation_only:
        import math
        from PIL import Image, ImageChops
        fixtures = []
        for intrinsic in range(4):
            path = output / f'rotation-{intrinsic}.pdf'
            document = fitz.open()
            for _ in range(2):
                page = document.new_page(width=620, height=840)
                page.draw_rect(fitz.Rect(105, 125, 175, 160), color=(1, 0, 0), fill=(1, 0, 0))
                page.draw_rect(fitz.Rect(330, 570, 365, 640), color=(0, 0, 1), fill=(0, 0, 1))
                page.insert_text((220, 390), 'ALPHA BETA GAMMA', fontsize=16)
                page.set_cropbox(fitz.Rect(20, 30, 600, 810))
                page.set_rotation(intrinsic * 90)
            document.save(path); document.close()
            sidecar = path.with_suffix('.clrop')
            sidecar.write_text(json.dumps({'version': 1,
                'pdf_id': {'path': path.name, 'size': path.stat().st_size, 'page_count': 2}, 'pages': [
                {'page': index, 'items': [{'type': 'shape', 'id': f'box-{index}',
                 'created': '2026-10-25T00:00:00Z', 'updated': '2026-10-25T00:00:00Z', 'alpha': 1,
                 'bbox': [240, 590, 80, 40], 'color': '#00FF00',
                 'shape_kind': 'rectangle', 'shape_draw_mode': 'fill'},
                 {'type': 'text', 'id': f'text-{index}', 'created': '2026-10-25T00:00:00Z',
                  'updated': '2026-10-25T00:00:00Z', 'bbox': [150, 440, 180, 40],
                  'color': '#FF8000', 'content': 'CLROP TEXT', 'pt': 20}]} for index in range(2)]}), encoding='utf-8')
            originals[path] = hashlib.sha256(path.read_bytes()).hexdigest()
            originals[sidecar] = hashlib.sha256(sidecar.read_bytes()).hexdigest()
            fixtures.append(path)

        def menu_state(hwnd, command):
            view = u.GetSubMenu(u.GetMenu(hwnd), 1)
            require(view, 'formal View menu exists')
            send(hwnd, 0x0117, view, 1)
            state = u.GetMenuState(view, command, 0)
            require(state != 0xffffffff, f'rotation menu {command} exists')
            return state

        def capture_panel(hwnd, name):
            path = output / name
            panel = u.GetDlgItem(hwnd, 1002)
            screenshot(panel, path)
            with Image.open(path) as image: return image.copy()

        def bounds(image, channel):
            r, green, b = image.convert('RGB').split()
            channels = (r, green, b)
            mask = channels[channel].point(lambda v: 255 if v > 180 else 0)
            for index in range(3):
                if index != channel:
                    mask = ImageChops.multiply(mask, channels[index].point(lambda v: 255 if v < 70 else 0))
            found = mask.getbbox()
            require(found, f'colored landmark {channel} visible')
            return found

        def center(box): return ((box[0]+box[2])/2, (box[1]+box[3])/2)

        def check_pixels(hwnd, intrinsic, turns, name):
            # Reset zoom/position so both landmarks must be fully visible.
            panel = u.GetDlgItem(hwnd, 1002)
            send(panel, 0x0111, 1113); send(panel, 0x0100, 0x24)  # fit, Home
            actual = capture_panel(hwnd, name)
            red, blue, green = (bounds(actual, index) for index in (0, 2, 1))
            with fitz.open(fixtures[intrinsic]) as document:
                page = document[0]
                pix = page.get_pixmap(matrix=fitz.Matrix(1, 1).prerotate(turns * 90))
                reference = Image.frombytes('RGB', (pix.width, pix.height), pix.samples)
            er, eb = bounds(reference, 0), bounds(reference, 2)
            # Use the long landmark separation for scale: a one-pixel rounding
            # error in the small red box otherwise amplifies over the page.
            scale = math.dist(center(red), center(blue)) / math.dist(center(er), center(eb))
            origin = tuple(center(red)[i] - center(er)[i]*scale for i in range(2))
            for axis in range(2):
                require(abs(center(blue)[axis] - (origin[axis]+center(eb)[axis]*scale)) < 5,
                        f'PDF landmark agrees with independent engine: intrinsic={intrinsic}, view={turns}')
            require(abs((red[3]-red[1]) - (er[3]-er[1])*scale) < 4, 'rotated page aspect ratio')
            r, g, b = actual.convert('RGB').split()
            orange = ImageChops.multiply(r.point(lambda v: 255 if v > 180 else 0),
                ImageChops.multiply(g.point(lambda v: 255 if 70 < v < 180 else 0),
                                    b.point(lambda v: 255 if v < 70 else 0))).getbbox()
            require(orange, 'CLROP text remains visible')
            require((orange[2]-orange[0] < orange[3]-orange[1]) == bool((intrinsic+turns) % 2),
                    'CLROP text content rotates with the paper')
            # Native CLROP bbox projected independently, including the CropBox offset.
            x, y, w, h = 260, 240, 580, 780  # center relative to cropped page, top-left origin
            for _ in range((intrinsic + turns) % 4): x, y, w, h = h-y, x, h, w
            for axis, expected in enumerate((x, y)):
                require(abs(center(green)[axis] - (origin[axis]+expected*scale)) < 6,
                        'CLROP follows the same paper rotation and CropBox origin')
            require(bool(menu_state(hwnd, 1116) & 3) == (turns == 0), 'reset enabled only for a rotated PDF')
            return actual, origin, scale

        class CopyData(ct.Structure):
            _fields_ = [('tag', ct.c_size_t), ('size', wt.DWORD), ('data', ct.c_void_p)]

        def add_tab(hwnd, path):
            buffer = ct.create_unicode_buffer(str(path))
            payload = CopyData(0x504E5254, ct.sizeof(buffer), ct.addressof(buffer))
            require(send(hwnd, 0x004A, hwnd, ct.addressof(payload)), 'open another tab through production entry')

        def selection(hwnd, intrinsic, turns, origin, scale):
            panel = u.GetDlgItem(hwnd, 1002)
            def point(x, y):
                w, h = 580, 780
                for _ in range((intrinsic+turns) % 4): x, y, w, h = h-y, x, h, w
                return tuple(round(origin[i] + value*scale) for i, value in enumerate((x, y)))
            def drag(a, b):
                pack = lambda p: (p[0] & 0xffff) | ((p[1] & 0xffff) << 16)
                send(panel, 0x0201, 1, pack(a)); send(panel, 0x0200, 1, pack(b)); send(panel, 0x0202, 0, pack(b))
            drag(point(195, 335), point(390, 375))
            frame = capture_panel(hwnd, f'selection-{intrinsic}-{turns}-frame.png')
            with fitz.open(fixtures[intrinsic]) as document:
                chars = [char for block in document[0].get_text('rawdict')['blocks'] if 'lines' in block
                         for line in block['lines'] for span in line['spans'] for char in span['chars']]
                start = next(i for i, char in enumerate(chars) if char['c'] == 'B')
                first, last = chars[start]['bbox'], chars[start+3]['bbox']
            drag(point(first[0]+2, (first[1]+first[3])/2), point(last[2]-2, (last[1]+last[3])/2))
            selected = capture_panel(hwnd, f'selection-{intrinsic}-{turns}-text.png')
            require(selected.tobytes() != frame.tobytes(), 'rotated second-stage character highlight visible')
            send(panel, 0x0100, 0x1b)
            require(capture_panel(hwnd, 'selection-cleared.png').tobytes() == frame.tobytes(),
                    'first Escape clears only characters in rotated frame')
            send(panel, 0x0100, 0x1b)

        def rotation_check(hwnd, intrinsic):
            for turns in range(4):
                image, origin, scale = check_pixels(hwnd, intrinsic, turns, f'rotation-{intrinsic}-{turns}.png')
                selection(hwnd, intrinsic, turns, origin, scale)
                set_text(u.GetDlgItem(hwnd, 810), 'BETA')
                send(hwnd, 0x0111, 811)
                wait_for(lambda: '1ページ' in text(u.GetDlgItem(hwnd, 817)) or
                         'page 1' in text(u.GetDlgItem(hwnd, 817)), 'search finds text in rotated page')
                send(hwnd, 0x0111, 811)
                wait_for(lambda: '2ページ' in text(u.GetDlgItem(hwnd, 817)) or
                         'page 2' in text(u.GetDlgItem(hwnd, 817)), 'search reaches second rotated page')
                set_text(u.GetDlgItem(hwnd, 810), '')
                send(u.GetDlgItem(hwnd, 1002), 0x0100, 0x24)
                check_pixels(hwnd, intrinsic, turns, f'rotation-{intrinsic}-{turns}-after-search.png')
                # Both pages share the tab's angle.
                panel = u.GetDlgItem(hwnd, 1002)
                send(panel, 0x0100, 0x27)  # next page
                second = capture_panel(hwnd, f'rotation-{intrinsic}-{turns}-page2.png')
                red, blue = bounds(second, 0), bounds(second, 2)
                require(all(abs((center(blue)[i]-center(red)[i]) -
                                (center(bounds(image, 2))[i]-center(bounds(image, 0))[i])) < 4 for i in range(2)),
                        'all pages share the tab rotation')
                send(panel, 0x0100, 0x24)
                send(hwnd, 0x0111, 1115)
            check_pixels(hwnd, intrinsic, 0, f'rotation-{intrinsic}-four-turns.png')
            send(hwnd, 0x0111, 1114)
            check_pixels(hwnd, intrinsic, 3, f'rotation-{intrinsic}-left.png')
            if intrinsic == 0:
                def transfer_check(target):
                    for value in ('4'+str(fixtures[0]), '3', '3'+str(fixtures[0])+'\0extra'):
                        buffer = ct.create_unicode_buffer(value)
                        payload = CopyData(0x504E5252, ct.sizeof(buffer), ct.addressof(buffer))
                        require(send(target, 0x004A, hwnd, ct.addressof(payload)) == 0,
                                'malformed rotated transfer is rejected')
                        require(send(u.GetDlgItem(target, 103), 0x1304) == 1,
                                'rejected transfer retains recipient tab')
                    buffer = ct.create_unicode_buffer('3'+str(fixtures[0]))
                    payload = CopyData(0x504E5252, ct.sizeof(buffer), ct.addressof(buffer))
                    require(send(target, 0x004A, 0, ct.addressof(payload)) == 0, 'untrusted rotated transfer rejected')
                    require(send(target, 0x004A, hwnd, ct.addressof(payload)) == 1, 'trusted rotated transfer accepted')
                    check_pixels(target, 0, 3, 'rotation-window-transfer.png')
                run(binary, transfer_check)
                run(fixtures[0], lambda child: check_pixels(child, 0, 3, 'rotation-new-window.png'),
                    extra_arguments=('--transfer-source', str(hwnd), '--transfer-token', '12345678',
                                     '--transfer-rotation', '3'))
                check_pixels(hwnd, 0, 3, 'rotation-source-retained.png')
            add_tab(hwnd, fixtures[(intrinsic+1) % 4])
            check_pixels(hwnd, (intrinsic+1) % 4, 0, f'rotation-{intrinsic}-new-tab.png')
            send(hwnd, 0x0111, 1115)
            add_tab(hwnd, md)
            require(menu_state(hwnd, 1114) & 3, 'rotation disabled for text tab')
            send(hwnd, 0x0111, 1115)
            send(hwnd, 0x8000+4, 401, 0)
            check_pixels(hwnd, intrinsic, 3, f'rotation-{intrinsic}-tab-return.png')
            send(hwnd, 0x0111, 1116)
            check_pixels(hwnd, intrinsic, 0, f'rotation-{intrinsic}-reset.png')
            send(hwnd, 0x0111, 1116)  # repeated reset is harmless
            if intrinsic == 3:
                send(hwnd, 0x0111, 117)  # enable session restore
                send(hwnd, 0x0111, 118)  # persist this tab path
            send(hwnd, 0x0111, 1115)  # exit rotated

        for intrinsic in range(4): run(fixtures[intrinsic], lambda hwnd, i=intrinsic: rotation_check(hwnd, i))
        run(None, lambda hwnd: check_pixels(hwnd, 3, 0, 'rotation-restored-tab.png'), restore_session=True)
        run(fixtures[0], lambda hwnd: check_pixels(hwnd, 0, 0, 'rotation-restart.png'))
        for path, digest in originals.items():
            require(hashlib.sha256(path.read_bytes()).hexdigest() == digest, f'original preserved: {path.name}')
        for settings in runtime_bin.glob('*.json'):
            def check_keys(value):
                if isinstance(value, dict):
                    require(all('rotation' not in key.lower() for key in value), 'view rotation is not persisted')
                    for child in value.values(): check_keys(child)
                elif isinstance(value, list):
                    for child in value: check_keys(child)
            check_keys(json.loads(settings.read_text(encoding='utf-8')))
        report = {'scope': 'pdf-rotation-only', 'passed': evidence, 'orientation_cases': 16,
                  'pages_per_case': 2, 'originals_unchanged': True, 'variant': args.variant, 'locale': locale,
                  'not_verified': ['physical keyboard shortcuts', 'real DPI changes', 'audible silence'],
                  'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest()}
        (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(f'PASS (pdf-rotation-only): {output}')
        return

    if args.pdf_grayscale_only:
        from PIL import Image
        grayscale_pdf = output / 'grayscale.pdf'
        document = fitz.open(); page = document.new_page(width=595, height=842)
        page.draw_rect(fitz.Rect(120, 170, 475, 610), color=(1, 0, 0), fill=(1, 0, 0))
        document.save(grayscale_pdf); document.close()
        original = hashlib.sha256(grayscale_pdf.read_bytes()).hexdigest()
        legacy_settings = runtime_bin / 'readonly_viewer_settings.json'
        legacy_settings.write_text('{"showGrayscale":true}', encoding='utf-8')
        settings_bytes = legacy_settings.read_bytes()

        def menu_state(hwnd):
            bar = u.GetMenu(hwnd); view = u.GetSubMenu(bar, 1)
            require(view, 'formal View menu exists')
            send(hwnd, 0x0117, view, 1)  # WM_INITMENUPOPUP
            return u.GetMenuState(view, 1106, 0)  # MF_BYCOMMAND

        def red_pixels(hwnd, name):
            path = output / name; screenshot(hwnd, path)
            root_rect = wt.RECT(); panel_rect = wt.RECT()
            panel = u.GetDlgItem(hwnd, 1002)
            require(u.GetWindowRect(hwnd, ct.byref(root_rect)) and
                    u.GetWindowRect(panel, ct.byref(panel_rect)), 'PDF panel crop')
            with Image.open(path) as image:
                area = image.crop((panel_rect.left - root_rect.left, panel_rect.top - root_rect.top,
                                   panel_rect.right - root_rect.left, panel_rect.bottom - root_rect.top))
                pixels = area.load()
                return sum(pixels[x, y][0] > 120 and
                           pixels[x, y][0] > pixels[x, y][1] + 40 and
                           pixels[x, y][0] > pixels[x, y][2] + 40
                           for y in range(0, area.height, 4) for x in range(0, area.width, 4))

        def grayscale_check(hwnd):
            require(u.IsWindowVisible(u.GetDlgItem(hwnd, 1002)), 'PDF panel visible')
            require(menu_state(hwnd) & 0x0003 == 0, 'grayscale menu enabled for PDF')
            require(menu_state(hwnd) & 0x0008 == 0, 'grayscale starts off despite legacy setting')
            wait_for(lambda: red_pixels(hwnd, 'grayscale-color.png') > 1000, 'color PDF pixels visible')
            send(hwnd, 0x0111, 1106)
            require(menu_state(hwnd) & 0x0008, 'grayscale menu checked after toggle')
            wait_for(lambda: red_pixels(hwnd, 'grayscale-on.png') < 50, 'PDF pixels became grayscale')
            require(legacy_settings.read_bytes() == settings_bytes, 'grayscale toggle did not persist')
            if 'grayscale.pdf' in evidence:
                send(hwnd, 0x0111, 1106)
                require(menu_state(hwnd) & 0x0008 == 0, 'grayscale menu unchecked after second toggle')
                wait_for(lambda: red_pixels(hwnd, 'grayscale-off.png') > 1000,
                         'second toggle restores color PDF pixels')

        def non_pdf_check(hwnd):
            require(menu_state(hwnd) & 0x0003, 'grayscale menu disabled outside PDF view')
            send(hwnd, 0x0111, 1106)
            require(menu_state(hwnd) & 0x0008 == 0, 'non-PDF command cannot enable grayscale')

        run(md, non_pdf_check)
        run(grayscale_pdf, grayscale_check)
        run(grayscale_pdf, grayscale_check)
        require(hashlib.sha256(grayscale_pdf.read_bytes()).hexdigest() == original, 'original PDF preserved')
        require(legacy_settings.read_bytes() == settings_bytes, 'legacy settings preserved')
        report = {'scope': 'pdf-grayscale-only', 'passed': evidence, 'originals_unchanged': True,
                  'variant': args.variant, 'locale': locale,
                  'not_verified': ['remaining full native-smoke scenarios', 'audible silence', 'power loss'],
                  'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest()}
        (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(f'PASS (pdf-grayscale-only): {output}')
        return

    def search_overview(hwnd):
        from PIL import Image
        editor = u.GetDlgItem(hwnd, 104); search = u.GetDlgItem(hwnd, 810); band = u.GetDlgItem(hwnd, 105)
        require(band and u.IsWindowVisible(band), 'search overview displayed')
        send(hwnd, 0x0111, 107)  # Raw: controlled physical lines, including UTF-16 surrogate pairs.
        original = text(editor)
        def rows():
            image_path = output / 'search-overview-current.png'
            screenshot(band, image_path)
            with Image.open(image_path) as image:
                return {y for y in range(image.height) if image.getpixel((image.width // 2, y)) == (255, 165, 0)}
        def expected(query):
            native = text(editor).replace('\r\n', '\r')
            rect = wt.RECT(); require(u.GetClientRect(band, ct.byref(rect)), 'overview client size')
            height = rect.bottom - rect.top; total = send(editor, 0x00BA)
            found = set(); start = 0
            while True:
                index = native.casefold().find(query.casefold(), start)
                if index < 0: break
                offset = len(native[:index].encode('utf-16-le')) // 2
                end = len(native[:index + len(query)].encode('utf-16-le')) // 2
                first = send(editor, 0x0436, 0, offset); last = send(editor, 0x0436, 0, end - 1)
                for line in range(first, last + 1):
                    y = line * height // total
                    found.update(range(y, min(y + 2, height)))
                start = index + len(query)
            return found
        def check(query, label):
            set_text(search, query)
            time.sleep(0.2)  # Let the debounced scan begin, including the no-hit case.
            wanted = expected(query)
            wait_for(lambda: rows() == wanted, label)
            require(text(editor) == original, 'search overview preserves source text')
            screenshot(hwnd, output / f'search-overview-{label}.png')
        default = rows()
        require(default, 'Japanese markers exist before searching')
        selection = send(editor, 0x00B0); top = send(editor, 0x00CE)
        check('needle', 'all-hits')
        require(rows() != default, 'search hits replace Japanese-only rows')
        require(send(editor, 0x00B0) == selection and send(editor, 0x00CE) == top,
                'passive scan preserves selection and scroll position')
        check('日本語', 'unicode')
        check('not-in-document', 'no-hits')
        set_text(search, '')
        wait_for(lambda: rows() == default, 'clearing query restores Japanese markers')
        check('needle', 'before-navigation')
        send(hwnd, 0x0111, 811)
        selected = send(editor, 0x00B0)
        native_bytes = text(editor).replace('\r\n', '\r').encode('utf-16-le')
        require(native_bytes[(selected & 0xffff) * 2:(selected >> 16) * 2].decode('utf-16-le').casefold() == 'needle',
                'next result uses the same case-insensitive Unicode match')
        send(hwnd, 0x0111, 812)
        require(send(editor, 0x00B0) != selected, 'previous result navigates to another hit')
        require(rows() == expected('needle'), 'next/previous retain all search markers')
        # Supported local tab-transfer message; all windows and files belong to this test.
        class CopyData(ct.Structure):
            _fields_ = [('kind', ct.c_size_t), ('size', wt.DWORD), ('data', ct.c_void_p)]
        buffer = ct.create_unicode_buffer(str(md))
        incoming = CopyData(0x504E5254, ct.sizeof(buffer), ct.addressof(buffer))
        require(send(hwnd, 0x004A, hwnd, ct.addressof(incoming)), 'owned second document opened')
        wait_for(lambda: rows() == set(), 'tab switch removes old search hits')
        send(hwnd, 0x8000 + 4, 401, 0)
        wait_for(lambda: rows() == expected('needle'), 'returning tab restores correct search hits')
        send(hwnd, 0x0111, 106)  # Reading omits source-only Markdown punctuation.
        original = text(editor)
        check('**', 'reading-excludes-markup')
        send(hwnd, 0x0111, 107); original = text(editor)
        check('**', 'raw-includes-markup')
        check('needle', 'before-resize')
        require(u.SetWindowPos(hwnd, None, 0, 0, 1100, 760, 0x0016), 'resize owned reader')
        wait_for(lambda: rows() == expected('needle'), 'resize remaps wrapped hit lines')
        send(hwnd, 0x0111, 814)
        wait_for(lambda: rows() == expected('needle'), 'zoom remaps wrapped hit lines')
        # A dense query spans multiple timer turns. Supersede it before publication.
        set_text(search, 'dense'); time.sleep(0.15); set_text(search, 'needle')
        wait_for(lambda: rows() == expected('needle'), 'new query discards a pending dense scan')
        check('dense', 'dense-complete')
        band_rect = wt.RECT(); require(u.GetClientRect(band, ct.byref(band_rect)), 'click overview size')
        click_y = band_rect.bottom // 2
        target = click_y * send(editor, 0x00BA) // band_rect.bottom
        send(band, 0x0201, 1, (click_y << 16) | 2)
        require(abs(send(editor, 0x00CE) - target) <= 1, 'band click uses displayed wrapped line count')
        send(hwnd, 0x0111, 405)
        wait_for(lambda: not u.IsWindowVisible(band), 'closing tabs clears and hides the band')

    if args.search_overview_only or not (args.pdf_wheel_zoom_only or args.pdf_layout_only or args.pdf_selection_only):
        overview = output / 'search-overview.md'
        lines = [f'Line {i}: plain text.' for i in range(120)]
        lines[8] = '日本語だけの行。'
        lines[40] = '📖 Mixed-case NEEDLE hit.'
        lines[60] = '日本語と **needle** の行。'
        lines[100] = ('long wrapped paragraph ' * 80) + 'needle'
        overview.write_text('# Overview\n\n' + '\n'.join(lines) + '\n' +
                            '\n'.join(f'dense row {i}' for i in range(3000)), encoding='utf-8')
        originals[overview] = hashlib.sha256(overview.read_bytes()).hexdigest()
        run(overview, search_overview)
        if args.search_overview_only:
            for path, original_hash in originals.items():
                require(hashlib.sha256(path.read_bytes()).hexdigest() == original_hash, f'original preserved: {path.name}')
            report = {'scope': 'search-overview-only', 'passed': evidence, 'originals_unchanged': True,
                      'variant': args.variant, 'locale': locale,
                      'not_verified': ['remaining full native-smoke scenarios', 'OS DPI switching', 'audible silence'],
                      'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest()}
            (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
            print(f'PASS (search-overview-only): {output}')
            return

    def markdown(hwnd, expected_name=md.name):
        editor = u.GetDlgItem(hwnd, 104); search = u.GetDlgItem(hwnd, 810); status = u.GetDlgItem(hwnd, 817)
        require(send(editor, 0x00CE) == 0, 'new Markdown opens at the top')
        require('読むことに' in text(editor), 'rendered markdown body')
        toc = u.GetDlgItem(hwnd, 102)
        require(send(toc, 0x1105) == 3, f'markdown outline has all three headings: {send(toc, 0x1105)}')
        first_heading = send(toc, 0x110A, 0)
        second_heading = send(toc, 0x110A, 4, first_heading)
        require(second_heading != 0, 'outline child exists')
        send(toc, 0x110B, 9, second_heading)
        require(send(editor, 0x00B0) != 0, 'outline selection moves text caret')
        send(editor, 0x00B1, 0, 0)
        require(u.IsWindowEnabled(search), 'search available')
        set_text(search, '検索対象')
        send(hwnd, 0x0111, 811); first = send(editor, 0x00B0)
        require('一致する箇所' in text(status) or 'matching text' in text(status), 'Unicode search found')
        send(hwnd, 0x0111, 811); second = send(editor, 0x00B0)
        require(first != second, 'next match advances')
        send(hwnd, 0x0111, 812); require(send(editor, 0x00B0) == first, 'previous match returns')
        require(u.PostMessageW(search, 0x0100, 0x0D, 0), 'Enter message posted')
        wait_for(lambda: send(editor, 0x00B0) == second, 'search Enter handled by reader message loop')
        screenshot(hwnd, output / 'reading.png')
        require('EndOfLongCell' in text(editor), 'table retains full long cell')
        require('┌' not in text(editor) and '┼' not in text(editor), 'table uses native cells rather than padded box characters')
        require('click A callback' in text(editor), 'unsupported diagram remains readable source')
        require('{\\rtf1 inert}\\object\\objdata' in text(editor), 'RTF controls from source remain inert literal text')
        table_heading = send(toc, 0x110A, 1, second_heading)
        require(table_heading, 'table heading follows previous heading')
        send(toc, 0x110B, 9, table_heading)
        screenshot(hwnd, output / 'table-and-diagram.png')
        previews = []
        @enum_callback
        def preview_child(child, _):
            name = ct.create_unicode_buffer(128); u.GetClassNameW(child, name, 128)
            if name.value == 'PdfNoteMermaidSubsetPreview' and u.GetParent(child) == editor:
                previews.append(child)
            return True
        u.EnumChildWindows(editor, preview_child, 0)
        require(len(previews) == 1, 'only the supported diagram is embedded as a native document child')
        catalog = json.loads((root / 'locales' / f'{locale}.json').read_text(encoding='utf-8-sig'))
        set_text(search, catalog['readonly.render.diagram_label'].replace('{NUMBER}', '1'))
        send(hwnd, 0x0111, 811)
        # Scroll the label to the top so the complete inline viewport is observable.
        send(editor, 0x00B6, 0, 12)
        send(editor, 0x0115, 0, 0)
        require(u.IsWindowVisible(previews[0]), 'inline diagram visible in its reserved document space')
        diagram_rect = wt.RECT(); u.GetWindowRect(previews[0], ct.byref(diagram_rect))
        require(diagram_rect.bottom - diagram_rect.top > 150, 'diagram has readable reserved height')
        screenshot(hwnd, output / 'embedded-diagram.png')
        send(previews[0], 0x0205)
        detached = []
        @enum_callback
        def detached_window(window, _):
            owner = wt.DWORD(); u.GetWindowThreadProcessId(window, ct.byref(owner))
            parent_owner = wt.DWORD(); u.GetWindowThreadProcessId(hwnd, ct.byref(parent_owner))
            name = ct.create_unicode_buffer(128); u.GetClassNameW(window, name, 128)
            if owner.value == parent_owner.value and name.value == 'PdfNoteDetachedMermaidDiagram':
                detached.append(window)
            return True
        def find_detached():
            detached.clear(); u.EnumWindows(detached_window, 0)
            return bool(detached)
        wait_for(find_detached, 'right click opens owned diagram window')
        detached_panel = u.GetDlgItem(detached[0], 204)
        require(detached_panel and u.IsWindowVisible(detached_panel), 'detached diagram canvas is visible')
        screenshot(detached[0], output / 'detached-diagram.png')
        from PIL import Image
        with Image.open(output / 'detached-diagram.png') as capture:
            pixels = capture.convert('RGB').tobytes()
            blue_pixels = sum(b > r + 10 and b > green + 4 for r, green, b in
                              zip(pixels[0::3], pixels[1::3], pixels[2::3]))
        require(blue_pixels > 100, 'detached canvas actually paints diagram nodes')
        send(detached[0], 0x0010)
        require(u.GetPropW(hwnd, 'ReaderNextPreparationCount') == 1, 'body and headings prepared once')

        send(hwnd, 0x0111, 107); require(text(editor).startswith('# '), 'raw mode')
        send(hwnd, 0x0111, 108); require('00000000' in text(editor), 'hex mode')
        send(hwnd, 0x0111, 106)
        require(u.GetPropW(hwnd, 'ReaderNextPreparationCount') == 1, 'mode switches reuse prepared body and headings')
        send(hwnd, 0x0111, 813); require('110%' in text(status), 'text zoom')
        send(hwnd, 0x0111, 815); require('100%' in text(status), 'text zoom reset')
        before = text(editor); send(editor, 0x0102, ord('X')); require(text(editor) == before, 'attempted edit leaves source display unchanged')
        set_text(search, 'not-in-this-document'); send(hwnd, 0x0111, 811)
        require('ありません' in text(status) or 'No matching' in text(status), 'no-match status')
        send(hwnd, 0x0111, 116); require(not u.IsWindowVisible(u.GetDlgItem(hwnd, 101)), 'focus mode hides file tree')
        screenshot(hwnd, output / 'focus.png')
        send(hwnd, 0x0111, 116); require(u.IsWindowVisible(u.GetDlgItem(hwnd, 101)), 'sidebar restored')
        file_tree = u.GetDlgItem(hwnd, 101)
        root_item = send(file_tree, 0x110A, 0)
        parent_item = send(file_tree, 0x110A, 4, root_item)
        binary_item = send(file_tree, 0x110A, 1, parent_item)
        require(binary_item != 0, 'file tree contains binary input')
        send(file_tree, 0x110B, 9, binary_item)
        require(send(u.GetDlgItem(hwnd, 103), 0x1304) == 2, 'file tree opens a second tab')
        require('00000000' in text(editor), 'file tree selected binary content')
        send(hwnd, 0x8000 + 4, 401, 0)
        require('読むことに' in text(editor), 'select first tab restores markdown')
        send(hwnd, 0x8000 + 4, 403, 0)
        require(send(u.GetDlgItem(hwnd, 103), 0x1304) == 1, 'close other tabs retains selected content')
        require(u.GetPropW(hwnd, 'ReaderNextPreparationCount') == 1, 'tab navigation does not repeat preparation')
        require(u.SetWindowPos(hwnd, None, 0, 0, 930, 620, 0x0016), 'compact resize')
        def reflow_complete():
            rect = wt.RECT(); send(editor, 0x00B2, 0, ct.addressof(rect))
            return u.GetPropW(hwnd, 'ReaderNextReadingWidth') == rect.right - rect.left
        wait_for(reflow_complete, 'native table reflows to compact viewport')
        require(expected_name in text(hwnd), 'active filename follows tab selection')
        set_text(search, 'EndOfLongCell'); send(hwnd, 0x0111, 811)
        screenshot(hwnd, output / 'compact.png')
        for identity in [104, 810, 811, 812, 813, 814, 815, 101, 102, 817, 818]:
            rect = wt.RECT(); outer = wt.RECT()
            require(u.GetWindowRect(u.GetDlgItem(hwnd, identity), ct.byref(rect)), f'control geometry {identity}')
            require(u.GetWindowRect(hwnd, ct.byref(outer)), 'root geometry')
            require(rect.right <= outer.right and rect.bottom <= outer.bottom and rect.right > rect.left and rect.bottom > rect.top, f'compact bounds {identity}')
        require(send(u.GetDlgItem(hwnd, 103), 0x1304) == 1, 'one tab before close')
        send(hwnd, 0x0111, 405)
        require(not u.IsWindowVisible(editor), f'last tab closes to welcome: tabs={send(u.GetDlgItem(hwnd, 103), 0x1304)}, title={text(hwnd)}, body={text(editor)[:40]}')
        screenshot(hwnd, output / 'welcome.png')
        send(hwnd, 0x8000 + 4, 409, -1); require('読むことに' in text(editor), 'reopen closed tab')

    def pdf_check(hwnd):
        require(u.IsWindowVisible(u.GetDlgItem(hwnd, 1002)), 'PDF panel displayed')
        require(not u.IsWindowVisible(u.GetDlgItem(hwnd, 820)), 'diagram panel cannot cover PDF')
        require(u.IsWindowVisible(u.GetDlgItem(hwnd, 112)), 'PDF page range action visible')
        require(not u.IsWindowVisible(u.GetDlgItem(hwnd, 106)), 'PDF excludes text-mode actions')
        set_text(u.GetDlgItem(hwnd, 810), 'SearchNeedle'); send(hwnd, 0x0111, 811)
        status = u.GetDlgItem(hwnd, 817)
        wait_for(lambda: '2ページ' in text(status) or 'page 2' in text(status), 'PDF search traverses pages')
        send(hwnd, 0x0111, 811)
        wait_for(lambda: '3ページ' in text(status) or 'page 3' in text(status), 'next PDF occurrence')
        screenshot(hwnd, output / 'pdf.png')
        send(hwnd, 0x0111, 812)
        wait_for(lambda: '2ページ' in text(status) or 'page 2' in text(status), 'previous PDF occurrence')
        set_text(u.GetDlgItem(hwnd, 810), 'absent'); send(hwnd, 0x0111, 811)
        wait_for(lambda: 'ありません' in text(status) or 'No matching' in text(status), 'PDF no-match')
        for command in [813, 814, 815]: send(hwnd, 0x0111, command)

    def binary_check(hwnd):
        require('00000000' in text(u.GetDlgItem(hwnd, 104)), 'binary opens in hex')
        require(not u.IsWindowEnabled(u.GetDlgItem(hwnd, 106)), 'binary cannot read as markdown')

    def empty_check(hwnd):
        require(not u.IsWindowVisible(u.GetDlgItem(hwnd, 104)), 'empty reader hides text editor')
        require(not u.IsWindowEnabled(u.GetDlgItem(hwnd, 810)), 'empty search disabled')
        screenshot(hwnd, output / 'empty.png')

    def missing_check(hwnd):
        require(send(u.GetDlgItem(hwnd, 103), 0x1304) == 0, 'missing input does not create a tab')
        status = text(u.GetDlgItem(hwnd, 817))
        catalog = json.loads((root / 'locales' / f'{locale}.json').read_text(encoding='utf-8'))
        require(status in {catalog['readonly.file.unreadable'], catalog['readonly.file.local_only']},
                f'missing input keeps its localized visible failure: {status}')

    def folder_navigation(hwnd):
        tree = u.GetDlgItem(hwnd, 101)
        root_item = send(tree, 0x110A, 0); parent_item = send(tree, 0x110A, 4, root_item)
        directory_item = send(tree, 0x110A, 1, parent_item)
        require(directory_item, 'navigation chapter exists')
        send(tree, 0x110B, 9, directory_item)
        require(u.IsWindowEnabled(u.GetDlgItem(hwnd, 819)), 'back enabled after navigating')
        send(hwnd, 0x0111, 819)
        require(send(tree, 0x1105) == 4, 'back restores folder entries')
        send(hwnd, 0x0111, 821)
        require(send(tree, 0x1105) > 4, 'up navigates to parent folder')
        require('Navigation' in text(u.GetDlgItem(hwnd, 104)), 'folder navigation retains open document')
        screenshot(hwnd, output / 'folder-navigation.png')

    def drag_transfer(hwnd):
        startup = subprocess.STARTUPINFO(); startup.dwFlags = subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow = 4
        recipient = subprocess.Popen([str(exe), str(binary)], cwd=exe.parent, startupinfo=startup)
        target = None
        try:
            wait_for(lambda: find_window(recipient.pid), 'transfer recipient')
            target = find_window(recipient.pid)
            wait_for(lambda: u.IsWindowVisible(target) and '00000000' in text(u.GetDlgItem(target, 104)),
                     'recipient startup and displayed binary document complete')
            source_pid = wt.DWORD(); u.GetWindowThreadProcessId(hwnd, ct.byref(source_pid))
            u.ShowWindow(hwnd, 9); u.ShowWindow(target, 9)
            screen_right = u.GetSystemMetrics(76) + u.GetSystemMetrics(78)
            require(u.SetWindowPos(hwnd, wt.HWND(-1), min(960, screen_right - 930), 0, 930, 650, 0x0010), 'owned source transfer geometry')
            require(u.SetWindowPos(target, wt.HWND(-1), u.GetSystemMetrics(76), 0, 930, 650, 0x0010), 'owned recipient transfer geometry')
            tab = u.GetDlgItem(hwnd, 103)
            tab_rect = wt.RECT(); target_rect = wt.RECT()
            u.GetWindowRect(tab, ct.byref(tab_rect)); u.GetWindowRect(target, ct.byref(target_rect))
            drop = wt.POINT(target_rect.left + 24, target_rect.top + 350)
            x = drop.x - tab_rect.left; y = drop.y - tab_rect.top
            pack = lambda x, y: (x & 0xffff) | ((y & 0xffff) << 16)
            def drag():
                # Keep the owned recipient above unrelated windows before
                # observing a real screen point. This does not bypass the
                # application drag path or its target/receipt validation.
                require(u.SetWindowPos(target, wt.HWND(-1), 0, 0, 0, 0, 0x0013), 'recipient visible above unrelated windows')
                send(tab, 0x0201, 1, pack(60, 12)); send(tab, 0x0200, 1, pack(x, y))
                actual_target = u.GetAncestor(u.WindowFromPoint(drop), 2)
                if actual_target != target: send(tab, 0x001F)
                actual_class = ct.create_unicode_buffer(128)
                u.GetClassNameW(actual_target, actual_class, 128)
                actual_rect = wt.RECT(); source_rect = wt.RECT(); recipient_rect = wt.RECT()
                u.GetWindowRect(actual_target, ct.byref(actual_rect))
                u.GetWindowRect(hwnd, ct.byref(source_rect)); u.GetWindowRect(target, ct.byref(recipient_rect))
                coordinates = lambda r: (r.left, r.top, r.right, r.bottom)
                require(actual_target == target,
                        f'drag drop point belongs to owned recipient: expected={target}, actual={actual_target}, class={actual_class.value}, '
                        f'point={drop.x},{drop.y}, actual_rect={coordinates(actual_rect)}, source={coordinates(source_rect)}, recipient={coordinates(recipient_rect)}')
                send(tab, 0x0202, 0, pack(x, y))
            kernel = ct.WinDLL('kernel32', use_last_error=True)
            kernel.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ct.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE]
            kernel.CreateFileW.restype = wt.HANDLE; kernel.CloseHandle.argtypes = [wt.HANDLE]
            lock = kernel.CreateFileW(str(md), 0x80000000, 0, None, 3, 0, None)
            require(lock and lock != ct.c_void_p(-1).value, 'owned fixture read lock')
            try:
                drag()
                require(send(tab, 0x1304) == 1, 'recipient read failure retains source tab')
                require(send(u.GetDlgItem(target, 103), 0x1304) == 1, 'recipient read failure retains its previous tab')
                require('EndOfLongCell' in text(u.GetDlgItem(hwnd, 104)), 'recipient read failure retains source content')
            finally:
                kernel.CloseHandle(lock)
            # Add a second source tab through the same trusted local IPC used
            # by transfer, so a successful non-final move must stay open.
            class CopyData(ct.Structure):
                _fields_ = [('kind', ct.c_size_t), ('size', wt.DWORD), ('data', ct.c_void_p)]
            buffer = ct.create_unicode_buffer(str(binary))
            incoming = CopyData(0x504E5254, ct.sizeof(buffer), ct.addressof(buffer))
            require(send(hwnd, 0x004A, target, ct.addressof(incoming)) == 1, 'second source tab opens')
            require(send(tab, 0x1304) == 2, 'two source tabs before first transfer')
            drag()
            require(find_window(source_pid.value) == hwnd and send(tab, 0x1304) == 1,
                    'successful non-final transfer retains the source instance')
            # Rechecking the queued close request must preserve a nonempty
            # instance even if an earlier request was queued when it was empty.
            send(hwnd, 0x8000 + 5)
            require(send(tab, 0x1304) == 1, 'queued close rechecks remaining tabs')
            require(send(u.GetDlgItem(target, 103), 0x1304) == 2, 'recipient receives first transferred document')
            drag()
            wait_for(lambda: not find_window(source_pid.value), 'last-tab transfer closes the source window after acceptance')
            require(send(u.GetDlgItem(target, 103), 0x1304) == 2, 'recipient receives detached document')
            send(target, 0x8000 + 4, 401, 1)
            require('EndOfLongCell' in text(u.GetDlgItem(target, 104)), 'transferred content retained')
            # A tab click with no drag must not spawn or move anything.
            target_tab = u.GetDlgItem(target, 103)
            send(target_tab, 0x0201, 1, pack(60, 12)); send(target_tab, 0x0202, 0, pack(60, 12))
            require(send(target_tab, 0x1304) == 2, 'click is not treated as a drag')
            screenshot(target, output / 'tab-transfer.png')
        finally:
            if target: u.PostMessageW(target, 0x0111, 0xF060, 0)
            try: recipient.wait(timeout=10)
            except subprocess.TimeoutExpired:
                recipient.terminate(); recipient.wait(timeout=5); raise RuntimeError('Owned transfer recipient did not close')
            require(recipient.returncode == 0, 'recipient exits normally')

    def source_reload(hwnd):
        editor = u.GetDlgItem(hwnd, 104); tree = u.GetDlgItem(hwnd, 101)
        require('Initial heading' in text(editor), 'initial reload fixture shown')
        require(u.GetPropW(hwnd, 'ReaderNextPreparationCount') == 1, 'initial reload preparation')
        # This test, as an external writer, changes only its freshly created fixture.
        changed = '# Updated heading\n\nReloaded body. 日本語 📖.\n\n## Updated section\n\nNew section body.\n'
        reload_file.write_text(changed.replace('\n', reload_newline), encoding='utf-8', newline='')
        originals[reload_file] = hashlib.sha256(reload_file.read_bytes()).hexdigest()
        root_item = send(tree, 0x110A, 0)
        parent_item = send(tree, 0x110A, 4, root_item)
        file_item = send(tree, 0x110A, 1, parent_item)
        require(file_item, 'reload fixture in navigation tree')
        send(tree, 0x110B, 9, root_item)
        pid = wt.DWORD(); u.GetWindowThreadProcessId(hwnd, ct.byref(pid))
        def changed_dialog():
            found = []
            @enum_callback
            def visit(candidate, _):
                owner = wt.DWORD(); u.GetWindowThreadProcessId(candidate, ct.byref(owner))
                name = ct.create_unicode_buffer(128); u.GetClassNameW(candidate, name, 128)
                if owner.value == pid.value and name.value == 'PdfNoteReadonlyFileChangedDialog': found.append(candidate)
                return True
            u.EnumWindows(visit, 0)
            return found[0] if found else None
        errors = []
        def select():
            try: send(tree, 0x110B, 9, file_item)
            except Exception as error: errors.append(error)
        worker = threading.Thread(target=select, daemon=True); worker.start()
        try:
            wait_for(changed_dialog, 'existing external-change confirmation')
            send(changed_dialog(), 0x0111, 622)
        finally:
            if worker.is_alive() and changed_dialog(): u.PostMessageW(changed_dialog(), 0x0010, 0, 0)
            worker.join(timeout=6)
        require(not worker.is_alive() and not errors, f'reload selection completes: {errors}')
        require('Updated heading' in text(editor) and 'Initial heading' not in text(editor), 'external reload replaces cached document')
        require(send(u.GetDlgItem(hwnd, 102), 0x1105) == 2, 'external reload replaces cached headings')
        require(u.GetPropW(hwnd, 'ReaderNextPreparationCount') == 2, 'external reload prepares exactly one new revision')
        send(hwnd, 0x0111, 107)
        require(text(editor).replace('\r\n', '\n') == changed, 'raw reload preserves complete Unicode source')
        toc = u.GetDlgItem(hwnd, 102); first = send(toc, 0x110A, 0); second = send(toc, 0x110A, 4, first)
        send(toc, 0x110B, 9, second)
        actual = send(editor, 0x00B0) & 0xffff
        expected = send(editor, 0x00BB, 4)
        require(actual == expected, f'raw outline reaches native heading line: actual={actual}, expected={expected}, text={text(editor)!r}')
        send(hwnd, 0x0111, 106)
        require(u.GetPropW(hwnd, 'ReaderNextPreparationCount') == 2, 'reloaded revision is reused')
        screenshot(hwnd, output / 'source-reload.png')

    def detach_new_window(hwnd):
        owner_pid = wt.DWORD(); u.GetWindowThreadProcessId(hwnd, ct.byref(owner_pid))
        kernel = ct.WinDLL('kernel32', use_last_error=True)
        class ProcessEntry(ct.Structure):
            _fields_ = [('size', wt.DWORD), ('usage', wt.DWORD), ('pid', wt.DWORD), ('heap', ct.c_size_t),
                        ('module', wt.DWORD), ('threads', wt.DWORD), ('parent', wt.DWORD),
                        ('priority', wt.LONG), ('flags', wt.DWORD), ('exe', wt.WCHAR * 260)]
        kernel.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
        kernel.CreateToolhelp32Snapshot.restype = wt.HANDLE
        kernel.Process32FirstW.argtypes = [wt.HANDLE, ct.POINTER(ProcessEntry)]
        kernel.Process32NextW.argtypes = [wt.HANDLE, ct.POINTER(ProcessEntry)]
        kernel.CloseHandle.argtypes = [wt.HANDLE]
        kernel.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]; kernel.OpenProcess.restype = wt.HANDLE
        kernel.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
        kernel.GetExitCodeProcess.argtypes = [wt.HANDLE, ct.POINTER(wt.DWORD)]
        def children():
            snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
            require(snapshot and snapshot != ct.c_void_p(-1).value, 'process ownership snapshot')
            owned = []
            try:
                entry = ProcessEntry(); entry.size = ct.sizeof(entry)
                ok = kernel.Process32FirstW(snapshot, ct.byref(entry))
                while ok:
                    if entry.parent == owner_pid.value and entry.exe.lower() == exe.name.lower():
                        owned.append(entry.pid)
                    ok = kernel.Process32NextW(snapshot, ct.byref(entry))
            finally: kernel.CloseHandle(snapshot)
            return owned
        child_pids = []
        try:
            tab = u.GetDlgItem(hwnd, 103); rect = wt.RECT(); u.GetWindowRect(tab, ct.byref(rect))
            pack = lambda x, y: (x & 0xffff) | ((y & 0xffff) << 16)
            x, y = -50 - rect.left, -50 - rect.top
            send(tab, 0x0201, 1, pack(60, 12)); send(tab, 0x0200, 1, pack(x, y)); send(tab, 0x0202, 0, pack(x, y))
            wait_for(lambda: children(), 'owned detached viewer process')
            child_pids = children(); require(len(child_pids) == 1, 'one child viewer for one drag')
            wait_for(lambda: find_window(child_pids[0]), 'detached viewer window')
            child = find_window(child_pids[0])
            wait_for(lambda: u.GetDlgItem(child, 104) and 'EndOfLongCell' in text(u.GetDlgItem(child, 104)), 'detached body loaded')
            wait_for(lambda: not find_window(owner_pid.value), 'last-tab detach closes source after acknowledgement')
            require(send(u.GetDlgItem(child, 103), 0x1304) == 1, 'detached viewer owns the tab')
            screenshot(child, output / 'detached-window.png')
        finally:
            for pid in set(child_pids + children()):
                process_handle = kernel.OpenProcess(0x100000 | 0x1000, False, pid)
                require(process_handle, 'owned detached process remains observable')
                try:
                    child = find_window(pid)
                    if child: u.PostMessageW(child, 0x0111, 0xF060, 0)
                    require(kernel.WaitForSingleObject(process_handle, 6000) == 0, 'owned detached process exits')
                    exit_code = wt.DWORD()
                    require(kernel.GetExitCodeProcess(process_handle, ct.byref(exit_code)) and exit_code.value == 0,
                            'owned detached process exits normally')
                finally: kernel.CloseHandle(process_handle)

    view_fixtures = output.parent / (output.name + '-views')
    view_fixtures.mkdir()
    position_file = view_fixtures / 'positions.md'
    position_file.write_text('# Position start\n\n| A | B |\n| --- | --- |\n| one | two |\n\n' +
        ''.join(f'## Heading {i}\n\n' + (f'Paragraph {i} with wrapped text 日本語。' * 12) + '\n\n'
                for i in range(100)), encoding='utf-8')
    alternate_file = view_fixtures / 'alternate.txt'
    alternate_file.write_text('Alternate document\n' + 'Separate reading position\n' * 250, encoding='utf-8')
    wide_diagram = view_fixtures / 'wide.md'
    wide_diagram.write_text('# Wide diagram\n\n```mermaid\nflowchart LR\n' +
        '\n'.join(f'N{i}[Wide node {i}] --> N{i+1}[Wide node {i+1}]' for i in range(12)) +
        '\n```\n\n```mermaid\nflowchart TB\nX[Only this second diagram] --> Y[Second end]\n```\n' +
        'Document continues below the diagrams.\n' * 100, encoding='utf-8')
    view_pdf = view_fixtures / 'view.pdf'
    shutil.copyfile(pdf, view_pdf)
    for fixture in (position_file, alternate_file, wide_diagram, view_pdf):
        originals[fixture] = hashlib.sha256(fixture.read_bytes()).hexdigest()

    def open_from_list(hwnd, path):
        tree = u.GetDlgItem(hwnd, 101)
        item = send(tree, 0x110A, 4, send(tree, 0x110A, 0))
        # This isolated folder contains exactly the four ASCII-named fixtures.
        # Use scalar tree messages; WM_USER pointer payloads are not marshalled
        # across processes and must never point into this Python process.
        names = sorted(p.name for p in view_fixtures.iterdir())
        require(len(names) == 4 and send(tree, 0x1105) == 6, 'fixed isolated file-list inputs')
        for _ in range(names.index(path.name) + 1):
            item = send(tree, 0x110A, 1, item)
        require(item, 'expected fixture exists in native file list')
        send(tree, 0x110B, 9, item)
        require(path.name in text(hwnd), 'native file selection opens expected document')
    def scroll_position(editor):
        return send(editor, 0x00CE)
    def reading_positions(hwnd):
        from PIL import Image
        editor = u.GetDlgItem(hwnd, 104)
        left = u.GetSystemMetrics(76)
        x = max(left, min(960, left + u.GetSystemMetrics(78) - 930))
        require(u.SetWindowPos(hwnd, wt.HWND(-1), x, 0, 930, 620, 0), 'place owned reader for presented-pixel inspection')
        time.sleep(0.2)
        def presented(name):
            # Allow normal queued paint/composition, but never request a repaint
            # or scroll before observing the restored frame.
            time.sleep(0.2)
            path = output / name
            screenshot(editor, path, onscreen=True)
            with Image.open(path) as capture:
                crop = capture.crop((24, 16, capture.width - 24, capture.height - 24))
                return hashlib.sha256(crop.tobytes()).hexdigest()
        require(send(editor, 0x00CE) == 0, 'long document initially starts at the top')
        send(editor, 0x00B6, 0, 75)
        reading_scroll = scroll_position(editor)
        require(reading_scroll > 0, 'native reading scroll changed')
        # Establish a fully painted reference before leaving the source tab.
        # The returned tab itself must paint correctly without this assistance.
        u.RedrawWindow.argtypes = [wt.HWND, ct.c_void_p, wt.HANDLE, wt.UINT]
        require(u.RedrawWindow(editor, None, None, 0x0185), 'paint reference reading viewport')
        moved = presented('reading-presented-moved.png')
        reading_scroll = scroll_position(editor)
        open_from_list(hwnd, alternate_file)
        require(send(editor, 0x00CE) == 0, 'different new file does not inherit old scroll')
        send(editor, 0x00B6, 0, 40); alternate_scroll = scroll_position(editor)
        send(hwnd, 0x8000 + 4, 401, 0)
        require(scroll_position(editor) == reading_scroll,
                f'tab returns to its reading scroll: expected={reading_scroll}, actual={scroll_position(editor)}')
        returned = presented('reading-presented-return.png')
        require(returned == moved, 'tab immediately presents the restored text without further scrolling')
        require(scroll_position(editor) == reading_scroll, 'normal painting does not move the restored viewport')
        for index in range(3):
            send(hwnd, 0x8000 + 4, 401, 1)
            send(hwnd, 0x8000 + 4, 401, 0)
            require(presented(f'reading-presented-repeat-{index}.png') == moved,
                    'repeated tab switches present the saved viewport without a scroll event')
        send(hwnd, 0x0111, 107)
        require(send(editor, 0x00CE) == 0, 'first raw view starts at the top')
        send(editor, 0x00B6, 0, 50); raw_scroll = scroll_position(editor)
        send(hwnd, 0x0111, 106)
        require(scroll_position(editor) == reading_scroll, 'reading mode retains its independent position')
        send(hwnd, 0x0111, 107)
        require(scroll_position(editor) == raw_scroll, 'raw mode retains its independent position')
        send(hwnd, 0x0111, 106)
        anchor = send(editor, 0x00BB, send(editor, 0x00CE))
        require(u.SetWindowPos(hwnd, None, 0, 0, 1100, 700, 0x0016), 'resize owned reading window')
        def wrapped_at_new_width():
            rect = wt.RECT(); send(editor, 0x00B2, 0, ct.addressof(rect))
            return u.GetPropW(hwnd, 'ReaderNextReadingWidth') == rect.right - rect.left
        wait_for(wrapped_at_new_width, 'reading reflow finishes')
        first = send(editor, 0x00CE)
        first_character = send(editor, 0x00BB, first)
        next_character = send(editor, 0x00BB, first + 1)
        require(first_character <= anchor < next_character,
                f'resize keeps the anchor text in the first visible line: {first_character}, {anchor}, {next_character}')
        send(hwnd, 0x8000 + 4, 401, 1)
        require(scroll_position(editor) == alternate_scroll, 'other tab keeps its independent position')
        screenshot(hwnd, output / 'reading-position-return.png')
    def panel_capture(hwnd, panel, name):
        from PIL import Image
        path = output / name; screenshot(hwnd, path)
        root_rect = wt.RECT(); panel_rect = wt.RECT()
        require(u.GetWindowRect(hwnd, ct.byref(root_rect)) and u.GetWindowRect(panel, ct.byref(panel_rect)), 'PDF crop geometry')
        with Image.open(path) as capture:
            crop = capture.crop((panel_rect.left-root_rect.left, panel_rect.top-root_rect.top,
                                 panel_rect.right-root_rect.left, panel_rect.bottom-root_rect.top))
            return hashlib.sha256(crop.tobytes()).hexdigest()
    def pdf_positions(hwnd):
        panel = u.GetDlgItem(hwnd, 1002)
        original = panel_capture(hwnd, panel, 'pdf-position-top.png')
        send(panel, 0x0100, 0x22)  # Page Down through the native PDF panel.
        send(hwnd, 0x0111, 813)
        moved = panel_capture(hwnd, panel, 'pdf-position-moved.png')
        require(original != moved, 'PDF scroll and zoom visibly change the document')
        open_from_list(hwnd, alternate_file)
        send(hwnd, 0x8000 + 4, 401, 0)
        returned = panel_capture(hwnd, panel, 'pdf-position-return.png')
        require(moved == returned, 'PDF tab restores the same visible content and zoom')

    zoom_pdf = output / 'wheel-zoom.pdf'
    document = fitz.open()
    for index, (width, height) in enumerate(((595, 842), (842, 595), (595, 842))):
        page = document.new_page(width=width, height=height)
        page.insert_text((72, 90), f'Wheel zoom page {index + 1}')
        page.insert_text((width * 0.72 + 24, height * 0.70), f'LayoutAnchorPage{index + 1}')
        # Isolated PDF drawing fixtures, never distributed visual assets.
        page.draw_circle((width * 0.72, height * 0.70), 16,
                         color=(1, 0, 0), fill=(1, 0, 0))
    document.save(zoom_pdf); document.close()
    originals[zoom_pdf] = hashlib.sha256(zoom_pdf.read_bytes()).hexdigest()

    def pdf_marker(hwnd, name):
        from PIL import Image, ImageChops
        panel = u.GetDlgItem(hwnd, 1002)
        path = output / name; screenshot(hwnd, path)
        outer = wt.RECT(); rect = wt.RECT()
        require(u.GetWindowRect(hwnd, ct.byref(outer)) and u.GetWindowRect(panel, ct.byref(rect)),
                'PDF panel geometry')
        with Image.open(path) as capture:
            crop = capture.crop((rect.left - outer.left, rect.top - outer.top,
                                 rect.right - outer.left, rect.bottom - outer.top))
            r, green, b = crop.split()
            mask = ImageChops.multiply(r.point(lambda v: 255 if v > 200 else 0),
                ImageChops.multiply(green.point(lambda v: 255 if v < 50 else 0),
                                    b.point(lambda v: 255 if v < 50 else 0)))
            bounds = mask.getbbox()
            require(bounds, 'PDF marker remains visible during the requested operation')
            require(bounds[0] > 0 and bounds[1] > 0 and bounds[2] < crop.width and bounds[3] < crop.height,
                    'entire PDF marker is visible; clipped geometry is not valid position evidence')
            return ((bounds[0] + bounds[2] - 1) / 2, (bounds[1] + bounds[3] - 1) / 2,
                    bounds[2] - bounds[0], bounds[3] - bounds[1])

    def changed_pdf_marker(hwnd, name, before, operation):
        deadline = time.monotonic() + 15
        after = before
        while time.monotonic() < deadline:
            after = pdf_marker(hwnd, name)
            if after != before:
                return after
            time.sleep(0.05)
        raise AssertionError(f'{operation} did not change the observed PDF marker: {before}, {after}')

    def pdf_wheel_zoom(hwnd):
        panel = u.GetDlgItem(hwnd, 1002)
        send(panel, 0x0100, 0x22)  # Start partway down the portrait page.
        marker = lambda name: pdf_marker(hwnd, name)
        def wheel(delta, flags, position):
            rect = wt.RECT(); require(u.GetWindowRect(panel, ct.byref(rect)), 'wheel target screen geometry')
            # WM_MOUSEWHEEL carries signed screen coordinates; do not move the
            # user's real cursor or pass local pointers into the reader process.
            point = ((rect.left + round(position[0])) & 0xffff) | (((rect.top + round(position[1])) & 0xffff) << 16)
            send(panel, 0x020A, ((delta & 0xffff) << 16) | flags, point)
        original = marker('wheel-zoom-before.png')
        for index, (delta, flags) in enumerate(((120, 8), (-120, 8), (120, 0), (-120, 0),
                                               (240, 8), (-240, 8), (30, 8), (-30, 8))):
            if index == 2:
                send(panel, 0x0111, 1108)  # Pan tool enables conventional wheel zoom.
            before = marker(f'wheel-zoom-{index}-before.png')
            wheel(delta, flags, before)
            after = changed_pdf_marker(hwnd, f'wheel-zoom-{index}-after.png', before, 'wheel zoom')
            require(abs(before[0] - after[0]) <= 2 and abs(before[1] - after[1]) <= 2,
                    f'wheel zoom keeps the PDF point under the cursor: before={before}, after={after}')
            require((after[2] > before[2]) if delta > 0 else (after[2] < before[2]),
                    f'wheel changes the rendered scale in the requested direction: {before}, {after}')
        returned = marker('wheel-zoom-return.png')
        require(abs(returned[0] - original[0]) <= 2 and abs(returned[1] - original[1]) <= 2,
                'zooming in and out does not jump to the page top')
        canvas_anchor = (8, returned[1])  # Blank canvas to the left of every PDF page.
        wheel(120, 8, canvas_anchor)
        on_canvas = marker('wheel-zoom-canvas.png')
        expected_x = canvas_anchor[0] + (returned[0] - canvas_anchor[0]) * 1.15
        require(abs(on_canvas[0] - expected_x) <= 2 and abs(on_canvas[1] - returned[1]) <= 2,
                'blank-canvas zoom preserves the cursor anchor instead of jumping to the page top')
        wheel(-120, 8, canvas_anchor)
        returned = marker('wheel-zoom-canvas-return.png')
        require(abs(returned[0] - original[0]) <= 2 and abs(returned[1] - original[1]) <= 2,
                'blank-canvas zoom round trip keeps the document position')
        wheel(-30, 0, returned)  # Precise touchpad input still scrolls.
        scrolled = marker('wheel-zoom-precise-scroll.png')
        require(abs(scrolled[1] - returned[1]) > 5 and scrolled[2] == returned[2],
                'precise wheel without Ctrl scrolls without changing scale')

    def pdf_layout(hwnd):
        panel = u.GetDlgItem(hwnd, 1002)
        send(panel, 0x0100, 0x22)
        def center():
            rect = wt.RECT(); require(u.GetClientRect(panel, ct.byref(rect)), 'PDF viewport size')
            return (rect.right / 2, (rect.bottom + 26) / 2)
        def stable(before, old_center, after, label):
            now = center()
            expected = (before[0] + now[0] - old_center[0], before[1] + now[1] - old_center[1])
            require(abs(after[0] - expected[0]) <= 2 and abs(after[1] - expected[1]) <= 2,
                    f'{label} preserves the document point at the viewport center: {before}, {after}, {expected}')
            require(abs(after[2] - before[2]) <= 1 and abs(after[3] - before[3]) <= 1,
                    f'{label} preserves the physical reading scale: {before}, {after}')
        for index, (width, height) in enumerate(((930, 650), (1100, 720), (1000, 800), (1200, 700))):
            before = pdf_marker(hwnd, f'pdf-layout-{index}-before.png'); old_center = center()
            require(u.SetWindowPos(hwnd, None, 0, 0, width, height, 0x0016), 'resize owned PDF reader')
            stable(before, old_center, pdf_marker(hwnd, f'pdf-layout-{index}-after.png'), 'window resize')
        for index in range(4):
            before = pdf_marker(hwnd, f'pdf-pane-{index}-before.png'); old_center = center()
            send(hwnd, 0x0111, 116)
            stable(before, old_center, pdf_marker(hwnd, f'pdf-pane-{index}-after.png'), 'left pane toggle')
        # Move the actual left/right splitter through the reader's mouse path.
        rect = wt.RECT(); origin = wt.POINT()
        require(u.GetWindowRect(panel, ct.byref(rect)), 'splitter-adjacent PDF geometry')
        u.ClientToScreen.argtypes = [wt.HWND, ct.POINTER(wt.POINT)]
        require(u.ClientToScreen(hwnd, ct.byref(origin)), 'reader client screen origin')
        dpi = u.GetDpiForWindow(hwnd); require(dpi > 0, 'reader DPI is observable')
        split = rect.left - origin.x - (12 * dpi + 48) // 96
        before = pdf_marker(hwnd, 'pdf-split-before.png'); old_center = center()
        pack = lambda x, y: (x & 0xffff) | ((y & 0xffff) << 16)
        send(hwnd, 0x0201, 1, pack(split + 1, 300))
        send(hwnd, 0x0200, 1, pack(split + 70, 300))
        send(hwnd, 0x0202, 0, pack(split + 70, 300))
        require(center() != old_center, 'splitter operation actually changes the PDF viewport')
        stable(before, old_center,
               changed_pdf_marker(hwnd, 'pdf-split-after.png', before, 'splitter movement'),
               'splitter movement')
        # Resize while the PDF is hidden by a text tab, then restore its logical
        # document point rather than replaying obsolete viewport pixel offsets.
        before = pdf_marker(hwnd, 'pdf-hidden-before.png'); old_center = center()
        open_from_list(hwnd, alternate_file)
        require(u.SetWindowPos(hwnd, None, 0, 0, 980, 680, 0x0016), 'resize with a text tab selected')
        send(hwnd, 0x8000 + 4, 401, 0)
        stable(before, old_center, pdf_marker(hwnd, 'pdf-hidden-return.png'), 'PDF tab after hidden resize')
        class CopyData(ct.Structure):
            _fields_ = [('kind', ct.c_size_t), ('size', wt.DWORD), ('data', ct.c_void_p)]
        before = pdf_marker(hwnd, 'pdf-second-tab-before.png'); old_center = center()
        buffer = ct.create_unicode_buffer(str(zoom_pdf))
        incoming = CopyData(0x504E5254, ct.sizeof(buffer), ct.addressof(buffer))
        require(send(hwnd, 0x004A, hwnd, ct.addressof(incoming)) == 1, 'trusted local IPC opens second PDF tab')
        require(u.SetWindowPos(hwnd, None, 0, 0, 1150, 740, 0x0016), 'resize while another PDF is loaded')
        send(hwnd, 0x8000 + 4, 401, 0)
        stable(before, old_center, pdf_marker(hwnd, 'pdf-second-tab-return.png'), 'PDF replacement and tab return')
        set_text(u.GetDlgItem(hwnd, 810), 'LayoutAnchorPage2'); send(hwnd, 0x0111, 811)
        wait_for(lambda: '2ページ' in text(u.GetDlgItem(hwnd, 817)) or 'page 2' in text(u.GetDlgItem(hwnd, 817)),
                 'layout fixture searches to the landscape page')
        # A wide page at the retained reading scale extends beyond the viewport.
        # Navigate intentionally so the complete marker stays visible at both sizes.
        send(panel, 0x020A, (360 << 16) | 4)
        before = pdf_marker(hwnd, 'pdf-landscape-before.png'); old_center = center()
        require(u.SetWindowPos(hwnd, None, 0, 0, 1040, 820, 0x0016), 'resize landscape page viewport')
        stable(before, old_center, pdf_marker(hwnd, 'pdf-landscape-after.png'), 'landscape page resize')
        before = pdf_marker(hwnd, 'pdf-minimize-before.png'); old_center = center()
        u.ShowWindow(hwnd, 6); u.ShowWindow(hwnd, 9)
        stable(before, old_center, pdf_marker(hwnd, 'pdf-minimize-return.png'), 'minimize and restore')

    shutil.copyfile(zoom_pdf, view_pdf)
    originals[view_pdf] = hashlib.sha256(view_pdf.read_bytes()).hexdigest()

    def diagram_navigation(hwnd):
        editor = u.GetDlgItem(hwnd, 104)
        inline = []
        @enum_callback
        def inline_child(child, _):
            name = ct.create_unicode_buffer(128); u.GetClassNameW(child, name, 128)
            if name.value == 'PdfNoteMermaidSubsetPreview' and u.GetParent(child) == editor:
                inline.append(child)
            return True
        u.EnumChildWindows(editor, inline_child, 0)
        require(len(inline) == 2, 'both inline diagrams have native viewports')
        catalog = json.loads((root / 'locales' / f'{locale}.json').read_text(encoding='utf-8-sig'))
        detached_pictures = []
        for index, preview in enumerate(inline):
            set_text(u.GetDlgItem(hwnd, 810), catalog['readonly.render.diagram_label'].replace('{NUMBER}', str(index + 1)))
            send(hwnd, 0x0111, 811)
            send(editor, 0x0115, 8)
            require(u.IsWindowVisible(preview), 'selected inline diagram is on screen')
            send(preview, 0x0205)
            found = []
            @enum_callback
            def owned_diagram(window, _):
                owner = wt.DWORD(); u.GetWindowThreadProcessId(window, ct.byref(owner))
                root_owner = wt.DWORD(); u.GetWindowThreadProcessId(hwnd, ct.byref(root_owner))
                name = ct.create_unicode_buffer(128); u.GetClassNameW(window, name, 128)
                if owner.value == root_owner.value and name.value == 'PdfNoteDetachedMermaidDiagram': found.append(window)
                return True
            def visible_diagram():
                found.clear(); u.EnumWindows(owned_diagram, 0)
                return found and u.IsWindowVisible(found[0])
            wait_for(visible_diagram, 'selected diagram opens a visible owned window')
            canvas = u.GetDlgItem(found[0], 204)
            require(u.IsWindowVisible(canvas), 'selected diagram canvas visible')
            detached_pictures.append(panel_capture(found[0], canvas, f'selected-diagram-{index+1}.png'))
            send(found[0], 0x0111, 203)
            send(found[0], 0x0111, 205)
            send(found[0], 0x0010)
        require(detached_pictures[0] != detached_pictures[1], 'right click displays the selected diagram, not the same combined list')
        old_line = scroll_position(editor)
        send(inline[1], 0x020A, ((-120 & 0xFFFF) << 16))
        require(scroll_position(editor) > old_line, 'wheel over inline diagram scrolls the document')
        send(hwnd, 0x0111, 111)
        panel = u.GetDlgItem(hwnd, 820)
        require(panel and u.IsWindowVisible(panel), 'diagram list visible')
        class ScrollInfo(ct.Structure):
            _fields_ = [('size', wt.UINT), ('mask', wt.UINT), ('minimum', ct.c_int), ('maximum', ct.c_int),
                        ('page', wt.UINT), ('position', ct.c_int), ('track', ct.c_int)]
        u.GetScrollInfo.argtypes = [wt.HWND, ct.c_int, ct.POINTER(ScrollInfo)]
        def info():
            send(panel, 0x000F)
            state = ScrollInfo(); state.size = ct.sizeof(state); state.mask = 0x17
            require(u.GetScrollInfo(panel, 0, ct.byref(state)), 'diagram scroll range is observable')
            return state
        for _ in range(9): send(hwnd, 0x0111, 813)  # Native toolbar zooms the diagram, not the hidden text.
        state = info()
        require(state.maximum + 1 > state.page, 'large diagram has a real horizontal scroll range')
        send(panel, 0x0114, 3)  # Page right.
        require(info().position > 0, 'diagram page scrolling moves horizontally')
        send(panel, 0x0114, 7)
        end = info().position
        require(end == info().maximum - info().page + 1, 'diagram can reach its right edge')
        open_from_list(hwnd, alternate_file)
        send(hwnd, 0x8000 + 4, 401, 0)
        require(info().position == end, 'diagram tab retains scroll and zoom in memory')
        send(panel, 0x0114, 6)
        require(info().position == 0, 'diagram can return to its left edge')
        screenshot(hwnd, output / 'large-diagram-navigation.png')

    selection_pdf = output / 'selection-ui.pdf'
    document = fitz.open(); page = document.new_page(width=595, height=842)
    page.insert_text((100, 110), 'ALPHA BETA GAMMA', fontsize=16)
    page.insert_text((100, 150), 'SECOND LINE', fontsize=16)
    document.save(selection_pdf); document.close()
    originals[selection_pdf] = hashlib.sha256(selection_pdf.read_bytes()).hexdigest()
    def pdf_selection(hwnd):
        panel = u.GetDlgItem(hwnd, 1002)
        rect = wt.RECT(); require(u.GetClientRect(panel, ct.byref(rect)), 'selection viewport geometry')
        scale = (rect.right - 96) / 595
        def point(x, y): return (round(48 + x * scale), round(58 + (842-y) * scale))
        def drag(a, b):
            pack = lambda p: (p[0] & 0xffff) | ((p[1] & 0xffff) << 16)
            send(panel, 0x0201, 1, pack(a)); send(panel, 0x0200, 1, pack(b)); send(panel, 0x0202, 0, pack(b))
        drag(point(95, 755), point(290, 670))
        frame = panel_capture(hwnd, panel, 'pdf-selection-frame.png')
        # Points on the B and final A of BETA, obtained independently from the fixture.
        with fitz.open(selection_pdf) as source:
            chars = [char for block in source[0].get_text('rawdict')['blocks'] if 'lines' in block
                     for line in block['lines'] for span in line['spans'] for char in span['chars']]
            start = next(i for i, char in enumerate(chars) if char['c'] == 'B')
            first, last = chars[start]['bbox'], chars[start+3]['bbox']
        a = point(first[0] + (first[2]-first[0])*0.15, 842-(first[1]+first[3])/2)
        b = point(last[2] - (last[2]-last[0])*0.15, 842-(last[1]+last[3])/2)
        drag(a, b)
        selected = panel_capture(hwnd, panel, 'pdf-selection-characters.png')
        require(selected != frame, 'second-stage character selection changes the visible highlight')
        send(panel, 0x0100, 0x1b)
        require(panel_capture(hwnd, panel, 'pdf-selection-clear-text.png') == frame,
                'first Escape restores the original frame without removing it')
        send(panel, 0x0100, 0x1b)
        require(panel_capture(hwnd, panel, 'pdf-selection-clear-frame.png') != frame, 'second Escape removes the frame')
    if args.pdf_selection_only: run(selection_pdf, pdf_selection)
    else: run(zoom_pdf, pdf_wheel_zoom)
    if not args.pdf_wheel_zoom_only and not args.pdf_selection_only:
        run(view_pdf, pdf_layout)
    if args.pdf_wheel_zoom_only or args.pdf_layout_only or args.pdf_selection_only:
        if args.pdf_layout_only: run(view_pdf, pdf_positions)
        for path, original in originals.items():
            require(hashlib.sha256(path.read_bytes()).hexdigest() == original, f'original preserved: {path.name}')
        selected_scope = 'pdf-selection-only' if args.pdf_selection_only else ('pdf-layout-only' if args.pdf_layout_only else 'pdf-wheel-zoom-only')
        report = {'scope': selected_scope, 'passed': evidence, 'originals_unchanged': True,
                  'variant': args.variant, 'locale': locale,
                  'not_verified': ['remaining full native-smoke scenarios', 'real wheel/touchpad devices',
                                   'OS DPI switching', 'audible silence', 'power loss'],
                  'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest()}
        if args.pdf_selection_only:
            report['not_verified'].append('OS clipboard transfer')
            report['clipboard_used'] = False
        (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(f'PASS ({selected_scope}): {output}')
        return
    run(selection_pdf, pdf_selection)
    run(position_file, reading_positions)
    run(view_pdf, pdf_positions)
    run(wide_diagram, diagram_navigation)

    # Performance diagnosis: complete body and outline are observed; no timing threshold
    # is used to conceal a missing result. The preparation count forbids repeat parses.
    large = output / 'many-headings.md'
    run(md, markdown)
    run(md, license_view)
    def license_missing(hwnd):
        editor = u.GetDlgItem(hwnd, 104)
        before = text(editor)
        send(hwnd, 0x0111, 122)
        require(text(editor) == before, 'missing notices preserve displayed original')
        require(send(u.GetDlgItem(hwnd, 103), 0x1304) == 1, 'missing notices preserve tab count')
        require('licenses' in text(u.GetDlgItem(hwnd, 817)), 'missing notices show visual guidance')
    edge_source = ('# Markdown boundaries\n\nThe next heading is Named section.\n\n## Named section\n\n'
                   '| Edge | Result |\n| --- | --- |\n| unmatched ` | CellAfterUnmatched |\n'
                   '| multiple ticks | ``a`b`` |\n| trailing slash | \\\\|\n\n'
                   '````text\n```mermaid\nflowchart LR\nA[LiteralCodeNode] --> B[AnotherCodeNode]\n```\n'
                   '## StillInsideCode\n````\n\n~~~text\n## AlsoCode\n~~~\n\n'
                   '```mermaid\nUnclosedDiagramSource\n# NotAHeading\n')
    edge_file = output / 'markdown-boundaries.md'; edge_file.write_text(edge_source, encoding='utf-8', newline='')
    originals[edge_file] = hashlib.sha256(edge_file.read_bytes()).hexdigest()
    def markdown_boundaries(hwnd):
        editor = u.GetDlgItem(hwnd, 104); toc = u.GetDlgItem(hwnd, 102)
        displayed = text(editor).replace('\r\n', '\n')
        require('UnclosedDiagramSource' in displayed and '# NotAHeading' in displayed,
                'unterminated Mermaid remains complete literal source')
        require('LiteralCodeNode' in displayed and '## StillInsideCode' in displayed and '## AlsoCode' in displayed,
                'long and tilde fences retain literal headings and Mermaid')
        require(send(toc, 0x1105) == 2, 'code headings never enter the outline')
        require('CellAfterUnmatched' in displayed and '| unmatched' not in displayed and '| trailing slash' not in displayed,
                'table boundaries survive unmatched backticks and even backslashes')
        require('a`b' in displayed and '``a`b``' not in displayed, 'inline code matches backtick run lengths')
        diagram_button = u.GetDlgItem(hwnd, 111)
        require(diagram_button and not u.IsWindowEnabled(diagram_button), 'literal Mermaid never enables diagram mode')
        first = send(toc, 0x110A, 0); second = send(toc, 0x110A, 4, first)
        send(toc, 0x110B, 9, second)
        expected = len(displayed[:displayed.index('\nNamed section\n') + 1].encode('utf-16-le')) // 2
        require(send(editor, 0x00B0) & 0xffff == expected, 'outline resolves heading rather than identical body text')
        require(u.GetPropW(hwnd, 'ReaderNextPreparationCount') == 1, 'boundary fixture prepared once')
        screenshot(hwnd, output / 'markdown-boundaries.png')
    run(edge_file, markdown_boundaries)
    clro_file = output / 'note.CLRO'; clro_file.write_bytes(md.read_bytes())
    originals[clro_file] = hashlib.sha256(clro_file.read_bytes()).hexdigest()
    run(clro_file, lambda hwnd: markdown(hwnd, clro_file.name))
    clrop_file = output / 'annotations.clrop'
    clrop_file.write_text(json.dumps({'version': 1, 'pdf_id': {'path': 'sample.pdf', 'size': 42, 'page_count': 1},
                                     'pages': [{'page': 0, 'items': [{'type': 'text', 'id': 'reader-fixture',
                                                                     'created': '2026-10-11T00:00:00Z',
                                                                     'updated': '2026-10-11T00:00:00Z',
                                                                     'content': 'AnnotationPreview', 'bbox': [1, 2, 3, 4]}]}]}),
                          encoding='utf-8')
    originals[clrop_file] = hashlib.sha256(clrop_file.read_bytes()).hexdigest()
    def clrop_preview(hwnd):
        editor = u.GetDlgItem(hwnd, 104)
        require('AnnotationPreview' in text(editor) and 'sample.pdf' in text(editor), f'CLROP annotation preview shown: {text(editor)!r}')
        require('"pdf_id"' not in text(editor), 'CLROP opens in its annotation mode')
        catalog = json.loads((root / 'locales' / f'{locale}.json').read_text(encoding='utf-8-sig'))
        require(catalog['readonly.clrop.annotations_by_page'] in text(editor), 'fresh annotation view preserves localized Unicode')
        send(hwnd, 0x0111, 107)
        require('"pdf_id"' in text(editor) and 'AnnotationPreview' in text(editor), 'CLROP raw JSON remains available')
        send(hwnd, 0x0111, 106)
        require('AnnotationPreview' in text(editor), 'CLROP annotation preview restored')
    run(clrop_file, clrop_preview)
    literal_file = output / 'literal.txt'
    literal_source = '{\\rtf1\\ansi injected} 日本語の原文 📖\nSecond line.\n'
    literal_file.write_text(literal_source, encoding='utf-8', newline='')
    originals[literal_file] = hashlib.sha256(literal_file.read_bytes()).hexdigest()
    def literal_text(hwnd):
        require(text(u.GetDlgItem(hwnd, 104)).replace('\r\n', '\n') == literal_source,
                'fresh raw view preserves Unicode and treats leading RTF as literal text')
    run(literal_file, literal_text)
    navigation = output / 'navigation'
    chapter = navigation / 'chapter'; chapter.mkdir(parents=True)
    navigation_file = navigation / 'nav.md'; navigation_file.write_text('# Navigation\n', encoding='utf-8')
    originals[navigation_file] = hashlib.sha256(navigation_file.read_bytes()).hexdigest()
    large.write_text('Setext heading\n===\n\n' + ''.join(f'## Section {i}\n\nBody {i} 日本語と英字。\n\n' for i in range(400)), encoding='utf-8')
    originals[large] = hashlib.sha256(large.read_bytes()).hexdigest()
    def outline_many(hwnd):
        require(send(u.GetDlgItem(hwnd, 102), 0x1105) == 401, 'all 400 ATX headings and the Setext heading published')
        require('Body 399' in text(u.GetDlgItem(hwnd, 104)), 'complete document published')
        count = u.GetPropW(hwnd, 'ReaderNextPreparationCount')
        for _ in range(3): send(hwnd, 0x0111, 107); send(hwnd, 0x0111, 106)
        require(count == 1 and u.GetPropW(hwnd, 'ReaderNextPreparationCount') == 1, 'large document is not reparsed')
        screenshot(hwnd, output / 'many-headings.png')
    outline_started = time.monotonic(); run(large, outline_many)
    outline_elapsed = time.monotonic() - outline_started
    run(pdf, pdf_check); run(binary, binary_check); run(None, empty_check)
    run(navigation_file, folder_navigation); run(md, drag_transfer); run(md, detach_new_window)
    reload_dir = output / 'reload-input'; reload_dir.mkdir()
    for name, reload_newline in [('crlf', '\r\n'), ('lf', '\n')]:
        case_dir = reload_dir / name; case_dir.mkdir()
        reload_file = case_dir / 'reload.md'
        reload_file.write_text('# Initial heading\n\nOriginal fixture body.\n', encoding='utf-8', newline='')
        originals[reload_file] = hashlib.sha256(reload_file.read_bytes()).hexdigest()
        run(reload_file, source_reload)
    missing = output / 'missing.md'
    run(missing, missing_check)
    require(not missing.exists(), 'missing file is not created by reading')
    incomplete_bin = output / 'missing-license-bin'
    incomplete_bin.mkdir()
    for name in (exe.name, 'pdfium.dll', 'libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll'):
        shutil.copyfile(exe.parent / name, incomplete_bin / name)
    run(md, license_missing, incomplete_bin / exe.name)
    session_name = 'readonly_setup.json' if args.variant == 'standard' else 'reader_next_setup.json'
    other_name = 'reader_next_setup.json' if args.variant == 'standard' else 'readonly_setup.json'
    stored_session = {'format': 'readonly_viewer_session_v1', 'persistenceEnabled': True,
                      'folder': {'path': str(output), 'persistent': True}, 'selectedTab': 0,
                      'tabs': [{'path': str(md), 'persistent': True}, {'path': str(literal_file), 'persistent': True}]}
    (runtime_bin / session_name).write_text(json.dumps(stored_session, ensure_ascii=False), encoding='utf-8')
    other_bytes = json.dumps({**stored_session, 'tabs': [{'path': str(binary), 'persistent': True}]}).encode('utf-8')
    (runtime_bin / other_name).write_bytes(other_bytes)
    def restore_legacy_session(hwnd):
        require(send(u.GetDlgItem(hwnd, 103), 0x1304) == 2, 'existing session restores both persistent tabs')
        require('検索対象 alpha' in text(u.GetDlgItem(hwnd, 104)), 'existing session selected tab restored')
        if args.variant == 'standard':
            require('Preview' not in text(hwnd) and '試作' not in text(hwnd), 'production title has no preview label')
        send(hwnd, 0x0111, 122)
        require('README.txt' in text(hwnd), 'restored session can browse license documents')
    run(None, restore_legacy_session, restore_session=True)
    saved_session = json.loads((runtime_bin / session_name).read_text(encoding='utf-8-sig'))
    require(saved_session['persistenceEnabled'] and {tab['path'] for tab in saved_session['tabs']} == {str(md), str(literal_file)},
            'legacy persistent paths survive normal shutdown')
    require(saved_session['folder'] == stored_session['folder'],
            'browsing licenses does not replace an explicitly persistent document folder')
    require((runtime_bin / other_name).read_bytes() == other_bytes, 'comparison session file is not modified')
    saved_bytes = (runtime_bin / session_name).read_bytes()
    def temporary_startup(hwnd):
        require(send(u.GetDlgItem(hwnd, 103), 0x1304) == 1, 'explicit document starts a temporary viewing session')
        require('検索対象 alpha' in text(u.GetDlgItem(hwnd, 104)), 'explicit startup document opens')
    run(md, temporary_startup)
    require((runtime_bin / session_name).read_bytes() == saved_bytes,
            'explicit document shutdown does not overwrite unloaded persistent session')
    invalid_session = b'{"format":"readonly_viewer_session_v1","persistenceEnabled":'
    (runtime_bin / session_name).write_bytes(invalid_session)
    run(None, empty_check, restore_session=True)
    require((runtime_bin / session_name).read_bytes() == invalid_session,
            'invalid existing preferences remain recoverable after startup and shutdown')
    for path, original in originals.items():
        require(hashlib.sha256(path.read_bytes()).hexdigest() == original, f'original preserved: {path.name}')
    report = {'scope': 'full', 'passed': evidence, 'originals_unchanged': True, 'license_bundle_and_ui': True,
              'legacy_session_restored': True, 'variant': args.variant, 'locale': locale,
              'not_verified': ['real keyboard focus/shortcuts', 'real scrollbar thumb dragging', 'IME',
                               'OS DPI switching', 'audible silence', 'power loss'],
              'many_headings_scenario_seconds': outline_elapsed, 'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest()}
    (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(f'PASS: {output}')

if __name__ == '__main__':
    main()
