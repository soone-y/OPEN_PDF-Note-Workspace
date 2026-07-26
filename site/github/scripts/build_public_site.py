#!/usr/bin/env python3
"""Build the deliberately limited static public site.

The output directory is intentionally separate from application build output.
It contains only files selected here and is used by GitHub Pages. Existing
output is never overwritten by this tool.
"""

from __future__ import annotations

import shutil
import sys
import uuid
import argparse
import json
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
GITHUB_SITE_ROOT = Path(__file__).resolve().parents[1]
OUTPUT_DIR = GITHUB_SITE_ROOT / "output" / "public"
ALLOWLIST_PATH = GITHUB_SITE_ROOT / "documentation_portal_allowlist.json"
VERSION_TOKEN = "__APP_VERSION__"
DEVELOPER_BUILD_GUIDE_URL = (
    "https://github.com/soone-y/DEV_PDF-Note-Workspace/blob/dev/docs/public/How_to_Build.md"
)


def copy_file(source: Path, destination: Path) -> None:
    if not source.is_file():
        raise FileNotFoundError(f"Required public-site input is missing: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def copy_tree(source: Path, destination: Path) -> None:
    if not source.is_dir():
        raise FileNotFoundError(f"Required public-site input directory is missing: {source}")
    shutil.copytree(source, destination, dirs_exist_ok=True)


def resolve_repo_relative_path(relative_path: str, *, label: str) -> Path:
    if not isinstance(relative_path, str) or not relative_path or Path(relative_path).is_absolute():
        raise ValueError(f"{label} must be a non-empty relative path")
    candidate = (REPO_ROOT / relative_path).resolve()
    if candidate != REPO_ROOT and REPO_ROOT not in candidate.parents:
        raise ValueError(f"{label} escapes the repository: {relative_path}")
    return candidate


def resolve_staging_relative_path(staging_dir: Path, relative_path: str, *, label: str) -> Path:
    if not isinstance(relative_path, str) or not relative_path or Path(relative_path).is_absolute():
        raise ValueError(f"{label} must be a non-empty relative path")
    candidate = (staging_dir / relative_path).resolve()
    if staging_dir.resolve() not in candidate.parents:
        raise ValueError(f"{label} escapes the site staging directory: {relative_path}")
    return candidate


def load_allowlist() -> dict:
    try:
        allowlist = json.loads(ALLOWLIST_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"Invalid public-site allowlist: {error}") from error
    if allowlist.get("schema_version") != 1:
        raise ValueError("Unsupported public-site allowlist schema_version")
    return allowlist


def copy_allowlisted_content(staging_dir: Path, allowlist: dict) -> None:
    rules = allowlist.get("documentation_portal", allowlist)
    if not isinstance(rules, dict):
        raise ValueError("Allowlist documentation_portal profile must be an object")

    for entry in rules.get("files", []):
        if not isinstance(entry, dict):
            raise ValueError("Allowlist files entries must be objects")
        source = resolve_repo_relative_path(entry.get("source", ""), label="allowlist file source")
        destination = resolve_staging_relative_path(
            staging_dir, entry.get("destination", ""), label="allowlist file destination"
        )
        copy_file(source, destination)

    for entry in rules.get("trees", []):
        if not isinstance(entry, dict):
            raise ValueError("Allowlist trees entries must be objects")
        source = resolve_repo_relative_path(entry.get("source", ""), label="allowlist tree source")
        destination = resolve_staging_relative_path(
            staging_dir, entry.get("destination", ""), label="allowlist tree destination"
        )
        copy_tree(source, destination)

    document_rule = rules.get("document_markdown")
    if document_rule is None:
        return
    if not isinstance(document_rule, dict):
        raise ValueError("Allowlist document_markdown entry must be an object")
    source = resolve_repo_relative_path(document_rule.get("source", ""), label="allowlist document source")
    destination = resolve_staging_relative_path(
        staging_dir, document_rule.get("destination", ""), label="allowlist document destination"
    )
    excluded = set(document_rule.get("exclude", []))
    destination.mkdir(parents=True, exist_ok=True)
    for markdown_file in source.glob("*.md"):
        if markdown_file.name not in excluded:
            copy_file(markdown_file, destination / markdown_file.name)


def replace_version_tokens(site_dir: Path, version: str) -> None:
    for markdown_file in site_dir.rglob("*.md"):
        text = markdown_file.read_text(encoding="utf-8")
        markdown_file.write_text(text.replace(VERSION_TOKEN, version), encoding="utf-8")


def replace_developer_only_links(site_dir: Path) -> None:
    """Keep public pages navigable while keeping the developer build guide private."""
    targets = (site_dir / "README.md", site_dir / "docs" / "public" / "Index.md")
    for markdown_file in targets:
        if not markdown_file.is_file():
            continue
        text = markdown_file.read_text(encoding="utf-8-sig")
        text = text.replace("docs/public/How_to_Build.md", DEVELOPER_BUILD_GUIDE_URL)
        text = text.replace("(How_to_Build.md)", f"({DEVELOPER_BUILD_GUIDE_URL})")
        markdown_file.write_text(text, encoding="utf-8")


def build_site(*, replace: bool = False, documentation_portal: bool = False) -> int:
    if OUTPUT_DIR.exists():
        if not replace:
            print(
                f"Refusing to overwrite existing output directory: {OUTPUT_DIR}\n"
                "Pass --replace only when it is the tracked generated site output.",
                file=sys.stderr,
            )
            return 2
        if OUTPUT_DIR.resolve() != (GITHUB_SITE_ROOT / "output" / "public").resolve():
            print(f"Refusing to replace an unexpected output directory: {OUTPUT_DIR}", file=sys.stderr)
            return 2

    staging_dir = REPO_ROOT / f".site-staging-{uuid.uuid4().hex}"
    try:
        staging_dir.mkdir()

        allowlist = load_allowlist()
        copy_allowlisted_content(staging_dir, allowlist)
        copy_file(GITHUB_SITE_ROOT / "index.html", staging_dir / "index.html")

        version_source = resolve_repo_relative_path(allowlist.get("version_source", ""), label="allowlist version source")
        version = version_source.read_text(encoding="utf-8").strip()
        if not version:
            raise ValueError("REPO_VERSION.txt must contain a version")
        replace_version_tokens(staging_dir, version)
        replace_developer_only_links(staging_dir)

        # Keep human-readable HTML local and leave raw Markdown unchanged for AI clients.
        tool_directory = Path(__file__).resolve().parent
        sys.path.insert(0, str(tool_directory))
        try:
            from render_human_docs import main as render_human_docs
        finally:
            sys.path.pop(0)

        if render_human_docs([str(staging_dir)]) != 0:
            raise RuntimeError("render_human_docs.py failed")

        if OUTPUT_DIR.exists():
            shutil.rmtree(OUTPUT_DIR)
        OUTPUT_DIR.parent.mkdir(parents=True, exist_ok=True)
        staging_dir.rename(OUTPUT_DIR)
        print(f"Built selected public documentation: {OUTPUT_DIR}")
        return 0
    except Exception as error:
        print(f"Public-site build failed: {error}", file=sys.stderr)
        return 1
    finally:
        if staging_dir.exists():
            shutil.rmtree(staging_dir)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--replace", action="store_true", help="replace the generated site/github/output/public directory")
    parser.add_argument(
        "--documentation-portal",
        action="store_true",
        help="build the repository documentation portal (the only supported profile)",
    )
    arguments = parser.parse_args()
    raise SystemExit(build_site(replace=arguments.replace, documentation_portal=arguments.documentation_portal))
