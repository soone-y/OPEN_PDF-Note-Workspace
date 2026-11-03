#!/usr/bin/env python3
"""Continue attempted corpus runs without overwriting evidence or claiming acceptance.

The caller supplies stopped result directories; source/runtimes come from their
identity files. Only absent/incomplete case records are converted in fresh batches.
Completed failures stay failures. No merged full-acceptance report is produced.
All writes belong to a new output directory; cancellation retains every artifact.
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
from collections import Counter
from datetime import datetime
from pathlib import Path
from uuid import uuid4

import libreoffice_upstream_conversion_test as upstream

corpus = upstream.corpus
expectations = upstream.expectations
clock = upstream.test_clock
GIB = 1024 ** 3


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_new(path: Path, data):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(data, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def separate(output: Path, protected: list[Path]):
    for root in protected:
        if output.is_relative_to(root) or root.is_relative_to(output):
            raise ValueError(f"Output overlaps protected input: {root}")


def free_space(parent: Path, minimum: int):
    free = shutil.disk_usage(parent).free
    if free < minimum:
        raise ValueError(f"Insufficient space: {free / GIB:.2f} GiB free; {minimum / GIB:.2f} GiB required")
    return free


def retained_file(path: Path, root: Path, digest: str, pins: dict):
    resolved = path.resolve(strict=True)
    if path.is_symlink() or not resolved.is_relative_to(root) or not resolved.is_file():
        raise ValueError(f"Retained artifact escaped its previous directory: {path}")
    if corpus.sha256(resolved) != digest:
        raise ValueError(f"Retained artifact hash changed: {path}")
    pins[str(resolved)] = digest


def plan_previous(previous: list[Path]) -> dict:
    """Audit identity/artifacts, but never turn interrupted runs into acceptance."""
    first = read_json(previous[0] / "identity.json")
    required_controls = {"input_path": "same absolute path; fresh copy per runtime",
                         "random": "SAL_RAND_REPEATABLE=1 (seed 42)",
                         "profile_configuration_root": "oor:items",
                         "conversion_scheduling": "sequential independent copies",
                         "expected_results": "pinned upstream refusals; conversion FAIL scores retained"}
    if any(first.get("controls", {}).get(key) != value for key, value in required_controls.items()):
        raise ValueError("Continuation requires the existing sequential fixed-clock comparison conditions")
    source = Path(first["source_dir"]).resolve(strict=True)
    catalog, _ = upstream.collect_samples(source, corpus.CATALOG, acceptance=True)
    known = {item["path"]: item for item in catalog["samples"]}
    expected = expectations.load_expectations(source, catalog)
    corpus.fixture_encodings(source, catalog["samples"])
    runtimes = {role: Path(first["runtimes"][role]["soffice"]).resolve(strict=True)
                for role in ("baseline", "candidate")}
    identities = {role: upstream.runtime_identity(path, full=True) for role, path in runtimes.items()}
    pins, completed, kinds, failures, incomplete = {}, set(), Counter(), [], []
    for root in previous:
        identity_path = root / "identity.json"
        identity = read_json(identity_path)
        pins[str(identity_path)] = corpus.sha256(identity_path)
        if (identity.get("report_version") != 4 or identity.get("mode") != "comparison"
                or identity.get("source_dir") != str(source)
                or identity.get("source_version") != catalog["source_version"]
                or identity.get("catalog_sha256") != corpus.sha256(corpus.CATALOG)
                or identity.get("fixture_encodings_sha256") != corpus.sha256(corpus.ENCODINGS)
                or identity.get("expectations_sha256") != corpus.sha256(expectations.DEFAULT_METADATA)
                or identity.get("runtimes") != identities
                or identity.get("controls") != first.get("controls")
                or identity.get("dpi") != 144 or identity.get("pixel_threshold") != 8):
            raise ValueError(f"Previous conditions/runtime/catalog differ: {root}")
        envelope = identity["clock_control"]
        retained_file(Path(envelope["path"]), root, envelope["sha256"], pins)
        if read_json(Path(envelope["path"])) != envelope["control"]:
            raise ValueError("Previous clock control mismatch")
        clock.audit(envelope["control"], source, catalog["source_version"])
        if envelope["control"] != first["clock_control"]["control"]:
            raise ValueError("Previous fixed clock conditions differ")
        selected = corpus.select(catalog, identity.get("selected_groups", []),
                                 identity.get("selected_individual", []), acceptance=False)
        for directory in sorted(root.glob("case_*")):
            path = directory / "result.json"
            if not path.is_file():
                incomplete.append(str(directory))
                continue
            try:
                record = read_json(path)
            except (json.JSONDecodeError, UnicodeError):
                incomplete.append(str(path))
                continue
            index = record.get("index")
            name = record.get("path")
            if (type(index) is not int or not 0 <= index < len(selected)
                    or directory.name != f"case_{index:02d}" or name != selected[index]["path"]
                    or name not in known or record.get("sha256") != known[name]["sha256"]):
                raise ValueError(f"Invalid previous result binding: {path}")
            runs = record.get("runs", {})
            if set(runs) != {"baseline", "candidate"} or any(
                    run.get("status") not in ("SUCCESS", "FAIL") for run in runs.values()):
                incomplete.append(str(path))
                continue
            pins[str(path)] = corpus.sha256(path)
            for role, run in runs.items():
                snapshot = run.get("input_snapshot")
                if snapshot:
                    retained_file(Path(snapshot), root, record["conversion_input_sha256"], pins)
                if run["status"] == "SUCCESS":
                    retained_file(Path(run["pdf"]), root, run["sha256"], pins)
                if run.get("clock_receipts"):
                    clock.check_run(run, envelope, role)
                    for receipt in run["clock_receipts"]:
                        retained_file(Path(receipt["path"]), root, receipt["sha256"], pins)
            verdict = expectations.judge(record, expected.get(name))
            if verdict.get("passed") and any(not run.get("input_snapshot") or not run.get("clock_receipts")
                                             for run in runs.values()):
                raise ValueError(f"Missing retained evidence: {path}")
            if name in completed:
                raise ValueError(f"Duplicate completed input across previous directories: {name}")
            completed.add(name)
            kinds[verdict["kind"]] += 1
            if not verdict["passed"]:
                failures.append({"path": name, "result": str(path), "validation": verdict})
    pending = [item["path"] for item in catalog["samples"] if item["path"] not in completed]
    return {"source": source, "runtimes": runtimes, "identities": identities,
            "catalog": catalog, "pins": pins, "completed": completed, "kinds": dict(kinds),
            "failures": failures, "incomplete": incomplete, "pending": pending,
            "fixed_time": first["clock_control"]["control"]["unix_time"]}


def unchanged(pins: dict):
    for name, digest in pins.items():
        if corpus.sha256(Path(name)) != digest:
            raise ValueError(f"Pinned file changed during continuation: {name}")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--previous-dir", type=Path, action="append", required=True)
    parser.add_argument("--output-parent", type=Path)
    parser.add_argument("--plan-only", action="store_true", help="Audit/count only; no conversions or output writes")
    parser.add_argument("--limit", type=int, help="Maximum pending inputs to attempt this run; omitted means all")
    parser.add_argument("--batch-size", type=int, default=25, help="1-100 inputs; check free space between batches")
    parser.add_argument("--minimum-free-gib", type=int, default=14)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args(argv)
    if not 1 <= args.batch_size <= 100 or args.minimum_free_gib < 1 or args.timeout < 1:
        parser.error("Require batch-size 1..100, positive minimum-free-gib and timeout")
    if args.limit is not None and args.limit < 1:
        parser.error("--limit must be a positive integer")
    previous = [path.resolve(strict=True) for path in args.previous_dir]
    plan = plan_previous(previous)
    pending_this_run = plan["pending"][:args.limit] if args.limit is not None else plan["pending"]
    brief = {"previous_completed": len(plan["completed"]), "previous_validation_kinds": plan["kinds"],
             "previous_validation_failures": plan["failures"], "pending": len(plan["pending"]),
             "total": len(plan["catalog"]["samples"]), "first_pending": pending_this_run[:3],
             "limit": args.limit, "selected_for_this_run": len(pending_this_run),
             "deferred_pending": len(plan["pending"]) - len(pending_this_run),
             "acceptance_passed": False}
    print(json.dumps(brief, ensure_ascii=False), flush=True)
    if args.plan_only:
        return 0
    if not plan["pending"]:
        print("No pending inputs. Existing results still require a separate acceptance audit.")
        return 1 if plan["failures"] else 0
    parent = (args.output_parent or previous[0].parent).resolve(strict=True)
    protected = [plan["source"], *previous, *(path.parent.parent for path in plan["runtimes"].values())]
    output = parent / ("pending_" + datetime.now().strftime("%Y%m%d_%H%M%S") + "_" + uuid4().hex[:8])
    separate(output, protected)
    initial_free = free_space(parent, args.minimum_free_gib * GIB)
    # Pin this code and the modules/configurations it calls. No prior-code
    # provenance is invented for the interrupted run, so acceptance stays false.
    controls = [Path(__file__), Path(upstream.__file__), Path(upstream.smoke.__file__),
                Path(upstream.quality.__file__), Path(corpus.__file__), Path(expectations.__file__),
                Path(clock.__file__), corpus.CATALOG, corpus.ENCODINGS, expectations.DEFAULT_METADATA, clock.METADATA]
    control_pins = {str(path.resolve()): corpus.sha256(path) for path in controls}
    output.mkdir(exist_ok=False)
    write_new(output / "resume_plan.json", {**brief, "previous_dirs": [str(p) for p in previous],
              "previous_artifact_sha256": plan["pins"], "control_sha256": control_pins,
              "pending_inputs": pending_this_run, "incomplete_cases": plan["incomplete"],
              "initial_free_bytes": initial_free, "minimum_free_gib": args.minimum_free_gib,
              "batch_size": args.batch_size, "workers": 1, "timeout": args.timeout,
              "fixed_time": plan["fixed_time"], "acceptance_passed": False})
    print(f"output={output}", flush=True)
    reports, converted, failed = [], 0, 0
    try:
        for offset in range(0, len(pending_this_run), args.batch_size):
            free_space(parent, args.minimum_free_gib * GIB)
            unchanged(control_pins)
            selected = pending_this_run[offset:offset + args.batch_size]
            batch = output / f"batch_{offset // args.batch_size:04d}"
            report = upstream.run_test(plan["source"], plan["runtimes"]["candidate"], batch,
                       baseline_soffice=plan["runtimes"]["baseline"], timeout=args.timeout,
                       manifest_path=corpus.CATALOG, individual=selected, acceptance=False,
                       workers=1, expectations_path=expectations.DEFAULT_METADATA,
                       fixed_time=plan["fixed_time"])
            converted += len(report["results"])
            failed += report["summary"]["validation_failures"]
            reports.append({"path": str(batch / "report.json"), "sha256": corpus.sha256(batch / "report.json"),
                            "summary": report["summary"]})
            print(f"PENDING PROGRESS {converted}/{len(pending_this_run)}; new failures={failed}", flush=True)
        upstream.collect_samples(plan["source"], corpus.CATALOG, acceptance=True)
        unchanged(control_pins)
        unchanged(plan["pins"])
        if plan["identities"] != {role: upstream.runtime_identity(path, full=True)
                                  for role, path in plan["runtimes"].items()}:
            raise ValueError("Runtime changed during continuation")
        write_new(output / "resume_summary.json", {**brief, "pending_attempted": converted,
                  "remaining_pending": len(plan["pending"]) - converted,
                  "new_validation_failures": failed, "batch_reports": reports,
                  "previous_artifacts_unchanged": True, "runtime_unchanged": True,
                  "inventory_unchanged": True, "status": "FAIL" if failed or plan["failures"] else "PASS",
                  "scope": "selected pending inputs only; interrupted previous run is not formal acceptance",
                  "acceptance_passed": False})
        return 1 if failed or plan["failures"] else 0
    except BaseException as exc:
        write_new(output / "resume_interrupted.json", {"pending_attempted_in_completed_batches": converted,
                  "limit": args.limit, "selected_for_this_run": len(pending_this_run),
                  "batch_reports": reports, "error": f"{type(exc).__name__}: {exc}",
                  "status": "INCOMPLETE", "acceptance_passed": False})
        raise


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("Interrupted; previous and new evidence retained.", file=sys.stderr)
        raise SystemExit(130)
    except Exception as exc:
        print(f"[FAIL] continuation incomplete: {exc}", file=sys.stderr)
        raise SystemExit(2)
