#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


DEFAULT_INCLUDE_ROOTS = ("src", "tests", "scripts", "tools")
DEFAULT_EXCLUDE_DIRS = {
    ".git",
    ".local",
    ".vscode",
    "__pycache__",
    "out",
    "obj",
    "release",
}
DEFAULT_OWN_EXCLUDE_DIRS = DEFAULT_EXCLUDE_DIRS | {"third_party"}
TEXT_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".inc",
    ".cppinc",
    ".inl",
    ".ipp",
    ".py",
    ".ps1",
    ".md",
    ".html",
    ".htm",
    ".txt",
    ".ini",
    ".cfg",
    ".xml",
    ".xcu",
    ".xcd",
    ".component",
    ".properties",
}
CPP_LIKE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".inc",
    ".cppinc",
    ".inl",
    ".ipp",
}
CPP_CONTROL_WORDS = {
    "if",
    "for",
    "while",
    "switch",
    "catch",
    "else",
    "do",
    "return",
    "namespace",
    "class",
    "struct",
    "enum",
}
CALL_RE = re.compile(r"\b([A-Za-z_~]\w*(?:::\w+)*)\s*\(")
IDENT_RE = re.compile(r"^[A-Za-z_~]\w*(?:::\w+)*$")
PY_DEF_RE = re.compile(r"^(?P<indent>\s*)(?:async\s+)?def\s+(?P<name>[A-Za-z_]\w*)\s*\(")
PS_FUNCTION_RE = re.compile(r"^\s*function\s+(?P<name>[A-Za-z_][\w-]*)\b", re.IGNORECASE)


@dataclass
class CodeBlock:
    kind: str
    name: str
    path: str
    start_line: int
    end_line: int
    signature: str
    calls: list[str]


@dataclass
class StringHit:
    path: str
    line: int
    column: int
    text: str
    block: str | None


@dataclass
class PathHit:
    path: str
    query: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Approximate block-level symbol and string tracer for this repository."
    )
    parser.add_argument("--root", default=".", help="Repository root. Defaults to current directory.")
    parser.add_argument(
        "--include",
        action="append",
        default=[],
        help="Additional root-relative file or directory to scan. Can be repeated.",
    )
    parser.add_argument(
        "--exclude",
        action="append",
        default=[],
        help="Additional root-relative path prefix to exclude. Can be repeated.",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="Scan the whole repository except excluded directories. By default scans own source roots.",
    )
    parser.add_argument(
        "--symbol",
        action="append",
        default=[],
        help="Function/symbol name to trace. Can be repeated.",
    )
    parser.add_argument(
        "--string",
        action="append",
        default=[],
        help="Literal string to search. Can be repeated.",
    )
    parser.add_argument(
        "--path",
        action="append",
        default=[],
        help="Substring to search in root-relative file paths. Can be repeated.",
    )
    parser.add_argument(
        "--format",
        choices=("text", "json"),
        default="text",
        help="Output format.",
    )
    parser.add_argument(
        "--show-blocks",
        action="store_true",
        help="Include all discovered blocks in text output.",
    )
    parser.add_argument(
        "--max-hits",
        type=int,
        default=80,
        help="Maximum hits per query in text output.",
    )
    return parser.parse_args()


def normalize_rel(path: Path) -> str:
    return path.as_posix().lstrip("./")


def is_under(rel_path: str, prefixes: Sequence[str]) -> bool:
    rel_path = rel_path.strip("/")
    for prefix in prefixes:
        key = prefix.strip("/").replace("\\", "/")
        if not key:
            continue
        if rel_path == key or rel_path.startswith(key + "/"):
            return True
    return False


def iter_files(root: Path, scan_all: bool, includes: Sequence[str], excludes: Sequence[str]) -> Iterable[Path]:
    include_roots = (list(includes) if includes else [""]) if scan_all else list(DEFAULT_INCLUDE_ROOTS) + list(includes)
    exclude_prefixes = [p.replace("\\", "/").strip("/") for p in excludes]
    exclude_dirs = DEFAULT_EXCLUDE_DIRS if scan_all else DEFAULT_OWN_EXCLUDE_DIRS

    for include in include_roots:
        base = root / include if include else root
        if not base.exists():
            continue
        candidates = [base] if base.is_file() else base.rglob("*")
        for path in candidates:
            if not path.is_file():
                continue
            rel = normalize_rel(path.relative_to(root))
            parts = set(Path(rel).parts)
            if parts & exclude_dirs:
                continue
            if is_under(rel, exclude_prefixes):
                continue
            if path.suffix.lower() not in TEXT_SUFFIXES:
                continue
            yield path


def iter_any_files(root: Path, scan_all: bool, includes: Sequence[str], excludes: Sequence[str]) -> Iterable[Path]:
    include_roots = (list(includes) if includes else [""]) if scan_all else list(DEFAULT_INCLUDE_ROOTS) + list(includes)
    exclude_prefixes = [p.replace("\\", "/").strip("/") for p in excludes]
    exclude_dirs = DEFAULT_EXCLUDE_DIRS if scan_all else DEFAULT_OWN_EXCLUDE_DIRS

    for include in include_roots:
        base = root / include if include else root
        if not base.exists():
            continue
        candidates = [base] if base.is_file() else base.rglob("*")
        for path in candidates:
            if not path.is_file():
                continue
            rel = normalize_rel(path.relative_to(root))
            parts = set(Path(rel).parts)
            if parts & exclude_dirs:
                continue
            if is_under(rel, exclude_prefixes):
                continue
            yield path


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="ignore")


def strip_line_comment(line: str) -> str:
    in_string = False
    escape = False
    quote = ""
    for idx, ch in enumerate(line):
        if escape:
            escape = False
            continue
        if ch == "\\":
            escape = True
            continue
        if in_string:
            if ch == quote:
                in_string = False
            continue
        if ch in ("'", '"'):
            in_string = True
            quote = ch
            continue
        if ch == "/" and idx + 1 < len(line) and line[idx + 1] == "/":
            return line[:idx]
    return line


def find_cpp_name(signature: str) -> str | None:
    clean = " ".join(signature.replace("\n", " ").split())
    if not clean or "(" not in clean or ")" not in clean:
        return None
    if clean.endswith(";"):
        return None
    before_paren = clean[: clean.find("(")].strip()
    if not before_paren:
        return None
    last = before_paren.split()[-1].strip("*&")
    if not IDENT_RE.match(last):
        return None
    if last.split("::")[-1] in CPP_CONTROL_WORDS:
        return None
    return last


def collect_calls(text: str, known_names: set[str] | None = None) -> list[str]:
    calls: list[str] = []
    seen: set[str] = set()
    for match in CALL_RE.finditer(text):
        name = match.group(1)
        short = name.split("::")[-1]
        if short in CPP_CONTROL_WORDS:
            continue
        if known_names is not None and name not in known_names and short not in known_names:
            continue
        if name not in seen:
            seen.add(name)
            calls.append(name)
    return calls


def find_matching_brace(lines: Sequence[str], start_idx: int, open_col: int) -> int:
    depth = 0
    for idx in range(start_idx, len(lines)):
        line = strip_line_comment(lines[idx])
        col_start = open_col if idx == start_idx else 0
        for ch in line[col_start:]:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return idx
    return len(lines) - 1


def parse_cpp_blocks(path: Path, rel: str, text: str) -> list[CodeBlock]:
    lines = text.splitlines()
    blocks: list[CodeBlock] = []
    idx = 0
    while idx < len(lines):
        line = strip_line_comment(lines[idx])
        if "{" not in line:
            idx += 1
            continue

        open_col = line.find("{")
        signature_lines = [line[:open_col].strip()]
        sig_start = idx
        back = idx - 1
        while back >= 0 and len(signature_lines) < 6:
            prev = strip_line_comment(lines[back]).strip()
            if not prev:
                break
            if prev.endswith(";") or prev.endswith("}") or prev.endswith("{"):
                break
            signature_lines.insert(0, prev)
            sig_start = back
            if "(" in prev:
                break
            back -= 1

        signature = " ".join(part for part in signature_lines if part)
        name = find_cpp_name(signature)
        if not name:
            idx += 1
            continue

        end_idx = find_matching_brace(lines, idx, open_col)
        block_text = "\n".join(lines[idx : end_idx + 1])
        blocks.append(
            CodeBlock(
                kind="cpp",
                name=name,
                path=rel,
                start_line=sig_start + 1,
                end_line=end_idx + 1,
                signature=signature,
                calls=collect_calls(block_text),
            )
        )
        idx = end_idx + 1
    return blocks


def parse_python_blocks(path: Path, rel: str, text: str) -> list[CodeBlock]:
    lines = text.splitlines()
    blocks: list[CodeBlock] = []
    for idx, line in enumerate(lines):
        match = PY_DEF_RE.match(line)
        if not match:
            continue
        indent = len(match.group("indent").replace("\t", "    "))
        end = len(lines) - 1
        for scan in range(idx + 1, len(lines)):
            candidate = lines[scan]
            if not candidate.strip():
                continue
            candidate_indent = len(candidate[: len(candidate) - len(candidate.lstrip())].replace("\t", "    "))
            if candidate_indent <= indent:
                end = scan - 1
                break
        block_text = "\n".join(lines[idx : end + 1])
        blocks.append(
            CodeBlock(
                kind="python",
                name=match.group("name"),
                path=rel,
                start_line=idx + 1,
                end_line=end + 1,
                signature=line.strip(),
                calls=collect_calls(block_text),
            )
        )
    return blocks


def parse_powershell_blocks(path: Path, rel: str, text: str) -> list[CodeBlock]:
    lines = text.splitlines()
    blocks: list[CodeBlock] = []
    idx = 0
    while idx < len(lines):
        match = PS_FUNCTION_RE.match(lines[idx])
        if not match:
            idx += 1
            continue
        open_col = lines[idx].find("{")
        if open_col < 0:
            scan = idx + 1
            while scan < len(lines) and "{" not in lines[scan]:
                scan += 1
            if scan >= len(lines):
                idx += 1
                continue
            open_col = lines[scan].find("{")
            brace_line = scan
        else:
            brace_line = idx
        end_idx = find_matching_brace(lines, brace_line, open_col)
        block_text = "\n".join(lines[idx : end_idx + 1])
        blocks.append(
            CodeBlock(
                kind="powershell",
                name=match.group("name"),
                path=rel,
                start_line=idx + 1,
                end_line=end_idx + 1,
                signature=lines[idx].strip(),
                calls=collect_calls(block_text),
            )
        )
        idx = end_idx + 1
    return blocks


def parse_blocks(path: Path, root: Path) -> list[CodeBlock]:
    rel = normalize_rel(path.relative_to(root))
    text = read_text(path)
    suffix = path.suffix.lower()
    if suffix in CPP_LIKE_SUFFIXES:
        return parse_cpp_blocks(path, rel, text)
    if suffix == ".py":
        return parse_python_blocks(path, rel, text)
    if suffix == ".ps1":
        return parse_powershell_blocks(path, rel, text)
    return []


def find_containing_block(blocks: Sequence[CodeBlock], rel: str, line: int) -> str | None:
    for block in blocks:
        if block.path == rel and block.start_line <= line <= block.end_line:
            return block.name
    return None


def find_string_hits(path: Path, root: Path, needles: Sequence[str], blocks: Sequence[CodeBlock]) -> list[StringHit]:
    if not needles:
        return []
    rel = normalize_rel(path.relative_to(root))
    hits: list[StringHit] = []
    for line_no, line in enumerate(read_text(path).splitlines(), start=1):
        for needle in needles:
            col = line.find(needle)
            if col < 0:
                continue
            hits.append(
                StringHit(
                    path=rel,
                    line=line_no,
                    column=col + 1,
                    text=line.strip(),
                    block=find_containing_block(blocks, rel, line_no),
                )
            )
    return hits


def block_to_dict(block: CodeBlock) -> dict[str, object]:
    return {
        "kind": block.kind,
        "name": block.name,
        "path": block.path,
        "start_line": block.start_line,
        "end_line": block.end_line,
        "signature": block.signature,
        "calls": block.calls,
    }


def hit_to_dict(hit: StringHit) -> dict[str, object]:
    return {
        "path": hit.path,
        "line": hit.line,
        "column": hit.column,
        "text": hit.text,
        "block": hit.block,
    }


def path_hit_to_dict(hit: PathHit) -> dict[str, object]:
    return {
        "path": hit.path,
        "query": hit.query,
    }


def build_report(args: argparse.Namespace) -> dict[str, object]:
    root = Path(args.root).resolve()
    files = sorted(set(iter_files(root, args.all, args.include, args.exclude)))
    blocks: list[CodeBlock] = []
    for path in files:
        blocks.extend(parse_blocks(path, root))

    known = {block.name for block in blocks}
    known.update(block.name.split("::")[-1] for block in blocks)
    for block in blocks:
        path = root / block.path
        block_text = "\n".join(read_text(path).splitlines()[block.start_line - 1 : block.end_line])
        block.calls = collect_calls(block_text, known)

    symbols = set(args.symbol)
    symbol_definitions = [block for block in blocks if block.name in symbols or block.name.split("::")[-1] in symbols]
    symbol_references = [
        block
        for block in blocks
        if any(call in symbols or call.split("::")[-1] in symbols for call in block.calls)
    ]

    string_hits: list[StringHit] = []
    for path in files:
        string_hits.extend(find_string_hits(path, root, args.string, blocks))

    path_hits: list[PathHit] = []
    if args.path:
        for path in iter_any_files(root, args.all, args.include, args.exclude):
            rel = normalize_rel(path.relative_to(root))
            rel_lower = rel.lower()
            for query in args.path:
                if query.lower() in rel_lower:
                    path_hits.append(PathHit(path=rel, query=query))

    return {
        "root": str(root),
        "file_count": len(files),
        "block_count": len(blocks),
        "symbols": sorted(symbols),
        "strings": list(args.string),
        "paths": list(args.path),
        "definitions": [block_to_dict(block) for block in symbol_definitions],
        "references": [block_to_dict(block) for block in symbol_references],
        "string_hits": [hit_to_dict(hit) for hit in string_hits],
        "path_hits": [path_hit_to_dict(hit) for hit in path_hits],
        "blocks": [block_to_dict(block) for block in blocks],
    }


def print_text(report: dict[str, object], max_hits: int, show_blocks: bool) -> None:
    print(f"Root: {report['root']}")
    print(f"Files scanned: {report['file_count']}")
    print(f"Blocks found: {report['block_count']}")

    definitions = report["definitions"]
    if definitions:
        print("\nDefinitions")
        for row in definitions[:max_hits]:
            print(f"  {row['name']}  {row['path']}:{row['start_line']}-{row['end_line']}")

    references = report["references"]
    if references:
        print("\nReference Blocks")
        for row in references[:max_hits]:
            calls = ", ".join(row["calls"][:8])
            print(f"  {row['name']}  {row['path']}:{row['start_line']}-{row['end_line']}  calls=[{calls}]")

    string_hits = report["string_hits"]
    if string_hits:
        print("\nString Hits")
        for row in string_hits[:max_hits]:
            block = row["block"] or "<file>"
            print(f"  {row['path']}:{row['line']}:{row['column']}  block={block}  {row['text']}")

    path_hits = report["path_hits"]
    if path_hits:
        print("\nPath Hits")
        for row in path_hits[:max_hits]:
            print(f"  {row['path']}  query={row['query']}")

    if show_blocks:
        print("\nBlocks")
        for row in report["blocks"][:max_hits]:
            print(f"  {row['kind']} {row['name']}  {row['path']}:{row['start_line']}-{row['end_line']}")


def main() -> int:
    args = parse_args()
    report = build_report(args)
    if args.format == "json":
        json.dump(report, sys.stdout, ensure_ascii=False, indent=2)
        print()
    else:
        print_text(report, args.max_hits, args.show_blocks)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
