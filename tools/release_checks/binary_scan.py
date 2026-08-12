#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


BINARY_SUFFIXES = {".exe", ".dll", ".com", ".pyd", ".ocx"}
PE_REQUIRED_SUFFIXES = {".exe", ".dll", ".pyd", ".ocx"}
DEFAULT_QUERIES = (
    "http://",
    "https://",
    "winhttp",
    "wininet",
    "urlmon",
    "ws2_32",
    "wsock32",
    "socket",
    "connect",
    "send",
    "recv",
    "curl",
    "update",
    "telemetry",
    "crash",
    "minidump",
    "mail",
)


@dataclass
class Section:
    virtual_address: int
    virtual_size: int
    raw_pointer: int
    raw_size: int


@dataclass
class BinaryFinding:
    path: str
    size_bytes: int
    import_dlls: list[str]
    import_symbols: list[str]
    matched_strings: list[str]
    matched_imports: list[str]
    pe_status: str = "not-pe"


def configure_utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Read-only binary scanner for PE imports and embedded strings."
    )
    parser.add_argument("--root", default=".", help="Repository root. Defaults to current directory.")
    parser.add_argument(
        "--include",
        action="append",
        default=[],
        help="Root-relative file or directory to scan. Can be repeated.",
    )
    parser.add_argument(
        "--query",
        action="append",
        default=[],
        help="Case-insensitive string/import query. Defaults to network/update/mail terms.",
    )
    parser.add_argument(
        "--imported-dll",
        action="append",
        default=[],
        help="Report binaries that import this DLL name. Can be repeated.",
    )
    parser.add_argument(
        "--imports-only",
        action="store_true",
        help="Inspect PE imports only; do not search embedded strings.",
    )
    parser.add_argument(
        "--all-strings",
        action="store_true",
        help="Return recovered strings without applying --query/default string filters.",
    )
    parser.add_argument(
        "--fail-on-import",
        action="store_true",
        help="Return failure when any --imported-dll entry is imported.",
    )
    parser.add_argument(
        "--fail-on-unparseable-pe",
        action="store_true",
        help="Return failure when an EXE/DLL/PYD/OCX cannot be parsed as a PE image.",
    )
    parser.add_argument(
        "--all-files",
        action="store_true",
        help="Scan every file under --include for embedded strings. PE imports are still parsed only for PE files.",
    )
    parser.add_argument("--min-string", type=int, default=5, help="Minimum extracted string length.")
    parser.add_argument("--max-strings", type=int, default=40, help="Maximum matched strings per file.")
    parser.add_argument(
        "--format",
        choices=("text", "json", "strings"),
        default="text",
        help="Output format. 'strings' prints recovered string values only.",
    )
    return parser.parse_args(argv)


def iter_binary_files(root: Path, includes: Sequence[str], all_files: bool) -> Iterable[Path]:
    roots = list(includes) if includes else ["."]
    for include in roots:
        base = root / include
        if not base.exists():
            continue
        candidates = [base] if base.is_file() else base.rglob("*")
        for path in candidates:
            if not path.is_file():
                continue
            if all_files or path.suffix.lower() in BINARY_SUFFIXES:
                yield path


def c_string(data: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(data):
        return ""
    end = data.find(b"\0", offset)
    if end < 0:
        end = min(len(data), offset + 4096)
    return data[offset:end].decode("ascii", errors="ignore")


def rva_to_offset(rva: int, sections: Sequence[Section]) -> int | None:
    for section in sections:
        size = max(section.virtual_size, section.raw_size)
        if section.virtual_address <= rva < section.virtual_address + size:
            return section.raw_pointer + (rva - section.virtual_address)
    return None


def requires_pe_parse(path: Path) -> bool:
    return path.suffix.lower() in PE_REQUIRED_SUFFIXES


def parse_pe_imports_with_status(data: bytes) -> tuple[list[str], list[str], str]:
    if data[:2] != b"MZ":
        return [], [], "not-pe"
    if len(data) < 0x100:
        return [], [], "invalid-pe"
    try:
        pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe_offset : pe_offset + 4] != b"PE\0\0":
            return [], [], "invalid-pe"
        coff = pe_offset + 4
        section_count = struct.unpack_from("<H", data, coff + 2)[0]
        optional_size = struct.unpack_from("<H", data, coff + 16)[0]
        optional = coff + 20
        magic = struct.unpack_from("<H", data, optional)[0]
        if magic == 0x10B:
            import_dir_rva = struct.unpack_from("<I", data, optional + 104)[0]
            thunk_size = 4
            ordinal_mask = 0x80000000
        elif magic == 0x20B:
            import_dir_rva = struct.unpack_from("<I", data, optional + 120)[0]
            thunk_size = 8
            ordinal_mask = 0x8000000000000000
        else:
            return [], [], "invalid-pe"

        section_table = optional + optional_size
        sections: list[Section] = []
        for idx in range(section_count):
            entry = section_table + idx * 40
            virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", data, entry + 8)
            sections.append(
                Section(
                    virtual_address=virtual_address,
                    virtual_size=virtual_size,
                    raw_pointer=raw_pointer,
                    raw_size=raw_size,
                )
            )

        import_offset = rva_to_offset(import_dir_rva, sections)
        if import_offset is None:
            return [], [], "valid"

        dlls: list[str] = []
        symbols: list[str] = []
        seen_dlls: set[str] = set()
        seen_symbols: set[str] = set()
        pos = import_offset
        for _ in range(2048):
            if pos + 20 > len(data):
                break
            original_first_thunk, _time, _chain, name_rva, first_thunk = struct.unpack_from("<IIIII", data, pos)
            if not any((original_first_thunk, name_rva, first_thunk)):
                break
            name_offset = rva_to_offset(name_rva, sections)
            dll = c_string(data, name_offset) if name_offset is not None else ""
            if dll and dll.lower() not in seen_dlls:
                seen_dlls.add(dll.lower())
                dlls.append(dll)

            thunk_rva = original_first_thunk or first_thunk
            thunk_offset = rva_to_offset(thunk_rva, sections)
            if thunk_offset is not None:
                for thunk_idx in range(4096):
                    entry_off = thunk_offset + thunk_idx * thunk_size
                    if entry_off + thunk_size > len(data):
                        break
                    if thunk_size == 4:
                        thunk = struct.unpack_from("<I", data, entry_off)[0]
                    else:
                        thunk = struct.unpack_from("<Q", data, entry_off)[0]
                    if thunk == 0:
                        break
                    if thunk & ordinal_mask:
                        continue
                    import_name_offset = rva_to_offset(thunk, sections)
                    if import_name_offset is None or import_name_offset + 2 >= len(data):
                        continue
                    symbol = c_string(data, import_name_offset + 2)
                    if symbol and symbol.lower() not in seen_symbols:
                        seen_symbols.add(symbol.lower())
                        symbols.append(symbol)
            pos += 20
        return dlls, symbols, "valid"
    except (struct.error, ValueError):
        return [], [], "invalid-pe"


def parse_pe_imports(data: bytes) -> tuple[list[str], list[str]]:
    """Return PE imports while preserving the original public two-value API."""
    dlls, symbols, _status = parse_pe_imports_with_status(data)
    return dlls, symbols


def extract_ascii_strings(data: bytes, min_len: int) -> Iterable[str]:
    pattern = rb"[\x20-\x7E]{" + str(min_len).encode("ascii") + rb",}"
    for match in re.finditer(pattern, data):
        yield match.group(0).decode("ascii", errors="ignore")


def extract_utf16le_strings(data: bytes, min_len: int) -> Iterable[str]:
    chars: list[int] = []
    for idx in range(0, len(data) - 1, 2):
        code = data[idx] | (data[idx + 1] << 8)
        if 0x20 <= code <= 0x7E:
            chars.append(code)
        else:
            if len(chars) >= min_len:
                yield "".join(chr(c) for c in chars)
            chars = []
    if len(chars) >= min_len:
        yield "".join(chr(c) for c in chars)


def scan_file(
    path: Path,
    root: Path,
    queries: Sequence[str],
    min_string: int,
    max_strings: int,
    all_strings: bool = False,
) -> BinaryFinding:
    data = path.read_bytes()
    dlls, symbols, pe_status = parse_pe_imports_with_status(data)
    query_lowers = [query.lower() for query in queries]
    matched_strings: list[str] = []
    seen_strings: set[str] = set()
    for value in list(extract_ascii_strings(data, min_string)) + list(extract_utf16le_strings(data, min_string)):
        lower = value.lower()
        if all_strings or any(query in lower for query in query_lowers):
            if value not in seen_strings:
                seen_strings.add(value)
                matched_strings.append(value[:240])
                if len(matched_strings) >= max_strings:
                    break

    imports = dlls + symbols
    matched_imports = [
        value for value in imports if any(query in value.lower() for query in query_lowers)
    ]
    return BinaryFinding(
        path=path.relative_to(root).as_posix(),
        size_bytes=path.stat().st_size,
        import_dlls=dlls,
        import_symbols=symbols,
        matched_strings=matched_strings,
        matched_imports=matched_imports,
        pe_status=pe_status,
    )


def finding_to_dict(finding: BinaryFinding) -> dict[str, object]:
    return {
        "path": finding.path,
        "size_bytes": finding.size_bytes,
        "import_dlls": finding.import_dlls,
        "import_symbols": finding.import_symbols,
        "matched_strings": finding.matched_strings,
        "matched_imports": finding.matched_imports,
        "pe_status": finding.pe_status,
    }


def main(argv: Sequence[str] | None = None) -> int:
    configure_utf8_output()
    args = parse_args(argv)
    root = Path(args.root).resolve()
    if args.fail_on_import and not args.imported_dll:
        print("error: --fail-on-import requires at least one --imported-dll value.")
        return 2
    queries = [] if args.imports_only else (args.query or list(DEFAULT_QUERIES))
    findings = [
        scan_file(path, root, queries, args.min_string, args.max_strings, args.all_strings)
        for path in sorted(set(iter_binary_files(root, args.include, args.all_files)))
    ]
    imported_dlls = {name.lower() for name in args.imported_dll}
    importers = [
        finding for finding in findings
        if any(dll.lower() in imported_dlls for dll in finding.import_dlls)
    ]
    unparseable_pe = [
        finding for finding in findings
        if requires_pe_parse(root / finding.path) and finding.pe_status != "valid"
    ]
    interesting = [
        finding for finding in findings
        if finding.matched_imports or finding.matched_strings or finding in importers
    ]
    if args.format == "json":
        print(json.dumps({
            "interesting": [finding_to_dict(item) for item in interesting],
            "importers": [finding_to_dict(item) for item in importers],
            "unparseable_pe": [finding_to_dict(item) for item in unparseable_pe],
        }, ensure_ascii=False, indent=2))
    elif args.format == "strings":
        seen: set[str] = set()
        for item in interesting:
            for value in item.matched_strings:
                if value in seen:
                    continue
                seen.add(value)
                print(value)
    else:
        print(f"Scanned files: {len(findings)}")
        print(f"Interesting files: {len(interesting)}")
        if args.imported_dll:
            print("\nDLL Importers")
            for dll in args.imported_dll:
                matches = [
                    item.path for item in findings
                    if any(imported.lower() == dll.lower() for imported in item.import_dlls)
                ]
                print(f"  {dll}: {len(matches)}")
                for path in matches[:40]:
                    print(f"    {path}")
        if unparseable_pe:
            print("\nInvalid PE images")
            for item in unparseable_pe:
                print(f"  {item.path}: {item.pe_status}")
        for item in interesting:
            print(f"\n{item.path}  ({item.size_bytes / 1024 / 1024:.2f} MB)")
            if item.matched_imports:
                print("  imports: " + ", ".join(item.matched_imports[:40]))
            if item.matched_strings:
                for value in item.matched_strings[:10]:
                    print(f"  string: {value}")
    if (args.fail_on_import and importers) or (args.fail_on_unparseable_pe and unparseable_pe):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
