#!/usr/bin/env python3
"""Validate locale inputs once and safely reuse generated locale catalogs.

The cache is content-addressed: a cached result is accepted only when every
source file read by the usage validator, both catalogs, and the three involved
tools have the same SHA-256 digest.  A missing or damaged cache is regenerated.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import tempfile
from pathlib import Path

import generate_locale_catalog
import validate_locale_usage


SCHEMA_VERSION = 1


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_tree_hash(source_root: Path) -> str:
    """Hash precisely the files inspected by validate_locale_usage.py."""
    digest = hashlib.sha256()
    for path in sorted(source_root.rglob("*")):
        if not path.is_file() or path.suffix not in validate_locale_usage.SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(source_root).as_posix().encode("utf-8")
        digest.update(len(relative).to_bytes(8, "big"))
        digest.update(relative)
        digest.update(bytes.fromhex(file_hash(path)))
    return digest.hexdigest()


def atomic_json_write(path: Path, value: object) -> None:
    atomic_text_write(path, json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n")


def atomic_text_write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", newline="\n", dir=path.parent, delete=False
    ) as temporary:
        temporary.write(text)
        temporary_path = Path(temporary.name)
    try:
        os.replace(temporary_path, path)
    finally:
        temporary_path.unlink(missing_ok=True)


def read_json(path: Path) -> object | None:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return None


def valid_usage_stamp(path: Path, inputs: dict[str, object]) -> bool:
    value = read_json(path)
    return isinstance(value, dict) and value.get("schema_version") == SCHEMA_VERSION and value.get("inputs") == inputs


def valid_catalog_stamp(path: Path, inputs: dict[str, object], output: Path, report: Path) -> bool:
    value = read_json(path)
    if not isinstance(value, dict) or value.get("schema_version") != SCHEMA_VERSION or value.get("inputs") != inputs:
        return False
    outputs = value.get("outputs")
    if not isinstance(outputs, dict) or not output.is_file() or not report.is_file():
        return False
    try:
        return outputs.get("header_sha256") == file_hash(output) and outputs.get("report_sha256") == file_hash(report)
    except OSError:
        return False


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--ja", type=Path, required=True)
    parser.add_argument("--en", type=Path, required=True)
    parser.add_argument("--locale", choices=("ja", "en"), required=True)
    parser.add_argument("--localized", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--usage-stamp", type=Path, required=True)
    parser.add_argument("--catalog-stamp", type=Path, required=True)
    args = parser.parse_args(argv)

    try:
        required_files = (args.ja, args.en, args.localized)
        if not args.source.is_dir():
            raise ValueError(f"source directory does not exist: {args.source}")
        for path in required_files:
            if not path.is_file():
                raise ValueError(f"localization input does not exist: {path}")

        usage_inputs = {
            "source_tree_sha256": source_tree_hash(args.source),
            "ja_sha256": file_hash(args.ja),
            "en_sha256": file_hash(args.en),
            "usage_validator_sha256": file_hash(Path(validate_locale_usage.__file__).resolve()),
            "preparer_sha256": file_hash(Path(__file__).resolve()),
        }
        if valid_usage_stamp(args.usage_stamp, usage_inputs):
            print("Locale usage validation cache hit.")
        else:
            errors = validate_locale_usage.validate(args.source, args.ja, args.en)
            if errors:
                print("Locale usage validation failed:", file=sys.stderr)
                for error in errors:
                    print(f"- {error}", file=sys.stderr)
                return 1
            atomic_json_write(args.usage_stamp, {"schema_version": SCHEMA_VERSION, "inputs": usage_inputs})
            print(f"Locale usage validation passed: {args.source}")

        catalog_inputs = {
            "locale": args.locale,
            "ja_sha256": file_hash(args.ja),
            "localized_sha256": file_hash(args.localized),
            "generator_sha256": file_hash(Path(generate_locale_catalog.__file__).resolve()),
            "preparer_sha256": file_hash(Path(__file__).resolve()),
        }
        if valid_catalog_stamp(args.catalog_stamp, catalog_inputs, args.output, args.report):
            print(f"Locale catalog cache hit: {args.locale}")
            return 0

        ja = generate_locale_catalog.load_catalog(args.ja)
        localized = generate_locale_catalog.load_catalog(args.localized)
        unknown = sorted(set(localized) - set(ja))
        if unknown:
            raise ValueError(f"{args.locale} catalog has IDs absent from Japanese catalog: {', '.join(unknown)}")
        for key, translated in localized.items():
            if set(generate_locale_catalog.PLACEHOLDER_RE.findall(translated)) != set(generate_locale_catalog.PLACEHOLDER_RE.findall(ja[key])):
                raise ValueError(f"placeholder mismatch for {key!r}")
        selected = ja if args.locale == "ja" else {key: localized.get(key, text) for key, text in ja.items()}
        fallback_ids = [] if args.locale == "ja" else sorted(set(ja) - set(localized))
        atomic_text_write(args.output, generate_locale_catalog.generate_header(selected))
        atomic_text_write(args.report, json.dumps({"locale": args.locale, "fallback_ids": fallback_ids}, ensure_ascii=False, indent=2) + "\n")
        atomic_json_write(args.catalog_stamp, {
            "schema_version": SCHEMA_VERSION,
            "inputs": catalog_inputs,
            "outputs": {"header_sha256": file_hash(args.output), "report_sha256": file_hash(args.report)},
        })
        print(json.dumps({"locale": args.locale, "fallback_count": len(fallback_ids)}, ensure_ascii=False))
        return 0
    except (OSError, UnicodeDecodeError, ValueError) as error:
        print(f"Locale catalog preparation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
