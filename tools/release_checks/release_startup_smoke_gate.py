#!/usr/bin/env python3
"""Extract each release ZIP into a temporary directory and prove its app starts."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path, PurePosixPath


STARTUP_WAIT_SECONDS = 4


def child_path(root: Path, value: object, *, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ValueError(f"release-set manifest is missing {label}")
    candidate = (root / value).resolve()
    if root.resolve() not in candidate.parents:
        raise ValueError(f"release-set manifest {label} escapes the release set: {value}")
    return candidate


def safe_extract(archive_path: Path, destination: Path, release_dir: Path) -> None:
    expected = {
        PurePosixPath(release_dir.name, *path.relative_to(release_dir).parts).as_posix(): path.stat().st_size
        for path in release_dir.rglob("*")
        if path.is_file()
    }
    if not expected:
        raise ValueError(f"release directory is empty: {release_dir}")
    allowed_directories = {
        parent.as_posix().rstrip("/") + "/"
        for name in expected
        for parent in PurePosixPath(name).parents
        if str(parent) != "."
    }
    allowed_directories.update(
        PurePosixPath(release_dir.name, *path.relative_to(release_dir).parts).as_posix().rstrip("/") + "/"
        for path in release_dir.rglob("*")
        if path.is_dir()
    )
    expected_bytes = sum(expected.values())
    archive_overhead_limit = max(16 * 1024 * 1024, len(expected) * 4096)
    if archive_path.stat().st_size > expected_bytes + archive_overhead_limit:
        raise ValueError("ZIP is too large for the unpacked release")
    with zipfile.ZipFile(archive_path) as archive:
        infos = archive.infolist()
        if len(infos) > len(expected) + len(allowed_directories):
            raise ValueError("ZIP contains more entries than the unpacked release")
        seen: set[str] = set()
        for info in infos:
            member = PurePosixPath(info.filename)
            if member.is_absolute() or ".." in member.parts:
                raise ValueError(f"unsafe ZIP entry: {info.filename}")
            normalized = member.as_posix()
            if normalized in seen:
                raise ValueError(f"duplicate ZIP entry: {info.filename}")
            seen.add(normalized)
            target = (destination / Path(*member.parts)).resolve()
            if destination.resolve() not in target.parents and target != destination.resolve():
                raise ValueError(f"ZIP entry escapes extraction directory: {info.filename}")
            if info.is_dir():
                if normalized.rstrip("/") + "/" not in allowed_directories:
                    raise ValueError(f"unexpected ZIP directory entry: {info.filename}")
                target.mkdir(parents=True, exist_ok=True)
                continue
            expected_size = expected.get(normalized)
            if expected_size is None:
                raise ValueError(f"unexpected ZIP file entry: {info.filename}")
            if info.file_size != expected_size:
                raise ValueError(f"ZIP entry size differs from unpacked release: {info.filename}")
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as source, target.open("wb") as output:
                written = 0
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    written += len(chunk)
                    if written > expected_size:
                        raise ValueError(f"ZIP entry expands beyond its expected size: {info.filename}")
                    output.write(chunk)
                if written != expected_size:
                    raise ValueError(f"ZIP entry extracted size differs from unpacked release: {info.filename}")
        missing = sorted(set(expected) - seen)
        if missing:
            raise ValueError(f"ZIP is missing release files: {', '.join(missing[:5])}")


def smoke_start(executable: Path) -> None:
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    process = subprocess.Popen(
        [str(executable)],
        cwd=executable.parent,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=creationflags,
    )
    try:
        time.sleep(STARTUP_WAIT_SECONDS)
        exit_code = process.poll()
        if exit_code is not None:
            raise RuntimeError(f"application exited during startup smoke test (exit={exit_code})")
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


def validate_release_set(release_set: Path) -> list[str]:
    try:
        manifest = json.loads((release_set / "release_set_manifest.json").read_text(encoding="utf-8-sig"))
        components = manifest["components"]
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, KeyError, TypeError) as error:
        return [f"invalid release-set manifest: {error}"]
    errors: list[str] = []
    with tempfile.TemporaryDirectory(prefix="pdf_note_release_smoke_") as temporary:
        root = Path(temporary)
        for label, directory_key, zip_key in (("full", "release", "release_zip"), ("Lite", "release_lite", "release_lite_zip")):
            try:
                release_dir = child_path(release_set, components.get(directory_key), label=f"{label} directory")
                directory_name = release_dir.name
                archive = child_path(release_set, components.get(zip_key), label=f"{label} ZIP")
                destination = root / label
                safe_extract(archive, destination, release_dir)
                executable = destination / directory_name / "pdf_note_workspace.exe"
                if not executable.is_file():
                    raise ValueError(f"extracted application is missing: {executable}")
                smoke_start(executable)
            except (OSError, RuntimeError, ValueError, zipfile.BadZipFile) as error:
                errors.append(f"{label}: {error}")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-set", type=Path, required=True)
    args = parser.parse_args(argv)
    errors = validate_release_set(args.release_set.resolve())
    if errors:
        print("Release startup smoke gate failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"Release startup smoke gate passed: {args.release_set}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
