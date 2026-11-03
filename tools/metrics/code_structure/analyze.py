"""Local C++ analysis CLI; default stdout, opt-in new-file reports only."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
    __package__ = "tools.metrics.code_structure"

from .clang_frontend import Frontend
from .compare import compare
from .inputs import Snapshot, SOURCE_SUFFIXES, clang_runtime, database_commands, signature_commands
from .model import AnalysisError, digest_bytes, display_path, empty_report, stable_id
from .report import json_report, markdown, save_outputs
from .responsibilities import derive, validate_rules
from .observations import profile
from .unused import unused_profile
from .worker import run_worker, wait_start
from .body_presence import validate_contracts, verify_declaration_only


def analyze(root: Path, selected: list[str], scopes: list[Path], *, database=None,
            signature=None, rules_path=None, clang="clang++", library=None):
    result = empty_report()
    snapshot = Snapshot(root)
    try:
        tool_manifest = [{"path": path.name, "sha256": digest_bytes(snapshot.capture(path))}
                         for path in sorted(Path(__file__).parent.glob("*.py"))]
        if not selected or not scopes:
            raise AnalysisError("at least one required TU and source scope must be selected")
        if bool(database) == bool(signature):
            raise AnalysisError("select exactly one compilation database or build signature")
        inventory = set()
        for scope in scopes:
            if not scope.exists():
                raise AnalysisError(f"missing source scope: {scope}")
            sources = scope.rglob("*") if scope.is_dir() else [scope]
            for source in sources:
                if source.is_file() and source.suffix.lower() in SOURCE_SUFFIXES:
                    snapshot.capture(source, text=True)
                    inventory.add(source.resolve())
        rules = snapshot.json(rules_path) if rules_path else {"version": 1, "contracts": [], "anchors": []}
        validate_rules(rules)
        presence_path = root / "tools/metrics/code_structure/body_presence.json"
        presence = validate_contracts(snapshot.json(presence_path) if presence_path.exists()
                                      else {"version": 1, "declaration_only_units": []})
        index, runtime = clang_runtime(clang, library, root, snapshot)
        commands = database_commands(database, selected, root, snapshot) if database else signature_commands(signature, selected, root, snapshot, runtime)
        participates, configurations = set(), set()
        completed = 0
        for command in commands:
            command["runtime_id"] = stable_id(runtime)
            frontend = Frontend(index, command, root, scopes, snapshot)
            configurations.add(frontend.config_id)
            unit = {"file": display_path(command["file"], root), "tu": frontend.tu_id,
                    "config": frontend.config_id, "command": command, "status": "FAILED"}
            try:
                summaries = frontend.parse()
                if not frontend.functions:
                    unit["body_presence"] = verify_declaration_only(frontend, snapshot, presence)
                else:
                    unit["body_presence"] = {"kind": "selected-function-bodies", "count": len(frontend.functions)}
                result["reference_summaries"].update(summaries)
                result["symbols"].update(frontend.symbols)
                result["states"].update(frontend.states)
                result["functions"].extend(frontend.functions)
                result["calls"].extend(frontend.calls)
                result["accesses"].extend(frontend.accesses)
                result["function_references"].extend(frontend.function_references)
                participates |= frontend.included_files
                unit["status"] = "COMPLETE"; completed += 1
            except Exception as exc:
                result["diagnostics"].append({"severity": "error", "message": f"{type(exc).__name__}: {exc}", "tu": frontend.tu_id})
            result["diagnostics"].extend(frontend.diagnostics)
            result["translation_units"].append(unit)
        # Deduplicate repeated external inline source definitions, preserving TU observations elsewhere.
        result["functions"] = list({row["id"]: row for row in result["functions"]}.values())
        result["functions"].sort(key=lambda row: (row["file"], row["evidence"]["start"]["offset"], row["id"]))
        result["files"] = [{"path": display_path(path, root), "participates": str(path) in participates,
                            "sha256": digest_bytes(snapshot.buffers[str(path)])} for path in sorted(inventory)]
        derive(result, rules)
        result["structure_profile"] = profile(result)
        result["unused_profile"] = unused_profile(result)
        result["metrics"].update(required_tu_configurations=len(commands), completed_tu_configurations=completed,
                                  tu_completion_rate=completed/len(commands) if commands else None)
        snapshot.verify()
        manifest = snapshot.manifest()
        git = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True,
                             timeout=10, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        dirty = subprocess.run(["git", "-C", str(root), "status", "--porcelain", "--", *[str(p) for p in scopes]],
                               capture_output=True, timeout=10, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        worktree = subprocess.run(["git", "-C", str(root), "status", "--porcelain"], capture_output=True,
                                  timeout=10, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        commit_text = git.stdout.decode("utf-8", errors="strict")
        result["provenance"] = {"root": str(root), "scopes": [display_path(path, root) for path in scopes],
                                 "selected_tus": selected, "configuration_ids": sorted(configurations),
                                 "source_snapshot_id": stable_id(manifest), "input_manifest": manifest,
                                 "tool_hash": stable_id(tool_manifest), "tool_manifest": tool_manifest,
                                 "rules_hash": stable_id(rules), "runtime": runtime,
                                 "commit": commit_text.strip() if git.returncode == 0 else None,
                                 "source_dirty": bool(dirty.stdout) if dirty.returncode == 0 else None,
                                 "worktree_dirty": bool(worktree.stdout) if worktree.returncode == 0 else None,
                                 "snapshot_method": "fixed-buffer dependency parse plus before/after verification"}
        result["status"] = "COMPLETE" if completed == len(commands) else "PARTIAL" if completed else "FAILED"
    except Exception as exc:
        result["diagnostics"].append({"severity": "error", "message": f"{type(exc).__name__}: {exc}"})
        result["status"] = "PARTIAL" if result["functions"] else "FAILED"
    return result, snapshot


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--compilation-database", type=Path)
    source.add_argument("--build-signature", type=Path)
    parser.add_argument("--tu", action="append", required=True)
    parser.add_argument("--scope", action="append", required=True)
    parser.add_argument("--roles", type=Path)
    parser.add_argument("--clang", default="clang++")
    parser.add_argument("--libclang")
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--json-report", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--timeout", type=int, default=600, help="Owned analysis worker timeout in seconds (default: 600)")
    parser.add_argument("--_worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--_start-event", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    root = args.root.resolve()
    resolve = lambda path: (root / path).resolve() if path is not None else None
    outputs = [resolve(path) for path in (args.report, args.json_report) if path is not None]
    try:
        if len(set(outputs)) != len(outputs) or any(path.exists() for path in outputs):
            raise AnalysisError("output already exists or paths collide")
        if args.timeout < 1:
            raise AnalysisError("timeout must be a positive number of seconds")
        if args._worker:
            wait_start(args._start_event)
            result, _ = analyze(root, args.tu, [resolve(Path(path)) for path in args.scope],
                                database=resolve(args.compilation_database), signature=resolve(args.build_signature),
                                rules_path=resolve(args.roles), clang=args.clang, library=args.libclang)
            print(json_report(result), end="")
            return 0 if result["status"] == "COMPLETE" else 2
        # Parent owns exactly this worker. Timeout kills/reaps it; partial results never become success.
        worker_args = [sys.executable, "-B", str(Path(__file__).resolve()), "--_worker", "--root", str(root),
                       "--clang", args.clang, "--timeout", str(args.timeout)]
        for unit in args.tu:
            worker_args += ["--tu", unit]
        for scope in args.scope:
            worker_args += ["--scope", scope]
        for flag, value in (("--compilation-database", args.compilation_database), ("--build-signature", args.build_signature),
                            ("--roles", args.roles), ("--libclang", args.libclang)):
            if value is not None:
                worker_args += [flag, str(value)]
        worker = run_worker(worker_args, cwd=root, timeout=args.timeout)
        if worker.stderr:
            print(worker.stderr, end="", file=sys.stderr)
        try:
            result = json.loads(worker.stdout)
        except ValueError as exc:
            raise AnalysisError(f"analysis worker returned no valid result (exit {worker.returncode})") from exc
        if worker.returncode != 0 and result.get("status") == "COMPLETE":
            raise AnalysisError(f"analysis worker failed despite complete payload (exit {worker.returncode})")
        result["worker_execution"] = worker.resources
        snapshot = Snapshot(root)
        for item in result.get("provenance", {}).get("input_manifest", []):
            content = snapshot.capture(resolve(Path(item["path"])))
            if digest_bytes(content) != item["sha256"]:
                raise AnalysisError(f"input changed after worker completion: {item['path']}")
        if args.baseline:
            baseline = snapshot.json(resolve(args.baseline))
            result["comparison"] = compare(baseline, result)
        snapshot.verify()
        output_data = []
        if args.report:
            output_data.append((resolve(args.report), markdown(result) if args.format == "markdown" else json_report(result)))
        if args.json_report:
            output_data.append((resolve(args.json_report), json_report(result)))
        if output_data:
            receipts = save_outputs(output_data, protected={Path(path) for path in snapshot.buffers})
            for receipt in receipts:
                print("Created " + receipt["path"], file=sys.stderr)
            print("ANALYSIS_COMPLETE" if result["status"] == "COMPLETE" else "ANALYSIS_INCOMPLETE", file=sys.stderr)
        else:
            print(markdown(result) if args.format == "markdown" else json_report(result), end="")
        return 0 if result["status"] == "COMPLETE" else 2
    except (AnalysisError, OSError, ValueError, KeyError) as exc:
        print(f"ANALYSIS_INCOMPLETE: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
