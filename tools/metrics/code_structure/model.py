"""Frontend-independent identities and explicit uncertainty contracts."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

SCHEMA = "code-structure/v3"
METHODS = {"decisions": "decision_count/v1", "nesting": "control_nesting/v1",
           "body_presence": "explicit-body-presence-contract/v1",
           "symbol_identity": "canonical-function-declaration/v1",
           "body_discovery": "selected-AST-locationless-wrapper-traversal/v1",
           "file_participation": "physical-path-union/v1",
           "access": "direct-access-ref-summary/v1", "roles": "contract-anchors/v1",
           "body_fingerprint": "clang-token-body/v1", "profile": "observed-structure/v1",
           "body_token_partition": "clang-body-literal-partition/v1",
           "function_references": "selected-AST-function-reference/v1",
           "unused": "conservative-observed-reachability/v1"}
LIMITS = [
    "Candidates require design review; analysis completion is not a quality PASS.",
    "Direct calls only; virtual dispatch and function pointers are unresolved candidates.",
    "Simple reference summaries only; pointer aliasing, implicit destructor effects and union aliasing are not fully modeled.",
    "Object origins and dynamic context keys are symbolic, not runtime instance proofs.",
    "Only selected translation units/configurations are analyzed; inactive branches are not covered.",
    "Role anchors cover selected contracts; unclassified code remains visible.",
]


class AnalysisError(Exception):
    """A required input/capability failed; never convert this to an empty success."""


def digest_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def stable_id(value) -> str:
    return digest_bytes(json.dumps(value, sort_keys=True, ensure_ascii=True,
                                   separators=(",", ":")).encode("utf-8"))


def display_path(path, root: Path) -> str:
    path = Path(path).resolve()
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def empty_report() -> dict:
    return {"schema": SCHEMA, "methods": dict(METHODS), "status": "FAILED",
            "diagnostics": [], "limitations": list(LIMITS), "translation_units": [],
            "functions": [], "symbols": {}, "states": {}, "calls": [], "accesses": [],
            "reference_summaries": {}, "function_references": [], "files": [], "candidates": [], "roles": [],
            "unclassified": [], "structural_groups": [], "cross_fragment_calls": [],
            "cross_fragment_states": [], "metrics": {}, "provenance": {}}


def merge_file_observation(table, row):
    """A physical file participates if any selected TU observed its inclusion."""
    if not isinstance(row.get("participates"), bool):
        raise AnalysisError("file participation must be boolean")
    path = row["path"]
    if path in table:
        if table[path]["sha256"] != row["sha256"]:
            raise AnalysisError("physical file observations disagree: " + path)
        table[path]["participates"] |= row["participates"]
    else:
        table[path] = dict(row)
