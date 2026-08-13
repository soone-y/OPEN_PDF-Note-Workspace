#!/usr/bin/env python3
"""Build a compact locator index of persistence-related C++ code.

This tool reads repository source files only. It does not validate safety by
itself; each indexed match must be inspected in its source context.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional


SOURCE_EXTENSIONS = {
    ".c", ".cc", ".cpp", ".cxx", ".c++",
    ".h", ".hh", ".hpp", ".hxx", ".ipp", ".inl",
    ".inc", ".cppinc",
}
DEFAULT_INCLUDE_ROOTS = ("src",)
DEFAULT_EXCLUDE_DIRS = {".git", ".local", "out", "bin", "obj", "__pycache__", "third_party"}

ATOMIC_WRITE_RE = re.compile(r"\batomic_write::(?P<symbol>[A-Za-z_]\w*)\s*\(")
FILESYSTEM_MUTATION_RE = re.compile(
    r"\bstd::filesystem::(?P<symbol>remove|rename|copy_file|create_directories)\s*\("
)
WIN32_MUTATION_RE = re.compile(
    r"\b(?P<symbol>MoveFileExW|MoveFileW|ReplaceFileW|DeleteFileW|CopyFileW|"
    r"SetFileInformationByHandle|RemoveDirectoryW|WriteFile|FlushFileBuffers)\s*\("
)
STREAM_WRITE_RE = re.compile(r"\bstd::(?P<symbol>ofstream)\b")
PERSISTENCE_SYMBOL_RE = re.compile(
    r"\b(?P<symbol>(?:Save|Write|Integrate|Commit|Discard|Remove|Quarantine|Recover|Backup)"
    r"[A-Za-z0-9_]*)\s*\("
)
NON_PERSISTENCE_SYMBOLS = {"SaveDC", "SavePumpWheelTarget"}


def configure_utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    category: str
    symbol: str
    snippet: str


def read_text_safely(path: Path) -> str:
    for encoding in ("utf-8", "utf-8-sig", "cp932"):
        try:
            return path.read_text(encoding=encoding)
        except UnicodeDecodeError:
            continue
    return path.read_text(encoding="utf-8", errors="replace")


def iter_source_files(root: Path, include_roots: list[str]) -> Iterable[Path]:
    for include in include_roots:
        base = (root / include).resolve()
        try:
            base.relative_to(root.resolve())
        except ValueError:
            continue
        if not base.exists():
            continue
        candidates = [base] if base.is_file() else base.rglob("*")
        for path in candidates:
            if not path.is_file() or path.suffix.lower() not in SOURCE_EXTENSIONS:
                continue
            relative = path.resolve().relative_to(root.resolve())
            if any(part in DEFAULT_EXCLUDE_DIRS for part in relative.parts):
                continue
            yield path


def classify_line(line: str) -> tuple[str, str] | None:
    for category, pattern in (
        ("atomic_write", ATOMIC_WRITE_RE),
        ("filesystem_mutation", FILESYSTEM_MUTATION_RE),
        ("win32_mutation", WIN32_MUTATION_RE),
        ("stream_write", STREAM_WRITE_RE),
        ("persistence_symbol", PERSISTENCE_SYMBOL_RE),
    ):
        match = pattern.search(line)
        if match:
            symbol = match.group("symbol")
            if category == "persistence_symbol" and symbol in NON_PERSISTENCE_SYMBOLS:
                continue
            return category, symbol
    return None


def collect_findings(root: Path, include_roots: list[str]) -> list[Finding]:
    findings: list[Finding] = []
    seen_paths: set[Path] = set()
    for path in sorted(iter_source_files(root, include_roots), key=lambda item: item.as_posix().lower()):
        resolved = path.resolve()
        if resolved in seen_paths:
            continue
        seen_paths.add(resolved)
        relative = resolved.relative_to(root.resolve()).as_posix()
        for line_number, raw_line in enumerate(read_text_safely(path).splitlines(), start=1):
            classified = classify_line(raw_line)
            if classified is None:
                continue
            category, symbol = classified
            snippet = raw_line.strip()
            findings.append(Finding(relative, line_number, category, symbol, snippet))
    return findings


def escape_field(text: str) -> str:
    return text.replace("\\", "\\\\").replace("\t", "\\t").replace("\r", "\\r").replace("\n", "\\n")


def render_index(findings: list[Finding]) -> str:
    lines = [
        "# persistence_index locator v1",
        "# Generated locator only; inspect source code before making a safety conclusion.",
        "# columns: path<TAB>line<TAB>category<TAB>symbol<TAB>snippet",
    ]
    for finding in findings:
        lines.append(
            f"{escape_field(finding.path)}\t{finding.line}\t{finding.category}\t"
            f"{escape_field(finding.symbol)}\t{escape_field(finding.snippet)}"
        )
    return "\n".join(lines) + "\n"


def write_text_atomically(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Optional[Path] = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="",
            dir=str(path.parent),
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as temporary:
            temporary_path = Path(temporary.name)
            temporary.write(text)
            temporary.flush()
            os.fsync(temporary.fileno())
        os.replace(temporary_path, path)
        temporary_path = None
    finally:
        if temporary_path is not None:
            try:
                temporary_path.unlink()
            except FileNotFoundError:
                pass


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Index persistence-related C++ source locations without external access."
    )
    parser.add_argument("--root", type=Path, default=Path("."), help="Repository root directory")
    parser.add_argument(
        "--include",
        action="append",
        default=[],
        help="Root-relative source file or directory. Defaults to src. Can be repeated.",
    )
    parser.add_argument("--out", type=Path, help="Write TSV locator index atomically to this path")
    parser.add_argument("--stdout", action="store_true", help="Print TSV locator index to stdout")
    return parser.parse_args(argv)


def main(argv: Optional[list[str]] = None) -> int:
    configure_utf8_output()
    args = parse_args(argv)
    root = args.root.resolve()
    if not root.is_dir():
        print(f"error: root is not a directory: {root}", file=sys.stderr)
        return 2

    includes = args.include or list(DEFAULT_INCLUDE_ROOTS)
    findings = collect_findings(root, includes)
    text = render_index(findings)

    if args.out:
        out_path = args.out if args.out.is_absolute() else root / args.out
        source_paths = {path.resolve() for path in iter_source_files(root, includes)}
        if out_path.resolve() in source_paths:
            print("error: --out must not replace a scanned source file.", file=sys.stderr)
            return 2
        write_text_atomically(out_path, text)
    else:
        out_path = None

    if args.stdout:
        print(text, end="")
        stream = sys.stderr
    else:
        stream = sys.stdout
    print(f"root: {root}", file=stream)
    print(f"findings: {len(findings)}", file=stream)
    if out_path:
        print(f"index: {out_path.resolve()}", file=stream)
    elif not args.stdout:
        print("index: none (pass --out out/persistence_index.tsv to write the locator)", file=stream)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
