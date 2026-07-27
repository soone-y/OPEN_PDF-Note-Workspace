#!/usr/bin/env python3
"""Synchronize only GitHub Pages source inputs into an isolated worktree.

The allowlist is the single definition of the documentation portal inputs.
This tool is deliberately separate from the Cloudflare introduction site.
"""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path


FORBIDDEN_PUBLIC_REFERENCES = (
    "DEV_PDF-Note-Workspace",
    "github.com/soone-y/DEV_PDF-Note-Workspace",
)
TEXT_EXTENSIONS = {".html", ".json", ".md", ".py", ".txt", ".xml", ".yaml", ".yml"}
DEVELOPMENT_ONLY_RETIRED_PATHS = (
    "build.ps1",
    "release.ps1",
    "scripts",
    "src",
    "tests",
    "tools",
    "third_party",
    "release_assets",
    "docs/internal",
    "tools/dev/build_public_site.py",
    "tools/dev/validate_public_site.py",
    "tools/dev/render_human_docs.py",
    "site/github/scripts/sync_publication_inputs.py",
    "site/scripts",
    "site/src",
)


def child_path(root: Path, relative: str, *, label: str) -> Path:
    if not isinstance(relative, str) or not relative or Path(relative).is_absolute():
        raise ValueError(f"{label} must be a non-empty relative path")
    resolved_root = root.resolve()
    candidate = (resolved_root / relative).resolve()
    if candidate == resolved_root or resolved_root not in candidate.parents:
        raise ValueError(f"{label} escapes its root: {relative}")
    return candidate


def load_allowlist(source_root: Path) -> dict:
    path = source_root / "site" / "github" / "documentation_portal_allowlist.json"
    try:
        payload = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"Invalid GitHub Pages allowlist: {error}") from error
    if payload.get("schema_version") != 1:
        raise ValueError("Unsupported GitHub Pages allowlist schema_version")
    if not isinstance(payload.get("documentation_portal"), dict):
        raise ValueError("GitHub Pages documentation_portal is missing")
    if not isinstance(payload.get("pages_submission"), dict):
        raise ValueError("GitHub Pages pages_submission is missing")
    return payload


def copy_file(source_root: Path, destination_root: Path, entry: dict) -> None:
    source = child_path(source_root, entry.get("source", ""), label="allowlist source")
    destination = child_path(destination_root, entry.get("destination", ""), label="allowlist destination")
    if not source.is_file():
        raise FileNotFoundError(f"Required publication input is missing: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def clear_destination(destination: Path) -> None:
    if destination.is_dir():
        shutil.rmtree(destination)
    elif destination.exists():
        destination.unlink()


def copy_tree(source_root: Path, destination_root: Path, entry: dict) -> None:
    source = child_path(source_root, entry.get("source", ""), label="allowlist tree source")
    destination = child_path(destination_root, entry.get("destination", ""), label="allowlist tree destination")
    if not source.is_dir():
        raise FileNotFoundError(f"Required publication tree is missing: {source}")
    excluded = tuple(entry.get("exclude_prefixes", []))
    if not all(isinstance(prefix, str) and prefix for prefix in excluded):
        raise ValueError("tree exclude_prefixes must contain non-empty strings")
    clear_destination(destination)
    for source_file in source.rglob("*"):
        if not source_file.is_file():
            continue
        relative = source_file.relative_to(source).as_posix()
        if any(relative.startswith(prefix) for prefix in excluded):
            continue
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_file, target)


def copy_document_markdown(source_root: Path, destination_root: Path, rule: dict) -> None:
    source = child_path(source_root, rule.get("source", ""), label="document source")
    destination = child_path(destination_root, rule.get("destination", ""), label="document destination")
    if not source.is_dir():
        raise FileNotFoundError(f"Required documentation directory is missing: {source}")
    excluded = set(rule.get("exclude", []))
    if not all(isinstance(name, str) and name for name in excluded):
        raise ValueError("document exclude must contain non-empty strings")
    clear_destination(destination)
    destination.mkdir(parents=True, exist_ok=True)
    for markdown_file in source.glob("*.md"):
        if markdown_file.name not in excluded:
            shutil.copy2(markdown_file, destination / markdown_file.name)


def remove_retired_paths(destination_root: Path, retired_paths: object) -> None:
    if not isinstance(retired_paths, list):
        raise ValueError("pages_submission.retired_paths must be an array")
    for relative in retired_paths:
        target = child_path(destination_root, relative, label="retired publication path")
        clear_destination(target)


def collect_allowed_destination_paths(source_root: Path, destination_root: Path, portal: dict, submission: dict) -> set[str]:
    """Return the exact public files that the allowlist permits in a worktree."""
    allowed: set[str] = set()

    def add_file_destination(entry: dict) -> None:
        target = child_path(destination_root, entry.get("destination", ""), label="allowlist destination")
        allowed.add(target.relative_to(destination_root).as_posix())

    for entry in [*portal.get("files", []), *submission.get("files", [])]:
        if not isinstance(entry, dict):
            raise ValueError("allowlist file entries must be objects")
        add_file_destination(entry)

    for entry in [*portal.get("trees", []), *submission.get("trees", [])]:
        if not isinstance(entry, dict):
            raise ValueError("allowlist tree entries must be objects")
        source = child_path(source_root, entry.get("source", ""), label="allowlist tree source")
        destination = child_path(destination_root, entry.get("destination", ""), label="allowlist tree destination")
        excluded = tuple(entry.get("exclude_prefixes", []))
        if not all(isinstance(prefix, str) and prefix for prefix in excluded):
            raise ValueError("tree exclude_prefixes must contain non-empty strings")
        for source_file in source.rglob("*"):
            if source_file.is_file():
                relative = source_file.relative_to(source).as_posix()
                if not any(relative.startswith(prefix) for prefix in excluded):
                    allowed.add((destination / relative).relative_to(destination_root).as_posix())

    document_rule = portal.get("document_markdown")
    if document_rule is not None:
        if not isinstance(document_rule, dict):
            raise ValueError("document_markdown must be an object")
        source = child_path(source_root, document_rule.get("source", ""), label="document source")
        destination = child_path(destination_root, document_rule.get("destination", ""), label="document destination")
        excluded = set(document_rule.get("exclude", []))
        if not all(isinstance(name, str) and name for name in excluded):
            raise ValueError("document exclude must contain non-empty strings")
        for markdown_file in source.glob("*.md"):
            if markdown_file.name not in excluded:
                allowed.add((destination / markdown_file.name).relative_to(destination_root).as_posix())

    return allowed


def remove_unallowlisted_files(destination_root: Path, allowed_paths: set[str]) -> None:
    """Keep the public worktree default-deny: only explicit allowlist files survive."""
    for path in destination_root.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(destination_root)
        if ".git" in relative.parts:
            continue
        if relative.as_posix() not in allowed_paths:
            path.unlink()


def validate_publication_inputs(destination_root: Path) -> None:
    """Reject private development-repository references before the public commit."""
    for path in destination_root.rglob("*"):
        if not path.is_file() or ".git" in path.relative_to(destination_root).parts:
            continue
        if path.suffix.lower() not in TEXT_EXTENSIONS:
            continue
        try:
            text = path.read_text(encoding="utf-8-sig")
        except UnicodeDecodeError as error:
            raise ValueError(f"Public publication input is not valid UTF-8: {path.relative_to(destination_root)}") from error
        if any(reference in text for reference in FORBIDDEN_PUBLIC_REFERENCES):
            raise ValueError(
                f"Public publication input contains a private development reference: {path.relative_to(destination_root)}"
            )


def sync_publication_inputs(source_root: Path, destination_root: Path) -> None:
    source_root = source_root.resolve()
    destination_root = destination_root.resolve()
    if not source_root.is_dir() or not destination_root.is_dir():
        raise ValueError("source-root and destination-root must be existing directories")
    allowlist = load_allowlist(source_root)
    portal = allowlist["documentation_portal"]
    submission = allowlist["pages_submission"]
    allowed_paths = collect_allowed_destination_paths(source_root, destination_root, portal, submission)

    for entry in portal.get("files", []):
        copy_file(source_root, destination_root, entry)
    for entry in portal.get("trees", []):
        copy_tree(source_root, destination_root, entry)
    document_rule = portal.get("document_markdown")
    if document_rule is not None:
        copy_document_markdown(source_root, destination_root, document_rule)
    for entry in submission.get("files", []):
        copy_file(source_root, destination_root, entry)
    for entry in submission.get("trees", []):
        copy_tree(source_root, destination_root, entry)
    retired_paths = submission.get("retired_paths")
    if not isinstance(retired_paths, list):
        raise ValueError("pages_submission.retired_paths must be an array")
    remove_retired_paths(destination_root, [*retired_paths, *DEVELOPMENT_ONLY_RETIRED_PATHS])
    remove_unallowlisted_files(destination_root, allowed_paths)
    validate_publication_inputs(destination_root)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--destination-root", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        sync_publication_inputs(args.source_root, args.destination_root)
    except (OSError, ValueError) as error:
        print(f"GitHub Pages input synchronization failed: {error}")
        return 1
    print(f"GitHub Pages publication inputs synchronized: {args.destination_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
