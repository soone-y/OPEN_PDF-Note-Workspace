#!/usr/bin/env python3
"""Read-only environment check for a custom LibreOffice Windows build."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Sequence


ARCHIVES = [
    "third_party/libreoffice/source_archives/libreoffice-26.2.3.2.tar.xz",
    "third_party/libreoffice/source_archives/libreoffice-dictionaries-26.2.3.2.tar.xz",
    "third_party/libreoffice/source_archives/libreoffice-help-26.2.3.2.tar.xz",
    "third_party/libreoffice/source_archives/libreoffice-translations-26.2.3.2.tar.xz",
]

COMMANDS = [
    ("git", ["git", "--version"], "required", "source checkout and patch management"),
    ("python", ["python", "--version"], "required", "LibreOffice build scripts and local tools"),
    ("perl", ["perl", "--version"], "required", "LibreOffice configure/build helpers"),
    ("tar", ["tar", "--version"], "required", "source archive extraction"),
    ("bash", ["bash", "--version"], "required", "Cygwin/MSYS shell used by LibreOffice Windows builds"),
    ("make", ["make", "--version"], "required", "GNU make frontend"),
    ("gmake", ["gmake", "--version"], "optional", "GNU make alias on some systems"),
    ("cl", ["cl"], "required", "MSVC C/C++ compiler"),
    ("link", ["link"], "required", "MSVC linker"),
    ("nmake", ["nmake", "/?"], "optional", "Visual Studio build utility"),
    ("patch", ["patch", "--version"], "required", "applying local source patches"),
    ("pkg-config", ["pkg-config", "--version"], "optional", "system library detection"),
    ("autoconf", ["autoconf", "--version"], "optional", "regenerating configure when needed"),
    ("automake", ["automake", "--version"], "optional", "regenerating external build files when needed"),
    ("gpg", ["gpg", "--version"], "optional", "verifying source archive signatures"),
    ("java", ["java", "-version"], "optional", "should be absent or unused for --without-java builds"),
    ("javac", ["javac", "-version"], "optional", "should be absent or unused for --without-java builds"),
]

VS_ENV_VARS = [
    "VSINSTALLDIR",
    "VCINSTALLDIR",
    "VCToolsInstallDir",
    "WindowsSdkDir",
    "INCLUDE",
    "LIB",
]


@dataclass
class CommandCheck:
    name: str
    status: str
    required: str
    path: str | None
    detail: str
    note: str


@dataclass
class ArchiveCheck:
    path: str
    exists: bool
    size_bytes: int | None
    sha256_file: str | None
    sha256_matches: bool | None
    detail: str


@dataclass
class EnvVarCheck:
    name: str
    set: bool
    value: str | None


def run_version(command: Sequence[str]) -> str:
    try:
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return f"version check failed: {exc}"
    raw_output = (completed.stdout or b"") + (completed.stderr or b"")
    output = decode_output(raw_output).strip()
    first_line = next((line.strip() for line in output.splitlines() if line.strip()), "")
    if not first_line:
        first_line = f"exit code {completed.returncode}"
    return first_line[:240]


def decode_output(data: bytes) -> str:
    candidates = [
        data.decode("utf-8", errors="replace"),
        data.decode("cp932", errors="replace"),
    ]
    return min(candidates, key=lambda value: value.count("\ufffd")).replace("\ufffd", "?")


def check_command(name: str, command: Sequence[str], required: str, note: str) -> CommandCheck:
    found = shutil.which(name)
    if not found:
        return CommandCheck(name, "missing", required, None, "not found in PATH", note)
    if name == "bash" and "windowsapps" in found.casefold():
        return CommandCheck(
            name,
            "missing",
            required,
            found,
            "WindowsApps bash.exe is a WSL launcher stub, not a LibreOffice build shell",
            note,
        )
    return CommandCheck(name, "found", required, found, run_version(command), note)


def read_expected_sha256(path: Path) -> str | None:
    if not path.exists():
        return None
    text = path.read_text(encoding="utf-8", errors="ignore").strip()
    if not text:
        return None
    return text.split()[0].lower()


def calculate_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check_archive(repo_root: Path, relative: str) -> ArchiveCheck:
    path = repo_root / relative
    sha_file = path.with_suffix(path.suffix + ".sha256")
    if not path.exists():
        return ArchiveCheck(relative, False, None, str(sha_file.relative_to(repo_root)), None, "missing")
    expected = read_expected_sha256(sha_file)
    if expected is None:
        return ArchiveCheck(
            relative,
            True,
            path.stat().st_size,
            str(sha_file.relative_to(repo_root)),
            None,
            "sha256 sidecar missing or empty",
        )
    actual = calculate_sha256(path)
    matches = actual == expected
    detail = "sha256 ok" if matches else f"sha256 mismatch: actual {actual}"
    return ArchiveCheck(relative, True, path.stat().st_size, str(sha_file.relative_to(repo_root)), matches, detail)


def build_result(repo_root: Path) -> dict[str, object]:
    commands = [check_command(*item) for item in COMMANDS]
    archives = [check_archive(repo_root, item) for item in ARCHIVES]
    env_vars = [
        EnvVarCheck(name, name in os.environ and bool(os.environ.get(name)), os.environ.get(name))
        for name in VS_ENV_VARS
    ]
    required_missing = [
        item.name for item in commands if item.required == "required" and item.status != "found"
    ]
    archive_failures = [
        item.path for item in archives if not item.exists or item.sha256_matches is False
    ]
    return {
        "host": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "python": platform.python_version(),
        },
        "commands": [asdict(item) for item in commands],
        "visual_studio_environment": [asdict(item) for item in env_vars],
        "source_archives": [asdict(item) for item in archives],
        "summary": {
            "ready_for_first_build": not required_missing and not archive_failures,
            "required_missing": required_missing,
            "archive_failures": archive_failures,
            "note": "This is a local readiness check only. It does not install tools, download dependencies, or build LibreOffice.",
        },
    }


def print_text(result: dict[str, object]) -> None:
    host = result["host"]
    print("LibreOffice custom build environment check")
    print(f"Host: {host['system']} {host['release']} {host['machine']}, Python {host['python']}")

    print("\nCommands")
    for item in result["commands"]:
        marker = "OK" if item["status"] == "found" else "MISS"
        print(f"  {marker:4} {item['name']:<12} {item['required']:<8} {item['detail']}")
        if item["path"]:
            print(f"       path: {item['path']}")

    print("\nVisual Studio environment")
    for item in result["visual_studio_environment"]:
        marker = "OK" if item["set"] else "MISS"
        value = item["value"] or ""
        if len(value) > 180:
            value = value[:177] + "..."
        print(f"  {marker:4} {item['name']:<18} {value}")

    print("\nSource archives")
    for item in result["source_archives"]:
        marker = "OK" if item["exists"] and item["sha256_matches"] is not False else "MISS"
        size = item["size_bytes"] or 0
        print(f"  {marker:4} {item['path']} ({size / 1024 / 1024:.2f} MB) - {item['detail']}")

    summary = result["summary"]
    print("\nSummary")
    print(f"  ready_for_first_build: {summary['ready_for_first_build']}")
    if summary["required_missing"]:
        print("  required_missing: " + ", ".join(summary["required_missing"]))
    if summary["archive_failures"]:
        print("  archive_failures: " + ", ".join(summary["archive_failures"]))
    print(f"  {summary['note']}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check local prerequisites for a custom LibreOffice build without changing files."
    )
    parser.add_argument("--repo-root", default=".", help="Repository root. Defaults to current directory.")
    parser.add_argument("--format", choices=("text", "json"), default="text")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    result = build_result(repo_root)
    if args.format == "json":
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print_text(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
