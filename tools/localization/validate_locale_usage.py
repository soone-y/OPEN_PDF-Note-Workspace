#!/usr/bin/env python3
"""Validate catalog references in C++ sources and English catalog text."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


JAPANESE = re.compile(r"[\u3040-\u30ff\u3400-\u9fff]")
ENGLISH_PROSE = re.compile(
    r"\b(?:the|and|with|requires|failed|could|cannot|please|select|check|file|folder|operation)\b",
    re.IGNORECASE,
)
ALPHABETIC_WORD = re.compile(r"\b[A-Za-z]{3,}\b")
PLACEHOLDER = re.compile(r"\{[A-Za-z0-9_]+\}")
ASCII_LETTER = re.compile(r"[A-Za-z]")
# These labels are intentionally spelled this way in both editions. Match the
# value as well as the ID so an English sentence cannot reuse an exception.
JAPANESE_ASCII_EXCEPTIONS = {
    "settings.shortcut.ctrl": "Ctrl",
    "settings.shortcut.shift": "Shift",
    "settings.shortcut.alt": "Alt",
    "settings.group.pdf": "PDF",
    "reader_next.brand": "READ ONLY",
    "reader_next.empty_detail": "PDF / Markdown / Text / Hex",
    "ui.app_title": "PDF Note Workspace",
    "note.assist.pdf": "PDF",
    "note.assist.target.pdf": "PDF",
    "note.assist.input.insert": "Vim Insert",
    "note.assist.input.visual": "Vim Visual",
    "note.assist.ime": "IME",
    "menu.common.pdf": "PDF",
    "pdf.javascript_notice.pdf_label": "PDF: ",
    "settings.annot.d6b8cb010d87": "ON",
    "settings.general.d6b8cb010d87": "ON",
    "settings.general.7d50fab6bf8b": "OFF",
    "ui.layout.d7555ac93b73": "d=",
    "ui.layout.3b77684645ca": "m=",
    "ui.layout.010acbca7c8a": "s=",
    "ui.layout.08e3a2cd01fd": "px",
    "ui.local_path.678c9ce46bbf": "PC",
    "app.command.beb82eae76ec": "PDF",
    "note.bottom_math.5f944ef08ac7": "Markup",
    "note.bottom_math.c7bec3279ce2": "LaTeX",
    "note.bottom_math_legacy.5f944ef08ac7": "Markup",
    "note.bottom_math_legacy.c7bec3279ce2": "LaTeX",
    "app.command.note_assist_pdf_ref": "PDF p.{PAGE}: {NAME}",
    "settings.note_render.off": "OFF",
    "settings.note_render.on": "ON",
    "search.ui.target_pdf": "PDF",
    "search.ui.tag_pdf": "PDF",
    "export.reservation_kind.note_text": "txt",
    "readonly.pdf.picker.filter_combined": "PDF / CLROP",
    "readonly.pdf.picker.filter_clrop": "CLROP",
}
REFERENCE = re.compile(
    r'localization::(?:Text|Format)\s*\(\s*L"(?P<id>[a-z][a-z0-9._]*)"'
)
SOURCE_SUFFIXES = {".cpp", ".h", ".cppinc"}
DIRECT_UI_LITERAL = re.compile(
    r'\b(?P<call>ShowSoftNotice|ShowSilentMessageDialog)\s*\(\s*[^,;]{1,160},\s*'
    r'L"(?P<text>(?:\\.|[^"\\])*)"', re.DOTALL,
)
DIRECT_DIALOG_MESSAGE = re.compile(
    r'\bShowSilentMessageDialog\s*\(\s*[^,;]{1,160},\s*[^,;]{1,160},\s*'
    r'L"(?:\\.|[^"\\])*"', re.DOTALL,
)
RAW_OPERATION_ERROR = re.compile(r'\*outErr\s*=\s*L"[A-Za-z]')
OPERATION_AUTOMATION_MARKER = "bool RunUiAutomationOperationTransactionRecoveryScenario"
RAW_VIEWER_MESSAGE = re.compile(
    r'(?:SetViewerStatus\s*\([^,;]{1,100},\s*|ApplyEditorText\s*\(\s*|'
    r'g_state\.status\s*=\s*|\bstatus\s*=\s*|\berr\s*=\s*)L"[A-Za-z]'
)


def load_catalog(path: Path) -> dict[str, str]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid catalog {path}: {error}") from error
    if not isinstance(value, dict) or not all(isinstance(key, str) and isinstance(text, str)
                                              for key, text in value.items()):
        raise ValueError(f"catalog must be a string-to-string object: {path}")
    return value


def referenced_ids(source_root: Path) -> set[str]:
    ids: set[str] = set()
    for path in source_root.rglob("*"):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        try:
            ids.update(match.group("id") for match in REFERENCE.finditer(path.read_text(encoding="utf-8")))
        except (OSError, UnicodeDecodeError) as error:
            raise ValueError(f"unreadable source {path}: {error}") from error
    return ids


def direct_ui_literals(source_root: Path) -> list[str]:
    """Catch a direct literal passed as a notice or dialog title.

    This intentionally does not guess whether an error string passed through a
    variable reaches the UI; those paths still require a separate review.
    """
    errors: list[str] = []
    for path in sorted(source_root.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        try:
            source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as error:
            raise ValueError(f"unreadable source {path}: {error}") from error
        for pattern in (DIRECT_UI_LITERAL, DIRECT_DIALOG_MESSAGE):
            for match in pattern.finditer(source):
                line = source.count("\n", 0, match.start()) + 1
                errors.append(f"direct UI literal: {path}:{line}: {match.group().split('(')[0].strip()}")
    return errors


def contains_english_prose(value: str) -> bool:
    without_placeholders = PLACEHOLDER.sub("", value)
    return (len(ALPHABETIC_WORD.findall(without_placeholders)) >= 3
            and ENGLISH_PROSE.search(without_placeholders) is not None)


def raw_operation_errors(source_root: Path) -> list[str]:
    path = source_root / "workspace" / "file_ops_move_rename.cppinc"
    if not path.is_file():
        return []
    try:
        source = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise ValueError(f"unreadable source {path}: {error}") from error
    production = source.split(OPERATION_AUTOMATION_MARKER, 1)[0]
    errors = []
    for match in RAW_OPERATION_ERROR.finditer(production):
        line = production.count("\n", 0, match.start()) + 1
        errors.append(f"raw operation error literal: {path}:{line}")
    return errors


def raw_viewer_messages(source_root: Path) -> list[str]:
    errors: list[str] = []
    for name in ("main.cpp", "pdf_preview_panel.cpp"):
        path = source_root / "readonly_viewer" / name
        if not path.is_file():
            continue
        try:
            source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as error:
            raise ValueError(f"unreadable source {path}: {error}") from error
        for match in RAW_VIEWER_MESSAGE.finditer(source):
            line = source.count("\n", 0, match.start()) + 1
            errors.append(f"raw viewer message: {path}:{line}")
    return errors


def validate(source_root: Path, japanese_catalog: Path, english_catalog: Path) -> list[str]:
    ja = load_catalog(japanese_catalog)
    en = load_catalog(english_catalog)
    errors = [f"source references ID absent from Japanese catalog: {text_id}"
              for text_id in sorted(referenced_ids(source_root) - set(ja))]
    errors.extend(f"Japanese catalog contains English prose: {text_id}"
                  for text_id, value in sorted(ja.items())
                  if contains_english_prose(value))
    errors.extend(f"Japanese catalog has unapproved ASCII-only text: {text_id}"
                  for text_id, value in sorted(ja.items())
                  if not JAPANESE.search(value) and ASCII_LETTER.search(PLACEHOLDER.sub("", value))
                  and JAPANESE_ASCII_EXCEPTIONS.get(text_id) != value)
    errors.extend(f"English catalog contains Japanese text: {text_id}"
                  for text_id, text in sorted(en.items()) if JAPANESE.search(text))
    errors.extend(direct_ui_literals(source_root))
    errors.extend(raw_operation_errors(source_root))
    errors.extend(raw_viewer_messages(source_root))
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("src"))
    parser.add_argument("--ja", type=Path, default=Path("locales/ja.json"))
    parser.add_argument("--en", type=Path, default=Path("locales/en.json"))
    args = parser.parse_args(argv)
    try:
        errors = validate(args.source, args.ja, args.en)
    except ValueError as error:
        errors = [str(error)]
    if errors:
        print("Locale usage validation failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"Locale usage validation passed: {args.source}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
