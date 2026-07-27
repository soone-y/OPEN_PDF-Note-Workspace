#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
md_structure_scanner.py

再帰的に Markdown(.md/.markdown) を取得し、見出しと構造を抽出するローカル開発補助ツール。

特徴:
- 標準ライブラリのみで動作
- 外部通信を行わず、既定実行では入力も出力先も変更しない
- CommonMark風の ATX 見出し (#, ##, ###...) を抽出
- Setext見出しにも対応
- fenced code block 内の # を見出しとして誤検出しない
- YAML front matter を任意でスキップ
- .git, third_party, .local, out, workspace 等を既定で除外
- ファイル単位の見出しツリーを生成
- 全体統合ツリーを生成
- 明示指定時のみ検索索引 / JSON / Markdown 目次を原子的に出力
- 見出しの欠落・階層飛びなどの簡易診断を出力

使い方:
    python md_structure_scanner.py .
    python md_structure_scanner.py . --index out/md_structure_index.tsv
    python md_structure_scanner.py . --json out/md_structure.json --toc out/md_structure_toc.md
    python md_structure_scanner.py . --max-depth 3 --include-empty
    python md_structure_scanner.py . --no-default-excludes --exclude .venv --exclude dist
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Any, Iterable, Iterator, Optional


# -----------------------------
# Data model
# -----------------------------


@dataclasses.dataclass
class Heading:
    level: int
    text: str
    line: int
    slug: str
    raw: str
    style: str  # "atx" or "setext"

    def to_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


@dataclasses.dataclass
class HeadingNode:
    level: int
    text: str
    line: int
    slug: str
    raw: str
    style: str
    children: list["HeadingNode"]

    @classmethod
    def from_heading(cls, h: Heading) -> "HeadingNode":
        return cls(
            level=h.level,
            text=h.text,
            line=h.line,
            slug=h.slug,
            raw=h.raw,
            style=h.style,
            children=[],
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "level": self.level,
            "text": self.text,
            "line": self.line,
            "slug": self.slug,
            "raw": self.raw,
            "style": self.style,
            "children": [c.to_dict() for c in self.children],
        }


@dataclasses.dataclass
class FileStructure:
    path: str
    absolute_path: str
    size_bytes: int
    sha256: str
    heading_count: int
    headings: list[Heading]
    tree: list[HeadingNode]
    diagnostics: list[str]

    def to_dict(self) -> dict[str, Any]:
        return {
            "path": self.path,
            "absolute_path": self.absolute_path,
            "size_bytes": self.size_bytes,
            "sha256": self.sha256,
            "heading_count": self.heading_count,
            "headings": [h.to_dict() for h in self.headings],
            "tree": [n.to_dict() for n in self.tree],
            "diagnostics": self.diagnostics,
        }


@dataclasses.dataclass
class ProjectStructure:
    root: str
    file_count: int
    heading_count: int
    files: list[FileStructure]
    diagnostics: list[str]

    def to_dict(self) -> dict[str, Any]:
        return {
            "root": self.root,
            "file_count": self.file_count,
            "heading_count": self.heading_count,
            "files": [f.to_dict() for f in self.files],
            "diagnostics": self.diagnostics,
        }


# -----------------------------
# Constants and regex
# -----------------------------


DEFAULT_MD_EXTENSIONS = {".md", ".markdown"}

DEFAULT_EXCLUDE_DIRS = {
    ".git",
    ".hg",
    ".svn",
    ".local",
    ".idea",
    ".vscode",
    ".vs",
    "node_modules",
    "target",
    "build",
    "bin",
    "obj",
    "dist",
    "out",
    "release",
    "third_party",
    "workspace",
    "__resource__",
    ".venv",
    "venv",
    "__pycache__",
}

DEFAULT_EXCLUDE_FILES = {
    "package-lock.json",
    "pnpm-lock.yaml",
    "yarn.lock",
}

# ATX heading:
#   # title
#   ### title ###
# Up to 3 leading spaces are allowed by Markdown.
ATX_HEADING_RE = re.compile(r"^(?P<indent> {0,3})(?P<marks>#{1,6})(?:[ \t]+|$)(?P<body>.*?)(?:[ \t]+#+[ \t]*)?$")

# Setext heading:
#   Title
#   =====
#   Title
#   -----
SETEXT_UNDERLINE_RE = re.compile(r"^ {0,3}(?P<char>=+|-+)[ \t]*$")

FENCE_START_RE = re.compile(r"^ {0,3}(?P<fence>`{3,}|~{3,})(?P<info>.*)$")

HTML_COMMENT_START_RE = re.compile(r"<!--")
HTML_COMMENT_END_RE = re.compile(r"-->")


# -----------------------------
# Utility
# -----------------------------


def normalize_path(path: Path) -> str:
    return path.as_posix()


def read_text_safely(path: Path) -> str:
    """UTF-8優先。失敗時はBOMやCP932も試す。最後は置換で読む。"""
    encodings = ["utf-8", "utf-8-sig", "cp932"]
    for enc in encodings:
        try:
            return path.read_text(encoding=enc)
        except UnicodeDecodeError:
            continue
    return path.read_text(encoding="utf-8", errors="replace")


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def print_console(text: str = "", *, stream: Any = None) -> None:
    """Windows の既定コードページにない文書文字でも CLI を停止させない。"""
    target = stream if stream is not None else sys.stdout
    encoding = getattr(target, "encoding", None)
    if encoding:
        text = text.encode(encoding, errors="backslashreplace").decode(encoding)
    print(text, file=target)


def configure_utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def slugify(text: str, used: dict[str, int]) -> str:
    """
    GitHub風に近い簡易 slug。
    日本語は残す。記号を削り、空白を - にする。
    同名見出しには -1, -2 を付ける。
    """
    s = text.strip().lower()
    s = re.sub(r"[`*_~\\]", "", s)
    s = re.sub(r"[^\w\s\-ぁ-んァ-ン一-龥々ー・。、「」『』（）()\[\]]", "", s, flags=re.UNICODE)
    s = re.sub(r"\s+", "-", s)
    s = s.strip("-")
    if not s:
        s = "heading"

    count = used.get(s, 0)
    used[s] = count + 1
    if count == 0:
        return s
    return f"{s}-{count}"


def strip_inline_markup(text: str) -> str:
    """見出しテキスト用の軽い整形。Markdownを完全解釈はしない。"""
    text = text.strip()
    # inline code marks only
    text = re.sub(r"`([^`]*)`", r"\1", text)
    # emphasis marks
    text = text.replace("**", "").replace("__", "")
    text = text.replace("*", "").replace("_", "")
    # markdown links [text](url) -> text
    text = re.sub(r"\[([^\]]+)\]\([^\)]+\)", r"\1", text)
    return text.strip()


def is_probably_setext_content(line: str) -> bool:
    stripped = line.strip()
    if not stripped:
        return False
    if stripped.startswith("#"):
        return False
    if stripped.startswith(">"):
        return False
    if stripped.startswith("-") or stripped.startswith("*") or stripped.startswith("+"):
        return False
    if re.match(r"^\d+[.)]\s+", stripped):
        return False
    return True


# -----------------------------
# Recursive file discovery
# -----------------------------


def should_exclude_dir(path: Path, exclude_dirs: set[str]) -> bool:
    return path.name in exclude_dirs


def should_exclude_file(path: Path, exclude_files: set[str]) -> bool:
    return path.name in exclude_files


def iter_markdown_files(
    root: Path,
    extensions: set[str],
    exclude_dirs: set[str],
    exclude_files: set[str],
    follow_symlinks: bool = False,
) -> Iterator[Path]:
    """
    os.walkではなくPath.rglob相当の制御を自前で行う。
    除外ディレクトリを探索前に弾けるため、大規模プロジェクトで少し有利。
    """
    root = root.resolve()
    stack = [root]

    while stack:
        current = stack.pop()
        try:
            entries = sorted(current.iterdir(), key=lambda p: (not p.is_dir(), p.name.lower()))
        except PermissionError:
            continue
        except OSError:
            continue

        for entry in entries:
            try:
                if entry.is_symlink() and not follow_symlinks:
                    continue
                if entry.is_dir():
                    if not should_exclude_dir(entry, exclude_dirs):
                        stack.append(entry)
                elif entry.is_file():
                    if should_exclude_file(entry, exclude_files):
                        continue
                    if entry.suffix.lower() in extensions:
                        yield entry.resolve()
            except OSError:
                continue


# -----------------------------
# Markdown heading extraction
# -----------------------------


def split_front_matter(lines: list[str]) -> tuple[int, int]:
    """
    YAML front matter の範囲を返す。
    戻り値: (start_line_index, end_line_index_exclusive)
    存在しなければ (-1, -1)
    """
    if not lines:
        return -1, -1
    if lines[0].strip() != "---":
        return -1, -1
    for i in range(1, min(len(lines), 300)):
        if lines[i].strip() in {"---", "..."}:
            return 0, i + 1
    return -1, -1


def extract_headings_from_text(text: str, skip_front_matter: bool = True) -> list[Heading]:
    lines = text.splitlines()
    used_slugs: dict[str, int] = {}
    headings: list[Heading] = []

    fm_start, fm_end = split_front_matter(lines) if skip_front_matter else (-1, -1)

    in_fence = False
    fence_char = ""
    fence_len = 0
    in_html_comment = False

    prev_line: Optional[str] = None
    prev_line_no: Optional[int] = None

    for idx, line in enumerate(lines):
        line_no = idx + 1

        if fm_start <= idx < fm_end:
            continue

        # HTML comment block. Markdown見出し検出の邪魔になるケースを簡易的に抑える。
        if in_html_comment:
            if HTML_COMMENT_END_RE.search(line):
                in_html_comment = False
            prev_line = None
            prev_line_no = None
            continue
        if HTML_COMMENT_START_RE.search(line) and not HTML_COMMENT_END_RE.search(line):
            in_html_comment = True
            prev_line = None
            prev_line_no = None
            continue

        # fenced code block handling
        fence_match = FENCE_START_RE.match(line)
        if fence_match:
            fence = fence_match.group("fence")
            ch = fence[0]
            ln = len(fence)
            if not in_fence:
                in_fence = True
                fence_char = ch
                fence_len = ln
            else:
                # closing fence must use same char and length >= opening
                if ch == fence_char and ln >= fence_len:
                    in_fence = False
                    fence_char = ""
                    fence_len = 0
            prev_line = None
            prev_line_no = None
            continue

        if in_fence:
            continue

        # ATX heading
        m = ATX_HEADING_RE.match(line)
        if m:
            level = len(m.group("marks"))
            raw_body = m.group("body").strip()
            clean = strip_inline_markup(raw_body)
            slug = slugify(clean, used_slugs)
            headings.append(
                Heading(
                    level=level,
                    text=clean,
                    line=line_no,
                    slug=slug,
                    raw=line,
                    style="atx",
                )
            )
            prev_line = None
            prev_line_no = None
            continue

        # Setext heading: previous line + underline
        sm = SETEXT_UNDERLINE_RE.match(line)
        if sm and prev_line is not None and prev_line_no is not None:
            if is_probably_setext_content(prev_line):
                level = 1 if sm.group("char").startswith("=") else 2
                clean = strip_inline_markup(prev_line.strip())
                slug = slugify(clean, used_slugs)
                headings.append(
                    Heading(
                        level=level,
                        text=clean,
                        line=prev_line_no,
                        slug=slug,
                        raw=prev_line,
                        style="setext",
                    )
                )
            prev_line = None
            prev_line_no = None
            continue

        if line.strip():
            prev_line = line
            prev_line_no = line_no
        else:
            prev_line = None
            prev_line_no = None

    return headings


# -----------------------------
# Tree builder and diagnostics
# -----------------------------


def build_heading_tree(headings: list[Heading]) -> list[HeadingNode]:
    root: list[HeadingNode] = []
    stack: list[HeadingNode] = []

    for h in headings:
        node = HeadingNode.from_heading(h)

        while stack and stack[-1].level >= node.level:
            stack.pop()

        if stack:
            stack[-1].children.append(node)
        else:
            root.append(node)

        stack.append(node)

    return root


def diagnose_headings(headings: list[Heading]) -> list[str]:
    diagnostics: list[str] = []
    if not headings:
        diagnostics.append("見出しがありません。")
        return diagnostics

    first = headings[0]
    if first.level != 1:
        diagnostics.append(f"最初の見出しが H1 ではありません: line {first.line}, H{first.level} {first.text!r}")

    h1_count = sum(1 for h in headings if h.level == 1)
    if h1_count == 0:
        diagnostics.append("H1 がありません。")
    elif h1_count > 1:
        diagnostics.append(f"H1 が複数あります: {h1_count}個")

    prev_level = headings[0].level
    for h in headings[1:]:
        if h.level > prev_level + 1:
            diagnostics.append(
                f"見出し階層が飛んでいます: line {h.line}, H{prev_level} の後に H{h.level} {h.text!r}"
            )
        prev_level = h.level

    duplicate_texts: dict[str, int] = {}
    for h in headings:
        key = h.text.strip().lower()
        duplicate_texts[key] = duplicate_texts.get(key, 0) + 1
    for text, count in sorted(duplicate_texts.items()):
        if text and count >= 3:
            diagnostics.append(f"同名または類似の見出しが多い可能性があります: {text!r} が {count}回")

    return diagnostics


# -----------------------------
# File/project analysis
# -----------------------------


def analyze_file(path: Path, root: Path, skip_front_matter: bool = True) -> FileStructure:
    text = read_text_safely(path)
    headings = extract_headings_from_text(text, skip_front_matter=skip_front_matter)
    tree = build_heading_tree(headings)
    diagnostics = diagnose_headings(headings)
    rel = normalize_path(path.relative_to(root.resolve()))

    return FileStructure(
        path=rel,
        absolute_path=str(path),
        size_bytes=path.stat().st_size,
        sha256=file_sha256(path),
        heading_count=len(headings),
        headings=headings,
        tree=tree,
        diagnostics=diagnostics,
    )


def analyze_project(
    root: Path,
    extensions: set[str],
    exclude_dirs: set[str],
    exclude_files: set[str],
    follow_symlinks: bool,
    skip_front_matter: bool,
    include_empty: bool,
) -> ProjectStructure:
    root = root.resolve()
    files: list[FileStructure] = []
    diagnostics: list[str] = []

    for md_path in iter_markdown_files(
        root=root,
        extensions=extensions,
        exclude_dirs=exclude_dirs,
        exclude_files=exclude_files,
        follow_symlinks=follow_symlinks,
    ):
        try:
            fs = analyze_file(md_path, root=root, skip_front_matter=skip_front_matter)
            if include_empty or fs.heading_count > 0:
                files.append(fs)
        except Exception as e:  # ツール用途なので、1ファイル失敗で全体停止しない
            rel = normalize_path(md_path.relative_to(root)) if md_path.is_relative_to(root) else str(md_path)
            diagnostics.append(f"解析失敗: {rel}: {type(e).__name__}: {e}")

    files.sort(key=lambda f: f.path.lower())
    total_headings = sum(f.heading_count for f in files)

    if not files:
        diagnostics.append("対象Markdownファイルが見つかりませんでした。")

    return ProjectStructure(
        root=str(root),
        file_count=len(files),
        heading_count=total_headings,
        files=files,
        diagnostics=diagnostics,
    )


# -----------------------------
# Markdown TOC output
# -----------------------------


def render_file_toc(
    file: FileStructure,
    max_depth: int = 6,
    include_lines: bool = True,
    source_prefix: str = "",
) -> str:
    lines: list[str] = []
    lines.append(f"## `{file.path}`")
    lines.append("")

    if not file.headings:
        lines.append("_見出しなし_")
        lines.append("")
        return "\n".join(lines)

    for h in file.headings:
        if h.level > max_depth:
            continue
        indent = "  " * max(h.level - 1, 0)
        line_note = f" :{h.line}" if include_lines else ""
        # ファイル内リンク。GitHub等でおおむね機能する形式。
        lines.append(f"{indent}- [{h.text}]({source_prefix}{file.path}#{h.slug}){line_note}")

    if file.diagnostics:
        lines.append("")
        lines.append("**Diagnostics**")
        for d in file.diagnostics:
            lines.append(f"- {d}")

    lines.append("")
    return "\n".join(lines)


def render_project_toc(
    project: ProjectStructure,
    max_depth: int = 6,
    include_lines: bool = True,
    source_prefix: str = "",
) -> str:
    lines: list[str] = []
    lines.append("# Markdown Structure TOC")
    lines.append("")
    lines.append(f"- Root: `{project.root}`")
    lines.append(f"- Files: {project.file_count}")
    lines.append(f"- Headings: {project.heading_count}")
    lines.append("")

    if project.diagnostics:
        lines.append("## Project Diagnostics")
        lines.append("")
        for d in project.diagnostics:
            lines.append(f"- {d}")
        lines.append("")

    lines.append("## Files")
    lines.append("")
    for f in project.files:
        lines.append(f"- `{f.path}`: {f.heading_count} headings")
    lines.append("")

    lines.append("## Structure")
    lines.append("")
    for f in project.files:
        lines.append(
            render_file_toc(
                f,
                max_depth=max_depth,
                include_lines=include_lines,
                source_prefix=source_prefix,
            )
        )

    return "\n".join(lines).rstrip() + "\n"


def escape_index_field(text: str) -> str:
    return text.replace("\\", "\\\\").replace("\t", "\\t").replace("\r", "\\r").replace("\n", "\\n")


def render_search_index(project: ProjectStructure) -> str:
    """Render a compact, line-oriented heading index intended for targeted text search."""
    lines = [
        "# md_structure_scanner search index v1",
        "# Generated locator only; read the source Markdown before relying on its content.",
        "# columns: path<TAB>line<TAB>level<TAB>heading",
    ]
    for file in project.files:
        path = escape_index_field(file.path)
        for heading in file.headings:
            lines.append(f"{path}\t{heading.line}\tH{heading.level}\t{escape_index_field(heading.text)}")
    return "\n".join(lines) + "\n"


# -----------------------------
# CLI
# -----------------------------


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Recursively scan Markdown files and extract heading structures.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("root", nargs="?", default=".", help="探索ルートディレクトリ")
    parser.add_argument("--index", dest="index_path", help="検索用 TSV 索引の出力先。指定時のみ原子的に生成する")
    parser.add_argument("--json", dest="json_path", help="JSON出力先。指定時のみ原子的に生成する")
    parser.add_argument("--toc", dest="toc_path", help="Markdown目次出力先。指定時のみ原子的に生成する")
    parser.add_argument("--ext", action="append", default=[], help="対象拡張子。例: --ext .md --ext .mdx")
    parser.add_argument("--exclude", action="append", default=[], help="除外ディレクトリ名またはファイル名。複数指定可")
    parser.add_argument("--no-default-excludes", action="store_true", help="既定除外を使わない")
    parser.add_argument("--follow-symlinks", action="store_true", help="シンボリックリンクを辿る")
    parser.add_argument("--include-empty", action="store_true", help="見出しがないMarkdownファイルも出力に含める")
    parser.add_argument("--no-front-matter-skip", action="store_true", help="YAML front matter をスキップしない")
    parser.add_argument("--max-depth", type=int, default=6, choices=range(1, 7), help="TOCに出す最大見出しレベル")
    parser.add_argument("--no-line", action="store_true", help="Markdown TOCに行番号を出さない")
    parser.add_argument("--stdout", action="store_true", help="Markdown TOCを標準出力にも表示する")
    return parser.parse_args(argv)


def ensure_parent(path: Path) -> None:
    if path.parent and not path.parent.exists():
        path.parent.mkdir(parents=True, exist_ok=True)


def write_text_atomically(path: Path, text: str) -> None:
    """Write a generated report without exposing a partially written file."""
    ensure_parent(path)
    temp_path: Optional[Path] = None
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


def output_would_be_markdown_input(
    path: Path,
    root: Path,
    extensions: set[str],
    exclude_dirs: set[str],
    exclude_files: set[str],
) -> bool:
    """Prevent a generated output from replacing or joining scanned source docs."""
    resolved_path = path.resolve()
    try:
        relative = resolved_path.relative_to(root.resolve())
    except ValueError:
        return False
    if resolved_path.suffix.lower() not in extensions or resolved_path.name in exclude_files:
        return False
    return not any(part in exclude_dirs for part in relative.parts[:-1])


def source_prefix_for_toc(root: Path, toc_path: Path) -> str:
    relative_root = Path(os.path.relpath(root.resolve(), toc_path.resolve().parent)).as_posix()
    return "" if relative_root == "." else f"{relative_root}/"


def print_run_summary(
    project: ProjectStructure,
    index_path: Optional[Path],
    json_path: Optional[Path],
    toc_path: Optional[Path],
    *,
    stream: Any = None,
) -> None:
    print_console(f"root: {project.root}", stream=stream)
    print_console(f"files: {project.file_count}", stream=stream)
    print_console(f"headings: {project.heading_count}", stream=stream)
    if index_path or json_path or toc_path:
        print_console("outputs:", stream=stream)
        if index_path:
            print_console(f"  index: {index_path.resolve()}", stream=stream)
        if json_path:
            print_console(f"  json: {json_path.resolve()}", stream=stream)
        if toc_path:
            print_console(f"  toc: {toc_path.resolve()}", stream=stream)
    else:
        print_console(
            "outputs: none (summary only; pass --index out/md_structure_index.tsv "
            "to write the search index)",
            stream=stream,
        )
    if project.diagnostics:
        print_console("diagnostics:", stream=stream)
        for diagnostic in project.diagnostics:
            print_console(f"  - {diagnostic}", stream=stream)


def main(argv: Optional[list[str]] = None) -> int:
    configure_utf8_output()
    args = parse_args(argv)
    root = Path(args.root)
    if not root.exists():
        print_console(f"error: root not found: {root}", stream=sys.stderr)
        return 2
    if not root.is_dir():
        print_console(f"error: root is not directory: {root}", stream=sys.stderr)
        return 2

    extensions = set(DEFAULT_MD_EXTENSIONS)
    if args.ext:
        extensions = {e if e.startswith(".") else f".{e}" for e in args.ext}
        extensions = {e.lower() for e in extensions}

    if args.no_default_excludes:
        exclude_dirs: set[str] = set()
        exclude_files: set[str] = set()
    else:
        exclude_dirs = set(DEFAULT_EXCLUDE_DIRS)
        exclude_files = set(DEFAULT_EXCLUDE_FILES)

    for x in args.exclude:
        # ディレクトリかファイルかは名前だけでは完全判定できないため両方に入れる。
        exclude_dirs.add(x)
        exclude_files.add(x)

    index_path = Path(args.index_path) if args.index_path else None
    json_path = Path(args.json_path) if args.json_path else None
    toc_path = Path(args.toc_path) if args.toc_path else None
    output_paths = [
        (label, path)
        for label, path in (("--index", index_path), ("--json", json_path), ("--toc", toc_path))
        if path
    ]
    resolved_outputs: dict[Path, str] = {}
    for label, path in output_paths:
        resolved = path.resolve()
        if resolved in resolved_outputs:
            print_console(
                f"error: {resolved_outputs[resolved]} and {label} output paths must be different.",
                stream=sys.stderr,
            )
            return 2
        resolved_outputs[resolved] = label
        if output_would_be_markdown_input(path, root, extensions, exclude_dirs, exclude_files):
            print_console(
                f"error: {label} must be outside scanned Markdown inputs; use an excluded generated-output "
                "directory such as out/.",
                stream=sys.stderr,
            )
            return 2

    project = analyze_project(
        root=root,
        extensions=extensions,
        exclude_dirs=exclude_dirs,
        exclude_files=exclude_files,
        follow_symlinks=args.follow_symlinks,
        skip_front_matter=not args.no_front_matter_skip,
        include_empty=args.include_empty,
    )

    if index_path:
        write_text_atomically(index_path, render_search_index(project))

    if json_path:
        write_text_atomically(
            json_path,
            json.dumps(project.to_dict(), ensure_ascii=False, indent=2) + "\n",
        )

    toc: Optional[str] = None
    if toc_path or args.stdout:
        toc = render_project_toc(
            project,
            max_depth=args.max_depth,
            include_lines=not args.no_line,
            source_prefix=source_prefix_for_toc(root, toc_path) if toc_path else "",
        )

    if toc_path and toc is not None:
        write_text_atomically(toc_path, toc)

    if args.stdout:
        print_console(toc or "")
        print_run_summary(project, index_path, json_path, toc_path, stream=sys.stderr)
    else:
        print_run_summary(project, index_path, json_path, toc_path)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
