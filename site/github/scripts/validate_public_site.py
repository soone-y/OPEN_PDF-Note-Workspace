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
FORBIDDEN_PUBLIC_REFERENCES = (
    "DEV" + "_PDF-Note-Workspace",
    "docs/internal/",
    "site/cloudflare/",
    "C:\\Users\\",
    "/Users/",
)
DOCUMENTATION_PORTAL_REQUIRED_FILES = (
    "index.html", "llms.txt", "For_AI.md", "README.md", "for_ai/ai_context.md", "for_ai/manifest.json", "for_ai/project_context.xml",
)
PORTAL_AI_ENTRY_LINKS = ("llms.txt",)
LLMS_REQUIRED_LINKS = ("for_ai/ai_context.md", "For_AI.md", "for_ai/manifest.json", "for_ai/project_context.xml")
MARKDOWN_LINK = re.compile(r"!?\[[^]]*\]\(([^)\s]+)(?:\s+[^)]*)?\)")
HTML_LINK = re.compile(r"(?:href|src)=[\"']([^\"'#]+)", re.IGNORECASE)
HTML_META = re.compile(r"<meta\s+[^>]*?name=[\"']([^\"']+)[\"'][^>]*?content=[\"']([^\"']*)[\"']", re.IGNORECASE)


def local_target(value: str) -> str | None:
    if value.startswith(("#", "data:", "http:", "https:", "mailto:")):
        return None
    return value.split("#", 1)[0]


def validate_local_target(site: Path, source_path: Path, target: str, *, kind: str, errors: list[str]) -> None:
    """Require local references to remain inside the generated public site."""
    site_root = site.resolve()
    resolved = (source_path.parent / target).resolve()
    if resolved != site_root and site_root not in resolved.parents:
        errors.append(f"local {kind} escapes public site: {source_path.relative_to(site)} -> {target}")
    elif not resolved.is_file():
        errors.append(f"broken {kind}: {source_path.relative_to(site)} -> {target}")


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
        for reference in FORBIDDEN_PUBLIC_REFERENCES:
            if reference in text:
                errors.append(
                    f"development-only reference in public site: {path.relative_to(site)} ({reference})"
                )


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
    markdown_like_paths = list(site.rglob("*.md"))
    llms_path = site / "llms.txt"
    if llms_path.is_file():
        markdown_like_paths.append(llms_path)
    for path in markdown_like_paths:
        text = path.read_text(encoding="utf-8-sig")
        for match in MARKDOWN_LINK.finditer(text):
            target = local_target(match.group(1))
            if target:
                validate_local_target(site, path, target, kind="Markdown link or image", errors=errors)
    for path in site.rglob("*.html"):
        text = path.read_text(encoding="utf-8-sig")
        for match in HTML_LINK.finditer(text):
            target = local_target(match.group(1))
            if target:
                validate_local_target(site, path, target, kind="HTML link or asset", errors=errors)


def validate_rendered_human_docs(site: Path, errors: list[str]) -> None:
    """Ensure raw documents and their browser-readable HTML are published together."""
    markdown_paths = list(site.glob("*.md"))
    for directory in (site / "docs" / "public", site / "for_ai"):
        if directory.is_dir():
            markdown_paths.extend(directory.rglob("*.md"))
    for markdown_path in markdown_paths:
        rendered = markdown_path.with_suffix(".html")
        if not rendered.is_file():
            errors.append(f"rendered HTML is missing for public Markdown: {markdown_path.relative_to(site)}")


def validate_manifest(site: Path, errors: list[str]) -> None:
    manifest_path = site / "for_ai" / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return
    if manifest.get("schema_version") != 1:
        errors.append("for_ai/manifest.json must use schema_version 1")
    entry_point = manifest.get("entry_point")
    if not isinstance(entry_point, str) or not entry_point:
        errors.append("for_ai/manifest.json must define a non-empty entry_point")
    else:
        validate_local_target(site, manifest_path, entry_point, kind="manifest entry_point", errors=errors)
    routes = manifest.get("routes")
    if not isinstance(routes, list) or not routes:
        errors.append("for_ai/manifest.json must define at least one route")
        return
    route_ids: set[str] = set()
    for route in routes:
        if not isinstance(route, dict):
            errors.append("for_ai/manifest.json routes must contain objects")
            continue
        route_id = route.get("id")
        if not isinstance(route_id, str) or not route_id:
            errors.append("manifest route has no non-empty id")
            route_id = "<unknown>"
        elif route_id in route_ids:
            errors.append(f"manifest route id is duplicated: {route_id}")
        route_ids.add(route_id)
        if not isinstance(route.get("when"), str) or not route["when"].strip():
            errors.append(f"manifest route '{route_id}' has no non-empty when")
        read_paths = route.get("read")
        if not isinstance(read_paths, list) or not read_paths:
            errors.append(f"manifest route '{route_id}' has no read paths")
            continue
        for relative_path in read_paths:
            if not isinstance(relative_path, str) or not relative_path:
                errors.append(f"manifest route '{route_id}' has an invalid read path")
                continue
            validate_local_target(site, manifest_path, relative_path, kind=f"manifest route '{route_id}'", errors=errors)


def validate_portal_entrypoint(site: Path, errors: list[str]) -> None:
    """Keep the HTML portal entrypoint aligned with the machine-readable AI entrypoints."""
    index_path = site / "index.html"
    if not index_path.is_file():
        return
    try:
        text = index_path.read_text(encoding="utf-8-sig")
    except UnicodeDecodeError:
        return
    metadata = {name.lower(): value for name, value in HTML_META.findall(text)}
    expected = {
        "ai-agent-entrypoint": "For_AI.md",
        "ai-manifest": "for_ai/manifest.json",
    }
    for name, target in expected.items():
        if metadata.get(name) != target:
            errors.append(f"index.html must declare {name}={target}")
        else:
            validate_local_target(site, index_path, target, kind=f"index metadata '{name}'", errors=errors)
    linked_targets = {
        local_target(match.group(1))
        for match in HTML_LINK.finditer(text)
        if local_target(match.group(1))
    }
    for target in PORTAL_AI_ENTRY_LINKS:
        if target not in linked_targets:
            errors.append(f"index.html must visibly link to AI entry document: {target}")


def validate_llms_entrypoint(site: Path, errors: list[str]) -> None:
    """Keep the single visible AI entrypoint connected to its required raw documents."""
    llms_path = site / "llms.txt"
    if not llms_path.is_file():
        return
    text = llms_path.read_text(encoding="utf-8-sig")
    linked_targets = {
        local_target(match.group(1))
        for match in MARKDOWN_LINK.finditer(text)
        if local_target(match.group(1))
    }
    for target in LLMS_REQUIRED_LINKS:
        if target not in linked_targets:
            errors.append(f"llms.txt must link to AI document: {target}")


def validate_site(site: Path) -> list[str]:
    errors: list[str] = []
    if not site.is_dir():
        return [f"site directory does not exist: {site}"]
    for relative_path in DOCUMENTATION_PORTAL_REQUIRED_FILES:
        if not (site / relative_path).is_file():
            errors.append(f"required public file is missing: {relative_path}")
    if (site / "docs" / "public" / "How_to_Build.md").exists():
        errors.append("developer-only docs/public/How_to_Build.md must not be public")
    validate_text_encoding(site, errors)
    validate_structured_files(site, errors)
    validate_local_links(site, errors)
    validate_manifest(site, errors)
    validate_portal_entrypoint(site, errors)
    validate_llms_entrypoint(site, errors)
    validate_rendered_human_docs(site, errors)
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
