#!/usr/bin/env python3
"""Offline historical aggregate collection from fixed, locally supplied archives.

Private-history catalogs additionally supply an allowlist-equivalent filter.
Archives and receipts remain local; only the closed aggregate schema is emitted.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import tarfile
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.metrics.public_statistics.collect import aggregate
from tools.metrics.public_statistics.model import compare, digest, profile, read_json, read_seed, validate_document
from tools.metrics.public_statistics.packages import measure
from tools.release_checks.public_snapshot_inventory import file_records, safe_relative, sha256_file, tree_sha256, write_new_json


def extract_selected(archive: Path, destination: Path, allowed: list[str] | None, path_map: dict[str, str] | None = None) -> None:
    if destination.exists(): raise ValueError("historical source destination must be new")
    destination.mkdir(parents=True)
    names, targets, size = set(), set(), 0
    if path_map is not None and not isinstance(path_map, dict): raise ValueError("invalid historical role path mapping")
    mapping = path_map or {}
    for original, target in mapping.items():
        if not isinstance(original, str) or not isinstance(target, str) or not original.endswith("/") or not target.endswith("/"):
            raise ValueError("historical role paths must be directory prefixes")
        safe_relative(original.rstrip("/")); safe_relative(target.rstrip("/"))
        if target not in {"src/", "src/python/", "tests/", "tools/", "scripts/", "assets/"}:
            raise ValueError("historical role mapping target is not reviewed")
    with tarfile.open(archive) as source:
        for entry in source:
            name = safe_relative(entry.name.rstrip("/"))
            if entry.isdir():
                continue
            if not entry.isfile(): raise ValueError("historical source archive contains links or special files")
            if name.casefold() in names: raise ValueError("duplicate historical source path")
            names.add(name.casefold())
            if len(names) > 100000: raise ValueError("historical file limit exceeded")
            if allowed is not None and (name.startswith(("docs/internal/", ".agents/", ".git/", "out/", ".local/"))):
                continue
            if allowed is not None and not any(name == rule.rstrip("/") or rule.endswith("/") and name.startswith(rule) for rule in allowed):
                continue
            for prefix in sorted(mapping, key=len, reverse=True):
                if name.startswith(prefix):
                    name = safe_relative(mapping[prefix] + name[len(prefix):])
                    break
            if name.casefold() in targets: raise ValueError("historical role mapping collision")
            targets.add(name.casefold())
            size += entry.size
            if entry.size > 256 * 1024**2 or size > 2 * 1024**3: raise ValueError("historical source size limit exceeded")
            target = destination / name
            target.parent.mkdir(parents=True, exist_ok=True)
            incoming = source.extractfile(entry)
            if incoming is None: raise ValueError("historical source member cannot be read")
            with incoming, target.open("xb") as outgoing:
                remaining = entry.size
                while remaining:
                    chunk = incoming.read(min(remaining, 1024**2))
                    if not chunk: raise ValueError("truncated historical source member")
                    outgoing.write(chunk)
                    remaining -= len(chunk)


def backfill(catalog_path: Path, output: Path, seed_path: Path | None, measured_at: str) -> dict:
    if output.exists(): raise ValueError("historical statistics output must be new")
    catalog = read_json(catalog_path)
    if catalog.get("schema_version") != 1 or not isinstance(catalog.get("releases"), list) or not catalog["releases"]:
        raise ValueError("invalid historical statistics catalog")
    document = read_seed(seed_path)
    active_method = profile()
    output.mkdir(parents=True)
    seen = set()
    evidence = []
    for index, entry in enumerate(catalog["releases"]):
        observed_on = entry.get("observed_on")
        version = entry.get("version")
        identity = (observed_on, version)
        if identity in seen or (observed_on is None and (not isinstance(version, str) or not re.fullmatch(r"[0-9]+(?:\.[0-9A-Za-z_-]+)+", version))):
            raise ValueError("duplicate or invalid historical observation")
        seen.add(identity)
        kind = entry["source_kind"]
        if kind not in {"public-snapshot", "private-history-allowlisted"}: raise ValueError("invalid historical source kind")
        if observed_on is not None and (kind != "private-history-allowlisted" or version is not None or any(entry.get(k) for k in ("published_at", "public_url", "tag", "packages"))):
            raise ValueError("development observations accept dates and aggregates only")
        archive = Path(entry["archive"])
        if not archive.is_absolute(): archive = catalog_path.parent / archive
        if sha256_file(archive) != entry["archive_sha256"]: raise ValueError("historical source archive hash mismatch")
        rules = entry.get("allowlist")
        path_map = entry.get("path_map")
        if path_map and (kind != "private-history-allowlisted" or observed_on is None):
            raise ValueError("role mapping requires a private development observation")
        if kind == "private-history-allowlisted" and not rules: raise ValueError("private historical input requires an explicit allowlist")
        if rules is not None:
            for rule in rules: safe_relative(rule.rstrip("/"))
            if any(rule.rstrip("/") in {"docs/internal", ".agents", "out", ".local", ".git"} for rule in rules):
                raise ValueError("private metadata is outside public-equivalent history")
        destination = output / "inputs" / str(index)
        extract_selected(archive, destination, rules, path_map)
        version_path = destination / "REPO_VERSION.txt"
        if version is not None and version_path.exists() and version_path.read_text(encoding="utf-8-sig").strip() != version:
            raise ValueError("historical version differs from source")
        rows = file_records(destination)
        receipt = {"schema_version": 1, "files": rows, "tree_sha256": tree_sha256(rows),
                   "allowlist_sha256": digest({"allowlist": rules, "path_map": path_map}) if path_map else digest(rules) if rules is not None else None}
        packages = []
        for package in entry.get("packages", []):
            if package.get("path"):
                path = Path(package["path"])
                if sha256_file(path) != package["sha256"]: raise ValueError("historical package hash mismatch")
                value = measure(path, package["locale"], package["edition"])
                if value["zip_bytes"] != package["zip_bytes"]: raise ValueError("historical package size mismatch")
            else:
                value = {"status": "PARTIAL", "locale": package["locale"], "edition": package["edition"],
                         "sha256": package["sha256"], "zip_bytes": package["zip_bytes"], "unpacked_bytes": None,
                         "file_count": None, "basis": "published-asset-metadata"}
            packages.append(value)
        record, active_profile = aggregate(destination, receipt, version=version, measured_at=measured_at, source_kind=kind,
                                           published_at=entry.get("published_at"), public_url=entry.get("public_url"),
                                           tag=entry.get("tag"), packages=sorted(packages, key=lambda row: (row["locale"], row["edition"])), observed_on=observed_on)
        history = document.setdefault("development_history", []) if observed_on is not None else document["history"]
        same_identity = [row for row in history if row["release"]["version"] == version and row["measurement"].get("observed_on") == observed_on and
                         all(row["measurement"][key] == record["measurement"][key] for key in ("profile_key", "input_sha256", "source_kind"))]
        if same_identity and all(row["packages"] == record["packages"] for row in same_identity):
            continue
        record["comparison"] = compare(record, history[-1] if history else None)
        document["profiles"][digest(active_profile)] = active_profile
        history.append(record)
        validate_document(document)
        evidence.append({"version": version, "observed_on": observed_on, "archive_sha256": entry["archive_sha256"], "record_id": record["record_id"], "files": len(rows)})
    if profile() != active_method: raise ValueError("historical measurement method changed")
    write_new_json(output / "metrics.json", document)
    write_new_json(output / "backfill_receipt.json", {"schema_version": 1, "catalog_sha256": sha256_file(catalog_path), "records": evidence})
    return document


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--history", type=Path)
    parser.add_argument("--measured-at")
    args = parser.parse_args(argv)
    try:
        value = backfill(args.catalog, args.output, args.history, args.measured_at or datetime.now(timezone.utc).isoformat())
    except (OSError, ValueError, KeyError, TypeError, tarfile.TarError) as error:
        print(f"Historical statistics failed: {error}", file=sys.stderr)
        return 1
    print(f"Historical statistics: PASS, releases={len(value['history'])}, development={len(value.get('development_history', []))}; current remains unfinalized")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
