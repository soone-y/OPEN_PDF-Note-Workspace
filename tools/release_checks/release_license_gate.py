#!/usr/bin/env python3
"""Refuse a release set whose distributable folders or ZIPs lack license material."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import zipfile
from pathlib import Path, PurePosixPath


REQUIRED_LICENSE_FILES = (
    "docs/LICENSE.md",
    "docs/LICENSES_INDEX.md",
    "docs/THIRD_PARTY_NOTICES.md",
    "licenses/README.txt",
    "licenses/pdfium/LICENSE",
    "licenses/md4c/LICENSE.md",
    "licenses/zlib/zlib.txt",
    "licenses/mingw-w64/gcc/COPYING.RUNTIME.txt",
    "licenses/mingw-w64/gcc/COPYING3.txt",
    "licenses/mingw-w64/winpthreads/COPYING.txt",
    "licenses/libreoffice/license.txt",
    "licenses/libreoffice/LICENSE.html",
    "licenses/libreoffice/NOTICE",
)


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def validate_release_directory(release_dir: Path) -> list[str]:
    """Check the license contract of one unpacked distribution folder."""
    errors: list[str] = []
    if not release_dir.is_dir():
        return [f"release directory does not exist: {release_dir}"]
    for relative_path in REQUIRED_LICENSE_FILES:
        path = release_dir / relative_path
        if not path.is_file():
            errors.append(f"required license file is missing: {relative_path}")
        elif path.stat().st_size == 0:
            errors.append(f"required license file is empty: {relative_path}")
    return errors


def zip_entry_for_release_file(zip_file: zipfile.ZipFile, release_dir: Path, relative_path: str) -> str | None:
    """Find exactly one ZIP member for a required file below the release root."""
    expected = PurePosixPath(release_dir.name, *PurePosixPath(relative_path).parts).as_posix()
    names = [name for name in zip_file.namelist() if name.rstrip("/") == expected]
    if len(names) == 1:
        return names[0]
    return None


def validate_release_zip(release_dir: Path, zip_path: Path) -> list[str]:
    """Require each ZIP to contain byte-identical required license files."""
    errors: list[str] = []
    if not zip_path.is_file() or zip_path.stat().st_size == 0:
        return [f"release ZIP does not exist or is empty: {zip_path}"]
    try:
        with zipfile.ZipFile(zip_path) as archive:
            bad_entries = [
                name for name in archive.namelist()
                if PurePosixPath(name).is_absolute() or ".." in PurePosixPath(name).parts
            ]
            for name in bad_entries:
                errors.append(f"release ZIP contains an unsafe entry: {name}")
            for relative_path in REQUIRED_LICENSE_FILES:
                directory_path = release_dir / relative_path
                if not directory_path.is_file():
                    continue
                member = zip_entry_for_release_file(archive, release_dir, relative_path)
                if member is None:
                    errors.append(f"release ZIP is missing required license file: {relative_path}")
                    continue
                if sha256_bytes(archive.read(member)) != sha256_bytes(directory_path.read_bytes()):
                    errors.append(f"release ZIP license file differs from unpacked release: {relative_path}")
    except zipfile.BadZipFile as error:
        errors.append(f"invalid release ZIP: {zip_path} ({error})")
    return errors


def child_path(root: Path, value: object, *, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ValueError(f"release-set manifest is missing {label}")
    candidate = (root / value).resolve()
    if root.resolve() not in candidate.parents:
        raise ValueError(f"release-set manifest {label} escapes the release set: {value}")
    return candidate


def validate_release_set(release_set: Path) -> list[str]:
    manifest_path = release_set / "release_set_manifest.json"
    if not manifest_path.is_file():
        return [f"release-set manifest does not exist: {manifest_path}"]
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
        components = manifest["components"]
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, KeyError, TypeError) as error:
        return [f"invalid release-set manifest: {manifest_path} ({error})"]

    errors: list[str] = []
    for label, directory_key, zip_key in (
        ("full", "release", "release_zip"),
        ("Lite", "release_lite", "release_lite_zip"),
    ):
        try:
            release_dir = child_path(release_set, components.get(directory_key), label=f"{label} directory")
            zip_path = child_path(release_set, components.get(zip_key), label=f"{label} ZIP")
        except ValueError as error:
            errors.append(str(error))
            continue
        errors.extend(f"{label}: {error}" for error in validate_release_directory(release_dir))
        errors.extend(f"{label}: {error}" for error in validate_release_zip(release_dir, zip_path))
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-set", type=Path, required=True, help="release set containing release_set_manifest.json")
    args = parser.parse_args(argv)
    errors = validate_release_set(args.release_set)
    if errors:
        print("Release license gate failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"Release license gate passed: {args.release_set}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
