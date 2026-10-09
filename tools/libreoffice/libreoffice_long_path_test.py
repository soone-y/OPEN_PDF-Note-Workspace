"""Windows long-path regression; retain fresh, owned fixtures and fail on missing PDFs."""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from urllib.parse import unquote, urlsplit
import uuid

import libreoffice_smoke_test as smoke


def utf16_length(value: str) -> int:
    return len(value.encode("utf-16-le")) // 2


def deep(path: Path, length: int) -> Path:
    while utf16_length(str(path)) < length:
        remaining = length - utf16_length(str(path)) - 1
        if remaining == 0:
            return path.with_name(path.name + "x")
        path /= "x" * min(remaining, 40)
    return path


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot(root: Path) -> dict[str, str]:
    return {str(p.relative_to(root)): digest(p) if p.is_file() else "directory"
            for p in root.rglob("*")}


def native_checks(program: Path, root: Path) -> None:
    """Run only in an isolated child; never alter the test runner's environment."""
    directory = deep(root / "native 日本語 space", 500)
    directory.mkdir(parents=True)
    sentinel = directory / "preserve.txt"
    sentinel.write_bytes(b"existing data must survive")
    original = digest(sentinel)
    trace = root / "native_steps.txt"
    def step(value):
        with trace.open("a", encoding="utf-8") as stream:
            stream.write(value + "\n")
    step("load SAL")
    sal = ctypes.WinDLL(str(program / "sal3.dll"))
    step("SAL loaded")
    os.environ["TMP"] = os.environ["TEMP"] = str(directory)
    ptr = ctypes.c_void_p
    output = ctypes.POINTER(ptr)

    def bind(name, args):
        function = getattr(sal, name)
        function.argtypes = args
        function.restype = ctypes.c_int
        return function

    new_string = bind("rtl_uString_newFromStr_WithLength", [output, ctypes.c_wchar_p, ctypes.c_int])
    release = bind("rtl_uString_release", [ptr])
    get_url = bind("osl_getFileURLFromSystemPath", [ptr, output])
    get_system = bind("osl_getSystemPathFromFileURL", [ptr, output])
    get_temp = bind("osl_getTempDirURL", [output])
    open_dir = bind("osl_openDirectory", [ptr, output])
    next_item = bind("osl_getNextDirectoryItem", [ptr, output, ctypes.c_uint])
    release_item = bind("osl_releaseDirectoryItem", [ptr])
    close_dir = bind("osl_closeDirectory", [ptr])
    create_temp = bind("osl_createTempFile", [ptr, output, output])
    write_file = bind("osl_writeFile", [ptr, ptr, ctypes.c_ulonglong, ctypes.POINTER(ctypes.c_ulonglong)])
    close_file = bind("osl_closeFile", [ptr])

    def checked(code):
        if code != 0:
            raise RuntimeError(f"Native SAL call failed: {code}")

    def string(value):
        result = ptr()
        new_string(ctypes.byref(result), value, utf16_length(value))
        return result

    def text(value):
        size = ctypes.c_int.from_address(value.value + 4).value
        return ctypes.wstring_at(value.value + 8, size)

    expected = directory.as_uri()
    for length in (247, 248, 249):
        path = deep(root / str(length), length)
        url = string(path.as_uri())
        native = ptr()
        checked(get_system(url, ctypes.byref(native)))
        release(url)
        has_prefix = text(native).startswith("\\\\?\\")
        release(native)
        if has_prefix != (length >= 248):
            raise RuntimeError("Directory path boundary did not use the safe native form")
    temporary = ptr()
    print("Native: long TMP", flush=True)
    step("long TMP")
    checked(get_temp(ctypes.byref(temporary)))
    if text(temporary) != expected:
        raise RuntimeError("Long TMP was replaced with another location")
    release(temporary)
    native = string(str(directory))
    url = ptr()
    checked(get_url(native, ctypes.byref(url)))
    release(native)
    if text(url) != expected:
        raise RuntimeError("Long system path did not produce its canonical file URL")
    release(url)
    for suffix in ("", "/"):
        step("directory " + repr(suffix))
        print("Native: enumerate " + repr(suffix), flush=True)
        url = string(expected + suffix)
        handle = ptr()
        checked(open_dir(url, ctypes.byref(handle)))
        release(url)
        try:
            item = ptr()
            checked(next_item(handle, ctypes.byref(item), 0))
            checked(release_item(item))
        finally:
            checked(close_dir(handle))

    for kind in ("url", "handle-and-url", "delete-on-close"):
        step(kind)
        print("Native: " + kind, flush=True)
        before = set(directory.iterdir())
        handle, url = ptr(), ptr()
        checked(create_temp(None, None if kind == "url" else ctypes.byref(handle),
                            None if kind == "delete-on-close" else ctypes.byref(url)))
        created = set(directory.iterdir()) - before
        if len(created) != 1:
            raise RuntimeError("Temporary file was not reserved inside long TMP")
        path = created.pop()
        if kind != "delete-on-close":
            returned = Path(unquote(urlsplit(text(url)).path).lstrip("/"))
            release(url)
            if returned != path:
                raise RuntimeError("Temporary URL does not name the reserved file")
        if kind != "url":
            payload = ctypes.create_string_buffer(b"owned long path payload")
            written = ctypes.c_ulonglong()
            checked(write_file(handle, payload, len(payload.value), ctypes.byref(written)))
            if written.value != len(payload.value):
                raise RuntimeError("Temporary write was incomplete")
            checked(close_file(handle))
        if kind == "delete-on-close":
            if path.exists():
                raise RuntimeError("Delete-on-close temporary file survived close")
        elif kind == "handle-and-url" and path.read_bytes() != payload.value:
            raise RuntimeError("Persistent temporary contents were lost")
    os.environ["TMP"] = "relative-temp-must-fail"
    temporary = ptr()
    if get_temp(ctypes.byref(temporary)) == 0:
        release(temporary)
        raise RuntimeError("Invalid explicit TMP silently fell back")
    if digest(sentinel) != original:
        raise RuntimeError("Native temporary operations changed existing data")
    (root / "native_passed.txt").write_text("completed", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--soffice", default="third_party/libreoffice/custom_runtime/instdir/program/soffice.com")
    parser.add_argument("--output-dir")
    parser.add_argument("--native-child", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if sys.platform != "win32":
        raise RuntimeError("This regression requires Windows")
    soffice = Path(args.soffice).resolve(strict=True)
    if args.native_child:
        native_checks(soffice.parent, Path(args.output_dir))
        return
    repo = Path(__file__).resolve().parents[2]
    root = Path(args.output_dir).resolve() if args.output_dir else repo / "out" / ("lo_longpath_" + uuid.uuid4().hex[:8])
    root.mkdir(parents=True, exist_ok=False)
    print(f"Owned retained fixture: {root}", flush=True)
    runtime = soffice.parent.parent
    before = snapshot(runtime)
    parent_environment = dict(os.environ)
    results = []
    try:
        subprocess.run([sys.executable, str(Path(__file__).resolve()), "--native-child",
                        "--soffice", str(soffice), "--output-dir", str(root)],
                       check=True, timeout=40, cwd=root,
                       creationflags=subprocess.CREATE_NO_WINDOW)
        if not (root / "native_passed.txt").is_file():
            raise RuntimeError("Native SAL worker exited without completing its checks")
        import fitz
        controls = {}
        for length in (0, 236, 237, 248, 260, 300, 500):
            for extension, creator in (("docx", smoke.create_docx), ("pptx", smoke.create_pptx)):
                case = root / f"case_{length}_{extension}"
                source_name = ("viscoelastic_fluid_simulation_design_full_ja.docx"
                               if extension == "docx" else "日本語 space.pptx")
                input_dir = case / "input 日本語 space"
                if length:
                    input_dir = deep(input_dir, length - 1 - utf16_length(source_name))
                source = input_dir / source_name
                input_dir.mkdir(parents=True)
                creator(source)
                source_hash = digest(source)
                profile = deep(case / "profile 日本語 space", length) if length else case / "profile"
                output_dir = deep(case / "output 日本語 space", length) if length else case / "output"
                output_dir.mkdir(parents=True)
                smoke.write_profile_path_config(profile)
                environment = parent_environment.copy()
                local = profile / "local"
                for key, leaf in (("TEMP", "temp"), ("TMP", "temp"), ("USERPROFILE", "user"),
                                  ("HOME", "user"), ("APPDATA", "appdata"), ("LOCALAPPDATA", "localappdata")):
                    path = local / leaf
                    path.mkdir(parents=True, exist_ok=True)
                    environment[key] = str(path)
                environment["PYTHONDONTWRITEBYTECODE"] = "1"
                command = [str(soffice), "--headless", "--nologo", "--nodefault", "--nolockcheck",
                           "--nofirststartwizard", "--norestore", "-env:UserInstallation=" + profile.as_uri(),
                           "--convert-to", "pdf", "--outdir", str(output_dir), str(source)]
                try:
                    completed = smoke.run_owned_conversion(command, soffice.parent, environment, 60)
                except subprocess.TimeoutExpired as error:
                    captured = error.output or b""
                    (case / "timeout_stdout.txt").write_bytes(captured if isinstance(captured, bytes) else captured.encode("utf-8"))
                    raise
                (case / "stdout.txt").write_text(completed.stdout, encoding="utf-8")
                pdf = output_dir / (source.stem + ".pdf")
                if completed.returncode != 0 or not pdf.is_file():
                    raise RuntimeError(f"Conversion failed ({length}/{extension}): {completed.returncode}")
                smoke.validate_pdf(pdf)
                with fitz.open(pdf) as document:
                    content = [page.get_text() for page in document]
                if length == 0:
                    controls[extension] = content
                elif content != controls[extension]:
                    raise RuntimeError("Long-path PDF differs from the short-path control")
                if digest(source) != source_hash:
                    raise RuntimeError("Conversion changed its input")
                results.append({"type": extension, "input_units": utf16_length(str(source)),
                                "profile_units": utf16_length(str(profile)), "output_units": utf16_length(str(output_dir)),
                                "pages": len(content), "passed": True})
                print(f"Passed {length or 'short'} / {extension}", flush=True)
    finally:
        if snapshot(runtime) != before or dict(os.environ) != parent_environment:
            raise RuntimeError("Regression changed the runtime or parent environment")
        (root / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Native SAL and {len(results)} fresh-profile conversions passed; runtime and input hashes unchanged.")


if __name__ == "__main__":
    main()
