#!/usr/bin/env python3
"""Read-only scanner for bundled LibreOffice source archives.

The tool is intentionally small: it can inspect archive member paths and,
optionally, text contents without extracting the archives into the workspace.
"""

from __future__ import annotations

import argparse
import json
import tarfile
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


DEFAULT_ARCHIVES = [
    "third_party/libreoffice/source_archives/libreoffice-26.2.3.2.tar.xz",
    "third_party/libreoffice/source_archives/libreoffice-dictionaries-26.2.3.2.tar.xz",
    "third_party/libreoffice/source_archives/libreoffice-help-26.2.3.2.tar.xz",
    "third_party/libreoffice/source_archives/libreoffice-translations-26.2.3.2.tar.xz",
]

TEXT_SUFFIXES = {
    ".c",
    ".cc",
    ".cxx",
    ".cpp",
    ".h",
    ".hh",
    ".hxx",
    ".hpp",
    ".idl",
    ".mk",
    ".py",
    ".sh",
    ".txt",
    ".xcu",
    ".xml",
    ".component",
    ".json",
    ".ini",
    ".in",
    ".ac",
}


@dataclass
class Hit:
    archive: str
    path: str
    kind: str
    query: str
    line: int | None = None
    text: str | None = None


def normalize(value: str) -> str:
    return value.replace("\\", "/").casefold()


def is_text_candidate(path: str) -> bool:
    suffix = Path(path).suffix.casefold()
    if suffix in TEXT_SUFFIXES:
        return True
    name = Path(path).name.casefold()
    return name in {"makefile", "readme", "license", "notice"}


def iter_archive_hits(
    archive: Path,
    path_queries: list[str],
    content_queries: list[str],
    *,
    scan_content: bool,
    max_file_size: int,
    max_hits: int,
) -> Iterable[Hit]:
    emitted = 0
    path_needles = [(query, normalize(query)) for query in path_queries]
    content_needles = [(query, query.casefold()) for query in content_queries]

    with tarfile.open(archive, "r:*") as tar:
        for member in tar:
            if emitted >= max_hits:
                return
            if not member.isfile():
                continue

            member_path = member.name
            member_path_norm = normalize(member_path)
            for query, needle in path_needles:
                if needle in member_path_norm:
                    yield Hit(str(archive), member_path, "path", query)
                    emitted += 1
                    if emitted >= max_hits:
                        return

            if not scan_content or not content_needles:
                continue
            if member.size > max_file_size or not is_text_candidate(member_path):
                continue

            extracted = tar.extractfile(member)
            if extracted is None:
                continue
            try:
                data = extracted.read()
            finally:
                extracted.close()
            text = data.decode("utf-8", errors="ignore")
            for line_no, line in enumerate(text.splitlines(), start=1):
                line_folded = line.casefold()
                for query, needle in content_needles:
                    if needle in line_folded:
                        yield Hit(
                            str(archive),
                            member_path,
                            "content",
                            query,
                            line_no,
                            line.strip()[:240],
                        )
                        emitted += 1
                        if emitted >= max_hits:
                            return


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Scan LibreOffice source tar archives without extraction."
    )
    parser.add_argument(
        "--repo-root",
        default=".",
        help="Repository root. Defaults to current directory.",
    )
    parser.add_argument(
        "--archive",
        action="append",
        help="Archive path relative to repo root. Defaults to bundled LibreOffice archives.",
    )
    parser.add_argument(
        "--query",
        action="append",
        default=[],
        help="Case-insensitive path query. Can be repeated.",
    )
    parser.add_argument(
        "--content-query",
        action="append",
        default=[],
        help="Case-insensitive text query. Requires --content. Can be repeated.",
    )
    parser.add_argument(
        "--content",
        action="store_true",
        help="Also scan small text files inside archives.",
    )
    parser.add_argument(
        "--max-file-size",
        type=int,
        default=512 * 1024,
        help="Maximum archived file size for content scanning.",
    )
    parser.add_argument(
        "--max-hits",
        type=int,
        default=200,
        help="Maximum hits per archive.",
    )
    parser.add_argument(
        "--format",
        choices=("text", "json"),
        default="text",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    repo_root = Path(args.repo_root).resolve()
    archive_names = args.archive or DEFAULT_ARCHIVES
    archives = [(repo_root / name).resolve() for name in archive_names]

    missing = [str(path) for path in archives if not path.exists()]
    if missing:
        raise SystemExit("missing archive(s): " + ", ".join(missing))

    path_queries = args.query
    content_queries = args.content_query
    if not path_queries and not content_queries:
        path_queries = [
            "update",
            "minidump",
            "mail",
            "webdav",
            "curl",
            "macro",
            "external",
        ]

    hits: list[Hit] = []
    for archive in archives:
        hits.extend(
            iter_archive_hits(
                archive,
                path_queries,
                content_queries,
                scan_content=args.content,
                max_file_size=args.max_file_size,
                max_hits=args.max_hits,
            )
        )

    if args.format == "json":
        print(json.dumps([asdict(hit) for hit in hits], indent=2, ensure_ascii=False))
        return 0

    current_archive = None
    for hit in hits:
        if hit.archive != current_archive:
            current_archive = hit.archive
            print(f"\n[{Path(hit.archive).name}]")
        if hit.kind == "content":
            print(f"{hit.path}:{hit.line}: {hit.query}: {hit.text}")
        else:
            print(f"{hit.path}  [{hit.query}]")
    if not hits:
        print("no hits")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
