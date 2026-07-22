#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


LOCAL_BUILD_PATH_RE = re.compile(
    rb"(?i)(?:[A-Z]:|(?:file://)?/mnt/[a-z])[\\/](?:Users)[\\/][^\\/\x00]{1,80}[\\/][^\x00]{0,180}?LIBREO(?:FFICE)?[-~][A-Za-z0-9_.~+-]+"
)
SENSITIVE_PATH_RE = re.compile(rb"(?i)(?:[A-Z]:|/mnt/[a-z])[\\/]Users[\\/]")
WORKSPACE_MARKER_RE = re.compile(rb"(?i)\b(?:PDF-Note-Workspace|release_probe|dependency_probe|lo_runtime_probe)\b")
PADDING_BYTE = b"_"
REPLACEMENT_PREFIX = b"SANITIZED_LIBREOFFICE_BUILD_PATH"
CONFIG_KEYS_TO_BLANK = ("ExtensionUpdateURL", "UpdateURL", "UpdateChannel")
DEFAULT_REDUCTION_MANIFEST = (
    Path(__file__).resolve().parents[2]
    / "third_party"
    / "libreoffice"
    / "custom_build"
    / "release_reduction_manifest.json"
)


@dataclass(frozen=True)
class SanitizeResult:
    files_changed: int
    replacements: int
    removed_paths: list[Path]
    removed_bytes: int
    manifest_path: Path


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Remove non-runtime SDK files and local build paths from a copied LibreOffice release runtime."
    )
    parser.add_argument("--image", required=True, help="Copied LibreOffice instdir inside the release output.")
    parser.add_argument(
        "--manifest",
        default=str(DEFAULT_REDUCTION_MANIFEST),
        help="Tracked release reduction manifest. Defaults to the repository custom-build manifest.",
    )
    parser.add_argument("--dry-run", action="store_true", help="Report actions without writing changes.")
    return parser.parse_args(argv)


def replace_equal_length(data: bytes, pattern: re.Pattern[bytes]) -> tuple[bytes, int]:
    replacements = 0

    def repl(match: re.Match[bytes]) -> bytes:
        nonlocal replacements
        replacements += 1
        size = len(match.group(0))
        if size <= len(REPLACEMENT_PREFIX):
            return PADDING_BYTE * size
        return REPLACEMENT_PREFIX + (PADDING_BYTE * (size - len(REPLACEMENT_PREFIX)))

    return pattern.sub(repl, data), replacements


def neutral_bytes(size: int, prefix: bytes = b"local") -> bytes:
    if size <= len(prefix):
        return prefix[:size].ljust(size, PADDING_BYTE)
    return prefix + (PADDING_BYTE * (size - len(prefix)))


def replace_case_insensitive_fixed(data: bytes, needle: bytes, replacement_prefix: bytes = b"local") -> tuple[bytes, int]:
    if not needle:
        return data, 0
    pattern = re.compile(re.escape(needle), re.IGNORECASE)
    replacement = neutral_bytes(len(needle), replacement_prefix)
    return pattern.subn(replacement, data)


def sensitive_user_markers() -> list[bytes]:
    markers: set[str] = {"localuser"}
    for value in (os.environ.get("USERNAME", ""), os.environ.get("USER", ""), Path.home().name):
        if value and len(value) >= 3:
            markers.add(value)
    return [item.encode("utf-8") for item in sorted(markers, key=str.casefold)]


def iter_files(root: Path):
    for path in sorted(root.rglob("*")):
        if path.is_file():
            yield path


def validate_relative_pattern(value: str, *, field: str) -> str:
    normalized = value.replace("\\", "/")
    path = Path(normalized)
    if not normalized or path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        raise ValueError(f"invalid {field} entry in reduction manifest: {value!r}")
    return normalized


def ensure_inside_image(path: Path, image: Path) -> Path:
    resolved = path.resolve()
    image_resolved = image.resolve()
    if resolved != image_resolved and image_resolved not in resolved.parents:
        raise ValueError(f"reduction target escapes runtime image: {path}")
    return resolved


def path_size(path: Path) -> int:
    if path.is_symlink() or path.is_file():
        return path.lstat().st_size
    return sum(child.stat().st_size for child in path.rglob("*") if child.is_file())


def load_reduction_manifest(manifest_path: Path) -> dict[str, object]:
    if not manifest_path.is_file():
        raise FileNotFoundError(f"LibreOffice reduction manifest not found: {manifest_path}")
    payload = json.loads(manifest_path.read_text(encoding="utf-8"))
    if payload.get("version") != 1:
        raise ValueError(f"unsupported LibreOffice reduction manifest version: {payload.get('version')!r}")
    for field in ("paths", "globs", "protected_paths"):
        values = payload.get(field)
        if not isinstance(values, list) or not all(isinstance(value, str) for value in values):
            raise ValueError(f"reduction manifest field must be a string array: {field}")
    return payload


def remove_manifest_extras(
    image: Path,
    manifest_path: Path,
    dry_run: bool,
) -> tuple[list[Path], int]:
    manifest = load_reduction_manifest(manifest_path)
    image_resolved = image.resolve()
    protected = [
        ensure_inside_image(image / validate_relative_pattern(value, field="protected_paths"), image)
        for value in manifest["protected_paths"]
    ]
    targets: set[Path] = set()

    def add_target(path: Path) -> None:
        if not path.exists() and not path.is_symlink():
            return
        resolved = ensure_inside_image(path, image)
        if any(resolved == item or resolved in item.parents for item in protected):
            raise ValueError(f"reduction manifest targets protected runtime path: {resolved}")
        targets.add(resolved)

    for value in manifest["paths"]:
        relative = validate_relative_pattern(value, field="paths")
        add_target(image / relative)
    for value in manifest["globs"]:
        pattern = validate_relative_pattern(value, field="globs")
        for path in image.glob(pattern):
            add_target(path)

    selected: list[Path] = []
    for target in sorted(targets, key=lambda item: (len(item.parts), str(item).casefold())):
        if any(parent == target or parent in target.parents for parent in selected):
            continue
        selected = [child for child in selected if target not in child.parents]
        selected.append(target)

    removed_bytes = sum(path_size(path) for path in selected)
    if not dry_run:
        for target in sorted(selected, key=lambda item: (len(item.parts), str(item).casefold()), reverse=True):
            if target.is_symlink() or target.is_file():
                target.unlink()
            else:
                shutil.rmtree(target)
    return selected, removed_bytes


def sanitize_local_build_paths(image: Path, dry_run: bool) -> tuple[int, int]:
    files_changed = 0
    total_replacements = 0
    for path in iter_files(image):
        data = path.read_bytes()
        updated, replacements = replace_equal_length(data, LOCAL_BUILD_PATH_RE)
        for marker in sensitive_user_markers():
            updated, marker_replacements = replace_case_insensitive_fixed(updated, marker)
            replacements += marker_replacements
        if replacements == 0:
            continue
        files_changed += 1
        total_replacements += replacements
        if not dry_run:
            path.write_bytes(updated)
    return files_changed, total_replacements


def sanitize_text_config_files(image: Path, dry_run: bool) -> tuple[int, int]:
    files_changed = 0
    replacements = 0
    for rel in ("program/version.ini",):
        path = image / rel
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        lines = []
        changed = False
        for line in text.splitlines():
            key = line.split("=", 1)[0] if "=" in line else ""
            if key in CONFIG_KEYS_TO_BLANK and line != f"{key}=":
                lines.append(f"{key}=")
                changed = True
                replacements += 1
            elif key == "Vendor" and line.casefold() != "Vendor=PDF Note Workspace".casefold():
                lines.append("Vendor=PDF Note Workspace")
                changed = True
                replacements += 1
            else:
                lines.append(line)
        if changed:
            files_changed += 1
            if not dry_run:
                path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return files_changed, replacements


def find_remaining_sensitive_markers(image: Path) -> list[tuple[Path, str]]:
    user_marker_patterns = [re.compile(re.escape(marker), re.IGNORECASE) for marker in sensitive_user_markers()]
    findings: list[tuple[Path, str]] = []
    for path in iter_files(image):
        data = path.read_bytes()
        if SENSITIVE_PATH_RE.search(data):
            findings.append((path, "absolute-user-path"))
        elif WORKSPACE_MARKER_RE.search(data):
            findings.append((path, "workspace-or-user-marker"))
        elif any(pattern.search(data) for pattern in user_marker_patterns):
            findings.append((path, "workspace-or-user-marker"))
    return findings


def sanitize(
    image: Path,
    dry_run: bool = False,
    manifest_path: Path | None = None,
) -> SanitizeResult:
    if not image.exists() or not image.is_dir():
        raise FileNotFoundError(f"LibreOffice runtime image not found: {image}")

    selected_manifest = (manifest_path or DEFAULT_REDUCTION_MANIFEST).resolve()
    removed_paths, removed_bytes = remove_manifest_extras(
        image,
        selected_manifest,
        dry_run=dry_run,
    )
    files_changed, replacements = sanitize_local_build_paths(image, dry_run=dry_run)
    config_files_changed, config_replacements = sanitize_text_config_files(image, dry_run=dry_run)
    files_changed += config_files_changed
    replacements += config_replacements

    if not dry_run:
        remaining = find_remaining_sensitive_markers(image)
        if remaining:
            details = "\n".join(f"- {path}: {kind}" for path, kind in remaining[:20])
            raise RuntimeError(f"Sensitive local build markers remain in LibreOffice runtime:\n{details}")

    return SanitizeResult(
        files_changed=files_changed,
        replacements=replacements,
        removed_paths=removed_paths,
        removed_bytes=removed_bytes,
        manifest_path=selected_manifest,
    )


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    image = Path(args.image)
    try:
        result = sanitize(image, dry_run=args.dry_run, manifest_path=Path(args.manifest))
    except Exception as exc:
        print(f"[NG] LibreOffice release runtime sanitization failed: {exc}", file=sys.stderr)
        return 1

    for path in result.removed_paths:
        action = "would remove" if args.dry_run else "removed"
        print(f"[OK] {action}: {path}")
    print(
        "[OK] LibreOffice release runtime sanitized: "
        f"removed={len(result.removed_paths)} path(s)/{result.removed_bytes} byte(s), "
        f"changed={result.files_changed} file(s), {result.replacements} local path replacement(s), "
        f"manifest={result.manifest_path}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
