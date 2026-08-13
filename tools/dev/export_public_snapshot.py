#!/usr/bin/env python3
"""Create a public-repository snapshot from an allowlist.

The tool copies only explicit allowlisted paths from the current working tree.
It never mutates source files and refuses to write into a non-empty destination
or into the repository root tree unless the caller changes the code.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
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
        help="Line-based allowlist file. Defaults to the release snapshot allowlist.",
    )
    parser.add_argument(
        "--gitignore-template",
        type=Path,
        default=DEFAULT_GITIGNORE_TEMPLATE,
        help="Template written to DEST/.gitignore.",
    )
    parser.add_argument(
        "--artifact-manifest",
        type=Path,
        help="SHA-256 manifest for reviewed Git-ignored vendor artifacts. Unlisted untracked files are rejected.",
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


def git_tracked_files(root: Path) -> set[PurePosixPath]:
    completed = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--cached"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"cannot list Git-tracked snapshot inputs: {detail or 'git ls-files failed'}")
    return {
        PurePosixPath(value.decode("utf-8", errors="surrogateescape"))
        for value in completed.stdout.split(b"\0")
        if value
    }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_artifact_manifest(root: Path, manifest: Path | None) -> dict[PurePosixPath, str]:
    if manifest is None:
        return {}
    if not manifest.is_file():
        raise FileNotFoundError(f"artifact manifest not found: {manifest}")
    artifacts: dict[PurePosixPath, str] = {}
    for lineno, raw_line in enumerate(manifest.read_text(encoding="utf-8-sig").splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 2 or not re.fullmatch(r"[0-9a-fA-F]{64}", parts[0]):
            raise ValueError(f"invalid artifact manifest line {lineno}: expected SHA-256<TAB>path")
        relative = PurePosixPath(parts[1].replace("\\", "/"))
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"artifact manifest line {lineno} escapes the repository: {parts[1]}")
        if relative in artifacts:
            raise ValueError(f"duplicate artifact manifest path: {relative.as_posix()}")
        source = (root / relative).resolve()
        ensure_relative_to_root(root, source)
        if not source.is_file():
            raise FileNotFoundError(f"artifact manifest file not found: {relative.as_posix()}")
        expected = parts[0].lower()
        if sha256_file(source) != expected:
            raise ValueError(f"artifact hash mismatch: {relative.as_posix()}")
        artifacts[relative] = expected
    if not artifacts:
        raise ValueError(f"artifact manifest has no active entries: {manifest}")
    return artifacts


def iter_allowed_files(
    root: Path,
    source_dir: Path,
    tracked_files: set[PurePosixPath],
    reviewed_artifacts: dict[PurePosixPath, str],
    used_artifacts: set[PurePosixPath],
    untracked_files: list[PurePosixPath],
) -> list[tuple[Path, PurePosixPath]]:
    files: list[tuple[Path, PurePosixPath]] = []
    for current_root, dir_names, file_names in os.walk(source_dir):
        dir_names[:] = sorted(name for name in dir_names if name not in EXCLUDED_DIR_NAMES)
        for file_name in sorted(file_names):
            source_file = (Path(current_root) / file_name).resolve()
            rel_file = ensure_relative_to_root(root, source_file)
            if is_excluded_public_path(rel_file):
                continue
            if rel_file in reviewed_artifacts:
                used_artifacts.add(rel_file)
            elif rel_file not in tracked_files:
                untracked_files.append(rel_file)
                continue
            files.append((source_file, rel_file))
    return files


def resolve_allowlist_entry(
    root: Path,
    entry: str,
    tracked_files: set[PurePosixPath],
    reviewed_artifacts: dict[PurePosixPath, str],
    used_artifacts: set[PurePosixPath],
    untracked_files: list[PurePosixPath],
) -> tuple[list[PurePosixPath], list[tuple[Path, PurePosixPath]]]:
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
        files.extend(iter_allowed_files(
            root, source_dir, tracked_files, reviewed_artifacts, used_artifacts, untracked_files
        ))
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
                files.extend(iter_allowed_files(
                    root, resolved, tracked_files, reviewed_artifacts, used_artifacts, untracked_files
                ))
            else:
                if is_excluded_public_path(rel_path):
                    continue
                if rel_path in reviewed_artifacts:
                    used_artifacts.add(rel_path)
                elif rel_path not in tracked_files:
                    untracked_files.append(rel_path)
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
        if rel_path in reviewed_artifacts:
            used_artifacts.add(rel_path)
        elif rel_path not in tracked_files:
            untracked_files.append(rel_path)
            return directories, files
        files.append((source, rel_path))
    return directories, files


def build_snapshot_plan(
    root: Path,
    allowlist_entries: list[str],
    tracked_files: set[PurePosixPath],
    reviewed_artifacts: dict[PurePosixPath, str] | None = None,
) -> SnapshotPlan:
    directory_set: set[PurePosixPath] = set()
    file_map: dict[PurePosixPath, Path] = {}
    untracked_files: list[PurePosixPath] = []
    artifacts = reviewed_artifacts or {}
    used_artifacts: set[PurePosixPath] = set()

    for entry in allowlist_entries:
        directories, files = resolve_allowlist_entry(
            root, entry, tracked_files, artifacts, used_artifacts, untracked_files
        )
        directory_set.update(directories)
        for source, rel_path in files:
            file_map.setdefault(rel_path, source)
            for parent in rel_path.parents:
                if str(parent) != ".":
                    directory_set.add(parent)

    if untracked_files:
        unique = sorted(set(untracked_files))
        preview = ", ".join(path.as_posix() for path in unique[:20])
        if len(unique) > 20:
            preview += f", ... ({len(unique) - 20} more)"
        raise ValueError(
            "allowlisted directories contain files that are not tracked by Git; "
            f"review and add or remove them before snapshot creation: {preview}"
        )
    unused_artifacts = sorted(set(artifacts) - used_artifacts)
    if unused_artifacts:
        preview = ", ".join(path.as_posix() for path in unused_artifacts[:20])
        raise ValueError(f"artifact manifest entries are outside the active allowlist: {preview}")

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


def read_repo_version(root: Path) -> str | None:
    version_path = root / "REPO_VERSION.txt"
    if not version_path.is_file():
        return None
    version = version_path.read_text(encoding="utf-8").strip()
    if not version:
        raise ValueError(f"repo version file is empty: {version_path}")
    return version


def copy_snapshot(
    dest: Path,
    plan: SnapshotPlan,
    gitignore_template: Path,
    repo_version: str | None,
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
        reviewed_artifacts = parse_artifact_manifest(
            root, args.artifact_manifest.resolve() if args.artifact_manifest is not None else None
        )
        plan = build_snapshot_plan(root, allowlist_entries, git_tracked_files(root), reviewed_artifacts)
        selected_dest = resolve_destination_argument(args.dest, args.select_dest)
        dest = validate_destination(root, selected_dest)
        copy_snapshot(
            dest,
            plan,
            args.gitignore_template.resolve(),
            read_repo_version(root),
            args.dry_run,
        )
    except (FileNotFoundError, ValueError, OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    print(render_plan(dest, plan, args.dry_run))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
