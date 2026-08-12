#!/usr/bin/env python3
"""Offline release gate for the most recent manual dependency security review."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_REVIEW = Path(__file__).with_name("dependency_security_review.json")
ALLOWED_DECISIONS = {"pass", "blocked"}
REQUIRED_COMPONENTS = {"PDFium", "LibreOffice custom runtime", "MD4C", "zlib runtime"}
ALLOWED_SOURCE_HOSTS = {
    "chromereleases.googleblog.com",
    "chromium.googlesource.com",
    "github.com",
    "osv.dev",
    "www.chromium.org",
    "www.libreoffice.org",
    "zlib.net",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--review", type=Path, default=DEFAULT_REVIEW)
    parser.add_argument(
        "--check-record-only",
        action="store_true",
        help="Validate the record and local evidence without enforcing its release decision.",
    )
    return parser.parse_args()


def load_review(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read review record {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError("review root must be an object")
    return value


def parse_iso_date(value: object, field: str) -> dt.date:
    if not isinstance(value, str):
        raise ValueError(f"{field} must be an ISO date")
    try:
        return dt.date.fromisoformat(value)
    except ValueError as exc:
        raise ValueError(f"{field} must be an ISO date") from exc


def read_pdfium_version(path: Path) -> str:
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    required = ("MAJOR", "MINOR", "BUILD", "PATCH")
    if any(key not in values for key in required):
        raise ValueError(f"incomplete PDFium VERSION: {path}")
    return ".".join(values[key] for key in required)


def local_version(component: dict, evidence: Path) -> str:
    name = component["name"]
    if name == "PDFium":
        return read_pdfium_version(evidence)
    if name == "LibreOffice custom runtime":
        match = re.search(r"(?m)^LibreOffice\s+([^\s]+)\s*$", evidence.read_text(encoding="utf-8"))
        if not match:
            raise ValueError(f"cannot parse LibreOffice VERSION: {evidence}")
        return match.group(1)
    if name == "MD4C":
        version = evidence.read_text(encoding="utf-8").splitlines()[0].strip()
        if not re.fullmatch(r"0\.5\.3\+git\.[0-9a-f]{40}", version):
            raise ValueError("MD4C VERSION must pin the full post-0.5.3 upstream commit")
        return version
    if name == "zlib runtime":
        version = evidence.read_text(encoding="utf-8").splitlines()[0].strip()
        if not re.fullmatch(r"\d+\.\d+\.\d+", version):
            raise ValueError("zlib VERSION is malformed")
        return version
    raise ValueError(f"unsupported reviewed component: {name!r}")


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def repository_path(value: object, field: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} is required")
    path = (REPO_ROOT / value).resolve()
    try:
        path.relative_to(REPO_ROOT.resolve())
    except ValueError as exc:
        raise ValueError(f"{field} escapes the repository") from exc
    return path


def validate(review: dict) -> list[str]:
    errors: list[str] = []
    if review.get("schema_version") != 1:
        errors.append("schema_version must be 1")
    try:
        reviewed_on = parse_iso_date(review.get("reviewed_on"), "reviewed_on")
        valid_until = parse_iso_date(review.get("review_valid_until"), "review_valid_until")
        if valid_until < reviewed_on:
            errors.append("review_valid_until precedes reviewed_on")
        if dt.date.today() > valid_until:
            errors.append(f"dependency security review expired on {valid_until.isoformat()}")
    except ValueError as exc:
        errors.append(str(exc))

    release_decision = review.get("release_decision")
    if release_decision not in ALLOWED_DECISIONS:
        errors.append("release_decision must be 'pass' or 'blocked'")
    components = review.get("components")
    if not isinstance(components, list) or not components:
        errors.append("components must be a non-empty array")
        return errors

    names: set[str] = set()
    for index, component in enumerate(components):
        if not isinstance(component, dict):
            errors.append(f"components[{index}] must be an object")
            continue
        name = component.get("name")
        if not isinstance(name, str) or not name:
            errors.append(f"components[{index}].name is required")
            continue
        if name in names:
            errors.append(f"duplicate component: {name}")
        names.add(name)
        if component.get("decision") not in ALLOWED_DECISIONS:
            errors.append(f"{name}: decision must be 'pass' or 'blocked'")
        if not isinstance(component.get("required_action"), str) or not component["required_action"].strip():
            errors.append(f"{name}: required_action is required")
        sources = component.get("sources")
        if not isinstance(sources, list) or not sources:
            errors.append(f"{name}: sources must be a non-empty array")
        else:
            for source in sources:
                match = re.fullmatch(r"https://([^/]+)(?:/.*)?", source) if isinstance(source, str) else None
                if not match or match.group(1).lower() not in ALLOWED_SOURCE_HOSTS:
                    errors.append(f"{name}: unapproved or malformed audit source: {source!r}")
        try:
            evidence = repository_path(component.get("evidence_file"), f"{name}.evidence_file")
        except ValueError as exc:
            errors.append(str(exc))
            continue
        if not evidence.is_file():
            errors.append(f"{name}: evidence_file does not exist: {evidence}")
            continue
        try:
            actual = local_version(component, evidence)
        except (OSError, ValueError) as exc:
            errors.append(f"{name}: {exc}")
            continue
        if actual != component.get("local_version"):
            errors.append(
                f"{name}: local version changed from reviewed {component.get('local_version')!r} "
                f"to {actual!r}; repeat the manual review"
            )
        artifacts = component.get("artifacts")
        if not isinstance(artifacts, list) or not artifacts:
            errors.append(f"{name}: artifacts must be a non-empty array")
            continue
        for artifact_index, artifact in enumerate(artifacts):
            if not isinstance(artifact, dict):
                errors.append(f"{name}: artifacts[{artifact_index}] must be an object")
                continue
            try:
                artifact_path = repository_path(
                    artifact.get("path"), f"{name}.artifacts[{artifact_index}].path"
                )
            except ValueError as exc:
                errors.append(str(exc))
                continue
            expected_hash = artifact.get("sha256")
            if not isinstance(expected_hash, str) or not re.fullmatch(r"[0-9a-f]{64}", expected_hash):
                errors.append(f"{name}: invalid SHA-256 for {artifact_path.name}")
                continue
            if not artifact_path.is_file():
                errors.append(f"{name}: reviewed artifact is missing: {artifact_path}")
                continue
            try:
                actual_hash = file_sha256(artifact_path)
            except OSError as exc:
                errors.append(f"{name}: cannot hash reviewed artifact {artifact_path}: {exc}")
                continue
            if actual_hash != expected_hash:
                errors.append(
                    f"{name}: reviewed artifact changed: {artifact_path}; repeat the manual review"
                )
    if names != REQUIRED_COMPONENTS:
        errors.append(
            "reviewed component set mismatch: expected "
            + ", ".join(sorted(REQUIRED_COMPONENTS))
        )
    return errors


def main() -> int:
    args = parse_args()
    try:
        review = load_review(args.review.resolve())
    except ValueError as exc:
        print(f"Dependency security gate failed:\n- {exc}", file=sys.stderr)
        return 1
    errors = validate(review)
    if errors:
        print("Dependency security gate failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1

    if args.check_record_only:
        print("Dependency security review record is structurally valid and matches local evidence.")
        return 0
    if review["release_decision"] == "blocked":
        print("Dependency security gate blocked this release:", file=sys.stderr)
        for component in review["components"]:
            if component["decision"] == "blocked":
                print(f"- {component['name']}: {component['required_action']}", file=sys.stderr)
        return 1
    if any(component["decision"] == "blocked" for component in review["components"]):
        print("Dependency security gate failed: component decision conflicts with release_decision", file=sys.stderr)
        return 1
    print("Dependency security gate passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
