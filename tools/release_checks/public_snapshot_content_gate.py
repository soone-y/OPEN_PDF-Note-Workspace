#!/usr/bin/env python3
"""Fail closed when a public snapshot contains private or unreviewed material."""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Iterable


FORBIDDEN_PATH_PREFIXES = (
    ".agents",
    ".git",
    ".local",
    "out",
    "docs/internal",
    "third_party/libreoffice/custom_runtime",
    "third_party/libreoffice/image",
    "third_party/libreoffice/source_archives",
)
FORBIDDEN_SUFFIXES = {
    ".7z",
    ".bak",
    ".dmp",
    ".gz",
    ".ilk",
    ".log",
    ".obj",
    ".orig",
    ".pch",
    ".rej",
    ".tar",
    ".tgz",
    ".zip",
}
TEXT_SUFFIXES = {
    "",
    ".bat",
    ".cfg",
    ".cmake",
    ".cpp",
    ".cppinc",
    ".csv",
    ".h",
    ".html",
    ".in",
    ".inc",
    ".ini",
    ".input",
    ".json",
    ".md",
    ".patch",
    ".ps1",
    ".py",
    ".sh",
    ".tsv",
    ".txt",
    ".xml",
    ".yml",
    ".yaml",
}
MAX_TEXT_BYTES = 32 * 1024 * 1024
PLACEHOLDER_USERS = {"example", "localuser", "testuser", "user", "username"}
WINDOWS_USER_PATH = re.compile(r"(?i)(?:^|[^A-Z0-9_])(?:[A-Z]:)?[\\/]Users[\\/](?P<user>[^\\/\s\"']+)")
UNIX_USER_PATH = re.compile(r"(?i)(?:^|[^A-Z0-9_])(?:/home|/Users)/(?P<user>[^/\s\"']+)")
PRIVATE_WORKSPACE_PATH = re.compile(r"(?i)(?:[A-Z]:)?[\\/]global_develop(?:[\\/]|$)")
SECRET_PATTERNS = (
    ("private-key", re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----")),
    ("github-token", re.compile(r"(?:github_pat_[A-Za-z0-9_]{20,}|gh[pousr]_[A-Za-z0-9]{20,})")),
    ("openai-token", re.compile(r"sk-[A-Za-z0-9_-]{20,}")),
    ("aws-access-key", re.compile(r"AKIA[0-9A-Z]{16}")),
)


@dataclass(frozen=True)
class Violation:
    kind: str
    path: str


def normalized_path(path: PurePosixPath) -> str:
    return path.as_posix().strip("/")


def path_is_forbidden(relative: PurePosixPath) -> bool:
    value = normalized_path(relative).lower()
    return any(value == prefix or value.startswith(prefix + "/") for prefix in FORBIDDEN_PATH_PREFIXES)


def iter_files(snapshot: Path) -> Iterable[Path]:
    for path in sorted(snapshot.rglob("*")):
        if path.is_symlink():
            yield path
        elif path.is_file():
            yield path


def user_path_violation(text: str) -> bool:
    for pattern in (WINDOWS_USER_PATH, UNIX_USER_PATH):
        for match in pattern.finditer(text):
            username = match.group("user").strip("<>{}[]()$%").lower()
            if username not in PLACEHOLDER_USERS:
                return True
    return False


def sensitive_host_literals() -> tuple[str, ...]:
    values: set[str] = set()
    for raw in (os.environ.get("USERPROFILE"), str(Path.home())):
        if raw:
            normalized = raw.replace("\\", "/").rstrip("/").lower()
            if normalized:
                values.add(normalized)
    return tuple(sorted(values))


def decode_text_bytes(content: bytes) -> str:
    if content.startswith((b"\xff\xfe", b"\xfe\xff")):
        try:
            return content.decode("utf-16")
        except UnicodeDecodeError:
            return content.decode("latin-1")
    try:
        return content.decode("utf-8-sig")
    except UnicodeDecodeError:
        # Vendored licenses and legacy compiler fixtures may intentionally use
        # a non-UTF-8 encoding. Latin-1 preserves every byte so ASCII paths,
        # key headers, and token formats remain detectable.
        return content.decode("latin-1")


def scan_text_content(
    text: str,
    relative: PurePosixPath,
    host_literals: tuple[str, ...],
) -> list[Violation]:
    violations: list[Violation] = []
    normalized_text = text.replace("\\", "/").lower()
    if user_path_violation(text) or any(value in normalized_text for value in host_literals):
        violations.append(Violation("private-user-path", relative.as_posix()))
    if PRIVATE_WORKSPACE_PATH.search(text):
        violations.append(Violation("private-workspace-path", relative.as_posix()))
    for name, pattern in SECRET_PATTERNS:
        if pattern.search(text):
            violations.append(Violation(name, relative.as_posix()))

    if relative.as_posix().lower() == "readme.md":
        history_markers = (
            "DEVELOPMENT_" + "HISTORY:START",
            "開発側の累積" + "コミット数",
        )
        if any(marker in text for marker in history_markers):
            violations.append(Violation("private-development-history", relative.as_posix()))
    return violations


def scan_text(path: Path, relative: PurePosixPath, host_literals: tuple[str, ...]) -> list[Violation]:
    if path.suffix.lower() not in TEXT_SUFFIXES or path.stat().st_size > MAX_TEXT_BYTES:
        return []
    try:
        content = path.read_bytes()
    except OSError:
        return [Violation("unreadable-public-text", relative.as_posix())]
    return scan_text_content(decode_text_bytes(content), relative, host_literals)


def collect_violations(snapshot: Path) -> list[Violation]:
    if not snapshot.is_dir():
        return [Violation("missing-snapshot", str(snapshot))]
    violations: list[Violation] = []
    host_literals = sensitive_host_literals()
    for path in iter_files(snapshot):
        try:
            relative = PurePosixPath(path.relative_to(snapshot).as_posix())
        except ValueError:
            violations.append(Violation("path-escapes-snapshot", str(path)))
            continue
        if path.is_symlink():
            violations.append(Violation("symbolic-link", relative.as_posix()))
            continue
        if path_is_forbidden(relative):
            violations.append(Violation("forbidden-path", relative.as_posix()))
        if path.suffix.lower() in FORBIDDEN_SUFFIXES:
            violations.append(Violation("forbidden-file-type", relative.as_posix()))
        violations.extend(scan_text(path, relative, host_literals))
    return sorted(set(violations), key=lambda item: (item.path, item.kind))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", type=Path, required=True)
    args = parser.parse_args(argv)
    violations = collect_violations(args.snapshot.resolve())
    if violations:
        print("Public snapshot content gate failed:", file=sys.stderr)
        for violation in violations:
            print(f"- {violation.kind}: {violation.path}", file=sys.stderr)
        return 1
    print(f"Public snapshot content gate passed: {args.snapshot.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
