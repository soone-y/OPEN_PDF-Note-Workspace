#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
cpp_include_visualizer.py

Local C/C++ include dependency visualizer.

Scope:
- Tracks only project-local includes written as #include "..."
- Ignores #include <...>
- Resolves includes only inside the selected project root
- Optional manual include roots can model compiler -I entries without reading build files
- Outputs:
    1. file dependency graph
    2. reverse references
    3. cycle detection
    4. central files ranking
    5. optional layer violation detection
    6. Mermaid or Graphviz DOT graph

Example:
    python cpp_include_visualizer.py C:/path/to/project --format mermaid --out deps.md
    python cpp_include_visualizer.py . --format dot --out deps.dot
    python cpp_include_visualizer.py . --include-root src --layers layers.json --report report.md --graph graph.mmd

layers.json example:
{
  "layers": [
    { "name": "domain", "paths": ["src/domain/"], "may_depend_on": [] },
    { "name": "app",    "paths": ["src/app/"],    "may_depend_on": ["domain"] },
    { "name": "infra",  "paths": ["src/infra/"],  "may_depend_on": ["domain", "app"] },
    { "name": "ui",     "paths": ["src/ui/"],     "may_depend_on": ["app", "domain"] }
  ]
}
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tempfile
from collections import defaultdict, deque
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


SOURCE_EXTS = {
    ".c", ".cc", ".cpp", ".cxx", ".c++",
    ".h", ".hh", ".hpp", ".hxx", ".ipp", ".inl",
    ".inc", ".cppinc",
}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')


def configure_utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


@dataclass(frozen=True)
class IncludeEdge:
    src: str
    dst: str
    include_text: str
    line_no: int


@dataclass
class LayerRule:
    name: str
    paths: list[str]
    may_depend_on: set[str]


@dataclass
class AnalysisResult:
    root: Path
    files: list[str]
    edges: list[IncludeEdge]
    unresolved: dict[str, list[tuple[str, int]]]
    graph: dict[str, set[str]]
    reverse_graph: dict[str, set[str]]


def normalize_rel(path: Path, root: Path) -> str:
    """Return a POSIX-style path relative to root."""
    return path.resolve().relative_to(root.resolve()).as_posix()


def is_source_file(path: Path) -> bool:
    return path.is_file() and path.suffix.lower() in SOURCE_EXTS


def iter_project_files(root: Path, ignore_dirs: set[str]) -> Iterable[Path]:
    for path in root.rglob("*"):
        if any(part in ignore_dirs for part in path.parts):
            continue
        if is_source_file(path):
            yield path


def read_text_safely(path: Path) -> str:
    for enc in ("utf-8", "utf-8-sig", "cp932", "latin-1"):
        try:
            return path.read_text(encoding=enc)
        except UnicodeDecodeError:
            continue
    return path.read_text(errors="replace")


def build_file_index(root: Path, files: list[Path]) -> dict[str, list[Path]]:
    """
    Maps include-like names to candidate files.

    Supports:
    - exact relative path from root: src/core/foo.h
    - basename fallback: foo.h

    The basename fallback is intentionally weaker and is only used when direct
    relative resolution from including file fails.
    """
    index: dict[str, list[Path]] = defaultdict(list)
    for f in files:
        rel = normalize_rel(f, root)
        index[rel].append(f)
        index[f.name].append(f)
    return index


def resolve_include(
    root: Path,
    including_file: Path,
    include_text: str,
    file_index: dict[str, list[Path]],
    include_roots: list[Path],
) -> Path | None:
    """
    Resolve #include "..." to a project-local file.

    Resolution order:
    1. Relative to the including file's directory.
    2. Relative to the project root.
    3. Relative to any manual --include-root directories.
    4. Unique basename match inside the project.

    If multiple basename matches exist, return None to avoid pretending certainty.
    """
    candidate = (including_file.parent / include_text).resolve()
    try:
        candidate.relative_to(root.resolve())
        if candidate.exists() and candidate.is_file():
            return candidate
    except ValueError:
        pass

    candidate = (root / include_text).resolve()
    try:
        candidate.relative_to(root.resolve())
        if candidate.exists() and candidate.is_file():
            return candidate
    except ValueError:
        pass

    for include_root in include_roots:
        candidate = (include_root / include_text).resolve()
        try:
            candidate.relative_to(root.resolve())
            if candidate.exists() and candidate.is_file():
                return candidate
        except ValueError:
            pass

    matches = file_index.get(include_text, [])
    if len(matches) == 1:
        return matches[0]

    return None


def normalize_include_roots(root: Path, include_roots: list[Path]) -> list[Path]:
    normalized: list[Path] = []
    root_resolved = root.resolve()
    for include_root in include_roots:
        candidate = include_root
        if not candidate.is_absolute():
            candidate = root / candidate
        candidate = candidate.resolve()
        try:
            candidate.relative_to(root_resolved)
        except ValueError:
            continue
        if candidate.is_dir():
            normalized.append(candidate)
    return normalized


def analyze_project(root: Path, ignore_dirs: set[str], include_roots: list[Path] | None = None) -> AnalysisResult:
    root = root.resolve()
    normalized_include_roots = normalize_include_roots(root, include_roots or [])
    files_abs = sorted(iter_project_files(root, ignore_dirs), key=lambda p: p.as_posix().lower())
    file_index = build_file_index(root, files_abs)

    rel_files = [normalize_rel(f, root) for f in files_abs]
    edges: list[IncludeEdge] = []
    unresolved: dict[str, list[tuple[str, int]]] = defaultdict(list)
    graph: dict[str, set[str]] = {rel: set() for rel in rel_files}
    reverse_graph: dict[str, set[str]] = {rel: set() for rel in rel_files}

    for src_abs in files_abs:
        src_rel = normalize_rel(src_abs, root)
        text = read_text_safely(src_abs)
        for line_no, line in enumerate(text.splitlines(), start=1):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            include_text = m.group(1).replace("\\", "/")
            dst_abs = resolve_include(root, src_abs, include_text, file_index, normalized_include_roots)
            if dst_abs is None:
                unresolved[src_rel].append((include_text, line_no))
                continue
            dst_rel = normalize_rel(dst_abs, root)
            graph.setdefault(src_rel, set()).add(dst_rel)
            reverse_graph.setdefault(dst_rel, set()).add(src_rel)
            edges.append(IncludeEdge(src=src_rel, dst=dst_rel, include_text=include_text, line_no=line_no))

    return AnalysisResult(
        root=root,
        files=rel_files,
        edges=edges,
        unresolved=dict(unresolved),
        graph=graph,
        reverse_graph=reverse_graph,
    )


def find_cycles(graph: dict[str, set[str]]) -> list[list[str]]:
    """Find simple cycles with DFS. Suitable for small to medium local projects."""
    visited: set[str] = set()
    in_stack: set[str] = set()
    stack: list[str] = []
    cycles: list[list[str]] = []
    seen_keys: set[tuple[str, ...]] = set()

    def canonical_cycle(cycle: list[str]) -> tuple[str, ...]:
        body = cycle[:-1]
        rotations = [tuple(body[i:] + body[:i]) for i in range(len(body))]
        rev = list(reversed(body))
        rotations += [tuple(rev[i:] + rev[:i]) for i in range(len(rev))]
        best = min(rotations)
        return best

    def dfs(node: str) -> None:
        visited.add(node)
        in_stack.add(node)
        stack.append(node)

        for nxt in sorted(graph.get(node, set())):
            if nxt not in visited:
                dfs(nxt)
            elif nxt in in_stack:
                idx = stack.index(nxt)
                cycle = stack[idx:] + [nxt]
                key = canonical_cycle(cycle)
                if key not in seen_keys:
                    seen_keys.add(key)
                    cycles.append(cycle)

        stack.pop()
        in_stack.remove(node)

    for node in sorted(graph):
        if node not in visited:
            dfs(node)

    return cycles


def centrality_ranking(result: AnalysisResult) -> list[tuple[str, int, int, int]]:
    """
    Return list of (file, incoming, outgoing, total), sorted by total desc.
    incoming: how many files include this file
    outgoing: how many project files this file includes
    """
    rows = []
    for f in result.files:
        incoming = len(result.reverse_graph.get(f, set()))
        outgoing = len(result.graph.get(f, set()))
        rows.append((f, incoming, outgoing, incoming + outgoing))
    return sorted(rows, key=lambda x: (-x[3], -x[1], x[0].lower()))


def mermaid_id(path: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_]", "_", path)
    if not safe or safe[0].isdigit():
        safe = "n_" + safe
    return safe


def emit_mermaid(result: AnalysisResult) -> str:
    lines = ["graph TD"]
    if not result.edges:
        lines.append('  empty["No local #include edges found"]')
        return "\n".join(lines) + "\n"

    for edge in sorted(result.edges, key=lambda e: (e.src.lower(), e.dst.lower())):
        src_id = mermaid_id(edge.src)
        dst_id = mermaid_id(edge.dst)
        lines.append(f'  {src_id}["{edge.src}"] --> {dst_id}["{edge.dst}"]')
    return "\n".join(lines) + "\n"


def dot_quote(s: str) -> str:
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def emit_dot(result: AnalysisResult) -> str:
    lines = ["digraph includes {", "  rankdir=LR;"]
    for f in result.files:
        lines.append(f"  {dot_quote(f)};")
    for edge in sorted(result.edges, key=lambda e: (e.src.lower(), e.dst.lower())):
        lines.append(f"  {dot_quote(edge.src)} -> {dot_quote(edge.dst)};")
    lines.append("}")
    return "\n".join(lines) + "\n"


def escape_index_field(text: str) -> str:
    return text.replace("\\", "\\\\").replace("\t", "\\t").replace("\r", "\\r").replace("\n", "\\n")


def emit_search_index(result: AnalysisResult) -> str:
    """Emit resolved and unresolved include locations for targeted text search."""
    lines = [
        "# cpp_include_visualizer search index v1",
        "# Generated locator only; inspect source files before relying on a dependency.",
        "# columns: status<TAB>source<TAB>line<TAB>destination-or-include",
    ]
    for edge in sorted(result.edges, key=lambda e: (e.src.lower(), e.line_no, e.dst.lower())):
        lines.append(
            f"resolved\t{escape_index_field(edge.src)}\t{edge.line_no}\t{escape_index_field(edge.dst)}"
        )
    for src, items in sorted(result.unresolved.items(), key=lambda item: item[0].lower()):
        for include_text, line_no in sorted(items, key=lambda item: item[1]):
            lines.append(
                f"unresolved\t{escape_index_field(src)}\t{line_no}\t{escape_index_field(include_text)}"
            )
    return "\n".join(lines) + "\n"


def load_layers(path: Path) -> list[LayerRule]:
    data = json.loads(path.read_text(encoding="utf-8"))
    rules = []
    for item in data.get("layers", []):
        rules.append(LayerRule(
            name=item["name"],
            paths=[p.replace("\\", "/") for p in item.get("paths", [])],
            may_depend_on=set(item.get("may_depend_on", [])),
        ))
    return rules


def file_layer(file_path: str, rules: list[LayerRule]) -> str | None:
    normalized = file_path.replace("\\", "/")
    matches = []
    for rule in rules:
        for prefix in rule.paths:
            if normalized.startswith(prefix):
                matches.append((len(prefix), rule.name))
    if not matches:
        return None
    return max(matches)[1]


def detect_layer_violations(result: AnalysisResult, rules: list[LayerRule]) -> list[tuple[str, str, str, str]]:
    by_name = {r.name: r for r in rules}
    violations: list[tuple[str, str, str, str]] = []

    for edge in result.edges:
        src_layer = file_layer(edge.src, rules)
        dst_layer = file_layer(edge.dst, rules)
        if src_layer is None or dst_layer is None or src_layer == dst_layer:
            continue
        allowed = by_name[src_layer].may_depend_on
        if dst_layer not in allowed:
            violations.append((edge.src, src_layer, edge.dst, dst_layer))

    return violations


def make_report(result: AnalysisResult, cycles: list[list[str]], layer_violations: list[tuple[str, str, str, str]] | None) -> str:
    lines: list[str] = []

    lines.append("# C/C++ Local Include Dependency Report")
    lines.append("")
    lines.append(f"Root: `{result.root}`")
    lines.append(f"Files scanned: **{len(result.files)}**")
    lines.append(f"Resolved local include edges: **{len(result.edges)}**")
    lines.append(f"Files with unresolved local includes: **{len(result.unresolved)}**")
    lines.append("")

    lines.append("## 1. File Dependency Graph")
    lines.append("")
    if result.edges:
        for edge in sorted(result.edges, key=lambda e: (e.src.lower(), e.dst.lower(), e.line_no)):
            lines.append(f"- `{edge.src}` -> `{edge.dst}`  ")
            lines.append(f"  - line {edge.line_no}: `#include \"{edge.include_text}\"`")
    else:
        lines.append("No project-local `#include \"...\"` edges were resolved.")
    lines.append("")

    lines.append("## 2. Reverse References")
    lines.append("")
    any_reverse = False
    for dst in sorted(result.files, key=str.lower):
        refs = sorted(result.reverse_graph.get(dst, set()), key=str.lower)
        if not refs:
            continue
        any_reverse = True
        lines.append(f"### `{dst}`")
        for src in refs:
            lines.append(f"- included by `{src}`")
        lines.append("")
    if not any_reverse:
        lines.append("No reverse references found.")
        lines.append("")

    lines.append("## 3. Cycle Detection")
    lines.append("")
    if cycles:
        for i, cycle in enumerate(cycles, start=1):
            chain = " -> ".join(f"`{x}`" for x in cycle)
            lines.append(f"{i}. {chain}")
    else:
        lines.append("No cycles detected.")
    lines.append("")

    lines.append("## 4. Central Files")
    lines.append("")
    lines.append("| file | included by | includes | total |")
    lines.append("|---|---:|---:|---:|")
    for f, incoming, outgoing, total in centrality_ranking(result)[:30]:
        lines.append(f"| `{f}` | {incoming} | {outgoing} | {total} |")
    lines.append("")

    lines.append("## 5. Layer Violations")
    lines.append("")
    if layer_violations is None:
        lines.append("Layer check was not enabled. Pass `--layers layers.json` to enable it.")
    elif not layer_violations:
        lines.append("No layer violations detected.")
    else:
        lines.append("| source | source layer | destination | destination layer |")
        lines.append("|---|---|---|---|")
        for src, src_layer, dst, dst_layer in layer_violations:
            lines.append(f"| `{src}` | `{src_layer}` | `{dst}` | `{dst_layer}` |")
    lines.append("")

    if result.unresolved:
        lines.append("## Unresolved Local Includes")
        lines.append("")
        lines.append("These are `#include \"...\"` entries that could not be resolved inside the project root.")
        lines.append("")
        for src, items in sorted(result.unresolved.items(), key=lambda x: x[0].lower()):
            lines.append(f"### `{src}`")
            for include_text, line_no in items:
                lines.append(f"- line {line_no}: `#include \"{include_text}\"`")
            lines.append("")

    return "\n".join(lines)


def write_text_atomically(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="",
            dir=str(path.parent),
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as temp_file:
            temp_path = Path(temp_file.name)
            temp_file.write(text)
            temp_file.flush()
            os.fsync(temp_file.fileno())
        os.replace(temp_path, path)
        temp_path = None
    finally:
        if temp_path is not None:
            try:
                temp_path.unlink()
            except FileNotFoundError:
                pass


def write_or_print(text: str, path: Path | None) -> None:
    if path is None:
        print(text, end="")
    else:
        write_text_atomically(path, text)


def validate_output_paths(result: AnalysisResult, outputs: list[tuple[str, Path | None]]) -> str | None:
    source_paths = {(result.root / rel).resolve() for rel in result.files}
    seen: dict[Path, str] = {}
    for label, path in outputs:
        if path is None:
            continue
        resolved = path.resolve()
        if resolved in seen:
            return f"{seen[resolved]} and {label} output paths must be different."
        if resolved in source_paths:
            return f"{label} output path must not replace a scanned source file."
        seen[resolved] = label
    return None


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Visualize project-local C/C++ #include \"...\" dependencies."
    )
    parser.add_argument("root", type=Path, help="Project root directory")
    parser.add_argument("--format", choices=["mermaid", "dot"], default="mermaid", help="Graph output format")
    parser.add_argument("--out", type=Path, help="Write graph output to this file. If omitted, print to stdout.")
    parser.add_argument("--graph", type=Path, help="Alias of --out, useful when also writing --report")
    parser.add_argument("--report", type=Path, help="Write Markdown analysis report")
    parser.add_argument("--index", type=Path, help="Write compact TSV include search index")
    parser.add_argument("--layers", type=Path, help="Optional layers.json for layer violation detection")
    parser.add_argument(
        "--include-root",
        type=Path,
        action="append",
        default=[],
        help="Project-local include directory, similar to compiler -I. Can be used multiple times.",
    )
    parser.add_argument(
        "--ignore-dir",
        action="append",
        default=[],
        help="Directory name to ignore. Can be used multiple times. Defaults include common build dirs.",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    configure_utf8_output()
    args = parse_args(argv)
    root: Path = args.root
    if not root.exists() or not root.is_dir():
        print(f"error: root is not a directory: {root}", file=sys.stderr)
        return 2

    ignore_dirs = {
        ".git", ".svn", ".hg", ".vs", ".vscode",
        "out",
        "build", "cmake-build-debug", "cmake-build-release",
        "Debug", "Release", "x64", "x86",
        "node_modules", "vendor",
        "__pycache__",
    }
    ignore_dirs.update(args.ignore_dir)

    result = analyze_project(root, ignore_dirs, args.include_root)
    cycles = find_cycles(result.graph)

    layer_violations: list[tuple[str, str, str, str]] | None = None
    if args.layers:
        rules = load_layers(args.layers)
        layer_violations = detect_layer_violations(result, rules)

    graph_path = args.graph or args.out
    output_error = validate_output_paths(
        result,
        [("--graph/--out", graph_path), ("--report", args.report), ("--index", args.index)],
    )
    if output_error:
        print(f"error: {output_error}", file=sys.stderr)
        return 2

    if graph_path is not None or (args.report is None and args.index is None):
        if args.format == "mermaid":
            graph_text = emit_mermaid(result)
        else:
            graph_text = emit_dot(result)
        write_or_print(graph_text, graph_path)

    if args.report:
        report_text = make_report(result, cycles, layer_violations)
        write_or_print(report_text, args.report)

    if args.index:
        write_text_atomically(args.index, emit_search_index(result))

    if args.report or args.index or graph_path:
        print(f"scanned files: {len(result.files)}")
        print(f"resolved edges: {len(result.edges)}")
        print(f"cycles: {len(cycles)}")
        if layer_violations is not None:
            print(f"layer violations: {len(layer_violations)}")
        if result.unresolved:
            print(f"unresolved include files: {len(result.unresolved)}")
        if args.index:
            print(f"index: {args.index.resolve()}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
