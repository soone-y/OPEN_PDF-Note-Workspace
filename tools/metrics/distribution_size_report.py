#!/usr/bin/env python3
"""Report read-only size shares for a distribution directory or ZIP."""

from __future__ import annotations

import argparse
import json
import sys
import zipfile
from collections import defaultdict
from pathlib import Path
from typing import Sequence


CATEGORY_SUFFIXES = {
    "実行ファイル・ライブラリ": {
        ".exe", ".dll", ".com", ".pyd", ".ocx", ".sys", ".so", ".dylib", ".a", ".lib",
    },
    "ソースコード・パッチ": {
        ".c", ".h", ".cc", ".hh", ".cpp", ".hpp", ".cxx", ".hxx", ".cppinc", ".inc", ".inl",
        ".py", ".pyi", ".ps1", ".psm1", ".bat", ".cmd", ".sh", ".js", ".mjs", ".cjs", ".ts",
        ".tsx", ".jsx", ".css", ".scss", ".java", ".rs", ".go", ".glsl", ".vert", ".frag",
        ".patch", ".diff",
    },
    "ノート (.clro)": {".clro"},
    "注釈データ (.clrop)": {".clrop"},
    "PDF": {".pdf"},
    "PNG画像": {".png"},
    "その他の画像": {".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".ico", ".svg", ".tif", ".tiff"},
    "フォント": {".ttf", ".otf", ".woff", ".woff2", ".eot"},
    "テキスト・文書": {
        ".md", ".markdown", ".txt", ".text", ".html", ".htm", ".rtf", ".doc", ".docx", ".xls",
        ".xlsx", ".ppt", ".pptx", ".odt", ".ods", ".odp", ".tex", ".csv", ".rst", ".log",
    },
    "設定・構造化データ": {
        ".json", ".xml", ".ini", ".cfg", ".conf", ".config", ".dtd", ".ui", ".xcd", ".xcu", ".xba",
        ".xlb", ".xlc", ".properties", ".toml", ".yaml", ".yml", ".resx",
    },
    "アーカイブ・データ": {
        ".zip", ".7z", ".tar", ".gz", ".bz2", ".xz", ".zst", ".dat", ".bin", ".pak", ".res",
        ".db", ".sqlite", ".dbf", ".dbt", ".pack", ".odb", ".rdb",
    },
}
ARCHIVE_OVERHEAD_CATEGORY = "ZIP管理情報"
OTHER_CATEGORY = "その他"


def configure_utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def classify_file(name: str) -> str:
    suffix = Path(name).suffix.lower()
    for category, suffixes in CATEGORY_SUFFIXES.items():
        if suffix in suffixes:
            return category
    return OTHER_CATEGORY


def is_link_or_junction(path: Path) -> bool:
    is_junction = getattr(path, "is_junction", lambda: False)
    return path.is_symlink() or is_junction()


def add_size(
    categories: dict[str, dict[str, int]], category: str, size_bytes: int, file_count: int = 1
) -> None:
    categories[category]["size_bytes"] += size_bytes
    categories[category]["files"] += file_count


def make_report(
    source: Path, total_size_bytes: int, categories: dict[str, dict[str, int]], basis: str
) -> dict[str, object]:
    rows = []
    for name, values in sorted(categories.items(), key=lambda item: (-item[1]["size_bytes"], item[0])):
        size_bytes = values["size_bytes"]
        rows.append({
            "category": name,
            "size_bytes": size_bytes,
            "percent": (size_bytes * 100 / total_size_bytes) if total_size_bytes else 0.0,
            "files": values["files"],
        })
    return {
        "input": str(source),
        "basis": basis,
        "total_size_bytes": total_size_bytes,
        "categories": rows,
    }


def analyze_zip(source: Path) -> dict[str, object]:
    categories: dict[str, dict[str, int]] = defaultdict(lambda: {"size_bytes": 0, "files": 0})
    payload_size = 0
    with zipfile.ZipFile(source) as archive:
        for member in archive.infolist():
            if member.is_dir():
                continue
            payload_size += member.compress_size
            add_size(categories, classify_file(member.filename), member.compress_size)

    total_size_bytes = source.stat().st_size
    if payload_size > total_size_bytes:
        raise ValueError("ZIP member sizes exceed the archive size")
    add_size(categories, ARCHIVE_OVERHEAD_CATEGORY, total_size_bytes - payload_size, file_count=0)
    return make_report(source, total_size_bytes, categories, "ZIP圧縮後サイズ")


def analyze_directory(source: Path) -> dict[str, object]:
    if is_link_or_junction(source):
        raise ValueError(f"distribution root must not be a link or junction: {source}")

    categories: dict[str, dict[str, int]] = defaultdict(lambda: {"size_bytes": 0, "files": 0})
    total_size_bytes = 0
    for path in source.rglob("*"):
        if is_link_or_junction(path):
            raise ValueError(f"distribution contains a link or junction: {path}")
        if not path.is_file():
            continue
        size_bytes = path.stat().st_size
        total_size_bytes += size_bytes
        add_size(categories, classify_file(path.name), size_bytes)
    return make_report(source, total_size_bytes, categories, "展開後ファイルサイズ")


def analyze_distribution(source: Path) -> dict[str, object]:
    if source.is_dir():
        return analyze_directory(source)
    if source.is_file():
        if not zipfile.is_zipfile(source):
            raise ValueError(f"input file is not a ZIP archive: {source}")
        return analyze_zip(source)
    raise ValueError(f"distribution does not exist: {source}")


def render_text(report: dict[str, object]) -> str:
    total_size_bytes = int(report["total_size_bytes"])
    lines = [
        f"対象: {report['input']}",
        f"基準: {report['basis']}",
        f"合計: {total_size_bytes:,} bytes ({total_size_bytes / (1024 * 1024):.2f} MiB)",
        "",
        "分類 | 容量 | 割合 | ファイル数",
        "--- | ---: | ---: | ---:",
    ]
    for row in report["categories"]:
        lines.append(
            f"{row['category']} | {row['size_bytes']:,} bytes | {row['percent']:.3f}% | {row['files']}"
        )
    return "\n".join(lines)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Read-only size-share report for a distribution directory or ZIP."
    )
    parser.add_argument("distribution", type=Path, help="Distribution directory or ZIP file")
    parser.add_argument("--format", choices=("text", "json"), default="text")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    configure_utf8_output()
    args = parse_args(argv)
    try:
        report = analyze_distribution(args.distribution)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    if args.format == "json":
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print(render_text(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())