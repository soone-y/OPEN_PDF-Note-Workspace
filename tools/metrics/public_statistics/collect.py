"""Aggregate only fixed public-equivalent inputs; never export source text."""
from __future__ import annotations

import json
from collections import defaultdict
from pathlib import Path

from tools.metrics.code_metrics.analyze_repo import analyze_text_lines
from tools.metrics.code_structure.inventory import cpp_bytes
from tools.metrics.public_statistics.model import GROUPS, SOURCE_SUFFIXES, TEXT_SUFFIXES, compare, digest, group, inventory_matches, profile, read_json

CLASSIFIED_SUFFIXES = SOURCE_SUFFIXES | {".py", ".ps1"}


def aggregate(root: Path, receipt: dict, *, version: str | None, measured_at: str, source_kind: str,
              published_at: str | None = None, public_url: str | None = None,
              tag: str | None = None, packages: list[dict] | None = None, observed_on: str | None = None) -> tuple[dict, dict]:
    if source_kind not in {"public-snapshot", "private-history-allowlisted", "release-candidate"}:
        raise ValueError("unrecognized statistics source kind")
    rows = inventory_matches(root, receipt)
    if not rows:
        raise ValueError("empty statistics input selection")
    active_profile = profile()
    profile_key = digest(active_profile)
    groups = {name: {"files": 0, "size_bytes": 0} for name in GROUPS}
    extensions = defaultdict(lambda: {"files": 0, "size_bytes": 0})
    scales = {name: dict.fromkeys(("files", "text_files", "size_bytes", "lines", "blank_lines", "code_lines", "comment_lines", "classified_files"), 0)
              for name in ("application", "tests_source", "tools")}
    lexical = {"status": "COMPLETE", "files": 0, "source_bytes": 0, "bytes": dict.fromkeys(("code", "literal", "comment", "whitespace"), 0),
               "literal_bytes": dict.fromkeys(("string", "character", "number", "other"), 0),
               "literal_counts": dict.fromkeys(("string", "character", "number", "other"), 0),
               "lexical_items": 0, "string_payload_source_bytes": 0, "string_purpose": "UNCLASSIFIED"}
    text_files, total_lines, blank_lines, legacy_text_files = 0, 0, 0, 0
    src_tree = {"files": 0, "size_bytes": 0}
    for row in rows:
        relative = row["path"]
        if relative.startswith("src/"):
            src_tree["files"] += 1
            src_tree["size_bytes"] += row["size"]
        category = group(relative)
        suffix = Path(relative).suffix.lower() or "<none>"
        for bucket in (groups[category], extensions[suffix]):
            bucket["files"] += 1
            bucket["size_bytes"] += row["size"]
        scale = scales.get(category)
        if scale is not None:
            scale["files"] += 1
            scale["size_bytes"] += row["size"]
        if suffix not in TEXT_SUFFIXES and Path(relative).name not in {".gitignore", ".gitattributes", "CMakeLists.txt", "LICENSE", "COPYING"}:
            continue
        raw = (root / relative).read_bytes()
        utf16 = raw.startswith((b"\xff\xfe", b"\xfe\xff"))
        if utf16 and category == "application" and suffix in SOURCE_SUFFIXES:
            raise ValueError("UTF-16 application source requires a separate lexical method")
        if b"\0" in raw and not utf16:
            if category == "tests_fixture":
                continue
            raise ValueError("declared text input contains NUL bytes")
        try:
            text = raw.decode("utf-16" if utf16 else "utf-8")
            legacy_text_files += int(utf16)
        except UnicodeDecodeError:
            if category == "tests_fixture":
                continue
            # The sole upstream legacy notice already accepted by the text gate.
            if relative != "third_party/pdfium/licenses/freetype.txt":
                raise ValueError("statistics text encoding is not declared") from None
            text = raw.decode("cp1252")
            legacy_text_files += 1
        lines = text.removeprefix("\ufeff").splitlines()
        text_files += 1
        total_lines += len(lines)
        blank_lines += sum(not line.strip() for line in lines)
        if scale is not None:
            scale["text_files"] += 1
            scale["lines"] += len(lines)
            scale["blank_lines"] += sum(not line.strip() for line in lines)
            if suffix in CLASSIFIED_SUFFIXES:
                counts = analyze_text_lines(Path(relative), text.removeprefix("\ufeff"))
                if sum(counts[key] for key in ("code_lines", "comment_lines", "blank_lines")) != len(lines):
                    raise ValueError("source line partition mismatch")
                scale["classified_files"] += 1
                for field in ("code_lines", "comment_lines"):
                    scale[field] += counts[field]
        if category == "application" and suffix in SOURCE_SUFFIXES:
            part = cpp_bytes(text)
            if sum(part["bytes"].values()) != len(raw):
                raise ValueError("source byte partition mismatch")
            lexical["files"] += 1
            lexical["source_bytes"] += len(raw)
            for field in ("bytes", "literal_bytes", "literal_counts"):
                for kind, value in part[field].items():
                    lexical[field][kind] += value
            lexical["lexical_items"] += part["lexical_items"]
            lexical["string_payload_source_bytes"] += part["string_payload_source_bytes"]
    if lexical["files"] == 0:
        lexical["status"] = "NOT_APPLICABLE"
    locale_maps = {}
    for locale in ("ja", "en"):
        path = root / "locales" / (locale + ".json")
        if path.exists():
            values = read_json(path)
            if not isinstance(values, dict):
                raise ValueError("invalid locale catalog")
            locale_maps[locale] = set(values)
    localization = {"status": "COMPLETE" if len(locale_maps) == 2 else "UNMEASURED",
                    "ja_ids": len(locale_maps["ja"]) if "ja" in locale_maps else None,
                    "en_ids": len(locale_maps["en"]) if "en" in locale_maps else None,
                    "en_missing_ids": len(locale_maps["ja"] - locale_maps["en"]) if len(locale_maps) == 2 else None}
    record = {"release": {"version": version, "published_at": published_at, "public_url": public_url, "tag": tag},
              "measurement": {"profile_key": profile_key, "measured_at": measured_at, "source_kind": source_kind,
                              "input_sha256": digest(rows), "allowlist_sha256": receipt.get("allowlist_sha256")},
              "inventory": {"status": "COMPLETE", "files": len(rows), "size_bytes": sum(row["size"] for row in rows),
                            "text_files": text_files, "binary_files": len(rows) - text_files, "lines": total_lines,
                            "blank_lines": blank_lines, "legacy_text_files": legacy_text_files,
                            "groups": groups, "extensions": dict(sorted(extensions.items()))},
              "source_scale": {"status": "COMPLETE", **scales, "src_tree": src_tree}, "lexical": lexical, "localization": localization,
              "packages": packages or [], "verification": {"status": "UNMEASURED", "reason": "no-bound-public-verification-receipt", "suites": []},
              "supplemental": {"status": "UNMEASURED", "reason": "not-selected"}}
    if observed_on is not None:
        record["measurement"]["observed_on"] = observed_on
    record["record_id"] = digest(record)
    record["comparison"] = compare(record, None)
    inventory_matches(root, receipt)
    if profile() != active_profile:
        raise ValueError("statistics measurement profile changed")
    return record, active_profile
