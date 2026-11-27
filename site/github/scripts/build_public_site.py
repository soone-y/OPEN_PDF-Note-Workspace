#!/usr/bin/env python3
"""Build the deliberately limited static public site.

The output directory is intentionally separate from application build output.
It contains only files selected here and is used by GitHub Pages. An explicit
replacement preserves the prior generation unless the staged generation is
ready to take its place.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import uuid
import argparse
import json
import re
from datetime import datetime
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
GITHUB_SITE_ROOT = Path(__file__).resolve().parents[1]
OUTPUT_DIR = GITHUB_SITE_ROOT / "output" / "public"
ALLOWLIST_PATH = GITHUB_SITE_ROOT / "documentation_portal_allowlist.json"
VERSION_TOKEN = "__APP_VERSION__"
DOCUMENT_COUNT_TOKEN = "__PUBLIC_DOCUMENT_COUNT__"
RELEASE_VERSION_MARKER = "(ZIP配布物ではここにバージョンが記載されます)"
RELEASE_VERSION_MARKER_LINE = re.compile(
    r"^[^\r\n]*" + re.escape(RELEASE_VERSION_MARKER) + r"[ \t]*(?:\r?\n(?:[ \t]*\r?\n)?|$)", re.MULTILINE)
WEB_ANALYTICS_SNIPPET = """<!-- Cloudflare Web Analytics -->
<script type='module' src='https://static.cloudflareinsights.com/beacon.min.js' data-cf-beacon='{"token": "385b96f498964281bc0c2a771cebd04f"}'></script>
<!-- End Cloudflare Web Analytics -->"""


def copy_file(source: Path, destination: Path) -> None:
    if not source.is_file():
        raise FileNotFoundError(f"Required public-site input is missing: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def copy_tree(source: Path, destination: Path, *, exclude_prefixes: tuple[str, ...] = ()) -> None:
    if not source.is_dir():
        raise FileNotFoundError(f"Required public-site input directory is missing: {source}")
    if not all(isinstance(prefix, str) and prefix for prefix in exclude_prefixes):
        raise ValueError("allowlist tree exclude_prefixes must contain non-empty strings")
    for source_file in source.rglob("*"):
        if not source_file.is_file():
            continue
        relative = source_file.relative_to(source).as_posix()
        if any(relative.startswith(prefix) for prefix in exclude_prefixes):
            continue
        copy_file(source_file, destination / relative)


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


def copy_allowlisted_content(staging_dir: Path, allowlist: dict) -> dict[str, Path]:
    """Copy the allowlisted inputs and return staged Markdown paths mapped to their sources."""
    rules = allowlist.get("documentation_portal", allowlist)
    if not isinstance(rules, dict):
        raise ValueError("Allowlist documentation_portal profile must be an object")
    markdown_sources: dict[str, Path] = {}

    def record(source: Path, destination: Path) -> None:
        if destination.suffix.lower() == ".md":
            markdown_sources[destination.resolve().relative_to(staging_dir.resolve()).as_posix()] = source

    for entry in rules.get("files", []):
        if not isinstance(entry, dict):
            raise ValueError("Allowlist files entries must be objects")
        source = resolve_repo_relative_path(entry.get("source", ""), label="allowlist file source")
        destination = resolve_staging_relative_path(
            staging_dir, entry.get("destination", ""), label="allowlist file destination"
        )
        copy_file(source, destination)
        record(source, destination)

    for entry in rules.get("trees", []):
        if not isinstance(entry, dict):
            raise ValueError("Allowlist trees entries must be objects")
        source = resolve_repo_relative_path(entry.get("source", ""), label="allowlist tree source")
        destination = resolve_staging_relative_path(
            staging_dir, entry.get("destination", ""), label="allowlist tree destination"
        )
        excluded = tuple(entry.get("exclude_prefixes", []))
        copy_tree(source, destination, exclude_prefixes=excluded)
        for source_file in source.rglob("*.md"):
            relative = source_file.relative_to(source).as_posix()
            if source_file.is_file() and not any(relative.startswith(prefix) for prefix in excluded):
                record(source_file, destination / relative)

    document_rule = rules.get("document_markdown")
    if document_rule is None:
        return markdown_sources
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
            record(markdown_file, destination / markdown_file.name)
    return markdown_sources


def run_git(*args: str) -> str | None:
    """Run a read-only local git query; None when git or the history is unavailable."""
    try:
        completed = subprocess.run(
            ["git", "-C", str(REPO_ROOT), *args],
            capture_output=True, text=True, encoding="utf-8", check=False,
        )
    except OSError:
        return None
    return completed.stdout if completed.returncode == 0 else None


RELEASE_TAG_PATTERN = "v[0-9]*"


def release_version_for_commit(commit: str, version_source: str) -> dict[str, str]:
    """Name the release a commit belongs to, using the release tags (vX.Y.Z).

    - "release": the first release tag that contains the commit.
    - "after": no release contains it yet; the latest release before it.
    - "at": no usable release tag (such as the development history);
      REPO_VERSION.txt as it was in that commit.
    """
    def tag_version(tag: str) -> str:
        # Some releases were tagged per language (v0.9.500-en / v0.9.500-ja); show one version.
        return re.sub(r"-(?:en|ja)$", "", tag[1:])

    contained = run_git("tag", "--contains", commit, "--list", RELEASE_TAG_PATTERN, "--sort=v:refname")
    first = next((line.strip() for line in (contained or "").splitlines() if line.strip()), "")
    if first:
        return {"version": tag_version(first), "version_kind": "release"}
    previous = (run_git("describe", "--tags", "--abbrev=0", "--match", RELEASE_TAG_PATTERN, commit) or "").strip()
    if previous:
        return {"version": tag_version(previous), "version_kind": "after"}
    return {"version": (run_git("show", f"{commit}:{version_source}") or "").strip(), "version_kind": "at"}


def collect_page_updates(markdown_sources: dict[str, Path], version_source: str,
                         current_version: str) -> dict[str, dict[str, str]]:
    """Map each staged Markdown page to the date and release of its source's last change.

    Only sources whose content changed get a new date. The release comes from
    release_version_for_commit. A source with uncommitted changes is dated now
    and given the current version as "at".
    """
    updates: dict[str, dict[str, str]] = {}
    versions: dict[str, dict[str, str]] = {}
    if (run_git("rev-parse", "--is-shallow-repository") or "").strip() != "false":
        # A shallow clone would date every page by its newest commit.
        print("Public-site build: full git history unavailable; page update lines omitted", file=sys.stderr)
        return {}
    for staged, source in sorted(markdown_sources.items()):
        repo_relative = source.relative_to(REPO_ROOT).as_posix()
        dirty = run_git("status", "--porcelain", "--", repo_relative)
        if dirty is None:
            print("Public-site build: git history unavailable; page update lines omitted", file=sys.stderr)
            return {}
        if dirty.strip():
            updates[staged] = {
                "datetime": datetime.now().astimezone().isoformat(timespec="minutes"),
                "version": current_version,
                "version_kind": "at",
            }
            continue
        log = run_git("log", "-1", "--format=%H%x09%cI", "--", repo_relative)
        if not log or "\t" not in log:
            continue
        commit, committed_at = log.strip().split("\t", 1)
        if commit not in versions:
            versions[commit] = release_version_for_commit(commit, version_source)
        updates[staged] = {"datetime": committed_at, **versions[commit]}
    return updates


def replace_version_tokens(site_dir: Path, version: str) -> None:
    for markdown_file in site_dir.rglob("*.md"):
        text = markdown_file.read_text(encoding="utf-8")
        text = text.replace(VERSION_TOKEN, version)
        # The ZIP release replaces this marker with its version (pack_release.ps1).
        # The site shows each page's last update under the title instead, so the
        # placeholder line is dropped rather than published unreplaced.
        text = RELEASE_VERSION_MARKER_LINE.sub("", text)
        markdown_file.write_text(text, encoding="utf-8")


def replace_document_count_tokens(site_dir: Path) -> None:
    """Count rendered documents once per Markdown source, including each locale."""
    count = sum(1 for path in site_dir.rglob("*.md") if path.with_suffix(".html").is_file())
    for relative in ("index.html", "en/index.html"):
        page = site_dir / relative
        if page.is_file():
            text = page.read_text(encoding="utf-8")
            page.write_text(text.replace(DOCUMENT_COUNT_TOKEN, str(count)), encoding="utf-8")


def add_web_analytics(site_dir: Path) -> None:
    """Add the supplied beacon only to the staged website, never source documents."""
    for html_file in sorted(site_dir.rglob("*.html")):
        text = html_file.read_text(encoding="utf-8")
        if text.count("</body>") != 1 or "data-cf-beacon" in text:
            raise ValueError(f"Expected one body end and no existing analytics: {html_file}")
        html_file.write_text(text.replace("</body>", WEB_ANALYTICS_SNIPPET + "\n</body>"), encoding="utf-8")


def replace_staged_site(staging_dir: Path) -> None:
    """Install a complete staged site without deleting the current generation first."""
    OUTPUT_DIR.parent.mkdir(parents=True, exist_ok=True)
    previous_dir: Path | None = None
    if OUTPUT_DIR.exists():
        previous_dir = OUTPUT_DIR.with_name(f".public-site-previous-{uuid.uuid4().hex}")
        # A locked output fails here while the existing published generation is
        # still intact. Do not use rmtree before the replacement is ready.
        OUTPUT_DIR.rename(previous_dir)

    try:
        staging_dir.rename(OUTPUT_DIR)
    except OSError as install_error:
        if previous_dir is not None and previous_dir.exists() and not OUTPUT_DIR.exists():
            try:
                previous_dir.rename(OUTPUT_DIR)
            except OSError as restore_error:
                raise RuntimeError(
                    f"could not install staged site ({install_error}) or restore the prior generation ({restore_error})"
                ) from restore_error
        raise

    if previous_dir is None:
        return
    try:
        shutil.rmtree(previous_dir)
    except OSError as cleanup_error:
        # The new complete generation is already installed. Retain the prior
        # one rather than risking the generated site when a viewer still holds
        # a file handle; .gitignore excludes this recoverable residue.
        print(
            f"Public-site build retained previous generation at {previous_dir}: {cleanup_error}",
            file=sys.stderr,
        )


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
        markdown_sources = copy_allowlisted_content(staging_dir, allowlist)

        version_source_name = allowlist.get("version_source", "")
        version_source = resolve_repo_relative_path(version_source_name, label="allowlist version source")
        version = version_source.read_text(encoding="utf-8").strip()
        if not version:
            raise ValueError("REPO_VERSION.txt must contain a version")
        replace_version_tokens(staging_dir, version)
        page_updates = collect_page_updates(markdown_sources, version_source_name, version)

        # Keep human-readable HTML local and leave raw Markdown unchanged for AI clients.
        tool_directory = Path(__file__).resolve().parent
        sys.path.insert(0, str(tool_directory))
        try:
            from render_human_docs import main as render_human_docs
        finally:
            sys.path.pop(0)

        if render_human_docs([str(staging_dir)], page_updates=page_updates) != 0:
            raise RuntimeError("render_human_docs.py failed")

        replace_document_count_tokens(staging_dir)
        add_web_analytics(staging_dir)
        replace_staged_site(staging_dir)
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
