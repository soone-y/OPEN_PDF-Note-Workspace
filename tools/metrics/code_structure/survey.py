"""Analyze several explicit local areas, then inventory and cross-area structure.

Owns bounded analysis workers. No build/download, no temporary IPC or implicit
report writes. On failure retain diagnostics and partial evidence, return 2.
The whole source inventory is never presented as whole-repository AST coverage.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
    __package__ = "tools.metrics.code_structure"

from .inputs import Snapshot
from .inventory import build_inventory, git_sources
from .model import AnalysisError, SCHEMA, empty_report, stable_id, digest_bytes, merge_file_observation
from .observations import merged_bytes, profile
from .report import json_report, markdown, save_outputs, source_composition_markdown
from .responsibilities import derive
from .worker import run_worker
from .unused import unused_profile


def relative_path(value):
    if not isinstance(value, str) or not value or "\\" in value or Path(value).is_absolute() or ".." in Path(value).parts or ":" in value:
        raise AnalysisError("plan paths must be repository-relative forward-slash paths")
    return value.rstrip("/")


def validate_plan(plan):
    if not isinstance(plan, dict) or plan.get("version") != 1:
        raise AnalysisError("survey plan requires version=1")
    locals_ = plan.get("locals", [])
    if len(locals_) < 2:
        raise AnalysisError("survey requires at least two explicit local areas before whole inventory")
    groups = plan.get("source_groups", [])
    for collection in (locals_, groups):
        ids = [row.get("id") for row in collection]
        if any(not isinstance(x, str) or not x for x in ids) or len(set(ids)) != len(ids):
            raise AnalysisError("plan ids must be nonempty and unique")
    for row in groups:
        relative_path(row["path"])
        if not isinstance(row.get("purpose"), str) or not row["purpose"]:
            raise AnalysisError("source group requires an explicit purpose label")
    if len({row["path"] for row in groups}) != len(groups):
        raise AnalysisError("duplicate source group path")
    for row in locals_:
        for field in ("translation_units", "scopes"):
            if not isinstance(row.get(field), list) or not row[field] or len(set(row[field])) != len(row[field]):
                raise AnalysisError("local area requires unique nonempty TU and scope lists")
            for value in row[field]:
                relative_path(value)
    if plan.get("roles"):
        relative_path(plan["roles"])
    return plan


def combine(reports, rules):
    if any(row.get("schema") != SCHEMA for row in reports):
        raise AnalysisError("local schema mismatch")
    for key in ("methods",):
        if any(row[key] != reports[0][key] for row in reports):
            raise AnalysisError("local method mismatch")
    for key in ("tool_hash", "rules_hash", "runtime"):
        if any(row["provenance"][key] != reports[0]["provenance"][key] for row in reports):
            raise AnalysisError("local analysis identity mismatch: " + key)
    merged = empty_report()
    for name in ("functions", "calls", "accesses", "function_references", "files", "translation_units", "diagnostics"):
        rows = [row for report in reports for row in report[name]]
        if name == "files":
            by_path = {}
            for row in rows:
                merge_file_observation(by_path, row)
            merged[name] = list(by_path.values())
        elif name == "functions":
            by_id = {}
            for row in rows:
                if row["id"] in by_id and row["body_sha256"] != by_id[row["id"]]["body_sha256"]:
                    raise AnalysisError("local definitions disagree")
                by_id[row["id"]] = row
            merged[name] = list(by_id.values())
        else:
            merged[name] = list({stable_id(row): row for row in rows}.values())
    for name in ("states", "symbols", "reference_summaries"):
        for report in reports:
            merged[name].update(report[name])
    merged["provenance"] = {"local_snapshot_ids": [row["provenance"]["source_snapshot_id"] for row in reports]}
    derive(merged, rules)
    merged["structure_profile"] = profile(merged)
    merged["unused_profile"] = unused_profile(merged)
    merged["status"] = "COMPLETE" if all(row["status"] == "COMPLETE" for row in reports) else "PARTIAL"
    return merged


def coverage(inventory, reports):
    intervals = defaultdict(list)
    participating, selected = set(), set()
    for report in reports:
        participating |= {row["path"] for row in report["files"] if row["participates"]}
        selected |= {row["file"] for row in report["translation_units"] if row["status"] == "COMPLETE"}
        for function in report["functions"]:
            body = function["body"]
            if body["start"]["path"] == body["end"]["path"]:
                intervals[body["start"]["path"]].append((body["start"]["offset"], body["end"]["offset"]))
    rows = []
    for file in inventory["files"]:
        if file["language"] != "C/C++":
            continue
        covered = merged_bytes(intervals[file["path"]])
        if covered > file["bytes"]:
            raise AnalysisError("body coverage exceeds physical source bytes")
        rows.append({"path": file["path"], "bytes": file["bytes"], "observed_body_bytes": covered,
                     "observed_participation": file["path"] in participating,
                     "selected_completed_tu": file["path"] in selected})
    denominator = sum(row["bytes"] for row in rows)
    bodies = sum(row["observed_body_bytes"] for row in rows)
    # Even 100% body coverage cannot prove all preprocessing configurations/TUs.
    return {"whole_ast_status": "PARTIAL" if bodies else "UNMEASURED", "files": rows,
            "cpp_file_denominator": len(rows), "cpp_byte_denominator": denominator,
            "observed_body_bytes": bodies, "observed_body_byte_share": bodies/denominator if denominator else None,
            "observed_participating_files": sum(row["observed_participation"] for row in rows),
            "selected_completed_tus": len(selected),
            "interpretation": "Union of observed function-body physical byte ranges across selected locals/configurations; not semantic analysis coverage or whole-build acceptance."}


def survey(root, plan_path, *, signature=None, database=None, clang="clang++", library=None, timeout=600):
    snapshot = Snapshot(root)
    plan = validate_plan(snapshot.json(plan_path))
    if bool(signature) == bool(database):
        raise AnalysisError("select exactly one build signature or compilation database")
    selection = git_sources(root)
    reports, stages, errors = [], [], []
    roles_path = root / plan["roles"] if plan.get("roles") else None
    rules = snapshot.json(roles_path) if roles_path else {"version": 1, "contracts": [], "anchors": []}
    for local in plan["locals"]:
        print("Local analysis: " + local["id"], file=sys.stderr, flush=True)
        command = [sys.executable, "-B", str(Path(__file__).with_name("analyze.py")), "--_worker", "--root", str(root), "--clang", clang]
        command += ["--build-signature", str(signature)] if signature else ["--compilation-database", str(database)]
        if roles_path:
            command += ["--roles", str(roles_path)]
        if library:
            command += ["--libclang", library]
        for unit in local["translation_units"]:
            command += ["--tu", unit]
        for scope in local["scopes"]:
            command += ["--scope", scope]
        try:
            worker = run_worker(command, cwd=root, timeout=timeout)
            report = json.loads(worker.stdout)
            if worker.returncode != 0 or report.get("status") != "COMPLETE":
                errors.append({"local": local["id"], "message": "required local analysis incomplete", "diagnostics": report.get("diagnostics", []), "stderr": worker.stderr})
            for item in report.get("provenance", {}).get("input_manifest", []):
                data = snapshot.capture(root / item["path"])
                if digest_bytes(data) != item["sha256"]:
                    raise AnalysisError("input changed between local stages: " + item["path"])
            if all(key in report.get("provenance", {}) for key in ("input_manifest", "tool_hash", "rules_hash", "runtime")):
                reports.append(report)
            stages.append({"local": local["id"], "status": report["status"], "report": report})
        except (AnalysisError, OSError, ValueError, KeyError) as exc:
            errors.append({"local": local["id"], "message": str(exc)})
            stages.append({"local": local["id"], "status": "FAILED"})
    print("Whole source inventory and cross-area observations", file=sys.stderr, flush=True)
    inventory = build_inventory(root, selection, plan.get("source_groups", []), snapshot)
    result = {"schema": "code-survey/v1", "status": "PARTIAL" if errors else "COMPLETE", "plan": plan,
              "local_stages": stages, "inventory": inventory, "coverage": coverage(inventory, reports), "errors": errors,
              "cross_area": combine(reports, rules) if reports else None}
    if errors and result["cross_area"]:
        result["cross_area"]["status"] = "PARTIAL"
    if git_sources(root) != selection:
        raise AnalysisError("Git-visible source selection changed during survey")
    snapshot.verify()
    result["provenance"] = {"root": str(root), "input_manifest": snapshot.manifest(), "source_snapshot_id": stable_id(snapshot.manifest())}
    return result, snapshot


def survey_markdown(data):
    lines = ["# 複数局所・全体ソース検査", "", f"選択した検査の完了: {data['status']}",
             f"全体ASTの状態: {data['coverage']['whole_ast_status']}（局所結果による観測範囲）", "",
             "## 局所の検査", "", "| 局所 | 完了 | 関数 | 分類済み |", "| --- | --- | ---: | ---: |"]
    for stage in data["local_stages"]:
        metrics = stage.get("report", {}).get("metrics", {})
        lines.append(f"| {stage['local']} | {stage['status']} | {metrics.get('functions', 0)} | {metrics.get('classified_functions', 0)} |")
    lines += ["", "## ソース構成割合", "", "分母はGit可視の物理ソース・設定ファイル。byte割合は実装量・保守負担・実行時間の割合ではない。", ""]
    for key, rows in data["inventory"]["breakdowns"].items():
        lines += [f"### {key}", "", "| 区分 | ファイル | bytes | byte割合 |", "| --- | ---: | ---: | ---: |"]
        for row in rows:
            share = f"{row['byte_share']:.2%}" if row["byte_share"] is not None else "unmeasured"
            lines.append(f"| {row['name']} | {row['files']} | {row['bytes']} | {share} |")
    summary = {key: value for key, value in data["coverage"].items() if key != "files"}
    lines += ["", "## 観測範囲", "", "```json", json.dumps(summary, indent=2, ensure_ascii=True), "```",
              "", "## C/C++物理byte内訳", "", "| 区分 | bytes | 分母に対する割合 |", "| --- | ---: | ---: |"]
    denominator = summary["cpp_byte_denominator"]
    for category in ("code", "literal", "comment", "whitespace"):
        count = sum(row.get("cpp_lexical", {}).get("bytes", {}).get(category, 0) for row in data["inventory"]["files"])
        share = f"{count/denominator:.2%}" if denominator else "unmeasured"
        lines.append(f"| {category} | {count} | {share} |")
    lines += ["", source_composition_markdown(data["inventory"])]
    if data.get("cross_area"):
        lines += ["", "## 局所を横断した構造", "", "以下は観測済み局所に限る。依存先の本体が対象外のcallは外側へのcallとして保持する。", "", markdown(data["cross_area"])]
    lines += ["", "## 失敗", "", "```json", json.dumps(data["errors"], indent=2, ensure_ascii=True), "```",
              "", "ANALYSIS_COMPLETE" if data["status"] == "COMPLETE" else "ANALYSIS_INCOMPLETE", ""]
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--plan", type=Path, required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--build-signature", type=Path)
    source.add_argument("--compilation-database", type=Path)
    parser.add_argument("--clang", default="clang++")
    parser.add_argument("--libclang")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--json-report", type=Path)
    args = parser.parse_args(argv)
    root = args.root.resolve()
    resolve = lambda p: (root / p).resolve() if p is not None else None
    try:
        outputs = [resolve(x) for x in (args.report, args.json_report) if x is not None]
        if len(set(outputs)) != len(outputs) or any(x.exists() or not x.parent.is_dir() for x in outputs):
            raise AnalysisError("report path exists, collides or its parent is missing")
        if args.timeout < 1:
            raise AnalysisError("timeout must be positive")
        result, snapshot = survey(root, resolve(args.plan), signature=resolve(args.build_signature),
                                  database=resolve(args.compilation_database), clang=args.clang, library=args.libclang, timeout=args.timeout)
        outputs = []
        if args.report:
            outputs.append((resolve(args.report), survey_markdown(result)))
        if args.json_report:
            outputs.append((resolve(args.json_report), json_report(result)))
        snapshot.verify()
        if outputs:
            for receipt in save_outputs(outputs, protected={Path(x) for x in snapshot.buffers}):
                print("Created " + receipt["path"], file=sys.stderr)
        else:
            print(survey_markdown(result))
        return 0 if result["status"] == "COMPLETE" else 2
    except (AnalysisError, OSError, ValueError, KeyError, TypeError) as exc:
        print("ANALYSIS_INCOMPLETE: " + str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
