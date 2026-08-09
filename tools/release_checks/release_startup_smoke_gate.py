#!/usr/bin/env python3
"""Extract each release ZIP into a temporary directory and prove its app starts."""

from __future__ import annotations

import argparse
import json
import os
import shutil
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


def safe_extract(archive_path: Path, destination: Path) -> None:
    with zipfile.ZipFile(archive_path) as archive:
        for info in archive.infolist():
            member = PurePosixPath(info.filename)
            if member.is_absolute() or ".." in member.parts:
                raise ValueError(f"unsafe ZIP entry: {info.filename}")
            target = (destination / Path(*member.parts)).resolve()
            if destination.resolve() not in target.parents and target != destination.resolve():
                raise ValueError(f"ZIP entry escapes extraction directory: {info.filename}")
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as source, target.open("wb") as output:
                shutil.copyfileobj(source, output)


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
                directory_name = child_path(release_set, components.get(directory_key), label=f"{label} directory").name
                archive = child_path(release_set, components.get(zip_key), label=f"{label} ZIP")
                destination = root / label
                safe_extract(archive, destination)
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
