"""Conservative unused-function candidates, never automatic deletion approval.

External linkage and implicit-dispatch kinds are retained as open-world roots.
All observed function references (including callback/address operands) add edges.
References outside observed function bodies add roots. Candidate components may
call one another; fan-in alone is not a reachability check. Aggregate all required
configurations and review external/test/resource use before suggesting deletion.
"""
from __future__ import annotations

from collections import Counter, defaultdict
from .model import stable_id


def unused_profile(report):
    functions = {f["id"]: f for f in report["functions"]}
    roots, adjacency, incoming, references = {}, defaultdict(set), defaultdict(set), defaultdict(list)
    for identity, function in functions.items():
        reasons = []
        if function.get("linkage") != "INTERNAL":
            reasons.append("external/unknown linkage; callers outside observed bodies are possible")
        if function["kind"] != "FUNCTION_DECL":
            reasons.append("method/template/lambda/constructor/destructor; implicit or indirect use not fully modeled")
        if function.get("entry") or function["name"] in {"main", "wmain", "WinMain", "wWinMain", "DllMain"}:
            reasons.append("declared/platform entry")
        if reasons:
            roots[identity] = reasons
    for call in report["calls"]:
        a, b = call["caller"], call["callee"]
        if b in functions:
            incoming[b].add(a)
            if a in functions:
                adjacency[a].add(b)
            else:
                roots.setdefault(b, []).append("call from outside observed bodies")
    for reference in report["function_references"]:
        a, b = reference["owner"], reference["target"]
        if b not in functions:
            continue
        references[b].append(reference)
        if a in functions:
            adjacency[a].add(b)
        else:
            roots.setdefault(b, []).append("reference from initializer/registration/outside observed function bodies")
    reached, pending = set(), list(roots)
    while pending:
        identity = pending.pop()
        if identity in reached:
            continue
        reached.add(identity)
        pending.extend(adjacency[identity] - reached)
    physical = defaultdict(list)
    for function in functions.values():
        key = (function["config"], function["file"], function["evidence"]["start"]["offset"], function["type"])
        physical[key].append(function)
    rows = []
    counts = Counter()
    for key, instances in sorted(physical.items()):
        identities = {f["id"] for f in instances}
        reachable = bool(identities & reached)
        observed_incoming = set().union(*(incoming[x] for x in identities))
        if reachable and observed_incoming:
            counts["retained_with_observed_call"] += 1
            continue
        status = "no-observed-call-but-retained" if reachable else "unreachable-in-observed-graph"
        counts[status] += 1
        representative = instances[0]
        row = {"id": stable_id([key, status]), "config": key[0], "file": key[1],
               "name": representative["qualified_name"], "kind": representative["kind"],
               "linkage": representative.get("linkage"), "functions": sorted(identities),
               "evidence": representative["evidence"], "status": status,
               "incoming_callers": sorted(observed_incoming),
               "root_reasons": sorted({reason for x in identities for reason in roots.get(x, [])}),
               "references": [ref for x in sorted(identities) for ref in references[x]],
               "review": "unreviewed", "deletion": "not authorized by this analysis"}
        rows.append(row)
    return {"method": "conservative-observed-reachability/v1", "counts": dict(sorted(counts.items())),
            "physical_definition_denominator": len(physical), "rows": rows,
            "limits": ["Unreachable is a candidate in selected ASTs, not proof of unused code.",
                       "All required profiles and non-AST test/resource/script references need separate review.",
                       "External linkage and implicit-dispatch kinds are conservative roots; this can miss unused code.",
                       "Indirect target resolution, overload-dependent templates and implicit effects are incomplete.",
                       "Incomplete TU/configuration sets cannot establish whole-application unused status."]}
