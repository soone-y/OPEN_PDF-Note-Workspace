#!/usr/bin/env python3
"""Validate the curated static public site before it is published."""

from __future__ import annotations

import argparse
import json
import re
import sys
import xml.etree.ElementTree as element_tree
from collections import deque
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit


REPO_ROOT = Path(__file__).resolve().parents[3]
ALLOWLIST_PATH = Path(__file__).resolve().parents[1] / "documentation_portal_allowlist.json"
TEXT_EXTENSIONS = {".html", ".json", ".md", ".txt", ".xml"}
FORBIDDEN_PUBLIC_REFERENCES = (
    "DEV" + "_PDF-Note-Workspace",
    "docs/internal/",
    "site/cloudflare/",
    "C:\\Users\\",
    "/Users/",
)
DOCUMENTATION_PORTAL_REQUIRED_FILES = (
    "index.html", "README.md", "introduction/index.md", "introduction/project_overview.md",
)
PORTAL_ENTRY_LINKS = ("introduction/index.html",)
HIGH_CONTRAST_STORAGE_KEY = "pdf-note-workspace-high-contrast"
MAX_VISIBLE_PORTAL_CLICKS = 2
PORTAL_EXTERNAL_LINKS = (
    "https://pdf-note-workspace.soone-y.com/",
    "https://github.com/soone-y/OPEN_PDF-Note-Workspace",
)
MARKDOWN_LINK = re.compile(r"!?\[[^]]*\]\(([^)\s]+)(?:\s+[^)]*)?\)")
HTML_LINK = re.compile(r"(?:href|src)=[\"']([^\"'#]+)", re.IGNORECASE)
HTML_META = re.compile(r"<meta\s+[^>]*?name=[\"']([^\"']+)[\"'][^>]*?content=[\"']([^\"']*)[\"']", re.IGNORECASE)
HTML_ID = re.compile(r"\bid=[\"']([^\"']+)[\"']", re.IGNORECASE)


class VisibleAnchorParser(HTMLParser):
    """Collect anchors a reader can use without opening a site-menu."""

    def __init__(self) -> None:
        super().__init__()
        self.hrefs: list[str] = []
        self._details_stack: list[bool] = []
        self._site_menu_depth = 0

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attributes = dict(attrs)
        if tag.lower() == "details":
            is_site_menu = "site-menu" in (attributes.get("class") or "").split()
            self._details_stack.append(is_site_menu)
            if is_site_menu:
                self._site_menu_depth += 1
            return
        if tag.lower() == "a" and self._site_menu_depth == 0:
            href = attributes.get("href")
            if href:
                self.hrefs.append(href)

    def handle_endtag(self, tag: str) -> None:
        if tag.lower() != "details" or not self._details_stack:
            return
        if self._details_stack.pop():
            self._site_menu_depth -= 1


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
        if text.startswith("\ufeff"):
            errors.append(f"duplicate UTF-8 BOM: {path.relative_to(site)}")
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
    for path in site.rglob("*.md"):
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
    for directory in (site / "docs" / "public", site / "introduction"):
        if directory.is_dir():
            markdown_paths.extend(directory.rglob("*.md"))
    for markdown_path in markdown_paths:
        rendered = markdown_path.with_suffix(".html")
        if not rendered.is_file():
            errors.append(f"rendered HTML is missing for public Markdown: {markdown_path.relative_to(site)}")
            continue
        rendered_text = rendered.read_text(encoding="utf-8-sig")
        if 'class="contrast-toggle"' not in rendered_text or HIGH_CONTRAST_STORAGE_KEY not in rendered_text:
            errors.append(f"rendered HTML is missing the persistent high-contrast control: {rendered.relative_to(site)}")


def validate_rendered_fragments(site: Path, errors: list[str]) -> None:
    """Keep Markdown heading links usable in the browser-rendered documents."""
    markdown_paths = list(site.glob("*.md"))
    for directory in (site / "docs" / "public", site / "introduction"):
        if directory.is_dir():
            markdown_paths.extend(directory.rglob("*.md"))

    for markdown_path in markdown_paths:
        text = markdown_path.read_text(encoding="utf-8-sig")
        for match in MARKDOWN_LINK.finditer(text):
            raw_target = match.group(1)
            if "#" not in raw_target or raw_target.startswith(("http:", "https:", "mailto:")):
                continue
            target, fragment = raw_target.split("#", 1)
            if not fragment:
                continue
            source_document = markdown_path if not target else markdown_path.parent / target
            if source_document.suffix.lower() != ".md":
                continue
            rendered = source_document.with_suffix(".html")
            if not rendered.is_file():
                continue
            ids = set(HTML_ID.findall(rendered.read_text(encoding="utf-8-sig")))
            decoded_fragment = unquote(fragment)
            if decoded_fragment not in ids:
                errors.append(
                    "broken rendered heading link: "
                    f"{markdown_path.relative_to(site)} -> {raw_target}"
                )


def validate_portal_entrypoint(site: Path, errors: list[str]) -> None:
    """Keep the HTML portal entrypoint aligned with the single common document."""
    index_path = site / "index.html"
    if not index_path.is_file():
        return
    try:
        text = index_path.read_text(encoding="utf-8-sig")
    except UnicodeDecodeError:
        return
    metadata = {name.lower(): value for name, value in HTML_META.findall(text)}
    if 'class="contrast-toggle"' not in text or HIGH_CONTRAST_STORAGE_KEY not in text:
        errors.append("index.html must provide the persistent high-contrast control")
    expected = {"ai-agent-entrypoint": "introduction/index.html"}
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
    for target in PORTAL_ENTRY_LINKS:
        if target not in linked_targets:
            errors.append(f"index.html must visibly link to common entry document: {target}")
    visible = VisibleAnchorParser()
    visible.feed(text)
    for target in PORTAL_EXTERNAL_LINKS:
        if target not in visible.hrefs:
            errors.append(f"index.html must visibly link to portal external destination: {target}")


def visible_local_html_links(site: Path, source_path: Path) -> set[Path]:
    """Return local HTML targets linked outside a collapsible site-menu."""
    parser = VisibleAnchorParser()
    parser.feed(source_path.read_text(encoding="utf-8-sig"))
    site_root = site.resolve()
    targets: set[Path] = set()
    for href in parser.hrefs:
        parts = urlsplit(href)
        if parts.scheme or parts.netloc or not parts.path:
            continue
        target = (source_path.parent / unquote(parts.path)).resolve()
        if target == site_root or site_root not in target.parents:
            continue
        if target.suffix.lower() == ".html" and target.is_file():
            targets.add(target.relative_to(site_root))
    return targets


def validate_portal_click_depth(site: Path, errors: list[str]) -> None:
    """Keep every public HTML document within two visible portal links."""
    root = site / "index.html"
    if not root.is_file():
        return
    pages = {path.relative_to(site) for path in site.rglob("*.html")}
    distances: dict[Path, int] = {Path("index.html"): 0}
    pending: deque[Path] = deque([Path("index.html")])
    while pending:
        current = pending.popleft()
        for target in visible_local_html_links(site, site / current):
            if target not in pages or target in distances:
                continue
            distances[target] = distances[current] + 1
            pending.append(target)

    for page in sorted(pages, key=lambda value: value.as_posix()):
        clicks = distances.get(page)
        if clicks is None:
            errors.append(
                "document is not reachable from the portal without opening a hamburger menu: "
                f"{page.as_posix()}"
            )
        elif clicks > MAX_VISIBLE_PORTAL_CLICKS:
            errors.append(
                f"document requires more than {MAX_VISIBLE_PORTAL_CLICKS} visible clicks from portal: "
                f"{page.as_posix()} ({clicks})"
            )


def resolve_child(root: Path, relative: object, *, label: str) -> Path:
    if not isinstance(relative, str) or not relative or Path(relative).is_absolute():
        raise ValueError(f"{label} must be a non-empty relative path")
    candidate = (root / relative).resolve()
    if candidate != root.resolve() and root.resolve() not in candidate.parents:
        raise ValueError(f"{label} escapes its root: {relative}")
    return candidate


def allowlisted_portal_paths(source_root: Path, allowlist_path: Path) -> tuple[set[Path], set[Path]]:
    """Return portal output paths and the Markdown source documents they derive from."""
    try:
        payload = json.loads(allowlist_path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid documentation portal allowlist: {error}") from error
    if payload.get("schema_version") != 1:
        raise ValueError("unsupported documentation portal allowlist schema_version")
    rules = payload.get("documentation_portal")
    if not isinstance(rules, dict):
        raise ValueError("documentation_portal allowlist profile must be an object")

    expected: set[Path] = set()
    markdown_sources: set[Path] = set()

    def add_expected(source: Path, destination: Path) -> None:
        if destination in expected:
            raise ValueError(f"documentation portal allowlist has duplicate destination: {destination.as_posix()}")
        expected.add(destination)
        if source.suffix.lower() == ".md":
            markdown_sources.add(source.relative_to(source_root))

    for kind in ("files", "trees"):
        entries = rules.get(kind, [])
        if not isinstance(entries, list):
            raise ValueError(f"documentation portal allowlist {kind} must be an array")
        for entry in entries:
            if not isinstance(entry, dict):
                raise ValueError(f"documentation portal allowlist {kind} entries must be objects")
            source = resolve_child(source_root, entry.get("source"), label=f"allowlist {kind} source")
            destination_value = entry.get("destination")
            if not isinstance(destination_value, str) or not destination_value:
                raise ValueError("allowlist destination must be a non-empty relative path")
            destination = Path(destination_value)
            if destination.is_absolute() or ".." in destination.parts:
                raise ValueError(f"allowlist destination escapes the site: {destination_value}")
            if kind == "files":
                if not source.is_file():
                    raise ValueError(f"allowlist file source is missing: {entry['source']}")
                add_expected(source, destination)
                continue
            if not source.is_dir():
                raise ValueError(f"allowlist tree source is missing: {entry['source']}")
            excluded = tuple(entry.get("exclude_prefixes", []))
            if not all(isinstance(prefix, str) and prefix for prefix in excluded):
                raise ValueError("allowlist tree exclude_prefixes must contain non-empty strings")
            for source_file in source.rglob("*"):
                if source_file.is_file():
                    relative_source = source_file.relative_to(source)
                    if not any(relative_source.as_posix().startswith(prefix) for prefix in excluded):
                        add_expected(source_file, destination / relative_source)

    return expected, markdown_sources


def expected_allowlisted_paths(source_root: Path, allowlist_path: Path) -> set[Path]:
    """Return the copied portal files and every HTML page derived from Markdown."""
    expected, _ = allowlisted_portal_paths(source_root, allowlist_path)

    # The renderer must create one browser page for every allowlisted Markdown
    # source. This catches a newly allowlisted document omitted from rendering.
    return expected | {path.with_suffix(".html") for path in expected if path.suffix.lower() == ".md"}


def validate_allowlist_coverage(site: Path, source_root: Path, allowlist_path: Path, errors: list[str]) -> None:
    try:
        expected = expected_allowlisted_paths(source_root.resolve(), allowlist_path.resolve())
    except ValueError as error:
        errors.append(f"documentation portal allowlist is invalid: {error}")
        return
    actual = {path.relative_to(site) for path in site.rglob("*") if path.is_file()}
    for path in sorted(expected - actual, key=lambda item: item.as_posix()):
        errors.append(f"allowlisted portal file is missing from generated site: {path.as_posix()}")
    for path in sorted(actual - expected, key=lambda item: item.as_posix()):
        errors.append(f"generated site contains file outside documentation portal allowlist: {path.as_posix()}")


def validate_documentation_map(source_root: Path, allowlist_path: Path, errors: list[str]) -> None:
    """Require DOCUMENTATION.md to link every allowlisted Markdown document exactly as a public index."""
    try:
        _, expected_documents = allowlisted_portal_paths(source_root.resolve(), allowlist_path.resolve())
    except ValueError as error:
        errors.append(f"documentation portal allowlist is invalid: {error}")
        return
    map_path = source_root / "DOCUMENTATION.md"
    if not map_path.is_file():
        errors.append("DOCUMENTATION.md is missing")
        return
    try:
        text = map_path.read_text(encoding="utf-8-sig")
    except UnicodeDecodeError as error:
        errors.append(f"DOCUMENTATION.md is not valid UTF-8: {error}")
        return
    linked_documents: set[Path] = set()
    for match in MARKDOWN_LINK.finditer(text):
        target = local_target(match.group(1))
        if not target or not target.lower().endswith(".md"):
            continue
        try:
            target_path = resolve_child(source_root, target, label="DOCUMENTATION.md link")
        except ValueError as error:
            errors.append(f"DOCUMENTATION.md has invalid document link: {error}")
            continue
        if target_path.is_file():
            linked_documents.add(target_path.relative_to(source_root))
    for path in sorted(expected_documents - linked_documents, key=lambda item: item.as_posix()):
        errors.append(f"DOCUMENTATION.md does not list allowlisted Markdown document: {path.as_posix()}")
    for path in sorted(linked_documents - expected_documents, key=lambda item: item.as_posix()):
        errors.append(f"DOCUMENTATION.md lists Markdown document outside documentation portal allowlist: {path.as_posix()}")


def validate_site(
    site: Path, *, source_root: Path | None = None, allowlist_path: Path | None = None
) -> list[str]:
    errors: list[str] = []
    if not site.is_dir():
        return [f"site directory does not exist: {site}"]
    for relative_path in DOCUMENTATION_PORTAL_REQUIRED_FILES:
        if not (site / relative_path).is_file():
            errors.append(f"required public file is missing: {relative_path}")
    if (site / "docs" / "public").exists():
        errors.append("retired docs/public directory must not be published")
    validate_text_encoding(site, errors)
    validate_structured_files(site, errors)
    validate_local_links(site, errors)
    validate_portal_entrypoint(site, errors)
    validate_portal_click_depth(site, errors)
    validate_rendered_human_docs(site, errors)
    validate_rendered_fragments(site, errors)
    if source_root is not None or allowlist_path is not None:
        if source_root is None or allowlist_path is None:
            errors.append("allowlist validation requires both source_root and allowlist_path")
        else:
            validate_allowlist_coverage(site, source_root, allowlist_path, errors)
            validate_documentation_map(source_root, allowlist_path, errors)
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, default=REPO_ROOT)
    parser.add_argument("--allowlist", type=Path, default=ALLOWLIST_PATH)
    args = parser.parse_args(argv)
    errors = validate_site(args.site, source_root=args.source_root, allowlist_path=args.allowlist)
    if errors:
        print("Public-site validation failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"Public-site validation passed: {args.site}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
