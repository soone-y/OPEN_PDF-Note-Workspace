"""Refuse incompatible comparisons; retain aggregate dependencies across file moves."""
from __future__ import annotations

from collections import defaultdict

from .model import AnalysisError, stable_id


def compare(before, after):
    required = ("schema", "methods")
    reasons = [key + " differs" for key in required if before.get(key) != after.get(key)]
    for key in ("tool_hash", "rules_hash", "configuration_ids", "scopes", "selected_tus"):
        if before.get("provenance", {}).get(key) != after.get("provenance", {}).get(key):
            reasons.append(key + " differs")
    if before.get("status") != "COMPLETE" or after.get("status") != "COMPLETE":
        reasons.append("both analyses must be COMPLETE")
    if reasons:
        return {"status": "NOT_COMPARABLE", "reasons": reasons}
    left = {row["id"]: row for row in before["functions"]}
    right = {row["id"]: row for row in after["functions"]}
    mapping = {identity: identity for identity in left.keys() & right.keys()}
    unmatched_left, unmatched_right = defaultdict(list), defaultdict(list)
    for identities, source, destination in ((left.keys()-right.keys(), left, unmatched_left), (right.keys()-left.keys(), right, unmatched_right)):
        for identity in identities:
            row = source[identity]
            if row.get("body_sha256"):
                destination[(row["config"], row["qualified_name"], row.get("type"), row["body_sha256"])].append(identity)
    for key in unmatched_left.keys() & unmatched_right.keys():
        if len(unmatched_left[key]) == len(unmatched_right[key]) == 1:
            mapping[unmatched_left[key][0]] = unmatched_right[key][0]
    changes = []
    for identity, current in sorted(mapping.items()):
        fields = {key: {"before": left[identity].get(key), "after": right[current].get(key)}
                  for key in ("file", "decision_count", "max_nesting", "body_tokens", "body_token_partition", "roles")
                  if left[identity].get(key) != right[current].get(key)}
        if fields:
            changes.append({"function": current, "previous_function": identity,
                            "matched_by": "symbol-id" if current == identity else "unique-definition-signature",
                            "name": right[current]["qualified_name"], "changes": fields})
    result = {"status": "COMPARABLE", "before_snapshot": before["provenance"]["source_snapshot_id"],
              "after_snapshot": after["provenance"]["source_snapshot_id"],
              "changed_functions": changes, "added_functions": sorted(right.keys()-set(mapping.values())),
              "removed_functions": sorted(left.keys()-mapping.keys()), "physical_split_candidates": []}
    # Source path is not the identity for external symbols; internal symbols may remain unmatched.
    for change in changes:
        if "file" not in change["changes"]:
            continue
        identity = change["function"]
        previous = change["previous_function"]
        old_writes = {row.get("state") for row in before["accesses"] if row["function"] == previous and row["mode"] in {"write", "read_write"}}
        new_writes = {row.get("state") for row in after["accesses"] if row["function"] == identity and row["mode"] in {"write", "read_write"}}
        old_calls = {mapping.get(row["callee"], row["callee"]) for row in before["calls"] if row["caller"] == previous and row["resolution"] == "direct"}
        new_calls = {row["callee"] for row in after["calls"] if row["caller"] == identity and row["resolution"] == "direct"}
        if old_writes == new_writes and old_calls == new_calls and (new_writes or new_calls):
            result["physical_split_candidates"].append({"rule": "R4", "function": identity,
                                                      "evidence": change, "unchanged_write_targets": sorted(new_writes),
                                                      "unchanged_direct_callees": sorted(new_calls),
                                                      "interpretation": "file moved but observed direct dependencies remain"})
    return result
