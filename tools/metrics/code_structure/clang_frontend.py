"""Two-pass libclang frontend: discover dependencies, then parse fixed buffers."""
from __future__ import annotations

import ctypes
from pathlib import Path

from .function_metrics import FUNCTION_KINDS, measure
from .model import AnalysisError, digest_bytes, display_path, stable_id
from .state_access import AccessExtractor


class Frontend:
    def __init__(self, index, command, root, scopes, snapshot):
        self.index, self.command, self.root = index, command, root
        self.scopes, self.snapshot = scopes, snapshot
        self.config_id = stable_id({k: command.get(k) for k in
                                   ("directory", "arguments", "compiler", "compiler_version", "compiler_sha256", "compiler_predefines_sha256", "runtime_id", "adapter", "adapter_driver_hash")})
        self.tu_id = stable_id([self.config_id, command["file"]])
        self.symbols, self.states, self.functions = {}, {}, []
        self.symbol_cache = {}
        self.calls, self.accesses, self.definitions, self.diagnostics = [], [], [], []
        self.function_references = []
        self.current_function, self.current_returns_reference = "", False
        self.included_files = set()

    def selected(self, file):
        if not file:
            return False
        path = Path(str(file)).resolve()
        return any(path == scope or path.is_relative_to(scope) for scope in self.scopes)

    def expression_text(self, cursor):
        file = cursor.extent.start.file
        if file:
            data = self.snapshot.buffers.get(str(Path(str(file)).resolve()))
            if data is not None and cursor.extent.end.offset - cursor.extent.start.offset < 1000:
                return data[cursor.extent.start.offset:cursor.extent.end.offset].decode("utf-8", errors="strict")
        return "unknown-expression@" + str(cursor.location.line)

    def location(self, location):
        return {"path": display_path(str(location.file), self.root) if location.file else None,
                "line": location.line, "column": location.column, "offset": location.offset}

    def evidence(self, cursor):
        from clang import cindex
        expansion = self.location(cursor.location)
        # Cursor locations are expansion locations; preserve spelling locations via the C API.
        file_ptr, line, column, offset = ctypes.c_void_p(), ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint()
        cindex.conf.lib.clang_getSpellingLocation(cursor.location, ctypes.byref(file_ptr),
                                                 ctypes.byref(line), ctypes.byref(column), ctypes.byref(offset))
        spelling = {"path": None, "line": line.value, "column": column.value, "offset": offset.value}
        if file_ptr.value:
            native_file = cindex.File(ctypes.cast(file_ptr, cindex.c_object_p))
            spelling["path"] = display_path(native_file.name, self.root)
        return {"expansion": expansion, "spelling": spelling,
                "start": self.location(cursor.extent.start), "end": self.location(cursor.extent.end),
                "tu": self.tu_id, "config": self.config_id, "method": "libclang-AST"}

    def symbol(self, cursor):
        if cursor in self.symbol_cache:
            return self.symbol_cache[cursor]
        kind = cursor.kind.name
        parent = cursor.semantic_parent
        parent_kind = parent.kind.name if parent else ""
        # A static forward declaration in the root and definition in .cppinc
        # can expose different USRs. Bind every redeclaration/reference through
        # the canonical declaration, retaining the observed definition evidence.
        canonical = cursor.canonical if kind in FUNCTION_KINDS else cursor
        usr = canonical.get_usr() or "location:" + str(canonical.location.file) + ":" + str(canonical.location.offset)
        linkage = canonical.linkage.name
        scope = "instance_field" if kind == "FIELD_DECL" else "symbol"
        if kind == "PARM_DECL":
            scope = "parameter"
        if kind == "VAR_DECL":
            if parent_kind in FUNCTION_KINDS:
                scope = "static_local" if cursor.storage_class.name == "STATIC" else "local"
            elif parent_kind in {"CLASS_DECL", "STRUCT_DECL", "CLASS_TEMPLATE"}:
                scope = "static_member"
            else:
                scope = "global"
        key = [self.config_id, usr]
        if linkage in {"INTERNAL", "NO_LINKAGE"} or scope in {"local", "parameter", "static_local"}:
            key += [self.tu_id]
        symbol_id = stable_id(key)
        if symbol_id not in self.symbols:
            names, ancestor = [cursor.spelling or "<anonymous>"], parent
            while ancestor and ancestor.kind.name != "TRANSLATION_UNIT":
                if ancestor.spelling:
                    names.append(ancestor.spelling)
                ancestor = ancestor.semantic_parent
            declaration = {"id": symbol_id, "usr": usr, "name": cursor.spelling,
                           "qualified_name": "::".join(reversed(names)), "kind": kind,
                           "type": cursor.type.spelling, "linkage": linkage, "scope": scope,
                           "const": cursor.type.is_const_qualified(),
                           "pointee_const": cursor.type.get_pointee().is_const_qualified(),
                           "evidence": self.evidence(cursor)}
            definition = cursor.get_definition()
            declaration["definition"] = self.evidence(definition) if definition else None
            self.symbols[symbol_id] = declaration
        self.symbol_cache[cursor] = self.symbols[symbol_id]
        return self.symbols[symbol_id]

    def parse(self):
        from clang import cindex
        source = self.command["file"]
        if Path(source).suffix.lower() not in {".cpp", ".cc", ".cxx"}:
            raise AnalysisError(f"parse a C++ TU, not an include fragment: {source}")
        self.snapshot.capture(source, text=True)
        args = self.command["arguments"] + ["-working-directory=" + self.command["directory"]]
        options = cindex.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD
        discovery = self.index.parse(source, args=args, unsaved_files=self.snapshot.unsaved(), options=options)
        dependencies = {str(Path(inc.include.name).resolve()) for inc in discovery.get_includes()}
        dependencies.add(str(Path(source).resolve()))
        for path in sorted(dependencies):
            self.snapshot.capture(path, text=True)
        unit = self.index.parse(source, args=args, unsaved_files=self.snapshot.unsaved(), options=options)
        actual = {str(Path(inc.include.name).resolve()) for inc in unit.get_includes()}
        actual.add(str(Path(source).resolve()))
        if not actual.issubset(dependencies):
            raise AnalysisError("include graph changed between discovery and fixed-buffer parse")
        self.included_files = actual
        for diagnostic in unit.diagnostics:
            self.diagnostics.append({"severity": diagnostic.severity, "message": str(diagnostic),
                                     "tu": self.tu_id, "config": self.config_id})
        if any(d["severity"] >= cindex.Diagnostic.Error for d in self.diagnostics):
            raise AnalysisError("required translation unit has parse errors: " + source)
        self.unit = unit
        self.discover(unit.cursor)
        extractor = AccessExtractor(self)
        extractor.build_summaries(self.definitions)
        for cursor, body, record in self.definitions:
            self.current_function = record["id"]
            self.current_returns_reference = cursor.kind.name != "LAMBDA_EXPR" and "&" in cursor.result_type.spelling
            extractor.extract(body, record["id"])
            self.extract_calls(body, record["id"])
            record["types"] = sorted(self.used_types(cursor))
        return extractor.summaries

    def used_types(self, cursor):
        found = set()
        def walk(node):
            if node != cursor and node.kind.name in FUNCTION_KINDS:
                return
            if node.kind.name == "TYPE_REF" and node.referenced:
                found.add(self.symbol(node.referenced)["qualified_name"])
            for child in node.get_children():
                walk(child)
        walk(cursor)
        return found

    def discover(self, cursor, owner=None):
        for child in cursor.get_children():
            if not self.selected(child.location.file):
                # Implicit lambda-to-function-pointer conversion inserts a
                # locationless MEMBER_REF_EXPR above the source lambda. Such
                # wrappers have no own source evidence, but their descendants
                # still need the normal selection check. External-file nodes
                # remain excluded, and no locationless observation is emitted.
                if child.location.file is None:
                    self.discover(child, owner)
                continue
            kind = child.kind.name
            child_owner = owner
            if kind in FUNCTION_KINDS:
                child_owner = self.symbol(child)["id"]
                bodies = [ch for ch in child.get_children() if ch.kind.name == "COMPOUND_STMT"]
                if bodies:
                    symbol = self.symbol(child)
                    record = {"id": symbol["id"], "usr": symbol["usr"], "name": symbol["name"],
                              "qualified_name": symbol["qualified_name"], "kind": kind,
                              "file": display_path(str(child.location.file), self.root),
                              "tu": self.tu_id, "config": self.config_id,
                              "type": symbol["type"],
                              "linkage": symbol["linkage"],
                              "body_sha256": digest_bytes(self.snapshot.buffers[str(Path(str(bodies[0].extent.start.file)).resolve())][bodies[0].extent.start.offset:bodies[0].extent.end.offset]),
                              "evidence": self.evidence(child), "body": self.evidence(bodies[0]),
                              **measure(bodies[0])}
                    self.functions.append(record)
                    self.definitions.append((child, bodies[0], record))
            if kind in {"VAR_DECL", "FIELD_DECL", "PARM_DECL"}:
                self.symbol(child)
            if kind in {"DECL_REF_EXPR", "MEMBER_REF_EXPR"}:
                reference = child.referenced
                if reference and reference.kind.name in FUNCTION_KINDS:
                    target = self.symbol(reference)
                    self.function_references.append({"owner": owner, "target": target["id"],
                                                     "target_name": target["qualified_name"], "kind": kind,
                                                     "interpretation": "AST reference; includes direct call operands and address/callback uses",
                                                     "evidence": self.evidence(child)})
            self.discover(child, child_owner)

    def extract_calls(self, body, function_id):
        def walk(cursor):
            kind = cursor.kind.name
            if kind in FUNCTION_KINDS:
                return
            if kind == "CALL_EXPR":
                ref = cursor.referenced
                virtual = bool(ref and ref.kind.name == "CXX_METHOD" and ref.is_virtual_method())
                resolved = bool(ref and ref.kind.name in FUNCTION_KINDS and not virtual)
                symbol = self.symbol(ref) if ref else None
                self.calls.append({"caller": function_id, "callee": symbol["id"] if symbol else None,
                                   "callee_name": symbol["qualified_name"] if symbol else cursor.spelling,
                                   "resolution": "direct" if resolved else "virtual-candidate" if virtual else "unresolved",
                                   "evidence": self.evidence(cursor)})
            for child in cursor.get_children():
                walk(child)
        walk(body)
