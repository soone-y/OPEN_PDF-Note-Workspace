"""Evidence-backed contract candidates; never an automatic design quality gate."""
from __future__ import annotations

from collections import Counter, defaultdict
from itertools import combinations
import re

from .model import AnalysisError, stable_id


def validate_rules(rules):
    if not isinstance(rules, dict) or rules.get("version") != 1:
        raise AnalysisError("role rules require version=1")
    contracts = {row["id"]: row for row in rules.get("contracts", [])}
    if len(contracts) != len(rules.get("contracts", [])):
        raise AnalysisError("duplicate contract id")
    for contract in contracts.values():
        for field in ("id", "component", "owner", "output", "level"):
            if not isinstance(contract.get(field), str) or not contract[field]:
                raise AnalysisError(f"contract requires {field}")
    for anchor in rules.get("anchors", []):
        if "file_prefixes" in anchor and (not isinstance(anchor["file_prefixes"], list) or not anchor["file_prefixes"] or any(not isinstance(p, str) or not p or ".." in p.split("/") or "\\" in p or p.startswith("/") for p in anchor["file_prefixes"])):
            raise AnalysisError("anchor file_prefixes requires repository-relative path prefixes")
        if anchor.get("contract") not in contracts or anchor.get("kind") not in {"callee", "state", "type", "function", "entry"}:
            raise AnalysisError("invalid role anchor contract/kind")
        if bool(anchor.get("symbol")) == bool(anchor.get("pattern")):
            raise AnalysisError("anchor requires exactly one symbol or full-match pattern")
        if anchor.get("pattern"):
            try:
                re.compile(anchor["pattern"])
            except re.error as exc:
                raise AnalysisError(f"invalid role anchor pattern: {exc}") from exc
    return contracts


def matches(anchor, name):
    return name == anchor.get("symbol") if "symbol" in anchor else re.fullmatch(anchor["pattern"], name or "") is not None


def derive(report, rules):
    contracts = validate_rules(rules)
    report["contracts"] = list(contracts.values())
    functions = {row["id"]: row for row in report["functions"]}
    calls, accesses, incoming, state_accesses = defaultdict(list), defaultdict(list), defaultdict(set), defaultdict(list)
    for row in report["calls"]:
        calls[row["caller"]].append(row)
        if row["resolution"] == "direct":
            incoming[row["callee"]].add(row["caller"])
    for row in report["accesses"]:
        accesses[row["function"]].append(row)
        state_accesses[row.get("state")].append(row)
    symbols = report["symbols"]
    roles, entries, features = {}, set(), {}
    anchor_scope = defaultdict(lambda: {"functions": 0, "with_applicable_anchors": 0,
                                         "classified": 0, "unclassified_with_applicable_anchors": 0})
    utility_patterns = rules.get("utilities", [])
    for function in functions.values():
        identity = function["id"]
        file = function["file"]
        scope = "/".join(file.split("/")[:2]) if file.startswith("src/") else file.rsplit("/", 1)[0]
        scope_counts = anchor_scope[scope]
        scope_counts["functions"] += 1
        applicable_anchors = [anchor for anchor in rules.get("anchors", [])
                              if not anchor.get("file_prefixes") or any(file.startswith(p) for p in anchor["file_prefixes"])]
        if applicable_anchors:
            scope_counts["with_applicable_anchors"] += 1
        direct_calls = [row for row in calls[identity] if row["resolution"] == "direct"]
        known_writes = [row for row in accesses[identity] if row["mode"] in {"write", "read_write"}]
        # Recognize syntactically thin forwarding bodies; call fan-out is not implementation ownership.
        forwarding = function["decision_count"] == 0 and not known_writes and all(
            row["mode"] in {"read", "escape"} for row in accesses[identity]) and function["forwarding_body"]
        function["body_role"] = "delegation" if forwarding and direct_calls else "implementation"
        evidence = defaultdict(list)
        ownership = set()
        for anchor in applicable_anchors:
            kind, contract = anchor["kind"], anchor["contract"]
            if kind in {"function", "entry"} and matches(anchor, function["qualified_name"]):
                if kind == "entry":
                    entries.add(identity)
                evidence[contract].append({"kind": kind, "symbol": function["qualified_name"], "evidence": function["evidence"]})
            if kind == "type":
                for type_name in function.get("types", []):
                    if matches(anchor, type_name):
                        evidence[contract].append({"kind": kind, "symbol": type_name, "evidence": function["evidence"]})
            if kind == "callee":
                for call in direct_calls:
                    if matches(anchor, call["callee_name"]):
                        evidence[contract].append({"kind": kind, "symbol": call["callee_name"], "evidence": call["evidence"]})
            if kind == "state":
                for access in accesses[identity]:
                    target = access["target"]
                    if target and target.get("declaration"):
                        name = symbols.get(target["declaration"], {}).get("qualified_name", "")
                        if matches(anchor, name):
                            evidence[contract].append({"kind": kind, "symbol": name,
                                                       "mode": access["mode"], "evidence": access["evidence"]})
                            if anchor.get("ownership"):
                                ownership.add(contract)
        # Component ownership can unify explicitly permitted operations, with evidence retained.
        absorbed = {}
        for owner in sorted(ownership):
            permits = contracts[owner].get("permits", [])
            for contract in list(evidence):
                if contract in permits and contract != owner:
                    absorbed[contract] = evidence.pop(contract)
        function["roles"] = sorted(evidence)
        function["delegated_roles"] = sorted(evidence) if function["body_role"] == "delegation" else []
        function["operation_evidence"] = absorbed
        roles[identity] = evidence
        for contract, anchor_evidence in sorted(evidence.items()):
            report["roles"].append({"function": identity, "contract": contract,
                                    "body_role": function["body_role"], "evidence": anchor_evidence})
        if not evidence:
            report["unclassified"].append(identity)
            if applicable_anchors:
                scope_counts["unclassified_with_applicable_anchors"] += 1
        else:
            scope_counts["classified"] += 1
        function["entry"] = identity in entries
        function["fan_out"] = len({row["callee"] for row in direct_calls})
        function["fan_in"] = len(incoming[identity])
        function["unresolved_calls"] = sum(row["resolution"] != "direct" for row in calls[identity])
        state_features = {(row["target"].get("declaration"), row["mode"])
                          for row in accesses[identity] if row["target"] and row["target"].get("declaration")}
        callee_features = {(row["callee"], "call") for row in direct_calls
                           if not any(re.fullmatch(pattern, row["callee_name"]) for pattern in utility_patterns)}
        features[identity] = state_features | callee_features
    frequency = Counter(feature for values in features.values() for feature in values)

    def affinity(first, second):
        union = features[first] | features[second]
        if not union:
            return None
        return sum(1/frequency[x] for x in features[first] & features[second]) / sum(1/frequency[x] for x in union)

    def group_affinity(first, second=None):
        pairs = combinations(first, 2) if second is None else ((a, b) for a in first for b in second if a != b)
        values = [value for a, b in pairs if (value := affinity(a, b)) is not None]
        return {"mean": sum(values)/len(values) if values else None, "observed_pairs": len(values)}

    distribution = defaultdict(lambda: defaultdict(list))
    for identity, evidence in roles.items():
        if functions[identity]["body_role"] == "implementation":
            for contract in evidence:
                distribution[functions[identity]["file"]][contract].append(identity)
    candidates = report["candidates"]
    for file, contract_groups in sorted(distribution.items()):
        for first, second in combinations(sorted(contract_groups), 2):
            a, b = contracts[first], contracts[second]
            independent = [field for field in ("owner", "output") if a[field] != b[field]]
            if a["level"] == b["level"] and independent:
                candidates.append({"rule": "R1", "file": file, "contracts": [first, second],
                                   "functions": sorted(set(contract_groups[first] + contract_groups[second])),
                                   "independence": independent, "within": {first: group_affinity(contract_groups[first]), second: group_affinity(contract_groups[second])},
                                   "between": group_affinity(contract_groups[first], contract_groups[second]),
                                   "interpretation": "mixed-contract candidate; review implementation intervals and normal collaboration"})
        report.setdefault("responsibility_matrix", []).extend(
            {"file": file, "contract": contract, "implementation_functions": sorted(set(identities)),
             "body_tokens": sum(functions[x]["body_tokens"] for x in set(identities))}
            for contract, identities in sorted(contract_groups.items()))
    by_contract = defaultdict(set)
    for file, groups in distribution.items():
        for contract in groups:
            by_contract[contract].add(file)
    for contract, files in sorted(by_contract.items()):
        if len(files) > 1:
            identities = {identity for file in files for identity in distribution[file][contract]}
            candidates.append({"rule": "R2", "contract": contract, "files": sorted(files), "functions": sorted(identities),
                               "cross_calls": [row for row in report["calls"] if row["caller"] in identities and row["callee"] in identities and
                                               functions[row["caller"]]["file"] != functions[row["callee"]]["file"]],
                               "interpretation": "implementation distributed across files; not necessarily excessive scattering"})
    for state_id, state in report["states"].items():
        observed = state_accesses[state_id]
        readers = sorted({row["function"] for row in observed if row["mode"] in {"read", "read_write"}})
        writers = sorted({row["function"] for row in observed if row["mode"] in {"write", "read_write"}})
        possible = sorted({row["function"] for row in observed if row["mode"] in {"may_write", "escape", "unknown"}})
        state.update(readers=readers, writers=writers, possible_writers=possible)
        contracts_used = sorted({contract for identity in writers for contract in roles[identity]})
        owner = next((anchor["contract"] for anchor in rules.get("anchors", []) if anchor["kind"] == "state" and anchor.get("ownership") and
                      (not anchor.get("file_prefixes") or all(any(functions[x]["file"].startswith(p) for p in anchor["file_prefixes"]) for x in writers)) and
                      matches(anchor, symbols.get(state["declaration"], {}).get("qualified_name", ""))), None)
        state["owner_contract"] = owner
        if len(writers) > 1 and (len(contracts_used) > 1 or not owner):
            candidates.append({"rule": "R3", "state": state_id, "functions": writers, "contracts": contracts_used,
                               "precision": state["precision"], "owner": owner,
                               "interpretation": "multiple direct writers; symbolic receiver is not a proven shared instance"})
        files = sorted({functions[x]["file"] for x in readers + writers + possible if x in functions})
        if len(files) > 1:
            report["cross_fragment_states"].append({"state": state_id, "files": files, "writers": writers, "possible_writers": possible})
    for identity in sorted(entries):
        if functions[identity]["body_role"] == "implementation" and len(roles[identity]) > 1:
            candidates.append({"rule": "R5", "function": identity, "contracts": sorted(roles[identity]),
                               "evidence": functions[identity]["body"], "interpretation": "entry has inline implementation anchors from multiple contracts"})
    report["complexity_ranking"] = [row["id"] for row in sorted(functions.values(), key=lambda f: (-f["decision_count"], -f["max_nesting"], -f["body_tokens"], f["id"]))]
    for row in report["calls"]:
        if row["resolution"] == "direct" and row["caller"] in functions and row["callee"] in functions and functions[row["caller"]]["file"] != functions[row["callee"]]["file"]:
            report["cross_fragment_calls"].append(row)
    # Unnamed connected groups are navigation aids, not automatically inferred semantic roles.
    unclassified = set(report["unclassified"])
    adjacency = defaultdict(set)
    for row in report["calls"]:
        a, b = row["caller"], row["callee"]
        if row["resolution"] == "direct" and a in unclassified and b in unclassified and a not in entries and b not in entries and not any(re.fullmatch(pattern, row["callee_name"]) for pattern in utility_patterns):
            adjacency[a].add(b); adjacency[b].add(a)
    state_users = defaultdict(set)
    for identity in unclassified - entries:
        for feature in features[identity]:
            if feature[1] != "call":
                state_users[feature].add(identity)
    for users in state_users.values():
        for a, b in combinations(sorted(users), 2):
            adjacency[a].add(b); adjacency[b].add(a)
    while unclassified:
        seed = min(unclassified); component, pending = set(), [seed]
        while pending:
            node = pending.pop()
            if node in component:
                continue
            component.add(node); pending += list(adjacency[node] - component)
        unclassified -= component
        report["structural_groups"].append(sorted(component))
    report["metrics"] = {"functions": len(functions), "classified_functions": len(functions)-len(report["unclassified"]),
                         "role_resolution_rate": (len(functions)-len(report["unclassified"]))/len(functions) if functions else None,
                         "role_anchor_scope": dict(sorted(anchor_scope.items())),
                         "direct_calls": sum(row["resolution"] == "direct" for row in report["calls"]),
                         "unresolved_calls": sum(row["resolution"] != "direct" for row in report["calls"]),
                         "accesses": len(report["accesses"]),
                         "unknown_accesses": sum(row["mode"] == "unknown" for row in report["accesses"]),
                         "symbolic_accesses": sum(row["target"] is not None and row["target"]["precision"] != "declaration-origin" for row in report["accesses"]),
                         "states": len(report["states"]), "candidates_by_rule": dict(Counter(row["rule"] for row in candidates))}
    for candidate in candidates:
        candidate["id"] = stable_id(candidate)
        candidate["review"] = "unreviewed"
