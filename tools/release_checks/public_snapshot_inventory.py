"""Strict, read-only inventory shared by export, statistics and release gates."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_relative(value: object) -> str:
    if not isinstance(value, str) or not value or "\\" in value or ":" in value or any(ord(c) < 32 for c in value):
        raise ValueError("invalid inventory relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in value.split("/")):
        raise ValueError("inventory path escapes its root")
    return value


def is_link(path: Path) -> bool:
    return path.is_symlink() or getattr(path, "is_junction", lambda: False)()


def file_records(root: Path, *, exclude_git_metadata: bool = False) -> list[dict]:
    if is_link(root) or not root.is_dir():
        raise ValueError("inventory root must be an existing regular directory")
    result, names = [], set()
    for directory, subdirs, files in os.walk(root, followlinks=False, onerror=lambda error: (_ for _ in ()).throw(error)):
        parent = Path(directory)
        if parent == root and exclude_git_metadata:
            subdirs[:] = [name for name in subdirs if name != ".git"]
            files = [name for name in files if name != ".git"]
        for name in subdirs + files:
            if is_link(parent / name):
                raise ValueError("inventory contains a link or junction")
        for name in files:
            path = parent / name
            relative = safe_relative(path.relative_to(root).as_posix())
            if relative.casefold() in names:
                raise ValueError("case-colliding inventory paths")
            names.add(relative.casefold())
            before = path.stat()
            digest = sha256_file(path)
            after = path.stat()
            if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                raise ValueError("inventory input changed while reading")
            result.append({"path": relative, "size": before.st_size, "sha256": digest})
    return sorted(result, key=lambda item: item["path"])


def tree_sha256(records: list[dict]) -> str:
    return hashlib.sha256("".join(f"{row['path']}\t{row['size']}\t{row['sha256']}\n" for row in records).encode("utf-8")).hexdigest()


def write_new_json(path: Path, payload: object) -> None:
    """Exclusive creation; callers own the new staging directory."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8", newline="\n") as target:
        json.dump(payload, target, ensure_ascii=False, indent=2)
        target.write("\n")


def read_inventory(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    if value.get("schema_version") != 1 or not isinstance(value.get("files"), list):
        raise ValueError("invalid snapshot inventory")
    names = set()
    for row in value["files"]:
        name = safe_relative(row.get("path"))
        if name.casefold() in names or type(row.get("size")) is not int or row["size"] < 0:
            raise ValueError("duplicate or invalid snapshot inventory entry")
        if not isinstance(row.get("sha256"), str) or len(row["sha256"]) != 64 or any(c not in "0123456789abcdef" for c in row["sha256"]):
            raise ValueError("invalid inventory digest")
        names.add(name.casefold())
    if value.get("tree_sha256") != tree_sha256(value["files"]):
        raise ValueError("snapshot inventory hash mismatch")
    return value
