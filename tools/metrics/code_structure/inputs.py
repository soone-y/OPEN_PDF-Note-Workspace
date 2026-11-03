"""Read-only compiler probes and fixed in-memory source buffers.

Input ownership: CLI owns paths and subprocesses. No build commands are run.
Failure keeps sources and old reports intact; compiler probes have bounded waits.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from pathlib import Path

from .model import AnalysisError, digest_bytes, display_path, stable_id

SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".hxx", ".c", ".cpp", ".cc", ".cxx", ".inc", ".cppinc", ".inl", ".ipp"}


def probe(args, *, cwd: Path, input_text=None) -> subprocess.CompletedProcess:
    try:
        raw = subprocess.run(args, cwd=cwd, input=input_text.encode("utf-8") if input_text is not None else None,
                                capture_output=True, timeout=30,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        result = subprocess.CompletedProcess(raw.args, raw.returncode, raw.stdout.decode("utf-8", errors="strict"), raw.stderr.decode("utf-8", errors="strict"))
    except (OSError, UnicodeError, subprocess.TimeoutExpired) as exc:
        raise AnalysisError(f"compiler probe failed: {args[0]}: {exc}") from exc
    if result.returncode:
        raise AnalysisError(f"compiler probe exit {result.returncode}: {args[0]}: {result.stderr[:2000]}")
    return result


def executable(value: str) -> Path:
    found = shutil.which(value)
    path = Path(found or value).resolve()
    if not path.is_file():
        raise AnalysisError(f"missing local executable: {value}")
    return path


class Snapshot:
    def __init__(self, root: Path):
        self.root = root
        self.buffers: dict[str, bytes] = {}
        self.encodings: dict[str, str] = {}

    def capture(self, path, *, text=False) -> bytes:
        path = str(Path(path).resolve())
        if path not in self.buffers:
            try:
                self.buffers[path] = Path(path).read_bytes()
            except OSError as exc:
                raise AnalysisError(f"required input read failed: {path}: {exc}") from exc
        data = self.buffers[path]
        if text:
            try:
                data.decode("utf-8-sig", errors="strict")
            except UnicodeError as exc:
                raise AnalysisError(f"required source decode failed (utf-8-sig, no replacement): {path}: {exc}") from exc
            self.encodings[path] = "utf-8-sig" if data.startswith(b"\xef\xbb\xbf") else "utf-8"
        return data

    def json(self, path):
        try:
            return json.loads(self.capture(path, text=True).decode("utf-8-sig"))
        except (ValueError, TypeError) as exc:
            raise AnalysisError(f"invalid JSON input: {path}: {exc}") from exc

    def verify(self):
        for path, data in self.buffers.items():
            try:
                if Path(path).read_bytes() != data:
                    raise AnalysisError(f"input changed during analysis: {path}")
            except OSError as exc:
                raise AnalysisError(f"input unavailable after analysis: {path}: {exc}") from exc

    def manifest(self):
        return [{"path": display_path(path, self.root), "sha256": digest_bytes(data),
                 "bytes": len(data), "encoding": self.encodings.get(path, "binary-identity")}
                for path, data in sorted(self.buffers.items())]

    def unsaved(self):
        return [(path, data) for path, data in self.buffers.items()
                if Path(path).suffix.lower() in SOURCE_SUFFIXES]


def clang_runtime(clang_value: str, library_value: str | None, root: Path, snapshot: Snapshot):
    clang = executable(clang_value)
    resource = Path(probe([str(clang), "-print-resource-dir"], cwd=root).stdout.strip()).resolve()
    if not (resource / "include").is_dir():
        raise AnalysisError(f"missing Clang resource headers: {resource}")
    library = Path(library_value).resolve() if library_value else clang.parent / ("libclang.dll" if os.name == "nt" else "libclang.so")
    if not library.is_file():
        raise AnalysisError("libclang not found beside selected Clang; supply --libclang explicitly")
    from clang import cindex
    if cindex.Config.loaded:
        if Path(cindex.conf.lib._name).resolve() != library.resolve():
            raise AnalysisError("a different libclang is already loaded; run in a fresh process")
    else:
        cindex.Config.set_library_file(str(library))
    try:
        index = cindex.Index.create()
        import ctypes
        cindex.conf.lib.clang_getClangVersion.restype = cindex._CXString
        version = str(cindex._CXString.from_result(cindex.conf.lib.clang_getClangVersion()))
    except Exception as exc:
        raise AnalysisError(f"libclang/binding initialization failed: {exc}") from exc
    driver_version = probe([str(clang), "--version"], cwd=root).stdout.strip()
    driver_major = re.search(r"clang version (\d+)", driver_version)
    library_major = re.search(r"clang version (\d+)", version, re.IGNORECASE)
    if not driver_major or not library_major or driver_major[1] != library_major[1]:
        raise AnalysisError(f"Clang/libclang resource version mismatch: {driver_version}; {version}")
    identity = {"driver": str(clang), "driver_version": driver_version,
                "library": str(library), "library_version": version,
                "resource_directory": str(resource)}
    identity["driver_sha256"] = digest_bytes(snapshot.capture(clang))
    identity["library_sha256"] = digest_bytes(snapshot.capture(library))
    identity["binding_sha256"] = digest_bytes(snapshot.capture(cindex.__file__, text=True))
    return index, identity


def safe_arguments(arguments: list[str], directory: Path, source: Path, snapshot: Snapshot):
    """Accept syntax arguments, never plugin execution, PCH or output-producing flags."""
    result, dropped = [], []
    skip_value = {"-o", "-MF", "-MT", "-MQ", "-MJ", "--serialize-diagnostics"}
    skip_single = {"-c", "-S", "-E", "-MMD", "-MD", "-MP", "-M", "-MM"}
    takes_path = {"-I", "-isystem", "-iquote", "-idirafter", "-include", "-imacros", "-isysroot", "--sysroot", "-resource-dir"}
    i = 0
    while i < len(arguments):
        arg = arguments[i]
        if not isinstance(arg, str):
            raise AnalysisError("compiler arguments must be strings")
        if arg.startswith("@"):
            # Compiler-specific response quoting cannot be approximated safely.
            snapshot.capture(directory / arg[1:])
            raise AnalysisError("response file arguments require expansion by the build exporter; not supported in v1")
        if arg in {"-Xclang", "-load", "-plugin", "-fplugin", "-include-pch"} or arg.startswith(("-fplugin", "-fmodules", "-fmodule", "-save-temps", "-ftime-trace", "-serialize-diagnostics", "--serialize-diagnostics=", "-dependency-file", "-ivfsoverlay")):
            raise AnalysisError(f"unsafe or unsupported analysis argument: {arg}")
        if arg in skip_value:
            if i + 1 >= len(arguments):
                raise AnalysisError(f"missing argument after {arg}")
            dropped += arguments[i:i+2]; i += 2; continue
        if arg in skip_single:
            dropped.append(arg); i += 1; continue
        if arg in takes_path:
            if i + 1 >= len(arguments):
                raise AnalysisError(f"missing path after {arg}")
            value = (directory / arguments[i+1]).resolve()
            result += [arg, str(value)]; i += 2; continue
        if arg.startswith("-I") and len(arg) > 2:
            result.append("-I" + str((directory / arg[2:]).resolve()))
        elif not arg.startswith("-"):
            if (directory / arg).resolve() != source:
                raise AnalysisError(f"unexpected positional compiler argument: {arg}")
            dropped.append(arg)
        else:
            result.append(arg)
        i += 1
    return result + ["-fsyntax-only", "-fno-modules", "-fno-implicit-modules", "-fno-implicit-module-maps"], dropped


REVIEWED_DRIVERS = {
    "024c5cab42ae39e0fb534e172ee9f8ac7b11cc34c5c9764ccc09b17bd777e44a":
        ("workspace", "scripts/build/build_workspace.ps1", "BuildSourceFiles"),
    "d18f5f6edaea6cf02c1d683fdbc80113014bc168e5c7b90f9442a1235112a921":
        ("readonly-viewer", "scripts/build/build_readonly_viewer.ps1", "ReadOnlyViewerSourceFiles"),
}


def validate_signature(signature, root: Path, snapshot: Snapshot):
    """Dispatch by reviewed driver identity, never by output directory/name."""
    expected = signature.get("BuildDriverHash", "").lower()
    if expected not in REVIEWED_DRIVERS:
        raise AnalysisError("build driver unreviewed; supply an explicit Clang arguments database")
    kind, driver_path, source_key = REVIEWED_DRIVERS[expected]
    driver = root / driver_path
    driver_hash = digest_bytes(snapshot.capture(driver, text=True))
    if signature.get("BuildDriverHash", "").lower() != driver_hash:
        raise AnalysisError("build signature driver hash is stale; create a fresh signature through the normal build")
    manifest = snapshot.json(root / "scripts/build/build_sources.json")
    if signature.get("SourceFiles") != manifest.get(source_key) or (kind == "workspace" and signature.get("TranslationUnitDesign") != manifest.get("TranslationUnitDesign")):
        raise AnalysisError("build signature source/TU manifest differs from current build inputs")
    locale = signature.get("Locale")
    flags = signature.get("Flags")
    if locale not in {"ja", "en"} or not isinstance(flags, list):
        raise AnalysisError("signature requires explicit locale and flags")
    required_locale = "-DPDF_NOTE_LOCALE_EN=" + str(int(locale == "en"))
    if [x for x in flags if x.startswith("-DPDF_NOTE_LOCALE_EN=")] != [required_locale]:
        raise AnalysisError("signature locale/flags mismatch")
    if signature.get("Configuration") not in {"Debug", "Release"}:
        raise AnalysisError("unsupported signature configuration")
    if kind == "workspace":
        edition = signature.get("Edition")
        if edition not in {"Full", "Lite"} or [x for x in flags if x.startswith("-DPDF_NOTE_LITE_EDITION=")] != ["-DPDF_NOTE_LITE_EDITION=" + str(int(edition == "Lite"))]:
            raise AnalysisError("signature edition/flags mismatch")
    elif signature.get("Edition") is not None or not {"-DCLROP_READ_ONLY_BUILD", "-DMD4C_USE_UTF16"}.issubset(flags):
        raise AnalysisError("readonly signature requires reviewed readonly/UTF16 flags and no edition")
    return kind, driver_hash


def signature_commands(path: Path, selected: list[str], root: Path, snapshot: Snapshot, runtime: dict):
    signature = snapshot.json(path)
    kind, driver_hash = validate_signature(signature, root, snapshot)
    for source in selected:
        if source not in signature["SourceFiles"] or Path(source).suffix != ".cpp":
            raise AnalysisError(f"signature adapter requires a selected C++ build TU: {source}")
    compiler = executable(signature["Compiler"])
    compiler_hash = digest_bytes(snapshot.capture(compiler))
    flags = list(signature["Includes"]) + list(signature["Flags"])
    # Reject plugin/response/output hazards before flags reach compiler probes.
    _, dropped_probe_flags = safe_arguments(flags, root, (root / selected[0]).resolve(), snapshot)
    if dropped_probe_flags:
        raise AnalysisError("signature contains output/mode flags unsuitable for read-only probes")
    includes_probe = probe([str(compiler), "-E", "-x", "c++", "-", "-v"] + flags, cwd=root, input_text="")
    macro_probe = probe([str(compiler), "-dM", "-E", "-x", "c++", "-"] + flags, cwd=root, input_text="")
    try:
        includes = includes_probe.stderr.split("#include <...> search starts here:", 1)[1].split("End of search list.", 1)[0]
        system_roots = [str(Path(line.strip()).resolve()) for line in includes.splitlines() if line.strip()]
        standard = re.search(r"^#define __cplusplus (\d+)L?", macro_probe.stdout, re.MULTILINE)[1]
    except (IndexError, TypeError) as exc:
        raise AnalysisError("cannot obtain compiler search paths/language standard") from exc
    standards = {"201103": "gnu++11", "201402": "gnu++14", "201703": "gnu++17", "202002": "gnu++20", "202100": "gnu++23", "202302": "gnu++23"}
    if standard not in standards:
        raise AnalysisError(f"unsupported compiler default C++ standard: {standard}")
    target = probe([str(compiler), "-dumpmachine"], cwd=root).stdout.strip()
    version = probe([str(compiler), "--version"], cwd=root).stdout.strip()
    adaptation = ["--target=" + target, "-std=" + standards[standard],
                  "-resource-dir=" + runtime["resource_directory"],
                  "-isystem", str(Path(runtime["resource_directory"]) / "include")]
    for include in system_roots:
        adaptation += ["-isystem", include]
    commands = []
    for source in selected:
        file = (root / source).resolve()
        original = [str(compiler), "-c", source] + flags
        effective, dropped = safe_arguments(adaptation + flags, root, file, snapshot)
        commands.append({"file": str(file), "directory": str(root), "original_arguments": original,
                         "arguments": effective, "dropped": dropped,
                         "adapter": "reviewed-build-signature/v1", "adapter_driver_hash": driver_hash,
                         "build_profile": {"kind": kind, "locale": signature["Locale"],
                                           "configuration": signature["Configuration"], "edition": signature.get("Edition")},
                         "compiler": str(compiler), "compiler_version": version,
                         "compiler_sha256": compiler_hash,
                         "compiler_predefines_sha256": digest_bytes(macro_probe.stdout.encode("utf-8")),
                         "adaptation": adaptation,
                         "compatibility": "exploratory GCC-to-Clang profile; not a proof of real compiler equivalence"})
    return commands


def database_commands(path: Path, selected: list[str], root: Path, snapshot: Snapshot):
    database = snapshot.json(path)
    if not isinstance(database, list):
        raise AnalysisError("compilation database must be a JSON array")
    commands = []
    for source in selected:
        file = (root / source).resolve()
        matches = [row for row in database if isinstance(row, dict) and
                   (Path(row.get("directory", "")) / row.get("file", "")).resolve() == file]
        if not matches:
            raise AnalysisError(f"missing required TU in compilation database: {source}")
        for row in matches:
            if not Path(row.get("directory", "")).is_absolute():
                raise AnalysisError("compilation database directory must be absolute")
            if not isinstance(row.get("arguments"), list) or not row["arguments"]:
                raise AnalysisError("arguments array required; shell command parsing/execution is unsupported")
            directory = Path(row["directory"]).resolve()
            compiler = executable(row["arguments"][0])
            if "clang" not in compiler.name.lower():
                raise AnalysisError("arguments database requires an explicit Clang profile; use the reviewed signature adapter for GCC")
            compiler_hash = digest_bytes(snapshot.capture(compiler))
            args, dropped = safe_arguments(row["arguments"][1:], directory, file, snapshot)
            commands.append({"file": str(file), "directory": str(directory),
                             "original_arguments": row["arguments"], "arguments": args,
                             "dropped": dropped, "compiler": str(compiler),
                             "compiler_sha256": compiler_hash,
                             "compiler_version": probe([str(compiler), "--version"], cwd=directory).stdout.strip(),
                             "adapter": "arguments-database/v1", "compatibility": "explicit Clang argument profile"})
    return commands
