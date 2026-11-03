"""One model, two renderings; exclusive output creation and end-of-stream markers."""
from __future__ import annotations

import json
import os
from pathlib import Path

from .model import AnalysisError, digest_bytes


def source_label(evidence):
    location = evidence.get("expansion", evidence.get("start", {}))
    return f"{location.get('path')}:{location.get('line')}:{location.get('column')}"


def source_composition_markdown(inventory):
    content = inventory.get("cpp_content")
    lines = ["## 物理容量と文字列・データの字句量", "",
             "文字列の用途は未分類。文字列を非ロジックと決めず、codeを処理量と同一視しない。", ""]
    if content is None:
        return "\n".join(lines + ["旧inventoryには分離測定がないため未測定。", ""])
    denominator = content["cpp_byte_denominator"]
    lines += [f"C/C++分母: {denominator} bytes / {content['cpp_files']} file。", "",
              "| 字句区分 | Source bytes | C/C++分母中 |", "| --- | ---: | ---: |"]
    rows = [("syntax / identifier / punctuation", content["bytes"]["code"])]
    rows += [(kind + " literal", count) for kind, count in content["literal_bytes"].items()]
    rows += [(kind, content["bytes"][kind]) for kind in ("comment", "whitespace")]
    for name, count in rows:
        share = f"{count/denominator:.2%}" if denominator else "unmeasured"
        lines.append(f"| {name} | {count} | {share} |")
    lines += ["", f"文字列内部のSource bytes: {content['string_payload_source_bytes']}。引用符・encoding prefix・raw delimiterを除き、escapeをdecodeしない。",
              f"文字列用途の状態: {content['string_purpose_status']}。UI文言・処理規則・埋込みデータ等の意味分類は未実施。", ""]
    return "\n".join(lines)


def markdown(data):
    names = {row["id"]: row["qualified_name"] for row in data["functions"]}
    functions = {row["id"]: row for row in data["functions"]}
    provenance = {key: value for key, value in data["provenance"].items() if key not in {"input_manifest", "tool_manifest"}}
    provenance["input_manifest_count"] = len(data["provenance"].get("input_manifest", []))
    lines = ["# C++ responsibility analysis", "", f"Analysis status: {data['status']}",
             "Design assessment: unreviewed candidates (not a quality PASS)", "", "## Provenance", "",
             "```json", json.dumps(provenance, indent=2, ensure_ascii=True), "```", "", "## Metrics", "",
             "```json", json.dumps(data["metrics"], indent=2), "```", "", "## Translation units", ""]
    for unit in data["translation_units"]:
        lines.append(f"- {unit['file']}: {unit['status']}; config={unit['config']}")
        presence = unit.get("body_presence", {})
        if presence.get("kind") == "fingerprinted-declaration-only":
            lines.append("  - Declaration-only TU verified by source/header hashes and observed declarations; function-body metrics are unmeasured, not a quality PASS.")
            lines.append("  - Declarations: " + ", ".join(presence["observed_declarations"]))
    lines += ["", "## File participation", "", "| File | Selected build |", "| --- | --- |"]
    for file in data["files"]:
        lines.append(f"| {file['path']} | {'participates' if file['participates'] else 'not observed in selected build (not proof of unused code)'} |")
    if "inventory" in data:
        lines += ["", source_composition_markdown(data["inventory"])]
    lines += ["", "## Function metrics (R6 investigation ranking)", "", "Non-literal tokens measure syntax/identifiers, not semantic logic. Decisions/nesting are separate structural observations.", "",
              "| Function | Source | Decisions | Nesting | Tokens | Non-literal tokens | String tokens | Role | Contracts |", "| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | --- |"]
    ordered = data.get("complexity_ranking", list(functions))
    for identity in ordered:
        function = functions[identity]
        name = function["qualified_name"].replace("|", "\\|")
        part = function.get("body_token_partition", {})
        nonliteral = part.get("non_literal_tokens", "unmeasured")
        strings = part.get("literal_tokens", {}).get("string", "unmeasured")
        lines.append(f"| {name} | {source_label(function['evidence'])} | {function['decision_count']} | {function['max_nesting']} | {function['body_tokens']} | {nonliteral} | {strings} | {function.get('body_role', 'unknown')} | {', '.join(function.get('roles', [])) or 'unclassified'} |")
    lines += ["", "## Responsibility distribution", "", "| Contract | File | Implementation functions | Tokens |", "| --- | --- | ---: | ---: |"]
    for row in data.get("responsibility_matrix", []):
        lines.append(f"| {row['contract']} | {row['file']} | {len(row['implementation_functions'])} | {row['body_tokens']} |")
    if "structure_profile" in data:
        lines += ["", "## Structural profile", "", "Technology shares refer to associated whole bodies and overlap; they are not exclusive source percentages.", ""]
        for config in data["structure_profile"]["configurations"]:
            lines += [f"### Configuration {config['config'][:12]}", "", "```json",
                      json.dumps({k: v for k, v in config.items() if k not in {"technologies", "module_edges"}}, indent=2), "```", "",
                      "| Technology | Functions / denominator | Associated tokens | Associated token share |", "| --- | ---: | ---: | ---: |"]
            for row in config["technologies"]:
                lines.append(f"| {row['technology']} | {row['function_count']} / {row['function_denominator']} | {row['associated_body_tokens']} | {row['associated_body_token_share']:.2%} |")
        lines += ["", "### Whole-body clone candidates", "", "```json", json.dumps(data["structure_profile"]["clone_candidates"], indent=2), "```"]
        lines += ["", "### Contract/write-target overlap candidates", "", "```json", json.dumps(data["structure_profile"]["contract_overlap_candidates"], indent=2), "```"]
    if data.get("unused_profile"):
        unused = data["unused_profile"]
        lines += ["", "## Unused function candidates", "", "Observed reachability is not deletion approval. All required configurations and non-AST references need review.",
                  "", "```json", json.dumps({k: v for k, v in unused.items() if k != "rows"}, indent=2), "```", "",
                  "| Function | Source | Status | Linkage |", "| --- | --- | --- | --- |"]
        for row in unused["rows"]:
            lines.append(f"| {row['name']} | {source_label(row['evidence'])} | {row['status']} | {row['linkage']} |")
    lines += ["", "## Candidates", ""]
    for candidate in data["candidates"]:
        compact = dict(candidate)
        if "cross_calls" in compact:
            compact["cross_call_count"] = len(compact.pop("cross_calls"))
        lines += [f"### {candidate['rule']} {candidate['id'][:12]}", "", candidate["interpretation"], "",
                  "```json", json.dumps(compact, ensure_ascii=True, indent=2), "```", ""]
        for identity in candidate.get("functions", [candidate.get("function")]):
            if identity in functions:
                function = functions[identity]
                lines.append(f"- {names[identity]}: {source_label(function['evidence'])}")
                for role in data["roles"]:
                    if role["function"] == identity:
                        for anchor in role["evidence"]:
                            lines.append(f"  - {role['contract']}: {anchor['kind']} {anchor['symbol']} at {source_label(anchor['evidence'])}")
        lines.append("")
    lines += ["## State access inventory", "", "| Declaration / receiver | Precision | Direct readers | Direct writers | May-write / escape |", "| --- | --- | --- | --- | --- |"]
    for state in data["states"].values():
        symbol = data["symbols"].get(state.get("declaration"), {})
        readers = ", ".join(names.get(x, x) for x in state.get("readers", []))
        writers = ", ".join(names.get(x, x) for x in state.get("writers", []))
        possible = ", ".join(names.get(x, x) for x in state.get("possible_writers", []))
        origin = state['origin']
        origin_symbol = data["symbols"].get(origin.split(":", 1)[-1], {})
        receiver = origin_symbol.get("qualified_name", origin)
        lines.append(f"| {symbol.get('qualified_name', 'unknown')} / {receiver} | {state['precision']} | {readers} | {writers} | {possible} |")
    lines += ["", "## Cross-fragment dependencies", "", "```json",
              json.dumps({"direct_call_edges": len(data["cross_fragment_calls"]), "cross_file_state_targets": len(data["cross_fragment_states"])}, indent=2), "```",
              "", "Detailed call/access edges, spelling/expansion positions and the full input manifest are in the JSON rendering.",
              "", "## Unclassified functions", ""]
    for identity in data["unclassified"]:
        lines.append(f"- {names[identity]}: {source_label(functions[identity]['evidence'])}")
    lines += ["", "## Unresolved accesses", "", "```json",
              json.dumps([row for row in data["accesses"] if row["mode"] == "unknown"], indent=2, ensure_ascii=True), "```",
              "", "## Diagnostics", "", "```json", json.dumps(data["diagnostics"], indent=2, ensure_ascii=True), "```",
              "", "## Limits / not performed", ""]
    lines += ["- " + value for value in data["limitations"]]
    if "comparison" in data:
        lines += ["", "## Comparison", "", "```json", json.dumps(data["comparison"], indent=2, ensure_ascii=True), "```"]
    lines += ["", "ANALYSIS_COMPLETE" if data["status"] == "COMPLETE" else "ANALYSIS_INCOMPLETE", ""]
    return "\n".join(lines)


def json_report(data):
    return json.dumps(data, indent=2, ensure_ascii=True) + "\n"


def save_outputs(outputs: list[tuple[Path, str]], *, protected: set[Path]):
    """Exclusive creation. Partial outputs remain; no cleanup, overwrite or fallback."""
    paths = [path.resolve() for path, _ in outputs]
    if len(set(paths)) != len(paths):
        raise AnalysisError("output paths collide")
    for path in paths:
        if path in protected or path.exists():
            raise AnalysisError(f"output already exists or is an input: {path}")
        if not path.parent.is_dir():
            raise AnalysisError(f"output parent must already exist: {path.parent}")
    receipts = []
    for (path, content), resolved in zip(outputs, paths):
        data = content.encode("utf-8")
        with resolved.open("xb") as output:
            output.write(data); output.flush(); os.fsync(output.fileno())
        if resolved.read_bytes() != data:
            raise AnalysisError(f"output verification failed: {resolved}")
        receipts.append({"path": str(resolved), "sha256": digest_bytes(data)})
    return receipts
