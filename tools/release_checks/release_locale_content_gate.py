#!/usr/bin/env python3
"""Validate locale-specific user documents and sample workspaces in a release set."""

from __future__ import annotations

import argparse
import hashlib
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
    "sample_workspace/workspace.json",
)
TOP_LEVEL_DOCUMENTS = ("README.md", "LICENSE.md", "SECURITY.md")
REQUIRED_LECTURE_SAMPLE_FILES = {
    "ja": (
        "01_講義サンプル/COURSE_GUIDE.html",
        "01_講義サンプル/第01回_基本操作/使い方_基本操作.pdf",
        "01_講義サンプル/第03回_最初期構想/PDF学習ワークスペース統合画面構成および基本仕様書.pdf",
    ),
    "en": (
        "01_Lecture_Samples/COURSE_GUIDE.html",
        "01_Lecture_Samples/Session_01_Basics/basic_workflow_guide.pdf",
        "01_Lecture_Samples/Session_03_Early_Design/early_design_reference.pdf",
    ),
}
REQUIRED_COMPANION_SESSION_NOTES = {
    "ja": (
        "01_講義サンプル/第03回_最初期構想/ノート_最初期構想.clro",
    ),
    "en": (
        "01_Lecture_Samples/Session_03_Early_Design/early_design_notes.clro",
    ),
}
REQUIRED_FULL_COMPANION_SESSION_NOTES = {
    "ja": (
        "01_講義サンプル/第04回_Office変換/ノート_Office変換.clro",
    ),
    "en": (
        "01_Lecture_Samples/Session_04_Office_Conversion/office_conversion_notes.clro",
    ),
}
REQUIRED_NOTE_FORMAT_SAMPLE_FILES = {
    "ja": (
        "01_講義サンプル/第02回_ノート形式/基本操作.clro",
        "01_講義サンプル/第02回_ノート形式/ノート_数式.md",
        "01_講義サンプル/第02回_ノート形式/ノート_TeX数式.tex",
        "01_講義サンプル/第02回_ノート形式/ノート_プレーンテキスト.txt",
        "01_講義サンプル/第02回_ノート形式/ノート_表データ.csv",
        "01_講義サンプル/第02回_ノート形式/PDFリンク練習.pdf",
        "01_講義サンプル/第02回_ノート形式/PDFリンク練習.clrop",
    ),
    "en": (
        "01_Lecture_Samples/Session_02_Note_Formats/basic_operation_note.clro",
        "01_Lecture_Samples/Session_02_Note_Formats/math.md",
        "01_Lecture_Samples/Session_02_Note_Formats/tex_math_note.tex",
        "01_Lecture_Samples/Session_02_Note_Formats/plain_text.txt",
        "01_Lecture_Samples/Session_02_Note_Formats/table_data.csv",
        "01_Lecture_Samples/Session_02_Note_Formats/pdf_link_practice.pdf",
        "01_Lecture_Samples/Session_02_Note_Formats/pdf_link_practice.clrop",
    ),
}
NOTE_FORMAT_SESSION_PATH = {
    "ja": "01_講義サンプル/第02回_ノート形式",
    "en": "01_Lecture_Samples/Session_02_Note_Formats",
}
NOTE_FORMAT_REQUIRED_SUFFIXES = {".clro", ".md", ".tex", ".txt", ".csv", ".pdf", ".clrop"}
NOTE_FORMAT_LINK_SAMPLE = {
    "ja": {
        "note": "基本操作.clro",
        "pdf": "PDFリンク練習.pdf",
        "clrop": "PDFリンク練習.clrop",
        "link_id": "session-02-pdf-note-link",
    },
    "en": {
        "note": "basic_operation_note.clro",
        "pdf": "pdf_link_practice.pdf",
        "clrop": "pdf_link_practice.clrop",
        "link_id": "session-02-pdf-note-link",
    },
}
REQUIRED_FULL_CONVERSION_SAMPLE_FILES = {
    "ja": (
        "01_講義サンプル/第04回_Office変換/PPTX_機能紹介とネイティブ図表_変換結果.pdf",
    ),
    "en": (
        "01_Lecture_Samples/Session_04_Office_Conversion/feature_overview_and_charts_conversion_result.pdf",
    ),
}
CONVERSION_SESSION_PATH = {
    "ja": "01_講義サンプル/第04回_Office変換",
    "en": "01_Lecture_Samples/Session_04_Office_Conversion",
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


def validate_note_format_pdf_link(release_dir: Path, locale: str) -> list[str]:
    """Verify that Session 02's PDF marker and standard note form a portable pair."""
    errors: list[str] = []
    session = release_dir / "sample_workspace" / NOTE_FORMAT_SESSION_PATH[locale]
    sample = NOTE_FORMAT_LINK_SAMPLE[locale]
    note_path = session / sample["note"]
    pdf_path = session / sample["pdf"]
    clrop_path = session / sample["clrop"]
    if not (note_path.is_file() and pdf_path.is_file() and clrop_path.is_file()):
        return errors
    try:
        note = note_path.read_text(encoding="utf-8-sig")
        clrop = json.loads(clrop_path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        return [f"note-format PDF link sample is unreadable: {error}"]

    link_id = sample["link_id"]
    if f"<link={link_id}>" not in note:
        errors.append("note-format PDF link sample is missing its note link marker")
    pdf_id = clrop.get("pdf_id") if isinstance(clrop, dict) else None
    if not isinstance(pdf_id, dict):
        errors.append("note-format PDF link sample has no PDF identity")
    else:
        if pdf_id.get("path") != pdf_path.name:
            errors.append("note-format PDF link sample targets a different PDF")
        if pdf_id.get("size") != pdf_path.stat().st_size:
            errors.append("note-format PDF link sample has a mismatched PDF size")
        expected_hash = hashlib.sha256(pdf_path.read_bytes()).hexdigest()
        if pdf_id.get("sha256") != expected_hash:
            errors.append("note-format PDF link sample has a mismatched PDF hash")

    items = [
        item
        for page in clrop.get("pages", []) if isinstance(page, dict)
        for item in page.get("items", []) if isinstance(item, dict)
    ] if isinstance(clrop, dict) else []
    markers = [item for item in items if item.get("type") == "link-marker" and item.get("link_id") == link_id]
    if len(markers) != 1:
        errors.append("note-format PDF link sample must contain exactly one matching PDF link marker")
    else:
        point = markers[0].get("p1")
        if not (isinstance(point, list) and len(point) == 2 and all(isinstance(value, (int, float)) for value in point)):
            errors.append("note-format PDF link sample has no usable PDF marker position")
        if markers[0].get("note_path"):
            errors.append("note-format PDF link sample must not contain a deployment-specific note path")
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
    for relative in REQUIRED_COMPANION_SESSION_NOTES[locale]:
        if not (release_dir / "sample_workspace" / relative).is_file():
            errors.append(f"required companion session note is missing: sample_workspace/{relative}")
    for relative in REQUIRED_NOTE_FORMAT_SAMPLE_FILES[locale]:
        if not (release_dir / "sample_workspace" / relative).is_file():
            errors.append(f"required note-format sample is missing: sample_workspace/{relative}")
    note_format_session = release_dir / "sample_workspace" / NOTE_FORMAT_SESSION_PATH[locale]
    if note_format_session.is_dir():
        note_format_files = [
            path for path in note_format_session.iterdir()
            if path.is_file() and path.suffix.lower() in NOTE_FORMAT_REQUIRED_SUFFIXES
        ]
        for suffix in NOTE_FORMAT_REQUIRED_SUFFIXES:
            if sum(path.suffix.lower() == suffix for path in note_format_files) != 1:
                errors.append(
                    "note-format session must contain exactly one file for each required extension: "
                    f"sample_workspace/{NOTE_FORMAT_SESSION_PATH[locale]} ({suffix})"
                )
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
        for relative in REQUIRED_FULL_COMPANION_SESSION_NOTES[locale]:
            if not (release_dir / "sample_workspace" / relative).is_file():
                errors.append(f"required companion session note is missing: sample_workspace/{relative}")
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
    errors.extend(validate_note_format_pdf_link(release_dir, locale))

    errors.extend(validate_markdown_links(release_dir, release_dir))

    if locale == "en":
        for root in (release_dir / "docs", release_dir / "sample_workspace"):
            if not root.is_dir():
                continue
            for path in root.rglob("*"):
                if JAPANESE.search(path.name):
                    errors.append(f"English release contains Japanese locale filename: {path.relative_to(release_dir).as_posix()}")
                if not path.is_file() or path.suffix.lower() not in {".clro", ".md", ".txt", ".json"}:
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
