#!/usr/bin/env python3
"""List direct, literal-only IsEnglishUi() text branches for safe migration planning.

This tool is deliberately read-only.  It identifies only an English and a
Japanese wide-string literal joined directly by ``IsEnglishUi() ? ... : ...``.
Call sites still need a C++ review because a Win32 ``LPCWSTR`` parameter and a
``std::wstring`` expression require different lifetime-safe replacements.
"""

from __future__ import annotations

import argparse
import hashlib
import re
from pathlib import Path


PAIR_RE = re.compile(
    r'IsEnglishUi\(\)\s*\?\s*L"(?P<en>(?:\\.|[^"\\])*)"\s*:\s*'
    r'L"(?P<ja>(?:\\.|[^"\\])*)"(?!\s*L")',
    re.DOTALL,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("src"))
    args = parser.parse_args()

    total = 0
    for path in sorted(args.source.rglob("*")):
        if path.suffix not in {".cpp", ".h", ".cppinc"}:
            continue
        source = path.read_text(encoding="utf-8")
        for match in PAIR_RE.finditer(source):
            total += 1
            line = source.count("\n", 0, match.start()) + 1
            # The digest is only an immutable migration handle; production IDs
            # are assigned by the reviewed migration patch.
            digest = hashlib.sha256(
                (match.group("ja") + "\0" + match.group("en")).encode("utf-8")
            ).hexdigest()[:12]
            print(f"{path}:{line}: {digest}")
    print(f"literal-only pairs: {total}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
