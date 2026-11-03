"""Two-phase, local release preparation; incomplete packages are never release sets."""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.dev.export_public_snapshot import build_snapshot_plan, git_tracked_files, parse_allowlist, parse_artifact_manifest
from tools.release_checks.public_snapshot_inventory import file_records, is_link, read_inventory, sha256_file, tree_sha256, write_new_json

SOURCE_RECEIPT = "release_source_inputs.json"
PACKAGE_RECEIPT = "release_packages_receipt.json"


def source_identity(repository: Path) -> dict:
    policy = repository / "docs/internal/public_repo_release_allowlist_2026-08-24.txt"
    artifacts = repository / "docs/internal/public_repo_release_artifact_manifest_2026-08-12.tsv"
    gitignore = repository / "docs/internal/public_repo_gitignoreテンプレート_2026-07-02.gitignore"
    plan = build_snapshot_plan(repository, parse_allowlist(policy), git_tracked_files(repository), parse_artifact_manifest(repository, artifacts))
    rows = [{"path": relative.as_posix(), "size": source.stat().st_size, "sha256": sha256_file(source)} for source, relative in plan.files]
    rows.sort(key=lambda row: row["path"])
    return {"schema_version": 1, "allowlist_sha256": sha256_file(policy), "artifact_manifest_sha256": sha256_file(artifacts),
            "gitignore_sha256": sha256_file(gitignore), "tree_sha256": tree_sha256(rows), "files": rows}


def read_prepared(root: Path, version: str, locale: str) -> dict:
    if is_link(root) or (root / "release_set_manifest.json").exists() or (root / "public_snapshot").exists():
        raise ValueError("prepared packages must be an unfrozen owned preparation")
    receipt = read_inventory(root / PACKAGE_RECEIPT)
    if receipt.get("phase") != "PREPARED_ONLY" or receipt.get("version") != version or receipt.get("locale") != locale:
        raise ValueError("prepared package version, locale or phase mismatch")
    expected_keys = {"release", "release_lite", "release_zip", "release_lite_zip"}
    components = receipt.get("components", {})
    if set(components) != expected_keys:
        raise ValueError("incomplete prepared package components")
    if any(not isinstance(value, str) or not value or Path(value).name != value or value in {".", ".."} for value in components.values()):
        raise ValueError("prepared component escapes its root")
    if len(set(components.values())) != 4:
        raise ValueError("duplicate prepared components")
    actual = [row for row in file_records(root) if row["path"] != PACKAGE_RECEIPT]
    if actual != receipt["files"]:
        raise ValueError("prepared package files changed")
    for relative in (row["path"] for row in actual):
        if relative != SOURCE_RECEIPT and relative.split("/")[0] not in components.values():
            raise ValueError("unexpected prepared package file")
    for name in components.values():
        if not (root / name).exists(): raise ValueError("prepared package component missing")
    return receipt


def seal(root: Path, repository: Path, locale: str, version: str, components: dict) -> None:
    if (root / "public_snapshot").exists() or (root / "release_set_manifest.json").exists():
        raise ValueError("cannot prepare a completed or snapshot-bearing set")
    initial = json.loads((root / SOURCE_RECEIPT).read_text(encoding="utf-8"))
    if initial != source_identity(repository):
        raise ValueError("release source inputs changed during packing")
    rows = file_records(root)
    receipt = {"schema_version": 1, "phase": "PREPARED_ONLY", "version": version, "locale": locale,
               "components": components, "tree_sha256": tree_sha256(rows), "files": rows}
    write_new_json(root / PACKAGE_RECEIPT, receipt)
    read_prepared(root, version, locale)


def adopt(source: Path, destination: Path, repository: Path, locale: str, version: str) -> dict:
    source, destination = source.absolute(), destination.absolute()
    if is_link(source) or is_link(destination) or source.resolve() == destination.resolve() or source.parent.resolve() != destination.parent.resolve():
        raise ValueError("preparation adoption requires distinct sibling directories")
    if not destination.is_dir() or any(destination.iterdir()):
        raise ValueError("release finalization destination must be a new empty directory")
    receipt = read_prepared(source, version, locale)
    if json.loads((source / SOURCE_RECEIPT).read_text(encoding="utf-8")) != source_identity(repository):
        raise ValueError("release source inputs changed before finalization")
    # Both directories are validated siblings. Remove only our empty target,
    # then use a same-parent rename; no copy/delete fallback can lose input.
    destination.rmdir()
    for attempt in range(3):
        try:
            source.rename(destination)
            break
        except OSError as error:
            # Windows may briefly hold a new directory without delete sharing.
            # Retry the same rename only; never copy/delete or overwrite a target.
            if getattr(error, "winerror", None) not in {5, 32, 33} or attempt == 2:
                raise
            time.sleep(0.1 * (attempt + 1))
            if destination.exists() or read_prepared(source, version, locale) != receipt:
                raise ValueError("prepared adoption inputs changed during rename retry") from error
            if json.loads((source / SOURCE_RECEIPT).read_text(encoding="utf-8")) != source_identity(repository):
                raise ValueError("release source inputs changed during rename retry") from error
    return receipt


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("begin", "seal", "adopt"))
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--destination", type=Path)
    parser.add_argument("--locale", choices=("ja", "en"))
    parser.add_argument("--version")
    for name in ("release", "release-lite", "release-zip", "release-lite-zip"):
        parser.add_argument("--" + name)
    args = parser.parse_args(argv)
    try:
        if args.operation == "begin":
            write_new_json(args.root / SOURCE_RECEIPT, source_identity(args.repository))
        elif args.operation == "seal":
            components = {key: getattr(args, key) for key in ("release", "release_lite", "release_zip", "release_lite_zip")}
            seal(args.root, args.repository, args.locale, args.version, components)
        else:
            if args.destination is None: raise ValueError("adopt requires a destination")
            adopt(args.root, args.destination, args.repository, args.locale, args.version)
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        print(f"Release preparation failed: {error}", file=sys.stderr)
        return 1
    print("Release preparation: PASS (not a publish approval)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
