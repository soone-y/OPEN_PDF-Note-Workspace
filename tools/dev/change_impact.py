#!/usr/bin/env python3
"""Suggest review context and verification commands for local changes.

The tool reads Git working-tree metadata, or explicit --path values. It never
modifies source files and never executes the suggested verification commands.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Iterable, Optional


def configure_utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def add_unique(items: list[str], value: str) -> None:
    if value not in items:
        items.append(value)


def git_output(root: Path, arguments: list[str]) -> bytes:
    result = subprocess.run(
        ["git", *arguments],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        message = result.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(message or f"git {' '.join(arguments)} failed")
    return result.stdout


def changed_paths_from_git(root: Path) -> list[str]:
    paths: set[str] = set()
    for arguments in (
        ["diff", "--name-only", "-z", "--"],
        ["diff", "--cached", "--name-only", "-z", "--"],
        ["ls-files", "--others", "--exclude-standard", "-z"],
    ):
        for raw_path in git_output(root, arguments).split(b"\0"):
            if raw_path:
                paths.add(raw_path.decode("utf-8", errors="replace").replace("\\", "/"))
    return sorted(paths, key=str.lower)


def changed_diff_text(root: Path) -> str:
    combined = git_output(root, ["diff", "--unified=0", "--"]) + git_output(
        root, ["diff", "--cached", "--unified=0", "--"]
    )
    return combined.decode("utf-8", errors="replace")


def recommendations_for_paths(paths: Iterable[str], diff_text: str = "") -> dict[str, list[str]]:
    normalized = sorted({path.replace("\\", "/") for path in paths}, key=str.lower)
    read: list[str] = []
    run: list[str] = []
    inspect: list[str] = []
    reasons: list[str] = []

    def matches(prefix: str) -> bool:
        return any(path.startswith(prefix) for path in normalized)

    cpp_changed = any(
        Path(path).suffix.lower()
        in {
            ".c", ".cc", ".cpp", ".cxx", ".c++",
            ".h", ".hh", ".hpp", ".hxx", ".ipp", ".inl",
            ".inc", ".cppinc",
        }
        for path in normalized
    )
    python_tool_changed = any(path.startswith("tools/") and path.endswith(".py") for path in normalized)
    markdown_changed = any(Path(path).suffix.lower() in {".md", ".markdown"} for path in normalized)
    verification_relevant = any(
        path.startswith(("src/", "tests/", "tools/", "scripts/"))
        or path in {"full_build.ps1", "full_release.ps1"}
        for path in normalized
    )
    full_entry_changed = any(path in {"full_build.ps1", "full_release.ps1"} for path in normalized)

    if markdown_changed:
        add_unique(inspect, "python tools/dev/md_structure_scanner.py . --index out/md_structure_index.tsv")
        add_unique(reasons, "Markdown documentation changed: refresh the document locator index.")

    persistence_changed = (
        matches("src/file_output/")
        or matches("src/core/atomic_write")
        or "src/core/app_core.cpp" in normalized
        or matches("src/main/")
        or "src/main.cpp" in normalized
        or matches("src/pdf_view/annotation_store")
        or matches("src/pdf_view/view_state")
    )

    if persistence_changed:
        add_unique(read, "docs/internal/architecture/persistence_保存系現行実装整理方針_2026-04-29.md")
        add_unique(read, "docs/internal/architecture/persistence_stage保存統合現行整理_2026-04-27.md")
        add_unique(run, "powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_atomic_write_tests.ps1")
        add_unique(run, "powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_fault_injection_tests.ps1")
        add_unique(run, "powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_ui_automation_fault_tests.ps1")
        add_unique(inspect, "python tools/dev/persistence_index.py --out out/persistence_index.tsv")
        add_unique(reasons, "Persistence implementation changed: confirm original-file retention and rollback paths.")

    if matches("src/clrop/"):
        add_unique(run, "powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_clrop_json_direct_parse_tests.ps1")
        add_unique(run, "powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_clrop_file_safety_tests.ps1")
        add_unique(inspect, "python tools/dev/persistence_index.py --out out/persistence_index.tsv")
        add_unique(reasons, "Annotation storage changed: confirm round-trip data preservation and file safety.")

    if matches("src/note/") or matches("src/note_view/"):
        add_unique(read, "docs/internal/architecture/note_記法新旧比較仕様_2026-04-14.md")
        add_unique(run, "powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_note_parser_tests.ps1")
        add_unique(reasons, "Note parsing or presentation changed: confirm malformed-input and export regressions.")

    if matches("src/main/") or "src/main.cpp" in normalized or matches("src/settings/"):
        add_unique(read, "docs/internal/operations/test_main回帰チェック手順_2026-05-22.md")
        add_unique(run, "powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_ui_automation_fault_tests.ps1")
        add_unique(reasons, "Application flow or settings changed: confirm UI rollback behavior.")

    if (
        matches("third_party/libreoffice/")
        or matches("tools/libreoffice_")
        or "scripts/release/pack_release.ps1" in normalized
        or "pack_release.ps1" in normalized
    ):
        add_unique(read, "docs/internal/reports/libreoffice_削減追跡表_2026-04-30.md")
        add_unique(run, "python tools/release_checks/libreoffice_runtime_gate.py --image third_party/libreoffice/custom_runtime/instdir")
        add_unique(inspect, "python tools/release_checks/binary_scan.py --include third_party/libreoffice/custom_runtime/instdir/program")
        add_unique(run, "powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_office_conversion_fixture_tests.ps1")
        add_unique(reasons, "Office conversion components changed: verify the custom runtime gate, local conversion, and binary risk indicators.")

    if python_tool_changed:
        add_unique(run, "python -m unittest tests/python/test_python_tools.py")
        add_unique(run, "python tests/python/validate_codebase.py")
        add_unique(reasons, "Python development tool changed: confirm tool regression coverage and syntax validation.")

    if cpp_changed:
        add_unique(run, "python tests/python/validate_codebase.py")
        add_unique(inspect, "python tools/metrics/cpp_include_visualizer.py src --include-root src --index out/cpp_include_index.tsv")
        add_unique(reasons, "C++ source changed: check structural dependencies and static validation.")

    if cpp_changed and any(token in diff_text for token in ("SetTimer", "KillTimer", "WM_TIMER", "TimerId")):
        add_unique(inspect, 'rg -n "TimerId|SetTimer|WM_TIMER|KillTimer" src')
        add_unique(reasons, "Timer-related edits detected: verify timer ID ownership and WM_TIMER routing.")

    if full_entry_changed:
        add_unique(reasons, "Full build/release entry changed: verify that the default commands build and package every distributable component.")

    if verification_relevant:
        add_unique(run, "powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_repo_checks.ps1")

    return {
        "changed_paths": normalized,
        "read": read,
        "inspect": inspect,
        "run": run,
        "reasons": reasons,
    }


def render_text(report: dict[str, list[str]]) -> str:
    lines: list[str] = []
    for heading, key in (
        ("Changed Paths", "changed_paths"),
        ("Why", "reasons"),
        ("Read", "read"),
        ("Inspect", "inspect"),
        ("Run", "run"),
    ):
        lines.append(f"{heading}:")
        values = report[key]
        if values:
            lines.extend(f"  - {value}" for value in values)
        else:
            lines.append("  - (none)")
    return "\n".join(lines) + "\n"


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Recommend local review context and verification steps for changed repository paths."
    )
    parser.add_argument("--root", type=Path, default=Path("."), help="Repository root")
    parser.add_argument(
        "--path",
        action="append",
        default=[],
        help="Analyze an explicit changed path instead of reading Git status. Can be repeated.",
    )
    parser.add_argument("--format", choices=("text", "json"), default="text")
    return parser.parse_args(argv)


def main(argv: Optional[list[str]] = None) -> int:
    configure_utf8_output()
    args = parse_args(argv)
    root = args.root.resolve()
    if not root.is_dir():
        print(f"error: root is not a directory: {root}", file=sys.stderr)
        return 2

    try:
        if args.path:
            paths = args.path
            diff_text = ""
        else:
            paths = changed_paths_from_git(root)
            diff_text = changed_diff_text(root)
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    report = recommendations_for_paths(paths, diff_text)
    if args.format == "json":
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print(render_text(report), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
