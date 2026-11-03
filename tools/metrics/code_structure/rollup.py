"""Incremental per-profile rollup of verified batch receipts.

Consumes one TU report at a time and retains only deduplicated model rows.
Profiles are independent; source/analysis identities must agree. New output
directory only, incomplete required sets remain PARTIAL. Evidence stays in
original attempts; no deletion, replacement or automatic build is performed.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import gzip
import json
from pathlib import Path
import sys
import uuid

sys.dont_write_bytecode = True
if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
    __package__ = "tools.metrics.code_structure"

from .batch import completed_receipts, create_file, create_json, load_json, run_lock, verify_manifest
from .inventory import git_sources
from .model import AnalysisError, empty_report, stable_id, merge_file_observation
from .observations import merged_bytes, profile
from .report import markdown
from .responsibilities import derive
from .unused import unused_profile


class Accumulator:
    def __init__(self):
        self.model = empty_report()
        self.tables = {name: {} for name in ("functions", "calls", "accesses", "function_references", "files", "translation_units", "diagnostics")}
        self.configs = set()
        self.snapshots = []

    def add(self, report):
        self.configs.update(report["provenance"]["configuration_ids"])
        self.snapshots.append(report["provenance"]["source_snapshot_id"])
        for name, table in self.tables.items():
            for row in report[name]:
                if name == "files":
                    merge_file_observation(table, row)
                    continue
                identity = row["id"] if name == "functions" else stable_id(row)
                if name == "functions" and identity in table and table[identity]["body_sha256"] != row["body_sha256"]:
                    raise AnalysisError("TU definitions disagree: " + row["qualified_name"])
                table[identity] = row
        for name in ("states", "symbols", "reference_summaries"):
            self.model[name].update(report[name])

    def finish(self, rules):
        if len(self.configs) != 1:
            raise AnalysisError("profile contains zero or multiple compiler configurations")
        for name, table in self.tables.items():
            self.model[name] = list(table.values())
            table.clear()
        self.model["provenance"] = {"configuration_ids": sorted(self.configs), "local_snapshot_ids": self.snapshots}
        derive(self.model, rules)
        self.model["structure_profile"] = profile(self.model)
        self.model["unused_profile"] = unused_profile(self.model)
        return self.model


def rollup(root, directory):
    root, directory = root.resolve(), directory.resolve()
    if not directory.is_relative_to(root / "out/tests") or directory == root / "out/tests":
        raise AnalysisError("rollup must use an explicit out/tests batch directory")
    with run_lock(directory):
        frozen = load_json(directory / "run.json")
        if frozen["root"] != str(root) or stable_id({k: v for k, v in frozen.items() if k != "run_id"}) != frozen["run_id"]:
            raise AnalysisError("run identity mismatch")
        verify_manifest(root, frozen["input_manifest"])
        if git_sources(root) != frozen["selection"]:
            raise AnalysisError("frozen Git source selection changed")
        known = {x["path"]: x["sha256"] for x in frozen["input_manifest"]}
        receipts = completed_receipts(directory, frozen, root, known)
        output = directory / ("rollup_" + uuid.uuid4().hex)
        output.mkdir(exist_ok=False)
        rules = load_json(root / frozen["plan"]["roles"]) if frozen["plan"].get("roles") else {"version": 1, "contracts": [], "anchors": []}
        summaries, intervals, participants = [], defaultdict(list), set()
        for configuration in frozen["plan"]["profiles"]:
            targets = [t for t in frozen["targets"] if t["profile"] == configuration["id"]]
            available = [t for t in targets if t["id"] in receipts]
            summary = {"profile": configuration["id"], "required": len(targets), "completed": len(available),
                       "status": "COMPLETE" if len(available) == len(targets) else "PARTIAL"}
            if available:
                print(f"Rollup {configuration['id']}: {len(available)}/{len(targets)}", flush=True)
                accumulator = Accumulator()
                paths = []
                for target in available:
                    path = directory / receipts[target["id"]]["receipt_path"]
                    paths.append(str(path.relative_to(directory)))
                    report = json.loads(gzip.decompress((path.parent / "report.json.gz").read_bytes()).decode("utf-8", errors="strict"))
                    accumulator.add(report)
                    del report
                model = accumulator.finish(rules)
                model["status"] = summary["status"]
                model["metrics"].update(required_tu_configurations=len(targets), completed_tu_configurations=len(available))
                model["provenance"].update(root=str(root), run_id=frozen["run_id"], tool_hash=frozen["tool_hash"],
                                           rules_hash=frozen["rules_hash"], runtime=frozen["runtime"], receipt_paths=paths)
                participants.update(row["path"] for row in model["files"] if row["participates"])
                for function in model["functions"]:
                    body = function["body"]
                    if body["start"]["path"] == body["end"]["path"]:
                        intervals[body["start"]["path"]].append((body["start"]["offset"], body["end"]["offset"]))
                basename = stable_id(configuration["id"])[:16]
                create_file(output / (basename + ".json.gz"), gzip.compress(json.dumps(model, ensure_ascii=True, separators=(",", ":")).encode("utf-8"), compresslevel=3, mtime=0))
                create_file(output / (basename + ".md"), markdown(model).encode("utf-8"))
                summary.update(metrics=model["metrics"], report=basename + ".json.gz", markdown=basename + ".md",
                               physical_functions=sum(row["physical_definitions"] for row in model["structure_profile"]["configurations"])
                               if isinstance(model["structure_profile"].get("configurations"), list) else None)
                del accumulator, model
            summaries.append(summary)
        cpp = [row for row in frozen["inventory"]["files"] if row["language"] == "C/C++"]
        observed = sum(merged_bytes(intervals[row["path"]]) for row in cpp)
        denominator = sum(row["bytes"] for row in cpp)
        result = {"schema": "code-batch-rollup/v1", "run_id": frozen["run_id"],
                  "status": "COMPLETE" if len(receipts) == len(frozen["targets"]) else "PARTIAL",
                  "required": len(frozen["targets"]), "completed": len(receipts), "profiles": summaries,
                  "inventory": frozen["inventory"], "coverage": {"cpp_byte_denominator": denominator,
                  "observed_body_bytes": observed, "observed_body_byte_share": observed/denominator if denominator else None,
                  "observed_participating_files": len(participants),
                  "interpretation": "Physical body byte union across profiles; required TU completion is a separate count. Unsupported languages, inactive branches and dependency bodies remain outside AST scope."},
                  "input_manifest": [{"path": p, "sha256": h} for p, h in sorted(known.items())]}
        verify_manifest(root, result["input_manifest"])
        if git_sources(root) != frozen["selection"]:
            raise AnalysisError("source selection changed during rollup")
        create_json(output / "summary.json", result)
        print("Created " + str(output / "summary.json"), flush=True)
        return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--run-directory", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        root = args.root.resolve()
        result = rollup(root, root / args.run_directory)
        return 0 if result["status"] == "COMPLETE" else 2
    except (AnalysisError, OSError, ValueError, KeyError, TypeError) as exc:
        print("ROLLUP_INCOMPLETE: " + str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
