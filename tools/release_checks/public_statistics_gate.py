#!/usr/bin/env python3
"""Read-only validation of statistics, source identity and actual pair ZIPs."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.metrics.public_statistics.cli import verify
from tools.metrics.public_statistics.model import read_json
from tools.metrics.public_statistics.packages import measure
from tools.release_checks.public_snapshot_inventory import safe_relative


def release_data(root: Path) -> tuple[dict, Path, list[dict]]:
    manifest = read_json(root / "release_set_manifest.json")
    components = manifest["components"]
    snapshot = root / safe_relative(components["public_snapshot"])
    if root.resolve() not in snapshot.resolve().parents:
        raise ValueError("statistics snapshot escapes release set")
    packages = []
    for name, edition in (("release_zip", "full"), ("release_lite_zip", "lite")):
        path = root / safe_relative(components[name])
        if root.resolve() not in path.resolve().parents:
            raise ValueError("statistics package escapes release set")
        packages.append(measure(path, manifest["locale"], edition, manifest["app_version"]))
    return manifest, snapshot, packages


def validate_set(root: Path) -> list[str]:
    try:
        raw = read_json(root / "release_set_manifest.json")
        snapshot = root / safe_relative(raw["components"]["public_snapshot"])
        if root.resolve() not in snapshot.resolve().parents: raise ValueError("statistics snapshot escapes release set")
        if raw.get("statistics_schema_version") is None and not (snapshot / "STATISTICS.md").exists():
            return []  # Historical pre-statistics and explicit SnapshotOnly sets.
        if raw.get("statistics_schema_version") is None and raw["components"].get("release_zip") is None and raw["components"].get("release_lite_zip") is None:
            return []  # SnapshotOnly has no distribution or publish eligibility.
        if raw["statistics_schema_version"] != 1:
            raise ValueError("unsupported release statistics schema")
        manifest, snapshot, own_packages = release_data(root)
        verify(snapshot)
        current = read_json(snapshot / "docs/statistics/metrics.json")["current"]
        if current is None:
            raise ValueError("release statistics current record missing")
        if current["release"]["version"] != manifest["app_version"]:
            raise ValueError("release statistics version differs from release manifest")
        for row in own_packages:
            matches = [item for item in current["packages"] if (item["locale"], item["edition"]) == (row["locale"], row["edition"])]
            if matches != [row]:
                raise ValueError("release statistics do not match the locale's actual archives")
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        return ["public statistics: " + str(error)]
    return []


def validate_pair(japanese: Path, english: Path) -> list[str]:
    try:
        ja = read_json(japanese / "release_set_manifest.json")
        en = read_json(english / "release_set_manifest.json")
        if ja["locale"] != "ja" or en["locale"] != "en" or ja["app_version"] != en["app_version"]:
            raise ValueError("statistics require a same-version JA/EN pair")
        has_statistics = any((root / safe_relative(manifest["components"]["public_snapshot"]) / "STATISTICS.md").exists()
                             for root, manifest in ((japanese, ja), (english, en)))
        if ja.get("statistics_schema_version") is None and en.get("statistics_schema_version") is None and not has_statistics:
            return []
        if ja.get("statistics_schema_version") != 1 or en.get("statistics_schema_version") != 1:
            raise ValueError("both locales must use the same statistics contract")
        ja, ja_snapshot, ja_packages = release_data(japanese)
        en, en_snapshot, en_packages = release_data(english)
        packages = sorted(ja_packages + en_packages, key=lambda row: (row["locale"], row["edition"]))
        for snapshot in (ja_snapshot, en_snapshot):
            verify(snapshot, packages, ["ja", "en"])
        for relative in ("STATISTICS.md", "docs/statistics/metrics.json", "docs/statistics/HISTORY.md"):
            if (ja_snapshot / relative).read_bytes() != (en_snapshot / relative).read_bytes():
                raise ValueError("JA/EN statistics output differs")
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        return ["public statistics pair: " + str(error)]
    return []


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-set", type=Path)
    parser.add_argument("--japanese-set", type=Path)
    parser.add_argument("--english-set", type=Path)
    args = parser.parse_args(argv)
    if args.release_set is not None and args.japanese_set is None and args.english_set is None:
        errors = validate_set(args.release_set)
    elif args.release_set is None and args.japanese_set is not None and args.english_set is not None:
        errors = validate_pair(args.japanese_set, args.english_set)
    else:
        parser.error("select one release set or both locale sets")
    for error in errors: print(error, file=sys.stderr)
    if not errors: print("Public statistics gate: PASS")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
