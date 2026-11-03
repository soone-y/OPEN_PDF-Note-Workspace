"""Read-only measurements of explicitly selected distribution archives."""
from __future__ import annotations

import stat
import zipfile
from pathlib import Path, PurePosixPath

from tools.metrics.distribution_size_report import analyze_zip
from tools.release_checks.public_snapshot_inventory import is_link, safe_relative, sha256_file


def measure(path: Path, locale: str, edition: str, version: str | None = None) -> dict:
    if is_link(path) or not path.is_file():
        raise ValueError("package must be a regular archive")
    digest = sha256_file(path)
    summary = analyze_zip(path)
    size, count, names = 0, 0, set()
    with zipfile.ZipFile(path) as archive:
        if len(archive.infolist()) > 100000:
            raise ValueError("package inventory limit exceeded")
        for member in archive.infolist():
            relative = safe_relative(member.filename.rstrip("/"))
            if relative.casefold() in names or stat.S_ISLNK(member.external_attr >> 16):
                raise ValueError("duplicate or linked ZIP member")
            names.add(relative.casefold())
            if version is not None and PurePosixPath(relative).parts[0] != "PDF-Note-Workspace-" + version:
                raise ValueError("package archive root version mismatch")
            if not member.is_dir():
                count += 1
                size += member.file_size
        if version is not None:
            if path.name != f"pdf_note_workspace_{version}_{locale}_{edition}.zip":
                raise ValueError("package filename variant mismatch")
            info_name = f"PDF-Note-Workspace-{version}/pdf_note_workspace.exe.buildinfo.txt"
            info = archive.getinfo(info_name)
            if info.file_size > 1024**2:
                raise ValueError("package build info limit exceeded")
            values = {}
            for line in archive.read(info).decode("utf-8-sig").splitlines():
                parts = line.split("\t")
                if len(parts) == 2:
                    if parts[0] in values: raise ValueError("duplicate package build info field")
                    values[parts[0]] = parts[1]
            if any(values.get(key, "").lower() != expected for key, expected in (("version", version), ("locale", locale), ("edition", edition))):
                raise ValueError("package build identity mismatch")
        if size > 8 * 1024**3:
            raise ValueError("unpacked package measurement limit exceeded")
    if digest != sha256_file(path):
        raise ValueError("package changed while measuring")
    return {"status": "COMPLETE", "locale": locale, "edition": edition, "sha256": digest,
            "zip_bytes": summary["total_size_bytes"], "unpacked_bytes": size, "file_count": count, "basis": "archive-bytes"}
