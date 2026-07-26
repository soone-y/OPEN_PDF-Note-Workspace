#!/usr/bin/env python3
"""Validate the curated static public site before it is published."""

from __future__ import annotations

import argparse
import json
import re
import sys
import xml.etree.ElementTree as element_tree
from pathlib import Path


TEXT_EXTENSIONS = {".html", ".json", ".md", ".txt", ".xml"}
REQUIRED_FILES = (
    "index.html", "For_AI.md", "README.md", "for_ai/manifest.json",
    "for_ai/project_context.xml", "for_ai/core/semantic_search_index.json",
)
MARKDOWN_LINK = re.compile(r"!?\[[^]]*\]\(([^)\s]+)(?:\s+[^)]*)?\)")
HTML_LINK = re.compile(r"(?:href|src)=[\"']([^\"'#]+)", re.IGNORECASE)


def local_target(value: str) -> str | None:
    if value.startswith(("#", "data:", "http:", "https:", "mailto:")):
        return None
    return value.split("#", 1)[0]


def validate_text_encoding(site: Path, errors: list[str]) -> None:
    for path in site.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in TEXT_EXTENSIONS:
            continue
        try:
            text = path.read_text(encoding="utf-8-sig")
        except UnicodeDecodeError as error:
            errors.append(f"invalid UTF-8: {path.relative_to(site)} ({error})")
            continue
        if "\ufffd" in text:
            errors.append(f"replacement character indicates possible mojibake: {path.relative_to(site)}")


def validate_structured_files(site: Path, errors: list[str]) -> None:
    for path in site.rglob("*.json"):
        try:
            json.loads(path.read_text(encoding="utf-8-sig"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            errors.append(f"invalid JSON: {path.relative_to(site)} ({error})")
    for path in site.rglob("*.xml"):
        try:
            element_tree.parse(path)
        except element_tree.ParseError as error:
            errors.append(f"invalid XML: {path.relative_to(site)} ({error})")


def validate_local_links(site: Path, errors: list[str]) -> None:
    for path in site.rglob("*.md"):
        text = path.read_text(encoding="utf-8-sig")
        for match in MARKDOWN_LINK.finditer(text):
            target = local_target(match.group(1))
            if target and not (path.parent / target).resolve().is_file():
                errors.append(f"broken Markdown link or image: {path.relative_to(site)} -> {target}")
    for path in site.rglob("*.html"):
        text = path.read_text(encoding="utf-8-sig")
        for match in HTML_LINK.finditer(text):
            target = local_target(match.group(1))
            if target and not (path.parent / target).resolve().is_file():
                errors.append(f"broken HTML link or asset: {path.relative_to(site)} -> {target}")


def validate_manifest(site: Path, errors: list[str]) -> None:
    manifest_path = site / "for_ai" / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return
    for route in manifest.get("routes", []):
        for relative_path in route.get("read", []):
            if not (manifest_path.parent / relative_path).resolve().is_file():
                errors.append(f"manifest route '{route.get('id', '<unknown>')}' references missing file: {relative_path}")


def validate_site(site: Path) -> list[str]:
    errors: list[str] = []
    if not site.is_dir():
        return [f"site directory does not exist: {site}"]
    for relative_path in REQUIRED_FILES:
        if not (site / relative_path).is_file():
            errors.append(f"required public file is missing: {relative_path}")
    if (site / "Document" / "How_to_Build.md").exists():
        errors.append("developer-only Document/How_to_Build.md must not be public")
    validate_text_encoding(site, errors)
    validate_structured_files(site, errors)
    validate_local_links(site, errors)
    validate_manifest(site, errors)
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site", type=Path, required=True)
    args = parser.parse_args(argv)
    errors = validate_site(args.site)
    if errors:
        print("Public-site validation failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"Public-site validation passed: {args.site}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
