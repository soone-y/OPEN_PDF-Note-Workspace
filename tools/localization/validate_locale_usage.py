#!/usr/bin/env python3
"""Validate catalog references in C++ sources and English catalog text."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


JAPANESE = re.compile(r"[\u3040-\u30ff\u3400-\u9fff]")
REFERENCE = re.compile(
    r'localization::(?:Text|Format)\s*\(\s*L"(?P<id>[a-z][a-z0-9._]*)"'
)
SOURCE_SUFFIXES = {".cpp", ".h", ".cppinc"}


def load_catalog(path: Path) -> dict[str, str]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid catalog {path}: {error}") from error
    if not isinstance(value, dict) or not all(isinstance(key, str) and isinstance(text, str)
                                              for key, text in value.items()):
        raise ValueError(f"catalog must be a string-to-string object: {path}")
    return value


def referenced_ids(source_root: Path) -> set[str]:
    ids: set[str] = set()
    for path in source_root.rglob("*"):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        try:
            ids.update(match.group("id") for match in REFERENCE.finditer(path.read_text(encoding="utf-8")))
        except (OSError, UnicodeDecodeError) as error:
            raise ValueError(f"unreadable source {path}: {error}") from error
    return ids


def validate(source_root: Path, japanese_catalog: Path, english_catalog: Path) -> list[str]:
    ja = load_catalog(japanese_catalog)
    en = load_catalog(english_catalog)
    errors = [f"source references ID absent from Japanese catalog: {text_id}"
              for text_id in sorted(referenced_ids(source_root) - set(ja))]
    errors.extend(f"English catalog contains Japanese text: {text_id}"
                  for text_id, text in sorted(en.items()) if JAPANESE.search(text))
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("src"))
    parser.add_argument("--ja", type=Path, default=Path("locales/ja.json"))
    parser.add_argument("--en", type=Path, default=Path("locales/en.json"))
    args = parser.parse_args(argv)
    try:
        errors = validate(args.source, args.ja, args.en)
    except ValueError as error:
        errors = [str(error)]
    if errors:
        print("Locale usage validation failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"Locale usage validation passed: {args.source}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
