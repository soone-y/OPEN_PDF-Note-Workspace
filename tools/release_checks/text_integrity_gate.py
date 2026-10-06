#!/usr/bin/env python3
"""Audit Git-visible text bytes and structured-text syntax without changing files."""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import xml.etree.ElementTree as element_tree
from pathlib import Path
from typing import Iterable


TEXT_EXTENSIONS = {
    ".c", ".cc", ".clro", ".clrop", ".cmake", ".cpp", ".cppinc", ".csv", ".cxx",
    ".h", ".hpp", ".html", ".ijg", ".inc", ".input", ".js", ".json", ".manifest",
    ".md", ".patch", ".props", ".ps1", ".py", ".rc", ".svg", ".tex", ".tsv", ".txt",
    ".vcxproj", ".xml", ".yaml", ".yml",
}
TEXT_FILE_NAMES = {".gitattributes", ".gitignore", "_headers", "license", "notice", "version"}
STRUCTURED_JSON_EXTENSIONS = {".clrop", ".json"}
STRUCTURED_XML_EXTENSIONS = {".svg", ".xml"}

# Upstream FreeType keeps this license in its original Windows-1252 form.  Its
# exact hash is pinned so an arbitrary non-UTF-8 file cannot be hidden here.
LEGACY_TEXT_EXCEPTIONS = {
    "third_party/pdfium/licenses/freetype.txt": {
        "encoding": "cp1252",
        "sha256": "f4b133e25df1f86ad3ffea453aa0e613f0474f34778dbbb3e437e7b2724937d8",
        "reason": "upstream FreeType license byte preservation",
    },
}


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Check Git-visible text bytes, BOMs, and JSON/XML syntax.")
    parser.add_argument("--root", default=".", help="Repository root. Defaults to the current directory.")
    parser.add_argument("--format", choices=("text", "json"), default="text", help="Report format.")
    parser.add_argument("--strict-bom", action="store_true", help="Treat UTF-8 BOMs as errors instead of warnings.")
    return parser.parse_args(argv)


def git_visible_paths(root: Path) -> list[Path]:
    completed = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"git ls-files failed: {detail or completed.returncode}")
    deleted = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--deleted"],
        capture_output=True, check=False,
    )
    if deleted.returncode != 0:
        detail = deleted.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"git deleted-path enumeration failed: {detail or deleted.returncode}")
    deleted_paths = set(deleted.stdout.split(b"\0"))
    paths: list[Path] = []
    for raw_path in completed.stdout.split(b"\0"):
        # Deleted working-tree entries have no bytes to audit. Required-file
        # existence remains the responsibility of manifest/license/code gates.
        # A later read failure on any enumerated existing file still fails.
        if not raw_path or raw_path in deleted_paths:
            continue
        try:
            relative = raw_path.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise RuntimeError(f"tracked path is not valid UTF-8: {raw_path!r}: {exc}") from exc
        path = Path(relative)
        if path not in paths:
            paths.append(path)
    return paths


def is_text_path(path: Path) -> bool:
    return path.suffix.lower() in TEXT_EXTENSIONS or path.name.lower() in TEXT_FILE_NAMES


def detect_bom(data: bytes) -> str | None:
    if data.startswith(b"\xef\xbb\xbf\xef\xbb\xbf"):
        return "utf-8-double"
    if data.startswith(b"\xef\xbb\xbf"):
        return "utf-8"
    if data.startswith(b"\xff\xfe\x00\x00"):
        return "utf-32-le"
    if data.startswith(b"\x00\x00\xfe\xff"):
        return "utf-32-be"
    if data.startswith(b"\xff\xfe"):
        return "utf-16-le"
    if data.startswith(b"\xfe\xff"):
        return "utf-16-be"
    return None


def exception_for(relative: str, data: bytes) -> dict[str, str] | None:
    exception = LEGACY_TEXT_EXCEPTIONS.get(relative)
    if not exception:
        return None
    digest = hashlib.sha256(data).hexdigest()
    if digest != exception["sha256"]:
        return {"error": "legacy exception hash changed"}
    return exception


def audit_paths(root: Path, paths: Iterable[Path], strict_bom: bool = False) -> dict[str, object]:
    errors: list[dict[str, str]] = []
    warnings: list[dict[str, str]] = []
    checked = 0
    structured_checked = 0
    exception_count = 0

    for relative_path in paths:
        if not is_text_path(relative_path):
            continue
        checked += 1
        relative = relative_path.as_posix()
        path = root / relative_path
        try:
            data = path.read_bytes()
        except OSError as exc:
            errors.append({"path": relative, "kind": "read-error", "detail": str(exc)})
            continue

        bom = detect_bom(data)
        if bom in {"utf-16-le", "utf-16-be", "utf-32-le", "utf-32-be"}:
            errors.append({"path": relative, "kind": "non-utf8-bom", "detail": bom})
            continue
        if bom == "utf-8-double":
            errors.append({"path": relative, "kind": "double-utf8-bom", "detail": bom})
            continue
        if bom == "utf-8":
            item = {"path": relative, "kind": "utf8-bom", "detail": "UTF-8 BOM"}
            (errors if strict_bom else warnings).append(item)

        try:
            text = data.decode("utf-8-sig", errors="strict")
        except UnicodeDecodeError as exc:
            exception = exception_for(relative, data)
            if exception and "error" not in exception:
                try:
                    data.decode(exception["encoding"], errors="strict")
                except UnicodeDecodeError as legacy_exc:
                    errors.append({"path": relative, "kind": "legacy-decode-error", "detail": str(legacy_exc)})
                else:
                    exception_count += 1
                    warnings.append({"path": relative, "kind": "approved-legacy-encoding", "detail": exception["reason"]})
                continue
            detail = exception["error"] if exception else str(exc)
            errors.append({"path": relative, "kind": "invalid-utf8", "detail": detail})
            continue

        if "\x00" in text:
            errors.append({"path": relative, "kind": "nul-in-text", "detail": "decoded UTF-8 text contains NUL"})
            continue
        if relative_path.suffix.lower() in STRUCTURED_JSON_EXTENSIONS:
            structured_checked += 1
            try:
                json.loads(text)
            except json.JSONDecodeError as exc:
                errors.append({"path": relative, "kind": "invalid-json", "detail": str(exc)})
        elif relative_path.suffix.lower() in STRUCTURED_XML_EXTENSIONS:
            structured_checked += 1
            try:
                element_tree.fromstring(text)
            except element_tree.ParseError as exc:
                errors.append({"path": relative, "kind": "invalid-xml", "detail": str(exc)})

    return {
        "summary": {
            "text_files_checked": checked,
            "structured_files_checked": structured_checked,
            "legacy_exceptions": exception_count,
            "errors": len(errors),
            "warnings": len(warnings),
        },
        "errors": errors,
        "warnings": warnings,
    }


def render_text(report: dict[str, object]) -> str:
    summary = report["summary"]
    assert isinstance(summary, dict)
    lines = [
        "Text integrity gate",
        "  text files checked: {0}".format(summary["text_files_checked"]),
        "  structured files checked: {0}".format(summary["structured_files_checked"]),
        "  approved legacy encodings: {0}".format(summary["legacy_exceptions"]),
        "  warnings: {0}".format(summary["warnings"]),
        "  errors: {0}".format(summary["errors"]),
    ]
    for severity in ("errors", "warnings"):
        entries = report[severity]
        assert isinstance(entries, list)
        for entry in entries:
            assert isinstance(entry, dict)
            safe_path = entry["path"].encode("ascii", errors="backslashreplace").decode("ascii")
            lines.append("  {0}: {1}: {2}".format(severity[:-1].upper(), safe_path, entry["kind"]))
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    root = Path(args.root).resolve()
    if not root.is_dir():
        print(f"root is not a directory: {root}", file=sys.stderr)
        return 2
    try:
        report = audit_paths(root, git_visible_paths(root), strict_bom=args.strict_bom)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if args.format == "json":
        print(json.dumps(report, ensure_ascii=True, indent=2))
    else:
        print(render_text(report))
    return 1 if report["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
