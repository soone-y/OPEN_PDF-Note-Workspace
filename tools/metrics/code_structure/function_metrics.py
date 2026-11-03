"""Versioned syntactic metrics; these are not CFG cyclomatic complexity."""
from __future__ import annotations

from collections import Counter
from .model import stable_id
from .literals import KINDS, literal_kind

FUNCTION_KINDS = {"FUNCTION_DECL", "CXX_METHOD", "CONSTRUCTOR", "DESTRUCTOR", "CONVERSION_FUNCTION", "FUNCTION_TEMPLATE", "LAMBDA_EXPR"}
DECISIONS = {"IF_STMT", "FOR_STMT", "CXX_FOR_RANGE_STMT", "WHILE_STMT", "DO_STMT", "CASE_STMT", "CXX_CATCH_STMT", "CONDITIONAL_OPERATOR"}
NESTING = DECISIONS - {"CASE_STMT", "CONDITIONAL_OPERATOR"} | {"SWITCH_STMT", "CXX_TRY_STMT"}


def operator(cursor):
    name = cursor.kind.name
    valid = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", "&&", "||", "+", "-", "*", "/", "%", "&", "|", "^", "==", "!=", "<", ">", "<=", ">=", "<<", ">>"}
    if cursor.spelling in valid and name in {"BINARY_OPERATOR", "COMPOUND_ASSIGNMENT_OPERATOR"}:
        return cursor.spelling
    children = list(cursor.get_children())
    if name in {"BINARY_OPERATOR", "COMPOUND_ASSIGNMENT_OPERATOR"} and len(children) == 2:
        # Only tokens between actual child ranges, not a search through the expression.
        start, end = children[0].extent.end.offset, children[1].extent.start.offset
        tokens = [t.spelling for t in cursor.get_tokens() if start <= t.location.offset < end]
        return tokens[0] if len(tokens) == 1 and tokens[0] in valid else None
    if name == "UNARY_OPERATOR" and len(children) == 1:
        extent = children[0].extent
        tokens = [t.spelling for t in cursor.get_tokens()
                  if t.location.offset < extent.start.offset or t.location.offset >= extent.end.offset]
        return tokens[0] if len(tokens) == 1 else None
    return None


def measure(body):
    decisions, maximum, unresolved = 0, 0, 0
    forwarding_nodes = {"COMPOUND_STMT", "RETURN_STMT", "CALL_EXPR", "UNEXPOSED_EXPR", "PAREN_EXPR", "DECL_REF_EXPR", "MEMBER_REF_EXPR", "TYPE_REF", "NAMESPACE_REF", "INTEGER_LITERAL", "STRING_LITERAL", "CXX_NULL_PTR_LITERAL_EXPR", "CXX_THIS_EXPR", "CXX_BOOL_LITERAL_EXPR"}
    forwarding = True
    excluded_ranges = []
    constructs = Counter()

    def walk(cursor, depth=0, else_if=False):
        nonlocal decisions, maximum, unresolved, forwarding
        kind = cursor.kind.name
        constructs[kind] += 1
        if kind not in forwarding_nodes:
            forwarding = False
        if kind in FUNCTION_KINDS:
            excluded_ranges.append((str(cursor.extent.start.file), cursor.extent.start.offset, cursor.extent.end.offset))
            return
        if kind in DECISIONS:
            decisions += 1
        if kind == "BINARY_OPERATOR":
            op = operator(cursor)
            if op in {"&&", "||"}:
                decisions += 1
            elif op is None:
                unresolved += 1
        increment = kind in NESTING and not else_if
        current = depth + int(increment)
        maximum = max(maximum, current)
        children = list(cursor.get_children())
        for index, child in enumerate(children):
            # Else child is last and follows a then-statement; else-if chains stay flat.
            chain = False
            if kind == "IF_STMT" and child.kind.name == "IF_STMT" and index == len(children)-1 and len(children) >= 3:
                gap_start, gap_end = children[index-1].extent.end.offset, child.extent.start.offset
                chain = any(token.spelling == "else" and gap_start <= token.location.offset < gap_end
                            for token in cursor.get_tokens())
            walk(child, current, chain)

    walk(body)
    all_tokens = [token for token in body.get_tokens() if token.kind.name != "COMMENT"]
    tokens = [token for token in all_tokens if not any(
        str(token.location.file) == file and start <= token.location.offset < end
        for file, start, end in excluded_ranges)]
    # Clone fingerprints preserve nested bodies, even though parent metrics exclude them.
    lexical = [token.spelling for token in all_tokens]
    normalized = ["<identifier>" if token.kind.name == "IDENTIFIER" else token.spelling for token in all_tokens]
    literals = dict.fromkeys(KINDS, 0)
    for token in tokens:
        if token.kind.name == "LITERAL":
            literals[literal_kind(token.spelling)] += 1
    partition = {"method": "clang-body-literal-partition/v1", "body_token_denominator": len(tokens),
                 "literal_tokens": literals, "non_literal_tokens": len(tokens)-sum(literals.values()),
                 "interpretation": "syntax/identifier tokens excluding literals; not semantic logic quantity"}
    return {"decision_count": decisions, "max_nesting": maximum,
            "body_tokens": len(tokens), "body_lines": body.extent.end.line - body.extent.start.line + 1,
            "body_token_partition": partition,
            "unresolved_binary_operators": unresolved, "forwarding_body": forwarding,
            "constructs": dict(sorted(constructs.items())),
            "fingerprint_tokens": len(all_tokens),
            "lexical_body_hash": stable_id(lexical), "normalized_body_hash": stable_id(normalized)}
