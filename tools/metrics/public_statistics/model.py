"""Public schema, input selection and immutable history rules."""
from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

from tools.release_checks.public_snapshot_inventory import file_records, safe_relative, tree_sha256

OUTPUTS = ("STATISTICS.md", "docs/statistics/metrics.json", "docs/statistics/HISTORY.md")
PROFILE = "public-aggregate/v1"
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".cppinc", ".ipp", ".inl"}
SCRIPT_SUFFIXES = {".py", ".ps1", ".psm1", ".js", ".ts", ".sh", ".bat", ".cmd", ".cmake"}
TEXT_SUFFIXES = SOURCE_SUFFIXES | SCRIPT_SUFFIXES | {".md", ".txt", ".json", ".xml", ".yml", ".yaml", ".toml", ".html", ".css", ".rc", ".manifest", ".ini", ".cfg", ".patch", ".diff", ".tsv", ".csv", ".clro"}
GROUPS = ("application", "tests_source", "tests_fixture", "tools", "dependencies", "dependency_patches", "documents", "resources", "samples", "metadata", "generated")


def canonical(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def read_json(path: Path) -> dict:
    if path.stat().st_size > 32 * 1024**2:
        raise ValueError("statistics JSON limit exceeded")
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate JSON key")
            result[key] = value
        return result
    return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=unique)


def digest(value: object) -> str:
    return hashlib.sha256(canonical(value)).hexdigest()


def excluded(path: str) -> bool:
    if path.startswith("docs/statistics/") and path not in OUTPUTS:
        raise ValueError("unreviewed statistics output path")
    return path in OUTPUTS or path.startswith("site/github/output/") or path in {"public_snapshot_manifest.json"}


def measured_records(rows: list[dict]) -> list[dict]:
    return [row for row in rows if not excluded(row["path"])]


def group(path: str) -> str:
    safe_relative(path)
    suffix = Path(path).suffix.lower()
    if path.startswith("src/"):
        if suffix in SOURCE_SUFFIXES or suffix == ".py":
            return "application"
        if suffix in SCRIPT_SUFFIXES:
            return "tools"
        if suffix in {".md", ".txt"}:
            return "documents"
        if suffix in {".ico", ".png", ".xcf", ".rc", ".manifest", ".json", ".xml", ".ttf", ".otf"}:
            return "resources"
        raise ValueError("unclassified application-tree input")
    if "/" not in path and suffix in SOURCE_SUFFIXES:
        return "application"
    if path.startswith("tests/"):
        return "tests_source" if suffix in SOURCE_SUFFIXES | SCRIPT_SUFFIXES else "tests_fixture"
    if path.startswith("third_party/"):
        return "dependency_patches" if "/patches/" in path or suffix in {".patch", ".diff"} else "dependencies"
    if path.startswith(("tools/", "scripts/", "site/")) or path in {"build.ps1", "release.ps1", "pack_release.ps1"}:
        return "tools"
    if path.startswith(("release_assets/", "samples/", "sample_workspace/")):
        return "samples"
    if path.startswith(("locales/", "docs/images/", "assets/")):
        return "resources"
    if path.startswith(("docs/", "introduction/", "for_ai/")) or suffix == ".md" and "/" not in path or path in {"LICENSE", "COPYING"}:
        return "documents"
    if path.startswith(".github/") or path in {".gitignore", ".gitattributes", "REPO_VERSION.txt", "VERSION.txt", "CMakeLists.txt"}:
        return "metadata"
    raise ValueError("unclassified public input")


_toolset_at_start: str | None = None


def profile() -> dict:
    global _toolset_at_start
    root = Path(__file__).resolve().parents[3]
    dependencies = [*sorted(Path(__file__).parent.glob("*.py")), root / "tools/metrics/code_metrics/analyze_repo.py",
                    root / "tools/metrics/code_structure/inventory.py", root / "tools/metrics/code_structure/literals.py",
                    root / "tools/release_checks/public_snapshot_inventory.py", root / "tools/metrics/distribution_size_report.py",
                    root / "tools/metrics/generate_public_statistics.py", root / "tools/metrics/backfill_public_statistics.py"]
    toolset = digest([{ "path": path.relative_to(root).as_posix(), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()} for path in dependencies])
    if _toolset_at_start is not None and toolset != _toolset_at_start:
        raise ValueError("statistics toolset changed during measurement")
    _toolset_at_start = toolset
    return {"id": PROFILE, "toolset_sha256": toolset,
            "classification": "public-path-groups/v2", "lines": "splitlines/v1; supported-source-classification-is-approximate",
            "lexical": "physical-utf8-byte-partition/v2; lexical-items-not-clang-tokens",
            "scope": "exported-input-minus-statistics-and-generated-pages/v1"}


def validate_record(record: dict) -> None:
    required = {"record_id", "release", "measurement", "inventory", "source_scale", "lexical", "localization", "packages", "verification", "supplemental", "comparison"}
    if not isinstance(record, dict) or set(record) != required:
        raise ValueError("invalid statistics record fields")
    development = "observed_on" in record["measurement"]
    if not development and not re.fullmatch(r"[0-9]+(?:\.[0-9A-Za-z_-]+)+", record["release"].get("version", "")):
        raise ValueError("invalid statistics version")
    expected = digest({key: value for key, value in record.items() if key not in {"record_id", "comparison"}})
    if record["record_id"] != expected:
        raise ValueError("statistics record digest mismatch")
    raw = canonical(record).decode("utf-8")
    if re.search(r'"[A-Za-z]:[\\/]|/(?:Users|home)/|\\\\|file://', raw):
        raise ValueError("private path in public statistics")
    inventory = record["inventory"]
    groups = inventory.get("groups", {})
    if set(groups) != set(GROUPS) or any(type(groups[k].get(field)) is not int or groups[k][field] < 0 for k in GROUPS for field in ("files", "size_bytes")):
        raise ValueError("invalid statistics inventory partition")
    for field in ("files", "size_bytes"):
        if sum(groups[k][field] for k in GROUPS) != inventory[field]:
            raise ValueError("statistics inventory total mismatch")
    from tools.metrics.public_statistics.schema import validate_record_fields
    validate_record_fields(record)


def validate_document(value: dict) -> None:
    base_fields = {"schema_version", "profiles", "current", "history"}
    if set(value) not in (base_fields, base_fields | {"development_history"}) or value["schema_version"] != 1:
        raise ValueError("invalid public statistics schema")
    if not isinstance(value["profiles"], dict) or not isinstance(value["history"], list):
        raise ValueError("invalid statistics history")
    development = value.get("development_history", [])
    if not isinstance(development, list): raise ValueError("invalid development history")
    dates = [row["measurement"].get("observed_on") for row in development]
    if any(not isinstance(day, str) for day in dates) or dates != sorted(set(dates)):
        raise ValueError("development observation dates must be unique and ordered")
    if any("observed_on" in row["measurement"] for row in value["history"] + ([value["current"]] if value["current"] else [])):
        raise ValueError("development observations must be separate from releases")
    records = ([value["current"]] if value["current"] is not None else []) + value["history"] + development
    identities = set()
    for key, item in value["profiles"].items():
        if set(item) != {"id", "toolset_sha256", "classification", "lines", "lexical", "scope"} or key != digest(item):
            raise ValueError("invalid measurement profile")
        if not all(isinstance(entry, str) and re.fullmatch(r"[A-Za-z0-9_./; =:-]+", entry) for entry in item.values()):
            raise ValueError("unreviewed measurement profile text")
    for record in records:
        validate_record(record)
        if record["record_id"] in identities:
            raise ValueError("duplicate statistics record")
        identities.add(record["record_id"])
        if record["measurement"]["profile_key"] not in value["profiles"]:
            raise ValueError("missing measurement profile")
    for record in records:
        baseline = record["comparison"]["baseline_record_id"]
        if baseline is not None and (baseline not in identities or baseline == record["record_id"]):
            raise ValueError("invalid comparison baseline")
        previous = next((row for row in value["history"] + development if row["record_id"] == baseline), None)
        if record["comparison"] != compare(record, previous):
            raise ValueError("historical comparison does not reproduce")


def read_seed(path: Path | None) -> dict:
    result = read_json(path) if path else {"schema_version": 1, "profiles": {}, "current": None, "history": []}
    validate_document(result)
    # An unconfirmed candidate is never silently promoted to release history.
    if result["current"] is not None:
        raise ValueError("history seed must have current=null; import confirmed history explicitly")
    return result


def scope_kind(record: dict) -> str:
    if "observed_on" in record["measurement"]:
        return "development-allowlisted"
    value = record["measurement"]["source_kind"]
    return "public-snapshot" if value == "release-candidate" else value


def compare(record: dict, previous: dict | None) -> dict:
    if previous is None:
        return {"status": "UNMEASURED", "reason": "no-compatible-baseline", "baseline_record_id": None}
    same = record["measurement"]["profile_key"] == previous["measurement"]["profile_key"] and scope_kind(record) == scope_kind(previous)
    if scope_kind(record) == "development-allowlisted":
        same = same and record["measurement"]["allowlist_sha256"] == previous["measurement"]["allowlist_sha256"]
    return {"status": "COMPLETE" if same else "NOT_APPLICABLE", "reason": None if same else "profile-or-source-kind-changed",
            "baseline_record_id": previous["record_id"],
            "application_lines_delta": record["source_scale"]["application"]["lines"] - previous["source_scale"]["application"]["lines"] if same else None}


def inventory_matches(root: Path, inventory: dict) -> list[dict]:
    actual = measured_records(file_records(root))
    expected = measured_records(inventory["files"])
    if actual != expected:
        raise ValueError("snapshot statistics inputs differ from exporter inventory")
    if inventory["tree_sha256"] != tree_sha256(inventory["files"]):
        raise ValueError("export inventory digest mismatch")
    return actual
