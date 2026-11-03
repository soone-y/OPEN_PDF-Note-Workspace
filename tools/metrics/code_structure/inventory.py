"""Git-visible physical source inventory, explicit groups and byte partitions.

No filesystem writes. Includes tracked and nonignored untracked sources. Inputs
are frozen and rechecked by the survey owner, including the Git file selection.
"""
from __future__ import annotations

from collections import defaultdict
from pathlib import Path
import re
import subprocess

from .inputs import SOURCE_SUFFIXES
from .model import AnalysisError, digest_bytes, stable_id
from .literals import KINDS, QUOTED, RAW_STRING

LANGUAGES = {**dict.fromkeys(SOURCE_SUFFIXES, "C/C++"), ".py": "Python", ".ps1": "PowerShell",
             ".js": "JavaScript", ".mjs": "JavaScript", ".ts": "TypeScript", ".tsx": "TypeScript",
             ".html": "HTML", ".css": "CSS", ".sh": "Shell", ".bat": "Batch", ".cmd": "Batch",
             ".json": "JSON", ".xml": "XML", ".yml": "YAML", ".yaml": "YAML", ".toml": "TOML",
             ".rc": "Windows resources", ".manifest": "XML", ".cmake": "CMake", ".rs": "Rust"}
WORD = re.compile(r"[A-Za-z_\u0080-\uffff][\w]*|(?:\d|\.[0-9])(?:[eEpP][+-]|[\w.'])*")


def language(path):
    return "CMake" if Path(path).name == "CMakeLists.txt" else LANGUAGES.get(Path(path).suffix.lower())


def git_sources(root):
    result = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                            capture_output=True, timeout=30,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode:
        raise AnalysisError("Git source selection failed: " + result.stderr.decode("utf-8", errors="replace"))
    return sorted({path for path in result.stdout.decode("utf-8", errors="strict").split("\0") if path and language(path)})


def select_group(path, groups):
    matches = [row for row in groups if path == row["path"] or path.startswith(row["path"].rstrip("/") + "/") or row["path"] == "."]
    return max(matches, key=lambda row: len(row["path"])) if matches else {"id": "unassigned", "purpose": "unassigned path group"}


def cpp_bytes(text):
    """Byte partition, preserving strings/raw strings. Lexical, not build participation."""
    parts = defaultdict(int)
    literal_bytes = dict.fromkeys(KINDS, 0)
    literal_counts = dict.fromkeys(KINDS, 0)
    strings, string_payload_bytes, byte_offset = [], 0, 0
    i, n = 0, len(text)
    tokens, directives = 0, 0
    while i < n:
        start = i
        category = "code"
        literal_type, payload = None, None
        if text[i].isspace() or text[i] == "\ufeff":
            while i < n and (text[i].isspace() or text[i] == "\ufeff"):
                i += 1
            category = "whitespace"
        elif text.startswith("//", i):
            end = text.find("\n", i)
            while end != -1 and text[max(0, end-2):end].rstrip("\r").endswith("\\"):
                end = text.find("\n", end+1)
            i = n if end == -1 else end
            category = "comment"
        elif text.startswith("/*", i):
            end = text.find("*/", i+2)
            if end == -1:
                raise AnalysisError("unterminated C/C++ block comment")
            i = end+2
            category = "comment"
        else:
            raw = RAW_STRING.match(text[i:i+24])
            quoted = QUOTED.match(text[i:i+4])
            if raw:
                terminal = ")" + raw[1] + '"'
                end = text.find(terminal, i+raw.end())
                if end == -1:
                    raise AnalysisError("unterminated C++ raw string")
                payload = text[i+raw.end():end]
                i = end+len(terminal)
                category = "literal"
                literal_type = "string"
            elif quoted:
                quote = text[i+quoted.end()-1]
                i += quoted.end()
                payload_start = i
                while i < n and text[i] != quote:
                    i += 2 if text[i] == "\\" else 1
                if i >= n:
                    raise AnalysisError("unterminated C/C++ quoted literal")
                payload = text[payload_start:i]
                i += 1
                category = "literal"
                literal_type = "string" if quote == '"' else "character"
            else:
                word = WORD.match(text, i)
                i = word.end() if word else i+1
                if word and (text[start].isdigit() or text[start] == "."):
                    category = "literal"
                    literal_type = "number"
                if text[start] == "#" and not text[text.rfind("\n", 0, start)+1:start].strip():
                    directives += 1
            tokens += 1
        source = text[start:i].encode("utf-8")
        size = len(source)
        parts[category] += size
        if literal_type:
            literal_bytes[literal_type] += size
            literal_counts[literal_type] += 1
            if literal_type == "string":
                payload_size = len(payload.encode("utf-8"))
                string_payload_bytes += payload_size
                strings.append({"start_byte": byte_offset, "end_byte": byte_offset+size,
                                "source_bytes": size, "payload_source_bytes": payload_size,
                                "sha256": digest_bytes(source), "purpose": "unclassified"})
        byte_offset += size
    if sum(literal_bytes.values()) != parts["literal"]:
        raise AnalysisError("literal byte partition mismatch")
    return {"bytes": {key: parts[key] for key in ("code", "literal", "comment", "whitespace")},
            "literal_bytes": literal_bytes, "literal_counts": literal_counts,
            "string_payload_source_bytes": string_payload_bytes, "strings": strings,
            "string_purpose": {"status": "UNCLASSIFIED", "source_byte_denominator": literal_bytes["string"],
                               "unclassified_source_bytes": literal_bytes["string"]},
            "lexical_items": tokens, "preprocessor_directives": directives,
            "method": "physical-utf8-byte-partition/v2; lexical items are not Clang tokens"}


def build_inventory(root, paths, groups, snapshot):
    rows = []
    for path in paths:
        target = (root / path).resolve()
        if not target.is_relative_to(root):
            raise AnalysisError("source path escapes repository: " + path)
        raw = snapshot.capture(target, text=True)
        text = raw.decode("utf-8", errors="strict")
        group = select_group(path, groups)
        lang = language(path)
        suffix = target.suffix.lower()
        form = "include-fragment" if suffix in {".cppinc", ".inc", ".inl", ".ipp"} else "header" if suffix in {".h", ".hpp", ".hh", ".hxx"} else "translation-unit" if suffix in {".cpp", ".cc", ".cxx", ".c"} else "resource-or-configuration" if lang in {"JSON", "XML", "YAML", "TOML", "Windows resources"} else "script-or-other-source"
        row = {"path": path, "sha256": digest_bytes(raw), "bytes": len(raw), "language": lang,
               "group": group["id"], "purpose": group["purpose"], "form": form,
               "path_module": "/".join(Path(path).parts[:2]) if len(Path(path).parts) > 2 else Path(path).parts[0]}
        if lang == "C/C++":
            row["cpp_lexical"] = cpp_bytes(text)
            if sum(row["cpp_lexical"]["bytes"].values()) != len(raw):
                raise AnalysisError("C/C++ byte partition mismatch: " + path)
        rows.append(row)
    if not rows:
        raise AnalysisError("required source inventory is empty")
    total = sum(row["bytes"] for row in rows)
    summary = {}
    for key in ("language", "group", "form", "path_module"):
        buckets = defaultdict(list)
        for row in rows:
            buckets[row[key]].append(row)
        summary[key] = [{"name": name, "files": len(values), "bytes": sum(x["bytes"] for x in values),
                         "file_share": len(values)/len(rows), "byte_share": sum(x["bytes"] for x in values)/total if total else None}
                        for name, values in sorted(buckets.items())]
    cpp = [row["cpp_lexical"] for row in rows if "cpp_lexical" in row]
    content = {"method": "cpp-physical-content-separation/v1", "cpp_files": len(cpp),
               "cpp_byte_denominator": sum(sum(row["bytes"].values()) for row in cpp),
               "bytes": {key: sum(row["bytes"][key] for row in cpp) for key in ("code", "literal", "comment", "whitespace")},
               "literal_bytes": {key: sum(row["literal_bytes"][key] for row in cpp) for key in KINDS},
               "literal_counts": {key: sum(row["literal_counts"][key] for row in cpp) for key in KINDS},
               "string_payload_source_bytes": sum(row["string_payload_source_bytes"] for row in cpp),
               "string_purpose_status": "UNCLASSIFIED" if cpp else "NOT_APPLICABLE"}
    return {"status": "COMPLETE", "method": "git-visible-source-physical/v2", "files": rows,
            "cpp_content": content,
            "file_denominator": len(rows), "byte_denominator": total, "breakdowns": summary,
            "selection_id": stable_id(paths),
            "limits": ["Includes physical Git-visible sources/configuration; excludes ignored runtime/build artifacts and documentation.",
                       "Byte shares describe stored source, not effort, executable size or runtime cost.",
                       "Path purposes are explicit plan labels, not inferred semantic responsibilities.",
                       "C/C++ lexical partition includes inactive branches; it is not parsed-body coverage.",
                       "String payload counts encoded source bytes, including escapes, not decoded display characters.",
                       "Strings, character/number literals and remaining code are lexical categories; purposes and semantic logic/data boundaries remain unclassified."]}
