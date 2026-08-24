#!/usr/bin/env python3
"""Validate locale-specific user documents and sample workspaces in a release set."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


JAPANESE = re.compile(r"[\u3040-\u30ff\u3400-\u9fff]")
MARKDOWN_LINK = re.compile(r"(?<!!)\[[^\]]*\]\(([^)\s]+)(?:\s+[^)]*)?\)")
REQUIRED_DOCUMENTS = (
    "docs/README.md",
    "docs/Getting_Started.md",
    "docs/Help_Reference.md",
    "docs/legal/LICENSE.md",
    "docs/legal/THIRD_PARTY_NOTICES.md",
)
REQUIRED_SAMPLE_FILES = (
    "sample_workspace/README.txt",
    "sample_workspace/Getting_Started.md",
    "sample_workspace/workspace.json",
)
TOP_LEVEL_DOCUMENTS = ("README.md", "LICENSE.md", "SECURITY.md")
REQUIRED_LECTURE_SAMPLE_FILES = {
    "ja": (
        "01_講義サンプル/COURSE_GUIDE.html",
        "01_講義サンプル/第01回_基本操作/使い方_基本操作.pdf",
        "01_講義サンプル/第02回_最初期構想/README.txt",
        "01_講義サンプル/第02回_最初期構想/PDF学習ワークスペース統合画面構成および基本仕様書.pdf",
    ),
    "en": (
        "01_Lecture_Samples/COURSE_GUIDE.html",
        "01_Lecture_Samples/Session_01_Basics/basic_workflow_guide.pdf",
        "01_Lecture_Samples/Session_02_Early_Design/README.txt",
        "01_Lecture_Samples/Session_02_Early_Design/early_design_reference.pdf",
    ),
}
REQUIRED_FULL_CONVERSION_SAMPLE_FILES = {
    "ja": (
        "01_講義サンプル/第03回_Office変換/README.txt",
        "01_講義サンプル/第03回_Office変換/PPTX_図形・画像・SmartArt総合検証_変換結果.pdf",
        "01_講義サンプル/第03回_Office変換/Word_埋め込みオブジェクト総合検証_変換結果.pdf",
    ),
    "en": (
        "01_Lecture_Samples/Session_03_Office_Conversion/README.txt",
        "01_Lecture_Samples/Session_03_Office_Conversion/presentation_conversion_result.pdf",
        "01_Lecture_Samples/Session_03_Office_Conversion/document_conversion_result.pdf",
    ),
}
CONVERSION_SESSION_PATH = {
    "ja": "01_講義サンプル/第03回_Office変換",
    "en": "01_Lecture_Samples/Session_03_Office_Conversion",
}
CONVERSION_PDF_SUFFIXES = ("_conversion_result.pdf", "変換結果.pdf")
STARTER_SESSION = {
    "ja": ("01_講義サンプル/第01回_基本操作", "使い方_基本操作.pdf", "ノート_基本操作.clro"),
    "en": ("01_Lecture_Samples/Session_01_Basics", "basic_workflow_guide.pdf", "basic_operations.clro"),
}
NOTE_SUFFIXES = {".txt", ".csv", ".md", ".markdown", ".note", ".tex", ".icr", ".clro"}


def validate_markdown_links(root: Path, release_dir: Path) -> list[str]:
    """Reject broken or escaping local links in released user content."""
    errors: list[str] = []
    root_resolved = root.resolve()
    for path in root.rglob("*.md"):
        try:
            text = path.read_text(encoding="utf-8-sig")
        except (OSError, UnicodeDecodeError) as error:
            errors.append(f"Markdown is unreadable: {path.relative_to(release_dir).as_posix()} ({error})")
            continue
        for match in MARKDOWN_LINK.finditer(text):
            target = match.group(1).split("#", 1)[0]
            if not target or "://" in target or target.startswith("mailto:"):
                continue
            candidate = (path.parent / target).resolve()
            relative = path.relative_to(release_dir).as_posix()
            if candidate != root_resolved and root_resolved not in candidate.parents:
                errors.append(f"Markdown link escapes locale content: {relative} -> {target}")
            elif not candidate.is_file():
                errors.append(f"Markdown link target is missing: {relative} -> {target}")
    return errors


def validate_top_level_documents(release_dir: Path, locale: str) -> list[str]:
    """Require bilingual common entry documents and reject locale copies."""
    errors: list[str] = []
    for name in TOP_LEVEL_DOCUMENTS:
        path = release_dir / name
        if not path.is_file():
            errors.append(f"required top-level common document is missing: {name}")
            continue
        if path.stat().st_size == 0:
            errors.append(f"required top-level common document is empty: {name}")

    for prefix in ("README", "LICENSE", "THIRD_PARTY_NOTICES"):
        for candidate in release_dir.glob(f"{prefix}.*.md"):
            errors.append(f"release contains an unexpected top-level locale document: {candidate.name}")
    return errors


def validate_release_directory(release_dir: Path, locale: str, edition: str = "full") -> list[str]:
    errors: list[str] = []
    if edition not in {"full", "lite"}:
        return [f"release edition must be full or lite, not {edition!r}"]
    for relative in (*REQUIRED_DOCUMENTS, *REQUIRED_SAMPLE_FILES):
        path = release_dir / relative
        if not path.is_file():
            errors.append(f"required locale content is missing: {relative}")
            continue
        if path.stat().st_size == 0:
            errors.append(f"required locale content is empty: {relative}")

    config_path = release_dir / "sample_workspace/workspace.json"
    if config_path.is_file():
        try:
            config = json.loads(config_path.read_text(encoding="utf-8-sig"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
            errors.append(f"invalid sample workspace configuration: {error}")
        else:
            if config.get("language") != locale:
                errors.append("sample workspace language does not match release locale")

    for relative in REQUIRED_LECTURE_SAMPLE_FILES[locale]:
        if not (release_dir / "sample_workspace" / relative).is_file():
            errors.append(f"required lecture sample is missing: sample_workspace/{relative}")
    starter_relative, expected_pdf, expected_note = STARTER_SESSION[locale]
    starter = release_dir / "sample_workspace" / starter_relative
    if starter.is_dir():
        direct_files = [path for path in starter.iterdir() if path.is_file()]
        pdfs = [path.name for path in direct_files if path.suffix.lower() == ".pdf"]
        notes = [path.name for path in direct_files if path.suffix.lower() in NOTE_SUFFIXES]
        if pdfs != [expected_pdf] or notes != [expected_note]:
            errors.append(
                "starter session must contain exactly one PDF and one note: "
                f"sample_workspace/{starter_relative}"
            )
    else:
        errors.append(f"starter session is missing: sample_workspace/{starter_relative}")
    conversion_session = release_dir / "sample_workspace" / CONVERSION_SESSION_PATH[locale]
    if edition == "full":
        for relative in REQUIRED_FULL_CONVERSION_SAMPLE_FILES[locale]:
            if not (release_dir / "sample_workspace" / relative).is_file():
                errors.append(f"required Full conversion sample is missing: sample_workspace/{relative}")
    else:
        if conversion_session.exists():
            errors.append(f"Lite release contains conversion sample session: sample_workspace/{CONVERSION_SESSION_PATH[locale]}")
        for path in (release_dir / "sample_workspace").rglob("*.pdf"):
            if path.name.endswith(CONVERSION_PDF_SUFFIXES):
                errors.append(f"Lite release contains conversion-result PDF sample: {path.relative_to(release_dir).as_posix()}")

    for forbidden in (
        "docs/ja",
        "docs/en",
        "docs/internal",
        "sample_workspace/ja",
        "sample_workspace/en",
    ):
        if (release_dir / forbidden).exists():
            errors.append(f"release contains an unselected locale or internal directory: {forbidden}")
    errors.extend(validate_top_level_documents(release_dir, locale))

    errors.extend(validate_markdown_links(release_dir, release_dir))

    if locale == "en":
        for root in (release_dir / "docs", release_dir / "sample_workspace"):
            if not root.is_dir():
                continue
            for path in root.rglob("*"):
                if JAPANESE.search(path.name):
                    errors.append(f"English release contains Japanese locale filename: {path.relative_to(release_dir).as_posix()}")
                if not path.is_file() or path.suffix.lower() not in {".md", ".txt", ".json"}:
                    continue
                try:
                    text = path.read_text(encoding="utf-8-sig")
                except (OSError, UnicodeDecodeError) as error:
                    errors.append(f"English locale text is unreadable: {path.relative_to(release_dir).as_posix()} ({error})")
                    continue
                if JAPANESE.search(text):
                    errors.append(f"English release contains Japanese locale text: {path.relative_to(release_dir).as_posix()}")
    return errors


def validate_release_set(release_set: Path) -> list[str]:
    try:
        manifest = json.loads((release_set / "release_set_manifest.json").read_text(encoding="utf-8-sig"))
        locale = manifest["locale"]
        components = manifest["components"]
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, KeyError, TypeError) as error:
        return [f"invalid release-set manifest: {error}"]
    if locale not in {"ja", "en"}:
        return ["release-set locale must be ja or en"]
    errors: list[str] = []
    for label, key, edition in (("full", "release", "full"), ("Lite", "release_lite", "lite")):
        value = components.get(key)
        if not isinstance(value, str) or not value:
            errors.append(f"release-set manifest is missing {label} directory")
            continue
        release_dir = (release_set / value).resolve()
        if release_set.resolve() not in release_dir.parents:
            errors.append(f"release-set {label} directory escapes the set")
            continue
        errors.extend(f"{label}: {error}" for error in validate_release_directory(release_dir, locale, edition))
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-set", type=Path, required=True)
    args = parser.parse_args(argv)
    errors = validate_release_set(args.release_set)
    if errors:
        print("Release locale-content gate failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"Release locale-content gate passed: {args.release_set}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
