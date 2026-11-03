"""Direct accesses and bounded reference-return summaries, with symbolic objects."""
from __future__ import annotations

from .function_metrics import FUNCTION_KINDS, operator
from .model import stable_id

WRAPPERS = {"UNEXPOSED_EXPR", "PAREN_EXPR"}
STATE_KINDS = {"VAR_DECL", "FIELD_DECL", "PARM_DECL"}


class AccessExtractor:
    def __init__(self, frontend):
        self.frontend = frontend
        self.summaries = {}
        self.aliases = {}

    def target(self, expression, stack=()):
        f = self.frontend
        kind = expression.kind.name
        children = list(expression.get_children())
        if kind in WRAPPERS and len(children) == 1:
            return self.target(children[0], stack)
        if kind == "DECL_REF_EXPR" and expression.referenced and expression.referenced.kind.name in STATE_KINDS:
            ref = expression.referenced
            symbol = f.symbol(ref)
            if symbol["id"] in self.aliases:
                return dict(self.aliases[symbol["id"]])
            return {"declaration": symbol["id"], "origin": symbol["id"], "path": [],
                    "context": None, "precision": "declaration-origin",
                    "shared": symbol["scope"] not in {"local", "parameter"}}
        if kind == "CXX_THIS_EXPR":
            return {"declaration": None, "origin": "this:" + f.current_function,
                    "path": [], "context": None, "precision": "symbolic", "shared": True}
        if kind == "MEMBER_REF_EXPR" and expression.referenced:
            field = f.symbol(expression.referenced)
            base = self.target(children[0], stack) if children else None
            base = base or {"declaration": None, "origin": "unknown-receiver",
                            "path": [], "context": None, "precision": "unknown", "shared": True}
            if base["declaration"] and f.symbols.get(base["declaration"], {}).get("scope") == "parameter":
                base = {**base, "precision": "symbolic-parameter", "shared": True}
            return {**base, "declaration": field["id"], "path": base["path"] + [field["id"]],
                    "shared": base["shared"] or base["precision"] != "declaration-origin"}
        if kind == "ARRAY_SUBSCRIPT_EXPR" and children:
            base = self.target(children[0], stack)
            if base:
                return {**base, "path": base["path"] + ["index:" + f.expression_text(children[-1])],
                        "precision": "symbolic", "shared": True}
        if kind == "UNARY_OPERATOR" and children and operator(expression) == "*":
            base = self.target(children[0], stack)
            if base:
                return {**base, "origin": "pointee:" + base["origin"], "precision": "symbolic", "shared": True}
        if kind == "CALL_EXPR" and expression.referenced:
            callee = expression.referenced
            if "&" not in callee.result_type.spelling:
                return None
            symbol = f.symbol(callee)
            args = list(expression.get_arguments())
            key = symbol["id"]
            summary = self.summaries.get(key)
            if summary and key not in stack:
                result = dict(summary)
                if result["origin"].startswith("parameter:"):
                    position = int(result["origin"].split(":", 1)[1])
                    if position < len(args):
                        actual = self.target(args[position], stack + (key,))
                        if actual:
                            result = {**result, "origin": actual["origin"], "context": actual["context"],
                                      "shared": actual["shared"], "precision": actual["precision"],
                                      "path": actual["path"] + result["path"]}
                return result
            return {"declaration": None, "origin": "reference-call:" + key,
                    "path": [], "context": [f.expression_text(arg) for arg in args],
                    "precision": "symbolic", "shared": True}
        return None

    def build_summaries(self, definitions):
        # Iterate to propagate simple accessor chains, without pretending to solve arbitrary aliasing.
        for _ in range(4):
            for function, body, record in definitions:
                if function.kind.name == "LAMBDA_EXPR" or "&" not in function.result_type.spelling:
                    continue
                statements = list(body.get_children())
                if len(statements) != 1 or statements[0].kind.name != "RETURN_STMT":
                    continue
                values = list(statements[0].get_children())
                if len(values) != 1:
                    continue
                self.frontend.current_function = record["id"]
                target = self.target(values[0], (record["id"],))
                if target:
                    parameters = list(function.get_arguments() or [])
                    for position, parameter in enumerate(parameters):
                        if target["origin"] == self.frontend.symbol(parameter)["id"]:
                            target = {**target, "origin": "parameter:" + str(position), "shared": False}
                    self.summaries[record["id"]] = target

    def add(self, expression, function_id, mode, reason=None):
        target = self.target(expression)
        if not target:
            if mode in {"write", "read_write", "escape", "may_write"}:
                self.frontend.accesses.append({"function": function_id, "mode": "unknown",
                                               "reason": reason or "lvalue/alias not resolved",
                                               "evidence": self.frontend.evidence(expression), "target": None})
            return
        if not target["shared"]:
            return
        state_id = stable_id([self.frontend.config_id, target])
        self.frontend.states.setdefault(state_id, {"id": state_id, **target})
        self.frontend.accesses.append({"function": function_id, "mode": mode,
                                      "state": state_id, "target": target, "reason": reason,
                                      "evidence": self.frontend.evidence(expression)})

    def extract(self, body, function_id):
        self.frontend.current_function = function_id
        self.aliases = {}

        def walk(cursor, mode="read"):
            kind = cursor.kind.name
            if kind in FUNCTION_KINDS:
                return
            children = list(cursor.get_children())
            if kind in WRAPPERS and len(children) == 1:
                walk(children[0], mode); return
            if kind in {"BINARY_OPERATOR", "COMPOUND_ASSIGNMENT_OPERATOR"}:
                op = operator(cursor)
                if len(children) == 2 and op in {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}:
                    walk(children[0], "write" if op == "=" else "read_write")
                    walk(children[1]); return
                if op is None:
                    self.add(cursor, function_id, "write", "unresolved binary operator")
            if kind == "UNARY_OPERATOR" and children:
                op = operator(cursor)
                if op in {"++", "--", "&"}:
                    walk(children[0], "read_write" if op != "&" else "escape"); return
                if op == "*":
                    self.add(cursor, function_id, mode, "pointer dereference is symbolic")
                    walk(children[0]); return
            if kind == "VAR_DECL" and "&" in cursor.type.spelling and children:
                target = self.target(children[-1])
                if target:
                    self.aliases[self.frontend.symbol(cursor)["id"]] = target
            if kind in {"DECL_REF_EXPR", "MEMBER_REF_EXPR", "ARRAY_SUBSCRIPT_EXPR"}:
                self.add(cursor, function_id, mode)
                # The lvalue receiver/index can be read to locate an object; do not count its field twice.
                if kind == "ARRAY_SUBSCRIPT_EXPR":
                    for child in children[1:]:
                        walk(child)
                if kind == "MEMBER_REF_EXPR":
                    for child in children:
                        if child.kind.name == "CALL_EXPR":
                            walk(child)
                return
            if kind == "CALL_EXPR":
                callee = cursor.referenced
                target = self.target(cursor)
                if target:
                    self.add(cursor, function_id, mode, "reference-return summary or symbolic accessor")
                args = list(cursor.get_arguments())
                parameters = list(callee.get_arguments() or []) if callee else []
                for position, arg in enumerate(args):
                    param = parameters[position] if position < len(parameters) else None
                    parameter_type = param.type.spelling if param else ""
                    if param and ("&" in parameter_type or "*" in parameter_type):
                        pointee = param.type.get_pointee()
                        may_write = not pointee.is_const_qualified()
                        walk(arg, "may_write" if may_write else "read")
                    else:
                        walk(arg)
                if callee and callee.kind.name == "CXX_METHOD":
                    receiver = next((ch for ch in children if ch.kind.name == "MEMBER_REF_EXPR"), None)
                    if receiver:
                        bases = list(receiver.get_children())
                        if bases:
                            walk(bases[0], "read" if callee.is_const_method() else "may_write")
                if mode in {"write", "read_write"} and target is None:
                    self.add(cursor, function_id, "write", "unmodeled call-return lvalue or overloaded assignment")
                return
            if kind == "RETURN_STMT":
                for child in children:
                    walk(child, "escape" if self.frontend.current_returns_reference else "read")
                return
            for child in children:
                walk(child, mode)

        walk(body)
