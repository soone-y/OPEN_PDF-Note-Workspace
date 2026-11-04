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
from urllib.parse import quote, unquote, urlsplit


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
PUBLIC_BASE_URL = "https://soone-y.github.io/OPEN_PDF-Note-Workspace/"
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

    def __init__(self, *, section_id: str | None = None) -> None:
        super().__init__()
        self.hrefs: list[str] = []
        self._details_stack: list[bool] = []
        self._site_menu_depth = 0
        self._section_id = section_id
        self._section_depth = 0
        self._active_section_depth: int | None = None

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attributes = dict(attrs)
        if tag.lower() == "section":
            self._section_depth += 1
            if attributes.get("id") == self._section_id and self._section_id is not None:
                self._active_section_depth = self._section_depth
        if tag.lower() == "details":
            is_site_menu = "site-menu" in (attributes.get("class") or "").split()
            self._details_stack.append(is_site_menu)
            if is_site_menu:
                self._site_menu_depth += 1
            return
        if (tag.lower() == "a" and self._site_menu_depth == 0
                and (self._section_id is None or self._active_section_depth is not None)):
            href = attributes.get("href")
            if href:
                self.hrefs.append(href)

    def handle_endtag(self, tag: str) -> None:
        if tag.lower() == "section":
            if self._section_depth == self._active_section_depth:
                self._active_section_depth = None
            self._section_depth -= 1
        if tag.lower() != "details" or not self._details_stack:
            return
        if self._details_stack.pop():
            self._site_menu_depth -= 1


class HTMLReferenceParser(HTMLParser):
    """Collect real element references; escaped inline code is not a link."""

    def __init__(self) -> None:
        super().__init__()
        self.ids: list[str] = []
        self.references: list[str] = []
        self.id_references: list[str] = []
        self.images_without_alt = 0
        self.indexing_blocked = False
        self.lang = ""
        self.metadata: dict[str, list[str]] = {}
        self.canonicals: list[str] = []
        self.alternates: list[tuple[str, str]] = []
        self.titles: list[str] = []
        self.language_switches: list[str] = []
        self._head = False
        self._title: list[str] | None = None

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attributes = dict(attrs)
        if tag == "html":
            self.lang = attributes.get("lang") or ""
        if tag == "head":
            self._head = True
        if self._head and tag == "title":
            self._title = []
        if self._head and tag == "meta":
            name = attributes.get("name") or attributes.get("property")
            if name:
                self.metadata.setdefault(name.lower(), []).append(attributes.get("content") or "")
        if self._head and tag == "link":
            relations = (attributes.get("rel") or "").lower().split()
            if "canonical" in relations:
                self.canonicals.append(attributes.get("href") or "")
            if "alternate" in relations and attributes.get("hreflang"):
                self.alternates.append((attributes["hreflang"], attributes.get("href") or ""))
        if tag == "a" and "language-switch" in (attributes.get("class") or "").split():
            self.language_switches.append(attributes.get("href") or "")
        if "id" in attributes:
            self.ids.append(attributes["id"] or "")
        for name in ("href", "src"):
            if attributes.get(name):
                self.references.append(attributes[name])
        for name in ("aria-controls", "aria-labelledby", "aria-describedby", "for"):
            self.id_references.extend((attributes.get(name) or "").split())
        if tag == "img" and "alt" not in attributes:
            self.images_without_alt += 1
        if tag == "meta" and (attributes.get("name") or "").lower() in ("robots", "googlebot"):
            tokens = set(re.split(r"[\s,;:]+", (attributes.get("content") or "").lower()))
            self.indexing_blocked |= bool(tokens & {"noindex", "none"})

    def handle_data(self, data: str) -> None:
        if self._title is not None:
            self._title.append(data)

    def handle_endtag(self, tag: str) -> None:
        if tag == "title" and self._title is not None:
            self.titles.append("".join(self._title).strip())
            self._title = None
        if tag == "head":
            self._head = False


def public_url(relative: Path) -> str:
    if relative.as_posix() == "index.html":
        return PUBLIC_BASE_URL
    if relative.as_posix() == "en/index.html":
        return PUBLIC_BASE_URL + "en/"
    return PUBLIC_BASE_URL + quote(relative.as_posix())


def validate_public_metadata(site: Path, errors: list[str]) -> None:
    """Check local head metadata and reciprocal translations without fetching URLs."""
    pages: dict[str, HTMLReferenceParser] = {}
    for path in site.rglob("*.html"):
        parser = HTMLReferenceParser()
        parser.feed(path.read_text(encoding="utf-8-sig"))
        relative = path.relative_to(site)
        canonical = public_url(relative)
        pages[canonical] = parser
        label = relative.as_posix()
        if len(parser.titles) != 1 or not parser.titles[0]:
            errors.append(f"HTML must declare one non-empty title: {label}")
        if parser.canonicals != [canonical]:
            errors.append(f"HTML canonical must match its public path: {label}")
        description = parser.metadata.get("description", [])
        if len(description) != 1 or not description[0].strip():
            errors.append(f"HTML must declare one non-empty description: {label}")
        required = {
            "og:url": [canonical], "og:title": parser.titles,
            "twitter:title": parser.titles, "og:description": description,
            "twitter:description": description, "twitter:card": ["summary_large_image"],
            "og:type": ["website" if label in ("index.html", "en/index.html") else "article"],
            "og:locale": ["en_US" if parser.lang == "en" else "ja_JP"],
            "og:image": [PUBLIC_BASE_URL + "docs/images/app_overview.png"],
            "twitter:image": [PUBLIC_BASE_URL + "docs/images/app_overview.png"],
        }
        for name, expected in required.items():
            if not expected or parser.metadata.get(name) != expected:
                errors.append(f"HTML metadata missing or inconsistent: {label} -> {name}")
        for name in ("og:image:alt", "twitter:image:alt"):
            values = parser.metadata.get(name, [])
            if len(values) != 1 or not values[0].strip():
                errors.append(f"HTML metadata missing or empty: {label} -> {name}")
        if parser.lang not in ("ja", "en"):
            errors.append(f"HTML must declare a supported document language: {label}")
        if parser.language_switches or label in ("index.html", "en/index.html"):
            if len(parser.alternates) != 2 or {lang for lang, _ in parser.alternates} != {"ja", "en"}:
                errors.append(f"paired HTML must declare ja/en hreflang: {label}")
        if parser.alternates and (parser.lang, canonical) not in parser.alternates:
            errors.append(f"hreflang must include the current document: {label}")
        for href in parser.language_switches:
            target_path = (path.parent / unquote(urlsplit(href).path)).resolve()
            if target_path.is_relative_to(site.resolve()):
                target_url = public_url(target_path.relative_to(site.resolve()))
                if target_url not in {url for _, url in parser.alternates}:
                    errors.append(f"hreflang must match the visible language switch: {label}")
    if not (site / "docs/images/app_overview.png").is_file():
        errors.append("social preview image is missing from the public site")
    for canonical, parser in pages.items():
        for language, target in parser.alternates:
            counterpart = pages.get(target)
            if counterpart is None or counterpart.lang != language:
                errors.append(f"hreflang target is missing or has wrong language: {canonical} -> {target}")
            elif set(counterpart.alternates) != set(parser.alternates):
                errors.append(f"hreflang must be reciprocal: {canonical} -> {target}")


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


def markdown_prose(text: str) -> str:
    """Return prose for link checks; fenced examples are literal source."""
    marker = ""
    length = 0
    lines: list[str] = []
    for line in text.splitlines(keepends=True):
        fence = re.match(r"^ {0,3}(`{3,}|~{3,})(.*)$", line.rstrip("\r\n"))
        if marker:
            if fence and fence[1][0] == marker and len(fence[1]) >= length and not fence[2].strip():
                marker = ""
            continue
        if fence and not (fence[1][0] == "`" and "`" in fence[2]):
            marker, length = fence[1][0], len(fence[1])
            continue
        lines.append(line)
    return "".join(lines)


def validate_local_links(site: Path, errors: list[str]) -> None:
    for path in site.rglob("*.md"):
        text = path.read_text(encoding="utf-8-sig")
        for match in MARKDOWN_LINK.finditer(markdown_prose(text)):
            target = local_target(match.group(1))
            if target:
                validate_local_target(site, path, target, kind="Markdown link or image", errors=errors)
    site_root = site.resolve()
    documents: dict[Path, HTMLReferenceParser] = {}
    for path in site.rglob("*.html"):
        parser = HTMLReferenceParser()
        parser.feed(path.read_text(encoding="utf-8-sig"))
        documents[path.resolve()] = parser
        label = path.relative_to(site).as_posix()
        if len(parser.ids) != len(set(parser.ids)) or "" in parser.ids:
            errors.append(f"duplicate or empty HTML IDs: {label}")
        if parser.images_without_alt:
            errors.append(f"HTML images must have alt attributes: {label}")
        if parser.indexing_blocked:
            errors.append(f"public HTML must not block indexing via meta robots/googlebot: {label}")
        for reference in parser.id_references:
            if reference not in parser.ids:
                errors.append(f"broken HTML control or label reference: {label} -> {reference}")
    for path, parser in documents.items():
        for reference in parser.references:
            try:
                parts = urlsplit(reference)
            except ValueError as error:
                errors.append(f"invalid HTML URL: {path.relative_to(site_root)} -> {reference} ({error})")
                continue
            if parts.scheme in ("http", "https", "mailto", "tel", "data") or (parts.netloc and not parts.scheme):
                continue
            if parts.scheme:
                errors.append(f"unsupported HTML URL scheme: {path.relative_to(site_root)} -> {reference}")
                continue
            decoded_path = unquote(parts.path)
            if "\\" in decoded_path or "\x00" in decoded_path:
                errors.append(f"invalid local HTML URL: {path.relative_to(site_root)} -> {reference}")
                continue
            target = (path.parent / decoded_path).resolve() if decoded_path else path
            if target != site_root and site_root not in target.parents:
                errors.append(f"local HTML link or asset escapes public site: {path.relative_to(site_root)} -> {reference}")
                continue
            if target.is_dir():
                target = (target / "index.html").resolve()
                if target != site_root and site_root not in target.parents:
                    errors.append(f"local HTML link or asset escapes public site: {path.relative_to(site_root)} -> {reference}")
                    continue
            if not target.is_file():
                errors.append(f"broken HTML link or asset: {path.relative_to(site_root)} -> {reference}")
            elif parts.fragment and target.suffix.lower() == ".html":
                destination = documents.get(target)
                if destination is None or unquote(parts.fragment) not in destination.ids:
                    errors.append(f"broken HTML fragment: {path.relative_to(site_root)} -> {reference}")


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


def visible_local_html_links(site: Path, source_path: Path, *, section_id: str | None = None) -> set[Path]:
    """Return local HTML targets linked outside a collapsible site-menu."""
    parser = VisibleAnchorParser(section_id=section_id)
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


def validate_portal_jump_index(site: Path, source_root: Path, allowlist_path: Path, errors: list[str]) -> None:
    """Both portal inventories must directly link every published document."""
    try:
        expected, _ = allowlisted_portal_paths(source_root.resolve(), allowlist_path.resolve())
    except ValueError as error:
        errors.append(f"documentation portal allowlist is invalid: {error}")
        return
    documents = {path.with_suffix(".html") for path in expected if path.suffix.lower() == ".md"}
    for relative in ("index.html", "en/index.html"):
        page = site / relative
        if not page.is_file():
            errors.append(f"portal jump index page is missing: {relative}")
            continue
        linked = visible_local_html_links(site, page, section_id="all-documents")
        for missing in sorted(documents - linked, key=lambda path: path.as_posix()):
            errors.append(f"portal jump index does not directly link document: {relative} -> {missing.as_posix()}")


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
    for check in (
        validate_structured_files, validate_local_links, validate_public_metadata, validate_portal_entrypoint,
        validate_portal_click_depth, validate_rendered_human_docs, validate_rendered_fragments,
    ):
        try:
            check(site, errors)
        except (OSError, UnicodeError, ValueError) as error:
            errors.append(f"{check.__name__} could not complete: {error}")
    if source_root is not None or allowlist_path is not None:
        if source_root is None or allowlist_path is None:
            errors.append("allowlist validation requires both source_root and allowlist_path")
        else:
            validate_allowlist_coverage(site, source_root, allowlist_path, errors)
            validate_documentation_map(source_root, allowlist_path, errors)
            validate_portal_jump_index(site, source_root, allowlist_path, errors)
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
