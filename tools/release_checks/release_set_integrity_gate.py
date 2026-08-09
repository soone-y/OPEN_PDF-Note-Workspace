#!/usr/bin/env python3
"""Bind a release set's public snapshot and ZIPs to immutable file manifests."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import zipfile
from pathlib import Path, PurePosixPath


SNAPSHOT_MANIFEST_NAME = "public_snapshot_manifest.json"
SCHEMA_VERSION = 1


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def child_path(root: Path, value: object, *, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ValueError(f"release-set manifest is missing {label}")
    candidate = (root / value).resolve()
    if root.resolve() not in candidate.parents:
        raise ValueError(f"release-set manifest {label} escapes the release set: {value}")
    return candidate


def file_records(root: Path) -> list[dict[str, object]]:
    return [
        {
            "path": path.relative_to(root).as_posix(),
            "sha256": sha256_file(path),
            "size": path.stat().st_size,
        }
        for path in sorted((path for path in root.rglob("*") if path.is_file()), key=lambda item: item.as_posix())
    ]


def tree_sha256(records: list[dict[str, object]]) -> str:
    canonical = "".join(
        f"{record['path']}\t{record['size']}\t{record['sha256']}\n" for record in records
    ).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def load_release_components(release_set: Path) -> tuple[dict[str, object], Path]:
    manifest_path = release_set / "release_set_manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
        components = manifest["components"]
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, KeyError, TypeError) as error:
        raise ValueError(f"invalid release-set manifest: {manifest_path} ({error})") from error
    if not isinstance(components, dict):
        raise ValueError(f"release-set manifest has invalid components: {manifest_path}")
    return components, manifest_path


def write_snapshot_manifest(release_set: Path, allowlist: Path) -> Path:
    components, _ = load_release_components(release_set)
    snapshot = child_path(release_set, components.get("public_snapshot"), label="public_snapshot")
    if not snapshot.is_dir():
        raise ValueError(f"public snapshot directory does not exist: {snapshot}")
    if not allowlist.is_file():
        raise ValueError(f"public snapshot allowlist does not exist: {allowlist}")
    records = file_records(snapshot)
    if not records:
        raise ValueError(f"public snapshot is empty: {snapshot}")
    output = release_set / SNAPSHOT_MANIFEST_NAME
    payload = {
        "schema_version": SCHEMA_VERSION,
        "allowlist_name": allowlist.name,
        "allowlist_sha256": sha256_file(allowlist),
        "snapshot_tree_sha256": tree_sha256(records),
        "files": records,
    }
    output.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return output


def validate_snapshot_manifest(release_set: Path, components: dict[str, object]) -> list[str]:
    errors: list[str] = []
    manifest_path = release_set / SNAPSHOT_MANIFEST_NAME
    try:
        payload = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        return [f"invalid or missing public snapshot manifest: {manifest_path} ({error})"]
    if payload.get("schema_version") != SCHEMA_VERSION:
        return [f"unsupported public snapshot manifest schema: {manifest_path}"]
    records = payload.get("files")
    if not isinstance(records, list) or not records:
        return [f"public snapshot manifest has no files: {manifest_path}"]
    if not isinstance(payload.get("allowlist_sha256"), str) or len(payload["allowlist_sha256"]) != 64:
        errors.append(f"public snapshot manifest has invalid allowlist hash: {manifest_path}")
    try:
        snapshot = child_path(release_set, components.get("public_snapshot"), label="public_snapshot")
    except ValueError as error:
        return [str(error)]
    if not snapshot.is_dir():
        return [f"public snapshot directory does not exist: {snapshot}"]
    actual = file_records(snapshot)
    if records != actual:
        errors.append("public snapshot files differ from the recorded allowlist snapshot manifest")
    if payload.get("snapshot_tree_sha256") != tree_sha256(actual):
        errors.append("public snapshot tree hash differs from the recorded snapshot manifest")
    return errors


def validate_release_zip(release_dir: Path, zip_path: Path) -> list[str]:
    errors: list[str] = []
    if not release_dir.is_dir():
        return [f"release directory does not exist: {release_dir}"]
    if not zip_path.is_file():
        return [f"release ZIP does not exist: {zip_path}"]
    expected = {record["path"]: record["sha256"] for record in file_records(release_dir)}
    try:
        with zipfile.ZipFile(zip_path) as archive:
            actual: dict[str, str] = {}
            prefix = f"{release_dir.name}/"
            for info in archive.infolist():
                member = PurePosixPath(info.filename)
                if member.is_absolute() or ".." in member.parts:
                    errors.append(f"release ZIP contains an unsafe entry: {info.filename}")
                    continue
                if info.is_dir():
                    continue
                if not info.filename.startswith(prefix):
                    errors.append(f"release ZIP entry is outside the release root: {info.filename}")
                    continue
                relative = info.filename[len(prefix):]
                if not relative or "/" not in info.filename:
                    errors.append(f"release ZIP has invalid file entry: {info.filename}")
                    continue
                if relative in actual:
                    errors.append(f"release ZIP has duplicate file entry: {relative}")
                    continue
                actual[relative] = hashlib.sha256(archive.read(info)).hexdigest()
    except zipfile.BadZipFile as error:
        return [f"invalid release ZIP: {zip_path} ({error})"]
    if actual != expected:
        missing = sorted(set(expected) - set(actual))
        unexpected = sorted(set(actual) - set(expected))
        changed = sorted(path for path in set(expected) & set(actual) if expected[path] != actual[path])
        if missing:
            errors.append(f"release ZIP is missing files: {', '.join(missing[:5])}")
        if unexpected:
            errors.append(f"release ZIP has unexpected files: {', '.join(unexpected[:5])}")
        if changed:
            errors.append(f"release ZIP has changed files: {', '.join(changed[:5])}")
    return errors


def parse_build_info(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        parts = line.split("\t")
        if len(parts) >= 2:
            values[parts[0] if parts[0] != "artifact" else f"artifact:{parts[1]}"] = parts[-1]
    return values


def validate_release_metadata(release_set: Path, components: dict[str, object]) -> list[str]:
    errors: list[str] = []
    try:
        release_manifest = json.loads((release_set / "release_set_manifest.json").read_text(encoding="utf-8-sig"))
        version = release_manifest["app_version"]
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, KeyError, TypeError) as error:
        return [f"unable to read release-set version: {error}"]
    if not isinstance(version, str) or not version:
        return ["release-set app_version is empty"]
    for label, directory_key, expected_edition in (("full", "release", "full"), ("Lite", "release_lite", "lite")):
        try:
            release_dir = child_path(release_set, components.get(directory_key), label=f"{label} directory")
        except ValueError as error:
            errors.append(str(error))
            continue
        build_info = release_dir / "pdf_note_workspace.exe.buildinfo.txt"
        if not build_info.is_file():
            errors.append(f"{label}: missing application build-info file")
            continue
        try:
            fields = parse_build_info(build_info)
        except (OSError, UnicodeDecodeError) as error:
            errors.append(f"{label}: invalid application build-info file ({error})")
            continue
        if fields.get("version") != version:
            errors.append(f"{label}: application build-info version does not match release-set app_version")
        if fields.get("edition") != expected_edition:
            errors.append(f"{label}: application build-info edition is not {expected_edition}")
        executable = release_dir / "pdf_note_workspace.exe"
        if not executable.is_file() or fields.get("artifact:pdf_note_workspace.exe") != sha256_file(executable):
            errors.append(f"{label}: application executable does not match its build-info hash")
    try:
        full_dir = child_path(release_set, components.get("release"), label="full directory")
        lite_dir = child_path(release_set, components.get("release_lite"), label="Lite directory")
    except ValueError as error:
        return errors + [str(error)]
    if not (full_dir / "libreoffice" / "custom_runtime" / "instdir").is_dir():
        errors.append("full: LibreOffice custom runtime is missing")
    if (lite_dir / "libreoffice" / "custom_runtime").exists():
        errors.append("Lite: LibreOffice custom runtime must not be included")
    return errors


def validate_release_set(release_set: Path) -> list[str]:
    try:
        components, _ = load_release_components(release_set)
    except ValueError as error:
        return [str(error)]
    errors = validate_snapshot_manifest(release_set, components)
    errors.extend(validate_release_metadata(release_set, components))
    for label, directory_key, zip_key in (("full", "release", "release_zip"), ("Lite", "release_lite", "release_lite_zip")):
        directory_value = components.get(directory_key)
        zip_value = components.get(zip_key)
        if directory_value is None and zip_value is None:
            continue
        try:
            release_dir = child_path(release_set, directory_value, label=f"{label} directory")
            zip_path = child_path(release_set, zip_value, label=f"{label} ZIP")
        except ValueError as error:
            errors.append(str(error))
            continue
        errors.extend(f"{label}: {error}" for error in validate_release_zip(release_dir, zip_path))
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-set", type=Path, required=True)
    parser.add_argument("--write-snapshot-manifest", action="store_true")
    parser.add_argument("--allowlist", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.write_snapshot_manifest:
            if args.allowlist is None:
                raise ValueError("--allowlist is required with --write-snapshot-manifest")
            written = write_snapshot_manifest(args.release_set.resolve(), args.allowlist.resolve())
            print(f"Public snapshot manifest written: {written}")
        errors = validate_release_set(args.release_set.resolve())
    except ValueError as error:
        errors = [str(error)]
    if errors:
        print("Release set integrity gate failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"Release set integrity gate passed: {args.release_set}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
