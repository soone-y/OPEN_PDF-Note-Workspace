"""Generate in new staging snapshots, or verify frozen statistics without writes."""
from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

from tools.metrics.public_statistics.collect import aggregate
from tools.metrics.public_statistics.model import OUTPUTS, compare, digest, measured_records, read_json, read_seed, validate_document
from tools.metrics.public_statistics.packages import measure
from tools.metrics.public_statistics.render import render
from tools.release_checks.public_snapshot_inventory import file_records, is_link, read_inventory, tree_sha256, sha256_file


def package_arguments(values: list[str], version: str | None = None) -> list[dict]:
    rows, seen = [], set()
    for value in values:
        variant, path = value.split("=", 1)
        locale, edition = variant.split(":", 1)
        if locale not in {"ja", "en"} or edition not in {"full", "lite"} or (locale, edition) in seen:
            raise ValueError("invalid package selection")
        seen.add((locale, edition))
        rows.append(measure(Path(path), locale, edition, version))
    return sorted(rows, key=lambda row: (row["locale"], row["edition"]))


def write_views(root: Path, document: dict, *, exclusive: bool) -> None:
    summary, history = render(document)
    outputs = {OUTPUTS[0]: summary, OUTPUTS[1]: json.dumps(document, ensure_ascii=False, indent=2) + "\n", OUTPUTS[2]: history}
    for relative, text in outputs.items():
        path = root / relative
        if is_link(path) or root.resolve() not in path.resolve().parents:
            raise ValueError("statistics output escapes staging root")
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("x" if exclusive else "w", encoding="utf-8", newline="\n") as target:
            target.write(text)


def check_views(root: Path, document: dict) -> None:
    summary, history = render(document)
    if (root / OUTPUTS[0]).read_bytes() != summary.encode("utf-8") or (root / OUTPUTS[2]).read_bytes() != history.encode("utf-8"):
        raise ValueError("statistics Markdown differs from validated JSON")


def generate(root: Path, inventory_path: Path, seed: Path, packages: list[dict], required_locales: list[str], measured_at: str) -> dict:
    root = root.resolve()
    if (root / ".git").exists() or (root.parent / "public_snapshot_manifest.json").exists():
        raise ValueError("statistics generation requires an unfrozen exported staging snapshot")
    if root in inventory_path.resolve().parents:
        raise ValueError("export inventory must be outside the public snapshot")
    receipt = read_inventory(inventory_path)
    seed_rows = [row for row in receipt["files"] if row["path"] == OUTPUTS[1]]
    if len(seed_rows) != 1 or seed_rows[0]["sha256"] != sha256_file(root / OUTPUTS[1]):
        raise ValueError("exported history seed identity mismatch")
    existing = read_seed(root / OUTPUTS[1])
    document = read_seed(seed)
    if existing != document:
        raise ValueError("snapshot history seed differs from selected seed")
    expected = {(locale, edition) for locale in required_locales for edition in ("full", "lite")}
    if {(row["locale"], row["edition"]) for row in packages} != expected:
        raise ValueError("required distribution variants are missing or extra")
    if not receipt.get("allowlist_sha256"):
        raise ValueError("candidate statistics require exporter allowlist evidence")
    version = (root / "REPO_VERSION.txt").read_text(encoding="utf-8-sig").strip()
    record, active_profile = aggregate(root, receipt, version=version, measured_at=measured_at,
                                       source_kind="release-candidate", packages=packages)
    if record["localization"]["status"] != "COMPLETE" or not record["source_scale"]["application"]["files"]:
        raise ValueError("candidate locale or application source inputs are incomplete")
    record["comparison"] = compare(record, document["history"][-1] if document["history"] else None)
    document["profiles"][digest(active_profile)] = active_profile
    document["current"] = record
    validate_document(document)
    write_views(root, document, exclusive=False)
    verify(root, packages, required_locales)
    return document


def verify(root: Path, packages: list[dict] | None = None, required_locales: list[str] | None = None) -> None:
    document = read_json(root / OUTPUTS[1])
    validate_document(document)
    check_views(root, document)
    current = document["current"]
    if current is None:
        if required_locales:
            raise ValueError("release statistics current record is missing")
        return
    rows = file_records(root)
    if digest(measured_records(rows)) != current["measurement"]["input_sha256"]:
        raise ValueError("statistics input hash differs from snapshot")
    if (root / "REPO_VERSION.txt").read_text(encoding="utf-8-sig").strip() != current["release"]["version"]:
        raise ValueError("statistics release version mismatch")
    if packages is not None and packages != current["packages"]:
        raise ValueError("statistics package identities or measurements differ")
    if required_locales is not None:
        expected = {(locale, edition) for locale in required_locales for edition in ("full", "lite")}
        if {(row["locale"], row["edition"]) for row in current["packages"]} != expected:
            raise ValueError("statistics required package variants differ")
    receipt = {"files": rows, "tree_sha256": tree_sha256(rows),
               "allowlist_sha256": current["measurement"]["allowlist_sha256"]}
    observed, active_profile = aggregate(root, receipt, version=current["release"]["version"], measured_at=current["measurement"]["measured_at"],
                                         source_kind=current["measurement"]["source_kind"], packages=current["packages"],
                                         published_at=current["release"]["published_at"], public_url=current["release"]["public_url"], tag=current["release"]["tag"])
    if observed["record_id"] != current["record_id"]:
        raise ValueError("statistics aggregates or measurement method do not reproduce")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="operation", required=True)
    for name in ("generate", "verify"):
        command = commands.add_parser(name)
        command.add_argument("--snapshot", type=Path, required=True)
        command.add_argument("--package", action="append", default=[])
        command.add_argument("--required-locales", nargs="*", choices=("ja", "en"), default=["ja", "en"])
        if name == "generate":
            command.add_argument("--inventory", type=Path, required=True)
            command.add_argument("--history", type=Path, required=True)
            command.add_argument("--measured-at", default=None)
    command = commands.add_parser("render-history")
    command.add_argument("--history", type=Path, required=True)
    command.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.operation == "render-history":
            if args.output.exists(): raise ValueError("history render output must be new")
            write_views(args.output, read_seed(args.history), exclusive=True)
        else:
            version = (args.snapshot / "REPO_VERSION.txt").read_text(encoding="utf-8-sig").strip()
            packages = package_arguments(args.package, version)
            if args.operation == "generate":
                generate(args.snapshot, args.inventory, args.history, packages, args.required_locales,
                         args.measured_at or datetime.now(timezone.utc).isoformat())
            else:
                verify(args.snapshot, packages, args.required_locales)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Public statistics failed: {error}", file=sys.stderr)
        return 1
    print("Public statistics: PASS")
    return 0
