#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import keyword
import os
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


TEXT_EXTENSIONS = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".inl",
    ".ipp",
    ".inc",
    ".cppinc",
    ".py",
    ".ps1",
    ".md",
    ".txt",
    ".json",
    ".yml",
    ".yaml",
    ".cmake",
    ".vcxproj",
    ".props",
    ".targets",
    ".filters",
    ".gitignore",
}

CPP_FUNCTION_RE = re.compile(
    r"""
    ^\s*
    (?!if\b|for\b|while\b|switch\b|catch\b|return\b|else\b|class\b|struct\b|enum\b|namespace\b)
    (?:(?:template\s*<[^;{}]+>\s*)?)?
    (?:
        [\w:\<\>\~\*&,\s]+
    )?
    \b
    (?P<name>[A-Za-z_~]\w*(?:::\w+)*)
    \s*
    \([^;{}]*\)
    \s*
    (?:
        const\b\s*
    )?
    (?:
        noexcept\b(?:\s*\([^)]*\))?\s*
    )?
    (?:
        ->\s*[\w:\<\>\~\*&,\s]+\s*
    )?
    $
    """,
    re.VERBOSE,
)

PY_FUNCTION_RE = re.compile(r"^\s*(?:async\s+)?def\s+(?P<name>[A-Za-z_]\w*)\s*\(")
PY_CLASS_RE = re.compile(r"^\s*class\s+[A-Za-z_]\w*")
CPP_TYPE_RE = re.compile(r"^\s*(?:class|struct|enum(?:\s+class)?)\s+[A-Za-z_]\w*")
CPP_INCLUDE_RE = re.compile(r"^\s*#\s*include\s+[<\"].+[>\"]")
PY_IMPORT_RE = re.compile(r"^\s*(?:from\s+[A-Za-z_][\w\.]*\s+import\b|import\s+[A-Za-z_][\w\.,\s]*)")
POWERSHELL_FUNCTION_RE = re.compile(r"^\s*function\s+[A-Za-z_][\w-]*\s*\{?", re.IGNORECASE)
TODO_RE = re.compile(r"\b(?:TODO|FIXME|NOTE|BUG|HACK)\b", re.IGNORECASE)
IDENTIFIER_RE = re.compile(r"\b[A-Za-z_]\w*\b")
CPP_VAR_DECL_RE = re.compile(
    r"""
    ^\s*
    (?!return\b|if\b|for\b|while\b|switch\b|catch\b|class\b|struct\b|enum\b|namespace\b|using\b|typedef\b)
    (?:(?:const|constexpr|static|mutable|volatile|inline|extern|register)\s+)*
    [A-Za-z_]\w*(?:::\w+)*(?:\s*<[^;{}()]+>)?
    (?:\s*[\*&])*
    \s+
    (?P<name>[A-Za-z_]\w*)
    \s*(?:[=;\[,)\{]|$)
    """,
    re.VERBOSE,
)
PY_ASSIGN_RE = re.compile(r"^\s*(?P<name>[A-Za-z_]\w*)\s*=")
PY_FOR_TARGET_RE = re.compile(r"^\s*for\s+(?P<name>[A-Za-z_]\w*)\s+in\b")
CPP_KEYWORDS = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case",
    "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval",
    "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
    "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
    "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new",
    "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected", "public",
    "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
    "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local", "throw", "true",
    "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
    "wchar_t", "while", "xor", "xor_eq",
}
COMMON_IGNORE_IDENTIFIERS = {
    "std", "size_t", "wstring", "string", "vector", "optional", "unique_ptr", "shared_ptr", "nullptr",
    "true", "false", "null", "self", "cls", "args", "kwargs",
}

DEFAULT_OWN_INCLUDE = ("src", "tests", "scripts", "tools", "docs", "README.md", "AGENTS.md")
DEFAULT_ALL_EXCLUDE_DIRS = {".git"}
DEFAULT_OWN_EXCLUDE_DIRS = {
    ".git",
    ".local",
    ".vscode",
    "out",
    "obj",
    "release",
    "third_party/mingw_runtime_licenses",
    "__pycache__",
}
DEFAULT_OWN_EXCLUDE_PREFIXES = (
    "third_party/pdfium",
    "src/clrop/README.md",
)
DEFAULT_EXCLUDE_SUFFIXES = {
    ".exe",
    ".dll",
    ".lib",
    ".obj",
    ".pdb",
    ".pdf",
    ".png",
    ".jpg",
    ".jpeg",
    ".gif",
    ".zip",
    ".7z",
}


@dataclass
class FileMetrics:
    path: str
    extension: str
    size_bytes: int
    is_text: bool
    lines: int = 0
    chars: int = 0
    blank_lines: int = 0
    comment_lines: int = 0
    code_lines: int = 0
    max_line_length: int = 0
    function_count: int = 0
    type_count: int = 0
    dependency_refs: int = 0
    todo_count: int = 0
    approx_function_decls: int = 0
    approx_function_refs: int = 0
    approx_variable_decls: int = 0
    approx_variable_refs: int = 0
    approx_unused_function_decls: int = 0
    approx_unused_variable_decls: int = 0


@dataclass
class ApproxSymbolMetrics:
    function_decl_counter: Counter[str]
    function_ref_counter: Counter[str]
    variable_decl_counter: Counter[str]
    variable_ref_counter: Counter[str]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Collect deterministic repository size metrics without external access."
    )
    parser.add_argument(
        "--root",
        default=".",
        help="Repository root to analyze. Defaults to the current directory.",
    )
    parser.add_argument(
        "--scope",
        choices=("own", "all"),
        default="own",
        help="own: focus on repository-authored text/code. all: include the wider repository except .git.",
    )
    parser.add_argument(
        "--format",
        choices=("text", "json"),
        default="text",
        help="Output format.",
    )
    parser.add_argument(
        "--top-files",
        type=int,
        default=10,
        help="How many largest files to list in text output.",
    )
    parser.add_argument(
        "--top-dirs",
        type=int,
        default=10,
        help="How many largest directories to list in text output.",
    )
    parser.add_argument(
        "--max-tree-depth",
        type=int,
        default=3,
        help="Tree depth for text output.",
    )
    parser.add_argument(
        "--include",
        action="append",
        default=[],
        help="Additional relative path prefix to include.",
    )
    parser.add_argument(
        "--exclude",
        action="append",
        default=[],
        help="Additional relative path prefix to exclude.",
    )
    return parser.parse_args()


def normalize_relpath(path: Path, root: Path) -> str:
    rel = path.relative_to(root).as_posix()
    return "." if rel == "" else rel


def should_include_root_child(name: str, scope: str, include_roots: Sequence[str]) -> bool:
    if scope == "all":
        return True
    return name in include_roots


def is_excluded_path(rel_path: str, scope: str, extra_excludes: Sequence[str]) -> bool:
    rel_path = rel_path.strip("./")
    if not rel_path:
        return False
    for item in extra_excludes:
        prefix = item.strip("./")
        if rel_path == prefix or rel_path.startswith(prefix + "/"):
            return True
    if scope == "own":
        for prefix in DEFAULT_OWN_EXCLUDE_PREFIXES:
            if rel_path == prefix or rel_path.startswith(prefix + "/"):
                return True
    return False


def is_excluded_dir(name: str, scope: str) -> bool:
    if scope == "all":
        return name in DEFAULT_ALL_EXCLUDE_DIRS
    return name in DEFAULT_OWN_EXCLUDE_DIRS


def is_binary_by_suffix(path: Path) -> bool:
    return path.suffix.lower() in DEFAULT_EXCLUDE_SUFFIXES


def is_text_file(path: Path) -> bool:
    suffix = path.suffix.lower()
    if suffix in TEXT_EXTENSIONS:
        return True
    if path.name in {"CMakeLists.txt", ".gitignore"}:
        return True
    return False


def read_text(path: Path) -> Optional[str]:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        try:
            return path.read_text(encoding="utf-8-sig")
        except UnicodeDecodeError:
            try:
                return path.read_text(encoding="cp932")
            except UnicodeDecodeError:
                try:
                    return path.read_text(encoding="utf-16")
                except UnicodeDecodeError:
                    return None
    except OSError:
        return None


def count_functions(path: Path, text: str) -> int:
    suffix = path.suffix.lower()
    if suffix in {".py"}:
        count = 0
        for line in text.splitlines():
            if PY_FUNCTION_RE.match(line):
                count += 1
        return count
    if suffix in {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".inc", ".cppinc"}:
        return count_cpp_functions(text)
    if suffix in {".ps1"}:
        count = 0
        for line in text.splitlines():
            if POWERSHELL_FUNCTION_RE.match(line):
                count += 1
        return count
    return 0


def count_types(path: Path, text: str) -> int:
    suffix = path.suffix.lower()
    count = 0
    if suffix in {".py"}:
        for line in text.splitlines():
            if PY_CLASS_RE.match(line):
                count += 1
        return count
    if suffix in {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".inc", ".cppinc"}:
        for line in strip_cpp_comments(text.splitlines()):
            if CPP_TYPE_RE.match(line.strip()):
                count += 1
        return count
    return 0


def count_dependency_refs(path: Path, text: str) -> int:
    suffix = path.suffix.lower()
    count = 0
    if suffix in {".py"}:
        for line in text.splitlines():
            if PY_IMPORT_RE.match(line):
                count += 1
        return count
    if suffix in {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".inc", ".cppinc"}:
        for line in text.splitlines():
            if CPP_INCLUDE_RE.match(line):
                count += 1
        return count
    return 0


def build_approx_symbol_metrics(path: Path, text: str) -> ApproxSymbolMetrics:
    suffix = path.suffix.lower()
    if suffix in {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".inc", ".cppinc"}:
        return analyze_cpp_symbols(text)
    if suffix in {".py"}:
        return analyze_python_symbols(text)
    return ApproxSymbolMetrics(Counter(), Counter(), Counter(), Counter())


def strip_cpp_comments(lines: Sequence[str]) -> List[str]:
    stripped: List[str] = []
    in_block = False
    for line in lines:
        current = line
        output_parts: List[str] = []
        index = 0
        while index < len(current):
            if in_block:
                end = current.find("*/", index)
                if end == -1:
                    index = len(current)
                    break
                in_block = False
                index = end + 2
                continue
            if current.startswith("//", index):
                break
            if current.startswith("/*", index):
                in_block = True
                index += 2
                continue
            output_parts.append(current[index])
            index += 1
        stripped.append("".join(output_parts))
    return stripped


def count_cpp_functions(text: str) -> int:
    count = 0
    brace_depth = 0
    signature_parts: List[str] = []

    for raw_line in strip_cpp_comments(text.splitlines()):
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("#"):
            signature_parts.clear()
            continue

        opens = raw_line.count("{")
        closes = raw_line.count("}")
        pre_depth = brace_depth

        if pre_depth <= 2:
            candidate_source = line
            if "{" in line:
                candidate_source = line.split("{", 1)[0].strip()
            if candidate_source:
                signature_parts.append(candidate_source)

            if "{" in line:
                candidate = " ".join(part for part in signature_parts if part).strip()
                candidate = re.sub(r"\s+", " ", candidate)
                if looks_like_cpp_function_signature(candidate):
                    count += 1
                signature_parts.clear()
            elif ";" in line or "=" in line:
                signature_parts.clear()
        else:
            signature_parts.clear()

        brace_depth += opens - closes
        if brace_depth < 0:
            brace_depth = 0

    return count


def looks_like_cpp_function_signature(candidate: str) -> bool:
    if not candidate:
        return False
    if "[" in candidate and "]" in candidate:
        return False
    if "::" not in candidate and "(" in candidate and candidate.split("(", 1)[0].strip().endswith(("if", "for", "while", "switch", "catch")):
        return False
    if candidate.startswith(("return ", "delete ", "new ")):
        return False
    return CPP_FUNCTION_RE.match(candidate) is not None


def extract_cpp_function_names(text: str) -> List[str]:
    names: List[str] = []
    brace_depth = 0
    signature_parts: List[str] = []
    for raw_line in strip_cpp_comments(text.splitlines()):
        line = raw_line.strip()
        if not line:
            continue
        if line.startswith("#"):
            signature_parts.clear()
            continue
        opens = raw_line.count("{")
        closes = raw_line.count("}")
        if brace_depth <= 2:
            candidate_source = line.split("{", 1)[0].strip() if "{" in line else line
            if candidate_source:
                signature_parts.append(candidate_source)
            if "{" in line:
                candidate = re.sub(r"\s+", " ", " ".join(signature_parts).strip())
                match = CPP_FUNCTION_RE.match(candidate)
                if match:
                    names.append(match.group("name").split("::")[-1])
                signature_parts.clear()
            elif ";" in line or "=" in line:
                signature_parts.clear()
        else:
            signature_parts.clear()
        brace_depth += opens - closes
        if brace_depth < 0:
            brace_depth = 0
    return names


def analyze_cpp_symbols(text: str) -> ApproxSymbolMetrics:
    cleaned_lines = strip_cpp_comments(text.splitlines())
    cleaned_text = "\n".join(cleaned_lines)
    function_decls = Counter(extract_cpp_function_names(text))
    function_refs: Counter[str] = Counter()
    variable_decls: Counter[str] = Counter()
    variable_refs: Counter[str] = Counter()

    for name in function_decls:
        function_refs[name] = len(re.findall(rf"\b{re.escape(name)}\s*\(", cleaned_text))

    for line in cleaned_lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        match = CPP_VAR_DECL_RE.match(stripped)
        if match and "(" not in stripped.split(match.group("name"), 1)[0]:
            variable_decls[match.group("name")] += 1

    for identifier in IDENTIFIER_RE.findall(cleaned_text):
        if identifier in CPP_KEYWORDS or identifier in COMMON_IGNORE_IDENTIFIERS:
            continue
        variable_refs[identifier] += 1

    for name in list(function_decls):
        if name in variable_refs:
            del variable_refs[name]

    return ApproxSymbolMetrics(function_decls, function_refs, variable_decls, variable_refs)


def analyze_python_symbols(text: str) -> ApproxSymbolMetrics:
    function_decls: Counter[str] = Counter()
    function_refs: Counter[str] = Counter()
    variable_decls: Counter[str] = Counter()
    variable_refs: Counter[str] = Counter()

    for line in text.splitlines():
        function_match = PY_FUNCTION_RE.match(line)
        if function_match:
            function_decls[function_match.group("name")] += 1
        assign_match = PY_ASSIGN_RE.match(line)
        if assign_match and not keyword.iskeyword(assign_match.group("name")):
            variable_decls[assign_match.group("name")] += 1
        for_match = PY_FOR_TARGET_RE.match(line)
        if for_match and not keyword.iskeyword(for_match.group("name")):
            variable_decls[for_match.group("name")] += 1

    for name in function_decls:
        function_refs[name] = len(re.findall(rf"\b{re.escape(name)}\s*\(", text))

    for identifier in IDENTIFIER_RE.findall(text):
        if keyword.iskeyword(identifier) or identifier in COMMON_IGNORE_IDENTIFIERS:
            continue
        variable_refs[identifier] += 1

    for name in list(function_decls):
        if name in variable_refs:
            del variable_refs[name]

    return ApproxSymbolMetrics(function_decls, function_refs, variable_decls, variable_refs)


def analyze_text_lines(path: Path, text: str) -> Dict[str, int]:
    suffix = path.suffix.lower()
    lines = text.splitlines()
    blank_lines = 0
    comment_lines = 0
    todo_count = 0
    max_line_length = 0
    in_cpp_block = False

    for raw_line in lines:
        stripped = raw_line.strip()
        max_line_length = max(max_line_length, len(raw_line))
        if TODO_RE.search(raw_line):
            todo_count += 1
        if not stripped:
            blank_lines += 1
            continue

        if suffix in {".py"}:
            if stripped.startswith("#"):
                comment_lines += 1
                continue
        elif suffix in {".ps1"}:
            if stripped.startswith("#"):
                comment_lines += 1
                continue
        elif suffix in {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".inc", ".cppinc"}:
            idx = 0
            has_code = False
            has_comment = False
            while idx < len(raw_line):
                if in_cpp_block:
                    has_comment = True
                    end = raw_line.find("*/", idx)
                    if end == -1:
                        idx = len(raw_line)
                        break
                    in_cpp_block = False
                    idx = end + 2
                    continue
                if raw_line.startswith("//", idx):
                    has_comment = True
                    break
                if raw_line.startswith("/*", idx):
                    has_comment = True
                    in_cpp_block = True
                    idx += 2
                    continue
                if not raw_line[idx].isspace():
                    has_code = True
                idx += 1
            if has_comment and not has_code:
                comment_lines += 1
                continue
        else:
            if stripped.startswith(("#", "//")):
                comment_lines += 1
                continue

    code_lines = max(len(lines) - blank_lines - comment_lines, 0)
    return {
        "blank_lines": blank_lines,
        "comment_lines": comment_lines,
        "code_lines": code_lines,
        "max_line_length": max_line_length,
        "todo_count": todo_count,
    }


def collect_files(
    root: Path,
    scope: str,
    include_roots: Sequence[str],
    extra_excludes: Sequence[str],
) -> List[FileMetrics]:
    metrics: List[FileMetrics] = []

    for child in sorted(root.iterdir(), key=lambda p: p.name.lower()):
        child_rel = normalize_relpath(child, root)
        if not should_include_root_child(child.name, scope, include_roots):
            continue
        if is_excluded_path(child_rel, scope, extra_excludes):
            continue
        if child.is_dir():
            for current_root, dirnames, filenames in os.walk(child):
                current_path = Path(current_root)
                rel_dir = normalize_relpath(current_path, root)
                dirnames[:] = [
                    name
                    for name in dirnames
                    if not is_excluded_dir(name, scope)
                    and not is_excluded_path(f"{rel_dir}/{name}", scope, extra_excludes)
                ]
                for filename in filenames:
                    file_path = current_path / filename
                    rel_path = normalize_relpath(file_path, root)
                    if is_excluded_path(rel_path, scope, extra_excludes):
                        continue
                    if scope == "own" and is_binary_by_suffix(file_path):
                        continue
                    metrics.append(build_file_metrics(root, file_path))
        elif child.is_file():
            if scope == "own" and is_binary_by_suffix(child):
                continue
            metrics.append(build_file_metrics(root, child))
    return metrics


def build_file_metrics(root: Path, file_path: Path) -> FileMetrics:
    stat = file_path.stat()
    rel_path = normalize_relpath(file_path, root)
    extension = file_path.suffix.lower() or "<no_ext>"
    metric = FileMetrics(
        path=rel_path,
        extension=extension,
        size_bytes=stat.st_size,
        is_text=is_text_file(file_path),
    )
    if metric.is_text:
        text = read_text(file_path)
        if text is not None:
            metric.lines = text.count("\n") + (0 if text == "" else 1)
            metric.chars = len(text)
            line_metrics = analyze_text_lines(file_path, text)
            metric.blank_lines = line_metrics["blank_lines"]
            metric.comment_lines = line_metrics["comment_lines"]
            metric.code_lines = line_metrics["code_lines"]
            metric.max_line_length = line_metrics["max_line_length"]
            metric.todo_count = line_metrics["todo_count"]
            metric.function_count = count_functions(file_path, text)
            metric.type_count = count_types(file_path, text)
            metric.dependency_refs = count_dependency_refs(file_path, text)
            approx = build_approx_symbol_metrics(file_path, text)
            metric.approx_function_decls = sum(approx.function_decl_counter.values())
            metric.approx_function_refs = sum(approx.function_ref_counter.values())
            metric.approx_variable_decls = sum(approx.variable_decl_counter.values())
            metric.approx_variable_refs = sum(approx.variable_ref_counter.values())
            metric.approx_unused_function_decls = sum(
                count for name, count in approx.function_decl_counter.items()
                if approx.function_ref_counter.get(name, 0) <= count
            )
            metric.approx_unused_variable_decls = sum(
                count for name, count in approx.variable_decl_counter.items()
                if approx.variable_ref_counter.get(name, 0) <= count
            )
        else:
            metric.is_text = False
    return metric


def build_directory_totals(files: Sequence[FileMetrics]) -> Dict[str, Dict[str, int]]:
    totals: Dict[str, Dict[str, int]] = defaultdict(
        lambda: {
            "files": 0,
            "size_bytes": 0,
            "lines": 0,
            "chars": 0,
            "blank_lines": 0,
            "comment_lines": 0,
            "code_lines": 0,
            "functions": 0,
            "types": 0,
            "dependency_refs": 0,
            "todo_count": 0,
            "max_line_length": 0,
            "approx_function_decls": 0,
            "approx_function_refs": 0,
            "approx_variable_decls": 0,
            "approx_variable_refs": 0,
            "approx_unused_function_decls": 0,
            "approx_unused_variable_decls": 0,
        }
    )
    for item in files:
        parts = Path(item.path).parts
        if len(parts) == 1:
            parents = ["."]
        else:
            parents = ["."]
            running: List[str] = []
            for part in parts[:-1]:
                running.append(part)
                parents.append("/".join(running))
        for parent in parents:
            totals[parent]["files"] += 1
            totals[parent]["size_bytes"] += item.size_bytes
            totals[parent]["lines"] += item.lines
            totals[parent]["chars"] += item.chars
            totals[parent]["blank_lines"] += item.blank_lines
            totals[parent]["comment_lines"] += item.comment_lines
            totals[parent]["code_lines"] += item.code_lines
            totals[parent]["functions"] += item.function_count
            totals[parent]["types"] += item.type_count
            totals[parent]["dependency_refs"] += item.dependency_refs
            totals[parent]["todo_count"] += item.todo_count
            totals[parent]["max_line_length"] = max(totals[parent]["max_line_length"], item.max_line_length)
            totals[parent]["approx_function_decls"] += item.approx_function_decls
            totals[parent]["approx_function_refs"] += item.approx_function_refs
            totals[parent]["approx_variable_decls"] += item.approx_variable_decls
            totals[parent]["approx_variable_refs"] += item.approx_variable_refs
            totals[parent]["approx_unused_function_decls"] += item.approx_unused_function_decls
            totals[parent]["approx_unused_variable_decls"] += item.approx_unused_variable_decls
    return dict(totals)


def build_extension_totals(files: Sequence[FileMetrics]) -> List[Dict[str, object]]:
    ext_counter: Dict[str, Dict[str, int]] = defaultdict(
        lambda: {
            "files": 0,
            "size_bytes": 0,
            "lines": 0,
            "chars": 0,
            "blank_lines": 0,
            "comment_lines": 0,
            "code_lines": 0,
            "functions": 0,
            "types": 0,
            "dependency_refs": 0,
            "todo_count": 0,
            "max_line_length": 0,
            "approx_function_decls": 0,
            "approx_function_refs": 0,
            "approx_variable_decls": 0,
            "approx_variable_refs": 0,
            "approx_unused_function_decls": 0,
            "approx_unused_variable_decls": 0,
        }
    )
    for item in files:
        bucket = ext_counter[item.extension]
        bucket["files"] += 1
        bucket["size_bytes"] += item.size_bytes
        bucket["lines"] += item.lines
        bucket["chars"] += item.chars
        bucket["blank_lines"] += item.blank_lines
        bucket["comment_lines"] += item.comment_lines
        bucket["code_lines"] += item.code_lines
        bucket["functions"] += item.function_count
        bucket["types"] += item.type_count
        bucket["dependency_refs"] += item.dependency_refs
        bucket["todo_count"] += item.todo_count
        bucket["max_line_length"] = max(bucket["max_line_length"], item.max_line_length)
        bucket["approx_function_decls"] += item.approx_function_decls
        bucket["approx_function_refs"] += item.approx_function_refs
        bucket["approx_variable_decls"] += item.approx_variable_decls
        bucket["approx_variable_refs"] += item.approx_variable_refs
        bucket["approx_unused_function_decls"] += item.approx_unused_function_decls
        bucket["approx_unused_variable_decls"] += item.approx_unused_variable_decls
    rows = []
    for extension, data in ext_counter.items():
        row = {"extension": extension}
        row.update(data)
        rows.append(row)
    rows.sort(key=lambda row: (-int(row["size_bytes"]), str(row["extension"])))
    return rows


def summarize(files: Sequence[FileMetrics]) -> Dict[str, object]:
    total_size = sum(item.size_bytes for item in files)
    total_lines = sum(item.lines for item in files)
    total_chars = sum(item.chars for item in files)
    total_blank_lines = sum(item.blank_lines for item in files)
    total_comment_lines = sum(item.comment_lines for item in files)
    total_code_lines = sum(item.code_lines for item in files)
    total_functions = sum(item.function_count for item in files)
    total_types = sum(item.type_count for item in files)
    total_dependency_refs = sum(item.dependency_refs for item in files)
    total_todo_count = sum(item.todo_count for item in files)
    total_approx_function_decls = sum(item.approx_function_decls for item in files)
    total_approx_function_refs = sum(item.approx_function_refs for item in files)
    total_approx_variable_decls = sum(item.approx_variable_decls for item in files)
    total_approx_variable_refs = sum(item.approx_variable_refs for item in files)
    total_approx_unused_function_decls = sum(item.approx_unused_function_decls for item in files)
    total_approx_unused_variable_decls = sum(item.approx_unused_variable_decls for item in files)
    text_files = sum(1 for item in files if item.is_text)
    binary_files = len(files) - text_files
    empty_files = sum(1 for item in files if item.size_bytes == 0)
    directory_totals = build_directory_totals(files)
    largest_files = sorted(files, key=lambda item: (-item.size_bytes, item.path))
    lines_files = sorted((item for item in files if item.lines > 0), key=lambda item: (-item.lines, item.path))
    function_files = sorted((item for item in files if item.function_count > 0), key=lambda item: (-item.function_count, item.path))
    dependency_files = sorted((item for item in files if item.dependency_refs > 0), key=lambda item: (-item.dependency_refs, item.path))
    todo_files = sorted((item for item in files if item.todo_count > 0), key=lambda item: (-item.todo_count, item.path))
    longest_line_files = sorted((item for item in files if item.max_line_length > 0), key=lambda item: (-item.max_line_length, item.path))
    approx_unused_function_files = sorted(
        (item for item in files if item.approx_unused_function_decls > 0),
        key=lambda item: (-item.approx_unused_function_decls, item.path),
    )
    approx_unused_variable_files = sorted(
        (item for item in files if item.approx_unused_variable_decls > 0),
        key=lambda item: (-item.approx_unused_variable_decls, item.path),
    )

    return {
        "summary": {
            "files": len(files),
            "text_files": text_files,
            "binary_files": binary_files,
            "empty_files": empty_files,
            "size_bytes": total_size,
            "lines": total_lines,
            "chars": total_chars,
            "blank_lines": total_blank_lines,
            "comment_lines": total_comment_lines,
            "code_lines": total_code_lines,
            "functions": total_functions,
            "types": total_types,
            "dependency_refs": total_dependency_refs,
            "todo_count": total_todo_count,
            "approx_function_decls": total_approx_function_decls,
            "approx_function_refs": total_approx_function_refs,
            "approx_variable_decls": total_approx_variable_decls,
            "approx_variable_refs": total_approx_variable_refs,
            "approx_unused_function_decls": total_approx_unused_function_decls,
            "approx_unused_variable_decls": total_approx_unused_variable_decls,
            "avg_bytes_per_file": (total_size / len(files)) if files else 0.0,
            "avg_lines_per_file": (total_lines / text_files) if text_files else 0.0,
            "avg_chars_per_line": (total_chars / total_lines) if total_lines else 0.0,
            "max_line_length": max((item.max_line_length for item in files), default=0),
        },
        "directories": directory_totals,
        "extensions": build_extension_totals(files),
        "largest_files": [file_to_dict(item) for item in largest_files],
        "largest_line_files": [file_to_dict(item) for item in lines_files],
        "largest_function_files": [file_to_dict(item) for item in function_files],
        "largest_dependency_files": [file_to_dict(item) for item in dependency_files],
        "largest_todo_files": [file_to_dict(item) for item in todo_files],
        "longest_line_files": [file_to_dict(item) for item in longest_line_files],
        "approx_unused_function_files": [file_to_dict(item) for item in approx_unused_function_files],
        "approx_unused_variable_files": [file_to_dict(item) for item in approx_unused_variable_files],
        "files": [file_to_dict(item) for item in sorted(files, key=lambda item: item.path)],
    }


def file_to_dict(item: FileMetrics) -> Dict[str, object]:
    return {
        "path": item.path,
        "extension": item.extension,
        "size_bytes": item.size_bytes,
        "is_text": item.is_text,
        "lines": item.lines,
        "chars": item.chars,
        "blank_lines": item.blank_lines,
        "comment_lines": item.comment_lines,
        "code_lines": item.code_lines,
        "max_line_length": item.max_line_length,
        "functions": item.function_count,
        "types": item.type_count,
        "dependency_refs": item.dependency_refs,
        "todo_count": item.todo_count,
        "approx_function_decls": item.approx_function_decls,
        "approx_function_refs": item.approx_function_refs,
        "approx_variable_decls": item.approx_variable_decls,
        "approx_variable_refs": item.approx_variable_refs,
        "approx_unused_function_decls": item.approx_unused_function_decls,
        "approx_unused_variable_decls": item.approx_unused_variable_decls,
    }


def format_bytes(value: int) -> str:
    units = ["B", "KB", "MB", "GB", "TB"]
    size = float(value)
    for unit in units:
        if size < 1024.0 or unit == units[-1]:
            if unit == "B":
                return f"{int(size)} {unit}"
            return f"{size:.1f} {unit}"
        size /= 1024.0
    return f"{value} B"


def format_ratio(part: int, total: int) -> str:
    if total <= 0:
        return "0.0%"
    return f"{(part / total) * 100:.1f}%"


def make_tree_lines(
    directory_totals: Dict[str, Dict[str, int]],
    max_depth: int,
    top_dir_count: int,
) -> List[str]:
    children: Dict[str, List[str]] = defaultdict(list)
    for path in directory_totals:
        if path == ".":
            continue
        parent = "." if "/" not in path else path.rsplit("/", 1)[0]
        children[parent].append(path)

    for path_list in children.values():
        path_list.sort(key=lambda p: (-directory_totals[p]["size_bytes"], p))

    lines = ["."]

    def walk(node: str, depth: int) -> None:
        if depth >= max_depth:
            return
        visible_children = children.get(node, [])
        if top_dir_count > 0:
            visible_children = visible_children[:top_dir_count]
        for child in visible_children:
            info = directory_totals[child]
            indent = "  " * (depth + 1)
            name = child.split("/")[-1]
            lines.append(
                f"{indent}{name}  files={info['files']} size={format_bytes(info['size_bytes'])} lines={info['lines']}"
            )
            walk(child, depth + 1)

    walk(".", 0)
    return lines


def render_text_report(data: Dict[str, object], top_files: int, top_dirs: int, max_tree_depth: int) -> str:
    summary = data["summary"]
    directories = data["directories"]
    extensions = data["extensions"]
    largest_files = data["largest_files"][:top_files]
    largest_line_files = data["largest_line_files"][:top_files]
    largest_function_files = data["largest_function_files"][:top_files]

    summary_lines = [
        "Summary",
        f"  files: {summary['files']}",
        f"  text_files: {summary['text_files']}",
        f"  binary_files: {summary['binary_files']}",
        f"  empty_files: {summary['empty_files']}",
        f"  size: {format_bytes(int(summary['size_bytes']))}",
        f"  lines: {summary['lines']}",
        f"  chars: {summary['chars']}",
        f"  blank_lines: {summary['blank_lines']}",
        f"  comment_lines: {summary['comment_lines']}",
        f"  code_lines: {summary['code_lines']}",
        f"  functions: {summary['functions']}",
        f"  types: {summary['types']}",
        f"  dependency_refs: {summary['dependency_refs']}",
        f"  todo_count: {summary['todo_count']}",
        f"  approx_function_decls: {summary['approx_function_decls']}",
        f"  approx_function_refs: {summary['approx_function_refs']}",
        f"  approx_variable_decls: {summary['approx_variable_decls']}",
        f"  approx_variable_refs: {summary['approx_variable_refs']}",
        f"  approx_unused_function_decls: {summary['approx_unused_function_decls']}",
        f"  approx_unused_variable_decls: {summary['approx_unused_variable_decls']}",
        f"  avg_bytes_per_file: {summary['avg_bytes_per_file']:.1f}",
        f"  avg_lines_per_file: {summary['avg_lines_per_file']:.1f}",
        f"  avg_chars_per_line: {summary['avg_chars_per_line']:.1f}",
        f"  max_line_length: {summary['max_line_length']}",
        "",
        "Directory Tree",
        *[f"  {line}" for line in make_tree_lines(directories, max_tree_depth, top_dirs)],
        "",
        "Top Directories By Size",
    ]

    sorted_dirs = sorted(
        ((path, metrics) for path, metrics in directories.items() if path != "."),
        key=lambda item: (-item[1]["size_bytes"], item[0]),
    )[:top_dirs]
    total_size = int(summary["size_bytes"])
    for path, metrics in sorted_dirs:
        summary_lines.append(
            "  "
            + f"{path}: {format_bytes(metrics['size_bytes'])} "
            + f"({format_ratio(metrics['size_bytes'], total_size)}), files={metrics['files']}, "
            + f"lines={metrics['lines']}, code={metrics['code_lines']}, comments={metrics['comment_lines']}, "
            + f"functions={metrics['functions']}, todos={metrics['todo_count']}, "
            + f"approx_unused_funcs={metrics['approx_unused_function_decls']}, "
            + f"approx_unused_vars={metrics['approx_unused_variable_decls']}"
        )

    summary_lines.extend(["", "Extensions"])
    for row in extensions:
        summary_lines.append(
            "  "
            + f"{row['extension']}: files={row['files']}, size={format_bytes(int(row['size_bytes']))}, "
            + f"lines={row['lines']}, code={row['code_lines']}, comments={row['comment_lines']}, "
            + f"chars={row['chars']}, functions={row['functions']}, types={row['types']}, "
            + f"deps={row['dependency_refs']}, todos={row['todo_count']}, max_line={row['max_line_length']}, "
            + f"approx_func_decls={row['approx_function_decls']}, approx_var_decls={row['approx_variable_decls']}"
        )

    summary_lines.extend(["", "Largest Files"])
    for row in largest_files:
        summary_lines.append(
            "  "
            + f"{row['path']}: {format_bytes(int(row['size_bytes']))}, lines={row['lines']}, "
            + f"functions={row['functions']}, todos={row['todo_count']}"
        )

    summary_lines.extend(["", "Most Lines"])
    for row in largest_line_files:
        summary_lines.append(
            "  "
            + f"{row['path']}: lines={row['lines']}, code={row['code_lines']}, comments={row['comment_lines']}, "
            + f"chars={row['chars']}, size={format_bytes(int(row['size_bytes']))}"
        )

    if largest_function_files:
        summary_lines.extend(["", "Most Functions"])
        for row in largest_function_files:
            summary_lines.append(
                "  "
                + f"{row['path']}: functions={row['functions']}, types={row['types']}, "
                + f"lines={row['lines']}, size={format_bytes(int(row['size_bytes']))}"
            )

    largest_dependency_files = data["largest_dependency_files"][:top_files]
    if largest_dependency_files:
        summary_lines.extend(["", "Most Dependency Refs"])
        for row in largest_dependency_files:
            summary_lines.append(
                "  "
                + f"{row['path']}: deps={row['dependency_refs']}, lines={row['lines']}, size={format_bytes(int(row['size_bytes']))}"
            )

    longest_line_files = data["longest_line_files"][:top_files]
    if longest_line_files:
        summary_lines.extend(["", "Longest Lines"])
        for row in longest_line_files:
            summary_lines.append(
                "  "
                + f"{row['path']}: max_line={row['max_line_length']}, lines={row['lines']}, size={format_bytes(int(row['size_bytes']))}"
            )

    largest_todo_files = data["largest_todo_files"][:top_files]
    if largest_todo_files:
        summary_lines.extend(["", "Most TODOs"])
        for row in largest_todo_files:
            summary_lines.append(
                "  "
                + f"{row['path']}: todos={row['todo_count']}, lines={row['lines']}, size={format_bytes(int(row['size_bytes']))}"
            )

    approx_unused_function_files = data["approx_unused_function_files"][:top_files]
    if approx_unused_function_files:
        summary_lines.extend(["", "Approx Unused Functions"])
        for row in approx_unused_function_files:
            summary_lines.append(
                "  "
                + f"{row['path']}: approx_unused_funcs={row['approx_unused_function_decls']}, "
                + f"approx_func_decls={row['approx_function_decls']}, approx_func_refs={row['approx_function_refs']}"
            )

    approx_unused_variable_files = data["approx_unused_variable_files"][:top_files]
    if approx_unused_variable_files:
        summary_lines.extend(["", "Approx Unused Variables"])
        for row in approx_unused_variable_files:
            summary_lines.append(
                "  "
                + f"{row['path']}: approx_unused_vars={row['approx_unused_variable_decls']}, "
                + f"approx_var_decls={row['approx_variable_decls']}, approx_var_refs={row['approx_variable_refs']}"
            )

    return "\n".join(summary_lines)


def analyze_repository(
    root: Path,
    scope: str = "own",
    include: Optional[Sequence[str]] = None,
    exclude: Optional[Sequence[str]] = None,
) -> Dict[str, object]:
    include_roots = list(DEFAULT_OWN_INCLUDE)
    for item in include or []:
        if item not in include_roots:
            include_roots.append(item)

    files = collect_files(
        root=root,
        scope=scope,
        include_roots=include_roots,
        extra_excludes=list(exclude or []),
    )
    data = summarize(files)
    data["meta"] = {
        "root": str(root),
        "scope": scope,
        "include_roots": include_roots if scope == "own" else ["<all>"],
        "extra_excludes": list(exclude or []),
    }
    return data


def main() -> int:
    args = parse_args()
    root = Path(args.root).resolve()
    if not root.exists() or not root.is_dir():
        print(f"root is not a directory: {root}", file=sys.stderr)
        return 1

    data = analyze_repository(
        root=root,
        scope=args.scope,
        include=args.include,
        exclude=args.exclude,
    )

    if args.format == "json":
        print(json.dumps(data, ensure_ascii=False, indent=2))
        return 0

    print(render_text_report(data, args.top_files, args.top_dirs, args.max_tree_depth))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
