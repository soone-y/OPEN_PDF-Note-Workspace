#!/usr/bin/env python3
"""Migrate reviewed literal-only IsEnglishUi() branches to catalog IDs.

The command is intentionally opt-in per source file.  It only accepts direct
wide-literal pairs, stores Japanese as the canonical value, and replaces the
pair with ``localization::Text``.  Callers must first be reviewed to ensure
that their destination accepts ``std::wstring`` rather than retaining an
``LPCWSTR`` pointer.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


PAIR_RE = re.compile(
    r'IsEnglishUi\(\)\s*\?\s*L"(?P<en>(?:\\.|[^"\\])*)"\s*:\s*'
    r'L"(?P<ja>(?:\\.|[^"\\])*)"(?!\s*L")',
    re.DOTALL,
)


def decode_literal(value: str) -> str:
    result: list[str] = []
    index = 0
    escapes = {"n": "\n", "r": "\r", "t": "\t", "\\": "\\", '"': '"'}
    while index < len(value):
        if value[index] != "\\" or index + 1 == len(value):
            result.append(value[index])
            index += 1
            continue
        index += 1
        result.append(escapes.get(value[index], "\\" + value[index]))
        index += 1
    return "".join(result)


def load(path: Path) -> dict[str, str]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"catalog is not an object: {path}")
    return value


def save(path: Path, catalog: dict[str, str]) -> None:
    path.write_text(json.dumps(catalog, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--ja", type=Path, default=Path("locales/ja.json"))
    parser.add_argument("--en", type=Path, default=Path("locales/en.json"))
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()

    source = args.source.read_text(encoding="utf-8")
    ja_catalog = load(args.ja)
    en_catalog = load(args.en)
    replacements: list[tuple[int, int, str]] = []
    for match in PAIR_RE.finditer(source):
        japanese = decode_literal(match.group("ja"))
        english = decode_literal(match.group("en"))
        digest = hashlib.sha256((japanese + "\0" + english).encode("utf-8")).hexdigest()[:12]
        text_id = f"{args.prefix}.{digest}"
        if text_id in ja_catalog and ja_catalog[text_id] != japanese:
            raise ValueError(f"Japanese catalog collision: {text_id}")
        if text_id in en_catalog and en_catalog[text_id] != english:
            raise ValueError(f"English catalog collision: {text_id}")
        ja_catalog[text_id] = japanese
        en_catalog[text_id] = english
        replacements.append((match.start(), match.end(), f'localization::Text(L"{text_id}")'))

    print(f"{args.source}: {len(replacements)} literal pairs")
    if not args.apply:
        return 0
    for begin, end, replacement in reversed(replacements):
        source = source[:begin] + replacement + source[end:]
    args.source.write_text(source, encoding="utf-8")
    save(args.ja, ja_catalog)
    save(args.en, en_catalog)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
