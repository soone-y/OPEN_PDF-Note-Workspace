#!/usr/bin/env python3
"""Validate local scripts and authored text before release checks run."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


TEXT_SUFFIXES = {
    ".c", ".cc", ".cfg", ".cpp", ".cppinc", ".csv", ".h", ".hh", ".hpp",
    ".htm", ".html", ".inc", ".ini", ".inl", ".ipp", ".json", ".md", ".ps1",
    ".py", ".txt", ".xml", ".yaml", ".yml",
}
JSON_SUFFIXES = {".json"}
CHECKED_ROOTS = {".github", "scripts", "site", "src", "tests", "tools"}
EXCLUDED_PREFIXES = (("tests", "fixtures"), ("tests", "integration"))


def looks_like_mojibake(text: str) -> bool:
    """Detect text reversibly repairable from Windows-1252 to UTF-8."""
    try:
        repaired = text.encode("cp1252").decode("utf-8")
    except UnicodeError:
        return False
    return repaired != text and any(ord(char) > 0x7F for char in repaired)


def validate_text_bytes(content: bytes, *, label: str) -> tuple[str | None, list[str]]:
    if content.startswith((b"\xff\xfe", b"\xfe\xff")):
        return None, [f"UTF-16 text is not supported: {label}"]
    if b"\x00" in content:
        return None, [f"NUL byte in text file: {label}"]
    try:
        text = content.decode("utf-8-sig")
    except UnicodeDecodeError as error:
        return None, [f"invalid UTF-8: {label} ({error})"]
    errors: list[str] = []
    if "\ufffd" in text:
        errors.append(f"replacement character indicates possible mojibake: {label}")
    if looks_like_mojibake(text):
        errors.append(f"likely Windows-1252/UTF-8 mojibake: {label}")
    return text, errors


def repository_files(repo_root: Path) -> list[Path]:
    result = subprocess.run(
        ["git", "-C", str(repo_root), "ls-files", "-z", "--cached"],
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"unable to enumerate repository files: {detail}")
    return [repo_root / Path(item.decode("utf-8")) for item in result.stdout.split(b"\0") if item]


def validate_python_syntax(path: Path, text: str, label: str) -> list[str]:
    try:
        compile(text, str(path), "exec")
    except SyntaxError as error:
        return [f"Python syntax error: {label}:{error.lineno}:{error.offset}: {error.msg}"]
    return []


def validate_json_syntax(text: str, label: str) -> list[str]:
    try:
        json.loads(text)
    except json.JSONDecodeError as error:
        return [f"invalid JSON: {label}:{error.lineno}:{error.colno}: {error.msg}"]
    return []


def validate_powershell_syntax(paths: list[Path], repo_root: Path) -> list[str]:
    if not paths:
        return []
    executable = shutil.which("powershell.exe") or shutil.which("powershell")
    if executable is None:
        return ["powershell.exe was not found; cannot parse PowerShell scripts"]
    script = (
        "$paths = [Environment]::GetEnvironmentVariable('PDF_NOTE_HYGIENE_PS1_PATHS') | ConvertFrom-Json; "
        "$errors = @(); "
        "foreach ($path in $paths) { "
        "$tokens = $null; $parseErrors = $null; "
        "[System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$parseErrors) | Out-Null; "
        "foreach ($error in $parseErrors) { "
        "Write-Output ($path + ':' + $error.Extent.StartLineNumber + ':' + $error.Extent.StartColumnNumber + ': ' + $error.Message); $errors += $error } "
        "}; if ($errors.Count -ne 0) { exit 1 }"
    )
    environment = os.environ.copy()
    environment["PDF_NOTE_HYGIENE_PS1_PATHS"] = json.dumps([str(path) for path in paths])
    result = subprocess.run(
        [executable, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
        cwd=repo_root,
        capture_output=True,
        check=False,
        env=environment,
    )
    if result.returncode == 0:
        return []
    output = result.stdout.decode("utf-8", errors="replace").strip()
    if not output:
        output = result.stderr.decode("utf-8", errors="replace").strip()
    details = output.splitlines() or ["PowerShell parser returned a failure without details"]
    return [f"PowerShell syntax error: {line}" for line in details]


def validate_repository(repo_root: Path) -> list[str]:
    errors: list[str] = []
    powershell_paths: list[Path] = []
    for path in repository_files(repo_root):
        relative_parts = path.relative_to(repo_root).parts
        if (not path.is_file() or path.suffix.lower() not in TEXT_SUFFIXES or
                not relative_parts or relative_parts[0] not in CHECKED_ROOTS or
                any(relative_parts[:len(prefix)] == prefix for prefix in EXCLUDED_PREFIXES)):
            continue
        label = path.relative_to(repo_root).as_posix()
        text, text_errors = validate_text_bytes(path.read_bytes(), label=label)
        errors.extend(text_errors)
        if text is None:
            continue
        suffix = path.suffix.lower()
        if suffix == ".py":
            errors.extend(validate_python_syntax(path, text, label))
        elif suffix in JSON_SUFFIXES:
            errors.extend(validate_json_syntax(text, label))
        elif suffix == ".ps1":
            powershell_paths.append(path)
    errors.extend(validate_powershell_syntax(powershell_paths, repo_root))
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args(argv)
    repo_root = args.repo_root.resolve()
    try:
        errors = validate_repository(repo_root)
    except (OSError, RuntimeError, UnicodeError) as error:
        print(f"Repository script/text gate could not complete: {error}", file=sys.stderr)
        return 1
    if errors:
        print("Repository script/text gate failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"Repository script/text gate passed: {repo_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
