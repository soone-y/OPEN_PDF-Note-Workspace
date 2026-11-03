"""Lexical literal kinds, never a semantic text/non-logic classification."""
from __future__ import annotations

import re

from .model import AnalysisError

RAW_STRING = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(')
QUOTED = re.compile(r'(?:u8|u|U|L)?["\']')
KINDS = ("string", "character", "number", "other")


def literal_kind(spelling):
    if RAW_STRING.match(spelling):
        return "string"
    quoted = QUOTED.match(spelling)
    if quoted:
        return "string" if spelling[quoted.end()-1] == '"' else "character"
    if re.match(r"(?:[0-9]|\.[0-9])", spelling):
        return "number"
    return "other"


def body_composition(rows):
    """Aggregate observed partitions; missing historical fields stay unmeasured."""
    partitions = []
    for row in rows:
        part = row.get("body_token_partition")
        if part is None:
            continue
        if not isinstance(part, dict) or not isinstance(part.get("literal_tokens"), dict):
            raise AnalysisError("function body literal partition malformed")
        values = part.get("literal_tokens", {})
        counts = [part.get("non_literal_tokens"), *values.values()]
        if (part.get("method") != "clang-body-literal-partition/v1" or set(values) != set(KINDS)
                or any(type(value) is not int or value < 0 for value in counts)
                or part.get("body_token_denominator") != row["body_tokens"]
                or sum(counts) != row["body_tokens"]):
            raise AnalysisError("function body literal partition inconsistent")
        partitions.append(part)
    complete = bool(rows) and len(partitions) == len(rows)
    return {"method": "clang-body-literal-partition/v1",
            "status": "COMPLETE" if complete else "PARTIAL" if partitions else "UNMEASURED",
            "function_denominator": len(rows), "measured_functions": len(partitions),
            "body_token_denominator": sum(row["body_tokens"] for row in rows) if complete else None,
            "non_literal_tokens": sum(part["non_literal_tokens"] for part in partitions) if complete else None,
            "literal_tokens": {kind: sum(part["literal_tokens"][kind] for part in partitions) if complete else None for kind in KINDS},
            "non_literal_values": None if not complete else [part["non_literal_tokens"] for part in partitions],
            "limits": ["String tokens may encode processing rules; purposes remain unclassified.",
                       "Remaining tokens include declarations, identifiers and punctuation; they are not semantic logic quantity."]}
