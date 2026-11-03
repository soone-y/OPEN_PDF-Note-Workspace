"""Bounded, resumable TU/configuration analysis with immutable evidence.

The CLI owns workers and one explicit out/tests run directory. A run freezes
Git-visible source selection, rules, tool, signatures and runtime before work.
Each attempt creates new files; only verified COMPLETE receipts are reused.
Crashes/timeouts leave earlier evidence intact and never complete a target.
No build, network, implicit temporary directory or source mutation is used.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, wait, FIRST_COMPLETED
from contextlib import contextmanager
import gzip
import math
import hashlib
import json
import os
from pathlib import Path
import sys
import time
import uuid

sys.dont_write_bytecode = True
if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
    __package__ = "tools.metrics.code_structure"

from .inputs import Snapshot, clang_runtime, validate_signature
from .inventory import build_inventory, git_sources
from .model import AnalysisError, SCHEMA, METHODS, digest_bytes, stable_id
from .responsibilities import validate_rules
from .survey import relative_path
from .worker import run_worker, DEADLINE_METHOD, CLOCK_METHODS, ActiveClock


def file_hash(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def create_file(path, data):
    """Exclusive create, durable close and readback. Never replace old evidence."""
    with path.open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    if file_hash(path) != digest_bytes(data):
        raise AnalysisError("evidence readback differs: " + str(path))


def create_json(path, value):
    create_file(path, json.dumps(value, ensure_ascii=True, separators=(",", ":"), sort_keys=True).encode("utf-8"))


def load_json(path):
    return json.loads(path.read_bytes().decode("utf-8-sig", errors="strict"))


@contextmanager
def run_lock(directory):
    """OS-owned nonblocking lock; process termination releases it, no stale PID."""
    path = directory / "owner.lock"
    if not path.exists():
        create_file(path, b"1")
    with path.open("r+b") as stream:
        if os.name == "nt":
            import msvcrt
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            yield
        finally:
            if os.name == "nt":
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def validate_batch_plan(plan, root, snapshot):
    if not isinstance(plan, dict) or plan.get("version") != 1 or not plan.get("profiles"):
        raise AnalysisError("batch requires version=1 and explicit profiles")
    ids = [row.get("id") for row in plan["profiles"]]
    if len(ids) != len(set(ids)) or any(not isinstance(x, str) or not x for x in ids):
        raise AnalysisError("batch profile ids must be unique nonempty strings")
    manifest = snapshot.json(root / "scripts/build/build_sources.json")
    targets = []
    for row in plan["profiles"]:
        signature_path = relative_path(row["signature"])
        signature = snapshot.json(root / signature_path)
        kind, _ = validate_signature(signature, root, snapshot)
        if any(row.get(key) != value for key, value in
               (("kind", kind), ("locale", signature["Locale"]), ("edition", signature.get("Edition")),
                ("configuration", signature["Configuration"]))):
            raise AnalysisError("explicit batch profile differs from signature: " + row["id"])
        key = "BuildSourceFiles" if kind == "workspace" else "ReadOnlyViewerSourceFiles"
        units = [x for x in manifest[key] if Path(x).suffix == ".cpp"]
        if not units or len(units) != len(set(units)):
            raise AnalysisError("required C++ TU set is empty or duplicated")
        for source in units:
            relative_path(source)
            snapshot.capture(root / source, text=True)
            targets.append({"id": stable_id([row["id"], source]), "profile": row["id"],
                            "source": source, "signature": signature_path, "build_profile":
                            {"kind": kind, "locale": row["locale"], "edition": row.get("edition"), "configuration": row["configuration"]}})
    scopes = plan.get("scopes")
    if not isinstance(scopes, list) or not scopes:
        raise AnalysisError("batch requires explicit source scopes")
    for path in scopes:
        relative_path(path)
        if not (root / path).exists():
            raise AnalysisError("missing batch scope: " + path)
    if plan.get("roles"):
        relative_path(plan["roles"])
    return targets


def freeze(root, plan_path, clang, library):
    snapshot = Snapshot(root)
    plan = snapshot.json(plan_path)
    targets = validate_batch_plan(plan, root, snapshot)
    rules = snapshot.json(root / plan["roles"]) if plan.get("roles") else {"version": 1, "contracts": [], "anchors": []}
    validate_rules(rules)
    tools = [{"path": p.name, "sha256": digest_bytes(snapshot.capture(p))} for p in sorted(Path(__file__).parent.glob("*.py"))]
    _, runtime = clang_runtime(clang, library, root, snapshot)
    selection = git_sources(root)
    inventory = build_inventory(root, selection, plan.get("source_groups", []), snapshot)
    snapshot.verify()
    if git_sources(root) != selection:
        raise AnalysisError("source selection changed during freeze")
    data = {"schema": "code-batch/v1", "root": str(root), "plan_path": str(plan_path), "plan": plan,
            "execution_contract": DEADLINE_METHOD,
            "targets": targets, "inventory": inventory, "selection": selection,
            "input_manifest": snapshot.manifest(), "runtime": runtime, "tool_hash": stable_id(tools),
            "rules_hash": stable_id(rules)}
    data["run_id"] = stable_id(data)
    return data


def verify_manifest(root, manifest):
    for row in manifest:
        if file_hash(root / row["path"]) != row["sha256"]:
            raise AnalysisError("frozen input changed: " + row["path"])


def verify_report(report, target, frozen, root, known_inputs):
    if report.get("schema") != SCHEMA or report.get("methods") != METHODS:
        raise AnalysisError("worker schema/method differs")
    metrics = report.get("metrics", {})
    if metrics.get("required_tu_configurations") != 1 or metrics.get("completed_tu_configurations") != 1:
        raise AnalysisError("worker required/completed TU counts differ")
    units = report.get("translation_units", [])
    provenance = report.get("provenance", {})
    if report.get("status") != "COMPLETE" or len(units) != 1 or units[0].get("status") != "COMPLETE" or units[0].get("file") != target["source"]:
        raise AnalysisError("required TU did not complete: " + target["source"])
    if units[0]["command"].get("build_profile") != target["build_profile"]:
        raise AnalysisError("worker build profile differs from frozen target")
    for key in ("tool_hash", "rules_hash", "runtime"):
        if provenance.get(key) != frozen[key]:
            raise AnalysisError("worker analysis identity differs: " + key)
    if provenance.get("selected_tus") != [target["source"]] or provenance.get("scopes") != frozen["plan"]["scopes"]:
        raise AnalysisError("worker scope/TU selection differs")
    inputs = provenance.get("input_manifest")
    if not isinstance(inputs, list) or not inputs:
        raise AnalysisError("worker input manifest missing")
    for row in inputs:
        previous = known_inputs.get(row["path"])
        if previous is not None and previous != row["sha256"]:
            raise AnalysisError("input differs between TUs: " + row["path"])
    verify_manifest(root, inputs)
    known_inputs.update({row["path"]: row["sha256"] for row in inputs})


def verify_execution_deadline(started, timing, resources):
    timeout = started.get("timeout_seconds")
    elapsed = timing.get("elapsed_seconds")
    active = timing.get("active_elapsed_seconds")
    worker_elapsed = resources.get("elapsed_seconds")
    worker_active = resources.get("active_elapsed_seconds")
    for name, value in (("timeout", timeout), ("target elapsed", elapsed), ("worker elapsed", worker_elapsed),
                        ("target active", active), ("worker active", worker_active)):
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
            raise AnalysisError("invalid execution deadline evidence: " + name)
    if timeout <= 0 or active >= timeout or worker_active >= timeout:
        raise AnalysisError("execution timeout exceeded; receipt cannot be complete")
    if (started.get("deadline_method") != DEADLINE_METHOD or resources.get("deadline_method") != DEADLINE_METHOD or timing.get("deadline_method") != DEADLINE_METHOD
            or resources.get("timeout_seconds") != timeout):
        raise AnalysisError("execution deadline contract differs or is missing")
    method = started.get("active_clock_method")
    if method not in CLOCK_METHODS or resources.get("active_clock_method") != method or timing.get("active_clock_method") != method:
        raise AnalysisError("active clock evidence differs or is missing")
    # Independent clocks can straddle a coarse Windows clock tick. This is a
    # consistency tolerance only; it never extends the active timeout budget.
    if worker_elapsed > elapsed + 0.05 or worker_active > active + 0.05 or active > elapsed + 0.05 or worker_active > worker_elapsed + 0.05:
        raise AnalysisError("active/wall execution measurements disagree")


def completed_receipts(directory, frozen, root, known_inputs):
    if frozen.get("execution_contract") != DEADLINE_METHOD:
        raise AnalysisError("frozen execution deadline contract differs or is missing")
    targets = {x["id"]: x for x in frozen["targets"]}
    result = {}
    for path in sorted(directory.glob("targets/*/attempt_*/receipt.json")):
        receipt = load_json(path)
        identity = receipt.get("target_id")
        if receipt.get("run_id") != frozen["run_id"] or identity not in targets:
            raise AnalysisError("receipt belongs to another run/target: " + str(path))
        started = load_json(path.parent / "started.json")
        resources = load_json(path.parent / "resources.json")
        timing = load_json(path.parent / "timing.json")
        if started.get("run_id") != frozen["run_id"] or started.get("target") != targets[identity]:
            raise AnalysisError("started evidence belongs to another run/target")
        if receipt.get("resources") != resources:
            raise AnalysisError("receipt resources differ from execution evidence")
        if receipt.get("elapsed_seconds") != timing.get("elapsed_seconds") or receipt.get("active_elapsed_seconds") != timing.get("active_elapsed_seconds"):
            raise AnalysisError("receipt timing differs from execution evidence")
        verify_execution_deadline(started, timing, resources)
        report_path = path.parent / "report.json.gz"
        if file_hash(report_path) != receipt["report_sha256"]:
            raise AnalysisError("saved report hash mismatch: " + str(report_path))
        report = json.loads(gzip.decompress(report_path.read_bytes()).decode("utf-8", errors="strict"))
        verify_report(report, targets[identity], frozen, root, known_inputs)
        if receipt.get("status") != "COMPLETE":
            raise AnalysisError("invalid complete receipt status")
        result[identity] = {**receipt, "receipt_path": str(path.relative_to(directory))}
    return result


def execute_target(target, frozen, root, directory, timeout):
    attempt = directory / "targets" / target["id"] / ("attempt_" + uuid.uuid4().hex)
    attempt.mkdir(parents=True, exist_ok=False)
    command = [sys.executable, "-B", str(Path(__file__).with_name("analyze.py")), "--_worker", "--root", str(root),
               "--clang", frozen["runtime"]["driver"], "--libclang", frozen["runtime"]["library"],
               "--build-signature", target["signature"], "--tu", target["source"]]
    for scope in frozen["plan"]["scopes"]:
        command += ["--scope", scope]
    if frozen["plan"].get("roles"):
        command += ["--roles", frozen["plan"]["roles"]]
    start = time.monotonic()
    clock = None
    try:
        clock = ActiveClock()
        active_start = clock.now()
        started = {"target": target, "run_id": frozen["run_id"], "parent_pid": os.getpid(),
                   "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "timeout_seconds": timeout,
                   "active_clock_method": clock.method, "deadline_method": DEADLINE_METHOD}
        create_json(attempt / "started.json", started)
        worker = run_worker(command, cwd=root, timeout=timeout)
        create_file(attempt / "stderr.txt", worker.stderr.encode("utf-8"))
        create_json(attempt / "resources.json", worker.resources)
        # Preserve failed payloads as well; compression limits disk cost, no loss of evidence.
        create_file(attempt / "report.json.gz", gzip.compress(worker.stdout.encode("utf-8"), compresslevel=3, mtime=0))
        report = json.loads(worker.stdout)
        active_elapsed = clock.now() - active_start
        elapsed = time.monotonic() - start
        timing = {"elapsed_seconds": elapsed, "active_elapsed_seconds": active_elapsed,
                  "excluded_inactive_seconds": max(0.0, elapsed-active_elapsed),
                  "active_clock_method": clock.method, "deadline_method": DEADLINE_METHOD}
        create_json(attempt / "timing.json", timing)
        verify_execution_deadline(started, timing, worker.resources)
        if worker.returncode != 0:
            raise AnalysisError("worker exit " + str(worker.returncode) + ": " + json.dumps(report.get("diagnostics", [])[:3]))
        return target, attempt, report, timing, None
    except (AnalysisError, OSError, ValueError, KeyError) as exc:
        failed_output = getattr(exc, "worker_output", None)
        if failed_output is not None:
            create_file(attempt / "stderr.txt", failed_output.stderr.encode("utf-8"))
            create_json(attempt / "resources.json", failed_output.resources)
            create_file(attempt / "report.json.gz", gzip.compress(failed_output.stdout.encode("utf-8"), compresslevel=3, mtime=0))
        create_json(attempt / "failure.json", {"status": "FAILED", "message": str(exc), "elapsed_seconds": time.monotonic() - start})
        return target, attempt, None, {"elapsed_seconds": time.monotonic() - start}, str(exc)


def run_batch(root, plan_path, directory, *, resume=False, clang="clang++", library=None, workers=2, timeout=1200, profiles=None, units=None):
    root, directory = root.resolve(), directory.resolve()
    if directory == root / "out/tests" or not directory.is_relative_to(root / "out/tests"):
        raise AnalysisError("batch directory must be explicitly selected under out/tests")
    if workers not in {1, 2} or timeout < 1:
        raise AnalysisError("batch supports one or two workers and positive timeout")
    if resume:
        if not directory.is_dir():
            raise AnalysisError("resume directory missing")
    else:
        directory.mkdir(parents=True, exist_ok=False)
    with run_lock(directory):
        if resume:
            frozen = load_json(directory / "run.json")
            if frozen["root"] != str(root) or frozen["plan_path"] != str(plan_path.resolve()):
                raise AnalysisError("resume root/plan differs")
            if stable_id({k: v for k, v in frozen.items() if k != "run_id"}) != frozen["run_id"]:
                raise AnalysisError("frozen run metadata changed")
        else:
            frozen = freeze(root, plan_path.resolve(), clang, library)
            create_json(directory / "run.json", frozen)
        verify_manifest(root, frozen["input_manifest"])
        if git_sources(root) != frozen["selection"]:
            raise AnalysisError("frozen Git-visible source set changed")
        known = {x["path"]: x["sha256"] for x in frozen["input_manifest"]}
        completed = completed_receipts(directory, frozen, root, known)
        selected = [x for x in frozen["targets"] if (not profiles or x["profile"] in profiles) and (not units or x["source"] in units)]
        if not selected or (profiles and set(profiles) - {x["profile"] for x in frozen["targets"]}) or (units and set(units) - {x["source"] for x in frozen["targets"]}):
            raise AnalysisError("requested batch selection missing or empty")
        pending = [x for x in selected if x["id"] not in completed]
        errors = []
        print(f"Required {len(frozen['targets'])}; verified {len(completed)}; selected pending {len(pending)}; workers {workers}", flush=True)
        with ThreadPoolExecutor(max_workers=workers) as executor:
            queue = iter(pending)
            futures = set()

            def submit_next():
                target = next(queue, None)
                if target:
                    futures.add(executor.submit(execute_target, target, frozen, root, directory, timeout))

            for _ in range(workers):
                submit_next()
            while futures:
                finished, _ = wait(futures, return_when=FIRST_COMPLETED)
                future = next(iter(finished))
                futures.remove(future)
                target, attempt, report, timing, error = future.result()
                try:
                    if error:
                        raise AnalysisError(error)
                    verify_report(report, target, frozen, root, known)
                    receipt = {"run_id": frozen["run_id"], "target_id": target["id"], "profile": target["profile"],
                               "source": target["source"], "status": "COMPLETE", "elapsed_seconds": timing["elapsed_seconds"],
                               "active_elapsed_seconds": timing["active_elapsed_seconds"],
                               "report_sha256": file_hash(attempt / "report.json.gz"), "metrics": report["metrics"]}
                    receipt["resources"] = load_json(attempt / "resources.json")
                    create_json(attempt / "receipt.json", receipt)
                    completed[target["id"]] = {**receipt, "receipt_path": str((attempt / "receipt.json").relative_to(directory))}
                    print(f"COMPLETE {len(completed)}/{len(frozen['targets'])} {target['profile']} {target['source']} active {timing['active_elapsed_seconds']:.1f}s; elapsed {timing['elapsed_seconds']:.1f}s", flush=True)
                except (AnalysisError, OSError, KeyError) as exc:
                    errors.append({"target": target, "message": str(exc), "attempt": str(attempt.relative_to(directory))})
                    create_json(attempt / ("validation_failure_" + uuid.uuid4().hex + ".json"), errors[-1])
                    print("FAILED " + target["profile"] + " " + target["source"] + " " + str(exc)[:1000], flush=True)
                del report
                del future
                submit_next()
        verify_manifest(root, [{"path": p, "sha256": h} for p, h in known.items()])
        if git_sources(root) != frozen["selection"]:
            raise AnalysisError("source selection changed during batch")
        complete = len(completed) == len(frozen["targets"]) and not errors
        result = {"schema": "code-batch-execution/v1", "run_id": frozen["run_id"], "status": "COMPLETE" if complete else "PARTIAL",
                  "required": len(frozen["targets"]), "completed": len(completed), "selected": len(selected),
                  "selected_status": "COMPLETE" if all(x["id"] in completed for x in selected) and not errors else "FAILED",
                  "receipts": list(completed.values()), "errors": errors,
                  "interpretation": "TU/configuration execution completion only; structural assessment and review remain separate."}
        create_json(directory / ("execution_" + uuid.uuid4().hex + ".json"), result)
        return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--run-directory", type=Path, required=True)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--clang", default="clang++")
    parser.add_argument("--libclang")
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=1200)
    parser.add_argument("--profile", action="append")
    parser.add_argument("--tu", action="append")
    args = parser.parse_args(argv)
    try:
        root = args.root.resolve()
        result = run_batch(root, (root / args.plan).resolve(), root / args.run_directory, resume=args.resume,
                           clang=args.clang, library=args.libclang, workers=args.workers, timeout=args.timeout,
                           profiles=args.profile, units=args.tu)
        print(f"BATCH_{result['status']} {result['completed']}/{result['required']}; selection {result['selected_status']}", flush=True)
        return 0 if result["status"] == "COMPLETE" else 2
    except (AnalysisError, OSError, ValueError, KeyError, TypeError) as exc:
        print("BATCH_INCOMPLETE: " + str(exc), file=sys.stderr, flush=True)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
