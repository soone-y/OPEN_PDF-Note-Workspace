"""Measured structure, technology evidence and lexical clone candidates.

No quality score or inferred semantic equivalence. Ratios always name their
denominator; technology-associated bodies overlap and are not token partitions.
"""
from __future__ import annotations

from collections import Counter, defaultdict
from itertools import combinations
import math
import re

from .model import AnalysisError, stable_id
from .literals import body_composition

TECHNOLOGIES = {
    "win32.window": r"(?:CreateWindowEx[AW]|DefWindowProc[AW]|SendMessage[AW]|PostMessage[AW]|SetWindowPos|ShowWindow|GetClientRect|GetWindowRect)",
    "win32.gdi": r"(?:GetTextExtent.*|GetTextMetrics[AW]|TextOut[AW]|ExtTextOut[AW]|DrawText[AW]|Create.*(?:Font|Brush|Pen).*|SelectObject|DeleteObject|FillRect|BitBlt|StretchBlt)",
    "win32.file": r"(?:CreateFile[AW]|ReadFile|WriteFile|FlushFileBuffers|MoveFile.*|ReplaceFile[AW]|DeleteFile[AW]|LockFileEx|UnlockFileEx)",
    "win32.process-thread": r"(?:CreateProcess[AW]|CreateThread|CreateEvent[AW]|SetEvent|ResetEvent|WaitFor.*|CreateJobObject[AW]|AssignProcessToJobObject|TerminateProcess)",
    "win32.ime": r"Imm.*",
    "pdfium": r"FPDF.*",
    "md4c": r"md_parse",
    "zlib": r"(?:deflate.*|inflate.*|compress2?|uncompress|crc32)",
    "std.container": r"std::(?:vector|map|unordered_map|set|unordered_set|deque|list|array|basic_string|basic_string_view)(?:::.*)?",
    "std.smart-pointer": r"std::(?:unique_ptr|shared_ptr|weak_ptr|make_unique|make_shared)(?:::.*)?",
    "std.concurrency": r"std::(?:thread|jthread|mutex|recursive_mutex|lock_guard|unique_lock|scoped_lock|atomic|condition_variable|future|async)(?:::.*)?",
    "std.filesystem": r"std::filesystem::.*",
}


def distribution(values):
    values = sorted(values)
    if not values:
        return {"count": 0, "median": None, "p95": None, "max": None}
    n = len(values)
    return {"count": n, "median": (values[(n-1)//2] + values[n//2])/2,
            "p95": values[math.ceil(n*.95)-1], "max": values[-1]}


def merged_bytes(intervals):
    total, end = 0, -1
    for start, stop in sorted(intervals):
        if stop < start or start < 0:
            raise AnalysisError("invalid source interval")
        total += max(0, stop-max(start, end))
        end = max(end, stop)
    return total


def physical_key(function):
    body = function["body"]
    return (function["config"], function["file"], body["start"]["offset"], body["end"]["offset"])


def graph_cycles(edges):
    """Iterative reachability SCCs; small module graph, no Python recursion limit."""
    adjacency = defaultdict(set)
    for source, target in edges:
        adjacency[source].add(target)
        adjacency.setdefault(target, set())
    reach = {}
    for seed in adjacency:
        seen, pending = set(), [seed]
        while pending:
            node = pending.pop()
            if node not in seen:
                seen.add(node)
                pending.extend(adjacency[node]-seen)
        reach[seed] = seen
    remaining, groups = set(adjacency), []
    while remaining:
        seed = min(remaining)
        group = {x for x in reach[seed] if seed in reach[x]}
        remaining -= group
        if len(group) > 1:
            groups.append(sorted(group))
    return groups


def module_name(path):
    parts = path.replace("\\", "/").split("/")
    return "/".join(parts[:2]) if len(parts) > 2 else parts[0]


def profile(report, *, minimum_clone_tokens=40):
    if minimum_clone_tokens < 1:
        raise AnalysisError("clone minimum must be positive")
    functions = {row["id"]: row for row in report["functions"]}
    if (report.get("methods", {}).get("body_token_partition") == "clang-body-literal-partition/v1"
            and any(row.get("body_token_partition") is None for row in functions.values())):
        raise AnalysisError("required function body literal partition missing")
    calls, accesses = defaultdict(list), defaultdict(list)
    for row in report["calls"]:
        calls[row["caller"]].append(row)
    for row in report["accesses"]:
        accesses[row["function"]].append(row)
    # TU-local copies of one source definition are not multiple implementations.
    physical = {physical_key(row): row for row in functions.values()}
    partitions = defaultdict(list)
    for row in physical.values():
        partitions[row["config"]].append(row)
    output = {"method": "observed-structure/v1", "configurations": [], "clone_candidates": [], "contract_overlap_candidates": [],
              "technology_rules": TECHNOLOGIES,
              "limits": ["Identifier-normalized clones preserve literals, but also normalize API/field names; review false positives.",
                         "Clone fingerprints cover whole bodies only; partial and edited clones are not searched.",
                         "Technology matches resolved direct callees or referenced types, not runtime execution or library versions.",
                         "Associated-body token shares overlap; do not add them to form a source partition.",
                         "Source module names are path groupings, not automatically proven responsibility boundaries."]}
    for config, rows in sorted(partitions.items()):
        total_tokens = sum(row["body_tokens"] for row in rows)
        composition = body_composition(rows)
        values = composition.pop("non_literal_values")
        composition["non_literal_distribution"] = distribution(values) if values is not None else None
        technology = defaultdict(list)
        constructs = Counter()
        clone_buckets = defaultdict(list)
        module_edges = defaultdict(list)
        external = unresolved = 0
        state_writers = defaultdict(set)
        for row in rows:
            constructs.update(row.get("constructs", {}))
            if row.get("body_role") != "delegation":
                for access in accesses[row["id"]]:
                    if access.get("state") and access["mode"] in {"write", "read_write"}:
                        state_writers[access["state"]].add(row["id"])
            evidence = defaultdict(list)
            for call in calls[row["id"]]:
                if call["resolution"] != "direct":
                    unresolved += 1
                    continue
                target = functions.get(call["callee"])
                if target and target["config"] == config:
                    first, second = module_name(row["file"]), module_name(target["file"])
                    if first != second:
                        module_edges[first, second].append(call)
                else:
                    external += 1
                for name, pattern in TECHNOLOGIES.items():
                    if re.fullmatch(pattern, call["callee_name"]):
                        evidence[name].append({"kind": "resolved-callee", "symbol": call["callee_name"], "source": call["evidence"]})
            for name, pattern in TECHNOLOGIES.items():
                for type_name in row.get("types", []):
                    if re.fullmatch(pattern, type_name):
                        evidence[name].append({"kind": "referenced-type", "symbol": type_name, "source": row["evidence"]})
            for name, proof in sorted(evidence.items()):
                technology[name].append({"function": row["id"], "evidence": proof})
            if row["fingerprint_tokens"] >= minimum_clone_tokens and row.get("body_role") != "delegation":
                for kind in ("lexical_body_hash", "normalized_body_hash"):
                    if row.get(kind):
                        clone_buckets[kind, row[kind]].append(row)
        exact_groups = set()
        for (kind, fingerprint), group in sorted(clone_buckets.items()):
            if len(group) < 2:
                continue
            identities = sorted(row["id"] for row in group)
            if kind == "lexical_body_hash":
                exact_groups.add(tuple(identities))
            if kind == "normalized_body_hash" and tuple(identities) in exact_groups:
                continue
            candidates = {"kind": kind, "config": config, "fingerprint": fingerprint,
                          "functions": identities, "tokens_per_body": group[0]["fingerprint_tokens"],
                          "files": sorted({row["file"] for row in group}), "review": "unreviewed",
                          "common_contracts": sorted(set.intersection(*(set(row.get("roles", [])) for row in group))),
                          "interpretation": "whole-body lexical similarity; not proof of duplicate responsibility"}
            candidates["id"] = stable_id(candidates)
            output["clone_candidates"].append(candidates)
        overlap = defaultdict(set)
        for state, writers in state_writers.items():
            for first, second in combinations(sorted(writers), 2):
                if set(functions[first].get("roles", [])) & set(functions[second].get("roles", [])):
                    overlap[first, second].add(state)
        for (first, second), states in sorted(overlap.items()):
            candidate = {"config": config, "functions": [first, second],
                         "common_contracts": sorted(set(functions[first]["roles"]) & set(functions[second]["roles"])),
                         "shared_write_targets": [{"state": state, "precision": report["states"][state]["precision"]} for state in sorted(states)],
                         "review": "unreviewed", "interpretation": "same contract and observed write target; review normal phases, distinct symbolic instances and duplicate implementation"}
            candidate["id"] = stable_id(candidate)
            output["contract_overlap_candidates"].append(candidate)
        output["configurations"].append({
            "config": config, "physical_definitions": len(rows), "associated_body_token_denominator": total_tokens,
            "body_token_composition": composition,
            "distributions": {key: distribution([row[key] for row in rows]) for key in
                              ("decision_count", "max_nesting", "body_tokens", "fan_in", "fan_out")},
            "construct_nodes": dict(sorted(constructs.items())),
            "function_forms": {"delegation": sum(row.get("body_role") == "delegation" for row in rows),
                               "lambda": sum(row["kind"] == "LAMBDA_EXPR" for row in rows),
                               "explicit_new": sum(row.get("constructs", {}).get("CXX_NEW_EXPR", 0) > 0 for row in rows),
                               "explicit_delete": sum(row.get("constructs", {}).get("CXX_DELETE_EXPR", 0) > 0 for row in rows),
                               "exception_constructs": sum(any(row.get("constructs", {}).get(x, 0) for x in ("CXX_TRY_STMT", "CXX_THROW_EXPR", "CXX_CATCH_STMT")) for row in rows),
                               "no_observed_state_access": sum(not accesses[row["id"]] for row in rows)},
            "technologies": [{"technology": name, "classification": "symbol-pattern candidate; inspect declaration/source evidence", "functions": observed,
                              "function_count": len(observed), "function_denominator": len(rows),
                              "associated_body_tokens": sum(functions[x["function"]]["body_tokens"] for x in observed),
                              "associated_body_token_share": sum(functions[x["function"]]["body_tokens"] for x in observed)/total_tokens if total_tokens else None}
                             for name, observed in sorted(technology.items())],
            "module_edges": [{"from": a, "to": b, "direct_call_sites": len(proof), "evidence": proof} for (a, b), proof in sorted(module_edges.items())],
            "module_cycles": graph_cycles(module_edges), "outside_observed_definitions_calls": external,
            "unresolved_calls": unresolved})
    return output
