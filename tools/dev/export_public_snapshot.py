#!/usr/bin/env python3
"""Create a public-repository snapshot from an allowlist.

The tool copies only explicit allowlisted paths from the current working tree.
It never mutates source files and refuses to write into a non-empty destination
or into the repository root tree unless the caller changes the code.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Literal


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ALLOWLIST = REPO_ROOT / "docs" / "internal" / "operations" / "public_repo_demo許可リスト_2026-07-02.txt"
DEFAULT_GITIGNORE_TEMPLATE = (
    REPO_ROOT / "docs" / "internal" / "operations" / "public_repo_gitignoreテンプレート_2026-07-02.gitignore"
)
EXCLUDED_DIR_NAMES = {"__pycache__", ".pytest_cache"}
EXCLUDED_FILE_SUFFIXES = {".pyc", ".pyo", ".orig", ".rej", ".bak"}
EXCLUDED_FILE_NAMES = {"Thumbs.db", "Desktop.ini"}
VERSION_TRACKED_DOCUMENTS = frozenset(
    {
        "README.md",
        "docs/public/README.md",
        "Document/Index.md",
        "Document/How_to_Build.md",
        "Document/What_is_File_Formats.md",
        "Document/How_to_Save_and_Recovery.md",
        "Document/How_to_Setup.md",
        "Document/How_to_Troubleshoot.md",
        "Document/How_to_Use.md",
    }
)


@dataclass(frozen=True)
class SnapshotPlan:
    directories: tuple[PurePosixPath, ...]
    files: tuple[tuple[Path, PurePosixPath], ...]


def configure_utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Copy an allowlisted working-tree snapshot into a clean public-repository directory."
    )
    parser.add_argument("--root", type=Path, default=REPO_ROOT, help="Repository root to snapshot.")
    parser.add_argument(
        "--dest",
        type=Path,
        help="Destination directory. Must be outside the repository root and empty or absent.",
    )
    parser.add_argument(
        "--select-dest",
        choices=("gui", "cui"),
        help="Select the destination interactively. Defaults to GUI when --dest is omitted.",
    )
    parser.add_argument(
        "--allowlist",
        type=Path,
        default=DEFAULT_ALLOWLIST,
        help="Line-based allowlist file. Defaults to the conservative public demo allowlist.",
    )
    parser.add_argument(
        "--gitignore-template",
        type=Path,
        default=DEFAULT_GITIGNORE_TEMPLATE,
        help="Template written to DEST/.gitignore.",
    )
    parser.add_argument("--dry-run", action="store_true", help="Validate and print the copy plan without writing files.")
    return parser.parse_args(argv)


def select_destination_via_gui() -> Path:
    try:
        import tkinter
        from tkinter import filedialog
    except Exception as exc:
        raise RuntimeError("GUI directory selection is unavailable on this Python environment") from exc

    root = tkinter.Tk()
    root.withdraw()
    root.attributes("-topmost", True)
    try:
        selected = filedialog.askdirectory(
            title="Select an empty public snapshot destination directory"
        )
    finally:
        root.destroy()

    if not selected:
        raise ValueError("destination selection was canceled")
    return Path(selected)


def select_destination_via_cui() -> Path:
    prompt = "Destination directory path: "
    try:
        selected = input(prompt)
    except EOFError as exc:
        raise ValueError("destination selection was canceled") from exc
    value = selected.strip().strip('"').strip("'")
    if not value:
        raise ValueError("destination selection was canceled")
    return Path(value)


def resolve_destination_argument(
    dest: Path | None,
    select_dest: Literal["gui", "cui"] | None,
) -> Path:
    if dest is not None and select_dest is not None:
        raise ValueError("use either --dest or --select-dest, not both")
    if dest is not None:
        return dest
    mode = select_dest or "gui"
    if mode == "gui":
        return select_destination_via_gui()
    if mode == "cui":
        return select_destination_via_cui()
    raise ValueError(f"unsupported destination selection mode: {mode}")


def parse_allowlist(path: Path) -> list[str]:
    if not path.is_file():
        raise FileNotFoundError(f"allowlist file not found: {path}")

    entries: list[str] = []
    for lineno, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("/") or line.startswith("\\"):
            raise ValueError(f"allowlist line {lineno} must be repository-relative: {line}")
        entries.append(line.replace("\\", "/"))
    if not entries:
        raise ValueError(f"allowlist has no active entries: {path}")
    return entries


def ensure_relative_to_root(root: Path, candidate: Path) -> PurePosixPath:
    try:
        relative = candidate.relative_to(root)
    except ValueError as exc:
        raise ValueError(f"path escapes repository root: {candidate}") from exc
    return PurePosixPath(relative.as_posix())


def is_excluded_public_path(rel_path: PurePosixPath) -> bool:
    if any(part in EXCLUDED_DIR_NAMES for part in rel_path.parts):
        return True
    if rel_path.name in EXCLUDED_FILE_NAMES:
        return True
    if Path(rel_path.name).suffix.lower() in EXCLUDED_FILE_SUFFIXES:
        return True
    return False


def iter_allowed_files(root: Path, source_dir: Path) -> list[tuple[Path, PurePosixPath]]:
    files: list[tuple[Path, PurePosixPath]] = []
    for current_root, dir_names, file_names in os.walk(source_dir):
        dir_names[:] = sorted(name for name in dir_names if name not in EXCLUDED_DIR_NAMES)
        for file_name in sorted(file_names):
            source_file = (Path(current_root) / file_name).resolve()
            rel_file = ensure_relative_to_root(root, source_file)
            if is_excluded_public_path(rel_file):
                continue
            files.append((source_file, rel_file))
    return files


def resolve_allowlist_entry(root: Path, entry: str) -> tuple[list[PurePosixPath], list[tuple[Path, PurePosixPath]]]:
    if ".." in PurePosixPath(entry).parts:
        raise ValueError(f"allowlist entry must not contain '..': {entry}")

    directories: list[PurePosixPath] = []
    files: list[tuple[Path, PurePosixPath]] = []

    if entry.endswith("/"):
        source_dir = (root / entry[:-1]).resolve()
        if not source_dir.is_dir():
            raise FileNotFoundError(f"allowlist directory not found: {entry}")
        rel_dir = ensure_relative_to_root(root, source_dir)
        directories.append(rel_dir)
        files.extend(iter_allowed_files(root, source_dir))
        return directories, files

    if "*" in entry:
        parts = PurePosixPath(entry).parts
        if any("**" in part for part in parts):
            raise ValueError(f"recursive glob is not allowed in allowlist entry: {entry}")
        matches = sorted((root.glob(entry)))
        if not matches:
            raise FileNotFoundError(f"allowlist glob matched nothing: {entry}")
        for match in matches:
            resolved = match.resolve()
            rel_path = ensure_relative_to_root(root, resolved)
            if match.is_dir():
                directories.append(rel_path)
                files.extend(iter_allowed_files(root, resolved))
            else:
                if is_excluded_public_path(rel_path):
                    continue
                files.append((resolved, rel_path))
        return directories, files

    source = (root / entry).resolve()
    if not source.exists():
        raise FileNotFoundError(f"allowlist path not found: {entry}")
    rel_path = ensure_relative_to_root(root, source)
    if source.is_dir():
        directories.append(rel_path)
        files.extend(iter_allowed_files(root, source))
    else:
        if is_excluded_public_path(rel_path):
            return directories, files
        files.append((source, rel_path))
    return directories, files


def build_snapshot_plan(root: Path, allowlist_entries: list[str]) -> SnapshotPlan:
    directory_set: set[PurePosixPath] = set()
    file_map: dict[PurePosixPath, Path] = {}

    for entry in allowlist_entries:
        directories, files = resolve_allowlist_entry(root, entry)
        directory_set.update(directories)
        for source, rel_path in files:
            file_map.setdefault(rel_path, source)
            for parent in rel_path.parents:
                if str(parent) != ".":
                    directory_set.add(parent)

    return SnapshotPlan(
        directories=tuple(sorted(directory_set)),
        files=tuple((file_map[rel_path], rel_path) for rel_path in sorted(file_map)),
    )


def validate_destination(root: Path, dest: Path) -> Path:
    root_resolved = root.resolve()
    dest_resolved = dest.resolve()

    if dest_resolved == root_resolved or root_resolved in dest_resolved.parents:
        raise ValueError(
            f"destination must be outside the repository root: repo={root_resolved} dest={dest_resolved}"
        )

    if dest_resolved.exists():
        if not dest_resolved.is_dir():
            raise ValueError(f"destination exists and is not a directory: {dest_resolved}")
        if any(dest_resolved.iterdir()):
            raise ValueError(f"destination directory must be empty: {dest_resolved}")

    return dest_resolved


def read_app_version(root: Path) -> str | None:
    version_path = root / "APP_VERSION.txt"
    if not version_path.is_file():
        return None
    version = version_path.read_text(encoding="utf-8").strip()
    if not version:
        raise ValueError(f"app version file is empty: {version_path}")
    return version


def render_version_tracked_document(source: Path, target: Path, version: str) -> None:
    text = source.read_text(encoding="utf-8")
    marker = "__APP_VERSION__"
    marker_count = text.count(marker)
    if marker_count == 1:
        rendered = text.replace(marker, version)
    elif marker_count > 1:
        raise ValueError(f"version-tracked document contains multiple app version markers: {source}")
    else:
        heading = re.search(r"^# .*\r?$", text, re.MULTILINE)
        if heading is None:
            raise ValueError(f"version-tracked document has no top-level heading: {source}")
        insertion = f"{heading.group(0)}\n\n対象アプリ版: {marker}"
        rendered = text[: heading.start()] + insertion + text[heading.end() :]
    target.write_text(rendered, encoding="utf-8", newline="")


def copy_snapshot(
    dest: Path,
    plan: SnapshotPlan,
    gitignore_template: Path,
    app_version: str | None,
    dry_run: bool,
) -> None:
    if not gitignore_template.is_file():
        raise FileNotFoundError(f"gitignore template not found: {gitignore_template}")

    if dry_run:
        return

    dest.mkdir(parents=True, exist_ok=True)
    for rel_dir in plan.directories:
        (dest / rel_dir).mkdir(parents=True, exist_ok=True)
    for source, rel_path in plan.files:
        target = dest / rel_path
        target.parent.mkdir(parents=True, exist_ok=True)
        if app_version is not None and rel_path.as_posix() in VERSION_TRACKED_DOCUMENTS:
            render_version_tracked_document(source, target, app_version)
        else:
            shutil.copy2(source, target)
    shutil.copy2(gitignore_template, dest / ".gitignore")


def render_plan(dest: Path, plan: SnapshotPlan, dry_run: bool) -> str:
    lines = [
        f"mode: {'dry-run' if dry_run else 'write'}",
        f"destination: {dest}",
        f"directories: {len(plan.directories)}",
        f"files: {len(plan.files)}",
        "generated:",
        "  - .gitignore",
    ]
    if plan.files:
        lines.append("copy:")
        lines.extend(f"  - {rel_path.as_posix()}" for _, rel_path in plan.files)
    else:
        lines.append("copy: none")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    configure_utf8_output()
    args = parse_args(argv)
    root = args.root.resolve()
    if not root.is_dir():
        print(f"error: repository root is not a directory: {root}", file=sys.stderr)
        return 2

    try:
        allowlist_entries = parse_allowlist(args.allowlist.resolve())
        plan = build_snapshot_plan(root, allowlist_entries)
        selected_dest = resolve_destination_argument(args.dest, args.select_dest)
        dest = validate_destination(root, selected_dest)
        copy_snapshot(
            dest,
            plan,
            args.gitignore_template.resolve(),
            read_app_version(root),
            args.dry_run,
        )
    except (FileNotFoundError, ValueError, OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    print(render_plan(dest, plan, args.dry_run))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
