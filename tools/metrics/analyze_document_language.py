#!/usr/bin/env python3
"""Read-only local vocabulary observation for separately scoped document groups."""
from __future__ import annotations

import argparse
import fnmatch
import json
import math
import re
import sys
import unicodedata
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path
from typing import Iterable, Optional, Sequence


DEFAULT_GROUPS = (
    "internal=docs/internal",
    "public=docs/public",
    "for_ai=for_ai",
)
DEFAULT_EXTENSIONS = (".md", ".txt")
DEFAULT_EXCLUDES = (
    "**/LICENSE*", "**/COPYING*", "**/NOTICE*", "**/docs/internal/reports/**",
    "**/docs/internal/operations/publish_records/**",
)
# Particles and common function words are removed only from the simple tokenizer.
DEFAULT_STOP_WORDS = frozenset(
    "の に は を が と で へ も や から まで より する いる ある ため こと この その これ "
    "それ ここ ため よう また 及び および として について できる ます です ある ない "
    "な し との など 等 場合 とき もの すべて 一部 本 それぞれ なる なり なく して した "
    "される された されて している ください くださいました でき られる れる せる "
    "the a an and or of to in for on with by from is are be as this that it".split()
)
TOKEN_RE = re.compile(r"[A-Za-z][A-Za-z0-9_+.#/-]*|[0-9]+|[\u3400-\u9fff々〆〤]+|[\u3041-\u3096]+|[\u30a1-\u30faー]+")
HEADING_RE = re.compile(r"^\s{0,3}#{1,6}\s+(?P<text>.+?)\s*#*\s*$")
FENCE_RE = re.compile(r"^\s*(`{3,}|~{3,})")
LINK_RE = re.compile(r"\[([^\]]+)\]\([^)]*\)")


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Observe document vocabulary locally without modifying source documents or using network access."
    )
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[2]), help="Repository root.")
    parser.add_argument(
        "--group", action="append", metavar="NAME=PATH",
        help="Document group to analyze. Repeatable; defaults to internal, public, and for_ai.",
    )
    parser.add_argument(
        "--extension", action="append", metavar=".EXT",
        help="Included file extension. Repeatable; defaults to .md and .txt.",
    )
    parser.add_argument(
        "--exclude", action="append", metavar="GLOB",
        help="Repository-relative glob to exclude. Repeatable; defaults exclude licenses and internal reports.",
    )
    parser.add_argument("--stop-word", action="append", default=[], help="Additional normalized stop word.")
    parser.add_argument("--stop-word-file", help="UTF-8 file containing one additional stop word per line.")
    parser.add_argument("--top", type=int, default=30, help="Top entries per n-gram size. Defaults to 30.")
    parser.add_argument("--theme-top", type=int, default=20, help="Top theme candidates. Defaults to 20.")
    parser.add_argument("--theme-min-docs", type=int, default=2, help="Minimum distinct documents for a theme candidate. Defaults to 2.")
    parser.add_argument("--theme-min-groups", type=int, default=2, help="Minimum document groups for a shared theme candidate. Defaults to 2.")
    parser.add_argument("--theme-min-ngram", type=int, choices=(1, 2, 3), default=2, help="Minimum n-gram size for theme candidates. Defaults to 2 to avoid generic one-word labels.")
    parser.add_argument("--include-quoted-lines", action="store_true", help="Include Markdown block quotes; they are excluded by default.")
    parser.add_argument("--include-code-blocks", action="store_true", help="Include fenced code blocks; they are excluded by default.")
    parser.add_argument("--format", choices=("text", "json", "md"), default="text", help="Output format.")
    parser.add_argument("--report", help="Optional new report path. Refuses to overwrite an existing file.")
    return parser.parse_args(argv)


def parse_groups(values: Optional[Sequence[str]]) -> list[tuple[str, Path]]:
    groups: list[tuple[str, Path]] = []
    names: set[str] = set()
    for value in values or DEFAULT_GROUPS:
        if "=" not in value:
            raise ValueError(f"group must be NAME=PATH: {value}")
        name, raw_path = value.split("=", 1)
        name = name.strip()
        if not name or not raw_path.strip() or name in names:
            raise ValueError(f"group name must be non-empty and unique: {value}")
        names.add(name)
        groups.append((name, Path(raw_path.strip())))
    return groups


def normalize_token(value: str) -> str:
    return unicodedata.normalize("NFKC", value).casefold()


def read_stop_words(args: argparse.Namespace, root: Path) -> set[str]:
    words = set(DEFAULT_STOP_WORDS)
    words.update(normalize_token(value) for value in args.stop_word if value.strip())
    if args.stop_word_file:
        path = Path(args.stop_word_file)
        if not path.is_absolute():
            path = root / path
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            value = line.strip()
            if value and not value.startswith("#"):
                words.add(normalize_token(value))
    return words


def path_is_excluded(relative_path: str, patterns: Iterable[str]) -> bool:
    candidate = relative_path.replace("\\", "/")
    return any(fnmatch.fnmatch(candidate, pattern) or fnmatch.fnmatch("/" + candidate, pattern) for pattern in patterns)


def collect_files(root: Path, relative_dir: Path, extensions: set[str], excludes: Sequence[str]) -> list[Path]:
    directory = (root / relative_dir).resolve() if not relative_dir.is_absolute() else relative_dir.resolve()
    if not directory.is_dir():
        raise ValueError(f"group directory is not a directory: {directory}")
    try:
        directory.relative_to(root.resolve())
    except ValueError as exc:
        raise ValueError(f"group directory must be inside root: {directory}") from exc
    files: list[Path] = []
    for path in sorted(directory.rglob("*")):
        if not path.is_file() or path.suffix.casefold() not in extensions:
            continue
        try:
            relative = path.resolve().relative_to(root.resolve()).as_posix()
        except ValueError:
            relative = path.as_posix()
        if not path_is_excluded(relative, excludes):
            files.append(path)
    return files


def tokenize(text: str, stop_words: set[str]) -> list[str]:
    tokens: list[str] = []
    previous_was_kanji = False
    for raw in TOKEN_RE.findall(unicodedata.normalize("NFKC", text)):
        token = raw.casefold()
        is_hiragana = bool(re.fullmatch(r"[\u3041-\u3096]+", token))
        is_kanji = bool(re.fullmatch(r"[\u3400-\u9fff々〆〤]+", token))
        if token in stop_words:
            previous_was_kanji = False
            continue
        if is_hiragana and previous_was_kanji:
            tokens[-1] += token
        else:
            tokens.append(token)
        previous_was_kanji = is_kanji
    return tokens


def iter_text_segments(path: Path, text: str, include_quoted_lines: bool, include_code_blocks: bool) -> Iterable[tuple[int, str, bool]]:
    """Yield prose one source line at a time so n-grams never span unrelated lines."""
    in_fence = False
    in_front_matter = path.suffix.casefold() == ".md" and text.startswith("---\n")
    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if in_front_matter:
            if line_number > 1 and line in {"---", "..."}:
                in_front_matter = False
            continue
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if in_fence and not include_code_blocks:
            continue
        if line.startswith(">") and not include_quoted_lines:
            continue
        heading_match = HEADING_RE.match(line) if path.suffix.casefold() == ".md" else None
        content = heading_match.group("text") if heading_match else line
        content = LINK_RE.sub(r"\1", content)
        if content:
            yield line_number, content, heading_match is not None


def make_location(relative: str, line: int) -> dict[str, object]:
    return {"file": relative, "line": line}


def join_ngram(tokens: Sequence[str]) -> str:
    """Keep Japanese phrase candidates readable while retaining spaces around Latin/numeric tokens."""
    result = ""
    for token in tokens:
        if result and (result[-1].isascii() or token[0].isascii()):
            result += " "
        result += token
    return result


def top_rows(counter: Counter[str], file_locations: dict[str, set[str]], locations: dict[str, list[dict[str, object]]], top: int) -> list[dict[str, object]]:
    return [
        {
            "term": term,
            "count": count,
            "source_files": sorted(file_locations[term])[:5],
            "source_locations": locations[term][:5],
        }
        for term, count in sorted(counter.items(), key=lambda item: (-item[1], item[0]))[:max(0, top)]
    ]


def analyze_group(root: Path, name: str, relative_dir: Path, extensions: set[str], excludes: Sequence[str], stop_words: set[str], top: int, include_quoted_lines: bool = False, include_code_blocks: bool = False) -> dict[str, object]:
    files = collect_files(root, relative_dir, extensions, excludes)
    counters: dict[int, Counter[str]] = {1: Counter(), 2: Counter(), 3: Counter()}
    file_locations: dict[int, dict[str, set[str]]] = {1: defaultdict(set), 2: defaultdict(set), 3: defaultdict(set)}
    locations: dict[int, dict[str, list[dict[str, object]]]] = {1: defaultdict(list), 2: defaultdict(list), 3: defaultdict(list)}
    documents: dict[int, dict[str, set[str]]] = {1: defaultdict(set), 2: defaultdict(set), 3: defaultdict(set)}
    heading_documents: dict[int, dict[str, set[str]]] = {1: defaultdict(set), 2: defaultdict(set), 3: defaultdict(set)}
    token_count = 0
    for path in files:
        relative = path.resolve().relative_to(root.resolve()).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        for line_number, content, is_heading in iter_text_segments(path, text, include_quoted_lines, include_code_blocks):
            tokens = tokenize(content, stop_words)
            token_count += len(tokens)
            for size in (1, 2, 3):
                for index in range(len(tokens) - size + 1):
                    term = join_ngram(tokens[index:index + size])
                    counters[size][term] += 1
                    file_locations[size][term].add(relative)
                    documents[size][term].add(relative)
                    if is_heading:
                        heading_documents[size][term].add(relative)
                    if len(locations[size][term]) < 5:
                        locations[size][term].append(make_location(relative, line_number))
    return {
        "name": name,
        "directory": str(relative_dir).replace("\\", "/"),
        "file_count": len(files),
        "token_count": token_count,
        "files": [path.resolve().relative_to(root.resolve()).as_posix() for path in files],
        "unigrams": top_rows(counters[1], file_locations[1], locations[1], top),
        "bigrams": top_rows(counters[2], file_locations[2], locations[2], top),
        "trigrams": top_rows(counters[3], file_locations[3], locations[3], top),
        "_term_stats": {
            size: {term: {"count": count, "documents": documents[size][term], "heading_documents": heading_documents[size][term], "locations": locations[size][term]} for term, count in counters[size].items()}
            for size in (1, 2, 3)
        },
    }


def is_theme_term(term: str) -> bool:
    tokens = TOKEN_RE.findall(term)
    return bool(tokens) and not any(token.isdecimal() for token in tokens) and any(len(token) >= 2 for token in tokens)


def build_theme_candidates(groups: Sequence[dict[str, object]], top: int, min_documents: int, min_groups: int, min_ngram: int) -> list[dict[str, object]]:
    combined: dict[tuple[int, str], dict[str, object]] = {}
    for group in groups:
        for size, terms in group["_term_stats"].items():
            for term, stats in terms.items():
                item = combined.setdefault((size, term), {"count": 0, "documents": set(), "heading_documents": set(), "groups": set(), "locations": []})
                item["count"] += stats["count"]
                item["documents"].update(stats["documents"])
                item["heading_documents"].update(stats["heading_documents"])
                item["groups"].add(group["name"])
                for location in stats["locations"]:
                    if location not in item["locations"] and len(item["locations"]) < 5:
                        item["locations"].append(location)
    rows: list[dict[str, object]] = []
    for (size, term), stats in combined.items():
        document_count = len(stats["documents"])
        group_count = len(stats["groups"])
        if size < min_ngram or document_count < min_documents or group_count < min_groups or not is_theme_term(term):
            continue
        heading_document_count = len(stats["heading_documents"])
        score = math.log2(1 + document_count) * math.log2(1 + stats["count"])
        score *= 1 + 0.5 * heading_document_count + 0.25 * (group_count - 1)
        rows.append({"term": term, "ngram": size, "count": stats["count"], "document_count": document_count, "heading_document_count": heading_document_count, "groups": sorted(stats["groups"]), "source_locations": stats["locations"], "score": round(score, 3)})
    selected: list[dict[str, object]] = []
    seen_token_sets: set[tuple[str, ...]] = set()
    for row in sorted(rows, key=lambda row: (-row["score"], -row["document_count"], -row["count"], row["term"])):
        token_set = tuple(sorted(token.casefold() for token in TOKEN_RE.findall(row["term"])))
        if token_set in seen_token_sets:
            continue
        seen_token_sets.add(token_set)
        selected.append(row)
        if len(selected) >= max(0, top):
            break
    return selected


def build_distinctive_terms(groups: Sequence[dict[str, object]], top: int) -> dict[str, list[dict[str, object]]]:
    total_documents = sum(group["file_count"] for group in groups)
    result: dict[str, list[dict[str, object]]] = {}
    for group in groups:
        rows: list[dict[str, object]] = []
        other_documents = total_documents - group["file_count"]
        for size, terms in group["_term_stats"].items():
            for term, stats in terms.items():
                if not is_theme_term(term):
                    continue
                in_group = len(stats["documents"])
                elsewhere = sum(len(other["_term_stats"][size].get(term, {}).get("documents", set())) for other in groups if other is not group)
                group_rate = in_group / max(1, group["file_count"])
                other_rate = elsewhere / max(1, other_documents)
                if in_group < 2 or group_rate <= other_rate:
                    continue
                score = math.log2(1 + stats["count"]) * group_rate * math.log2((group_rate + 0.05) / (other_rate + 0.05))
                rows.append({"term": term, "ngram": size, "count": stats["count"], "document_count": in_group, "document_rate": round(group_rate, 3), "other_document_rate": round(other_rate, 3), "score": round(score, 3), "source_locations": stats["locations"]})
        result[group["name"]] = sorted(rows, key=lambda row: (-row["score"], -row["document_count"], -row["count"], row["term"]))[:max(0, top)]
    return result


def analyze_documents(root: Path, groups: Sequence[tuple[str, Path]], extensions: set[str], excludes: Sequence[str], stop_words: set[str], top: int, theme_top: int = 20, theme_min_docs: int = 2, theme_min_groups: int = 2, theme_min_ngram: int = 2, include_quoted_lines: bool = False, include_code_blocks: bool = False) -> dict[str, object]:
    analyzed_groups = [analyze_group(root, name, path, extensions, excludes, stop_words, top, include_quoted_lines, include_code_blocks) for name, path in groups]
    theme_candidates = build_theme_candidates(analyzed_groups, theme_top, max(1, theme_min_docs), max(1, theme_min_groups), max(1, theme_min_ngram))
    distinctive_terms = build_distinctive_terms(analyzed_groups, theme_top)
    for group in analyzed_groups:
        del group["_term_stats"]
    return {
        "tool": "analyze_document_language",
        "report_version": 1,
        "generated_at": datetime.now().astimezone().isoformat(),
        "root": str(root),
        "conditions": {
            "groups": [{"name": name, "directory": str(path).replace("\\", "/")} for name, path in groups],
            "extensions": sorted(extensions), "excludes": list(excludes), "stop_words": sorted(stop_words),
            "top": top, "theme_top": theme_top, "theme_min_documents": max(1, theme_min_docs), "theme_min_groups": max(1, theme_min_groups), "theme_min_ngram": max(1, theme_min_ngram),
            "quoted_lines": "included" if include_quoted_lines else "excluded",
            "fenced_code_blocks": "included" if include_code_blocks else "excluded",
            "tokenizer": "Unicode script-run tokenizer; this is not Japanese morphological analysis.",
            "normalization": "Unicode NFKC followed by Unicode casefold.",
        },
        "groups": analyzed_groups,
        "theme_candidates": theme_candidates,
        "distinctive_terms": distinctive_terms,
        "theme_candidate_method": "Scores favor occurrence in distinct documents, Markdown headings, and separate document groups; they are review candidates, not inferred conclusions.",
        "interpretation_limit": "Frequency is an observation aid, not a ranking of importance, accuracy, quality, or authors.",
    }


def render_text(data: dict[str, object]) -> str:
    lines = ["Document Language Observation", f"generated_at: {data['generated_at']}", ""]
    lines.append("Theme candidates (review required):")
    lines.extend(f"  {row['term']} [n={row['ngram']}, score={row['score']}, docs={row['document_count']}, groups={','.join(row['groups'])}]" for row in data["theme_candidates"]) or lines.append("  none")
    lines.append("")
    for group in data["groups"]:
        lines.extend([f"[{group['name']}] files={group['file_count']} tokens={group['token_count']}"])
        for label, key in (("unigrams", "unigrams"), ("bigrams", "bigrams"), ("trigrams", "trigrams")):
            lines.append(f"  {label}:")
            lines.extend(f"    {row['term']} ({row['count']}) [{', '.join(row['source_files'])}]" for row in group[key]) or lines.append("    none")
        lines.append("")
        lines.append("  distinctive terms (relative to other selected groups):")
        lines.extend(f"    {row['term']} (score={row['score']}, docs={row['document_count']}, rate={row['document_rate']})" for row in data["distinctive_terms"][group["name"]]) or lines.append("    none")
        lines.append("")
    return "\n".join(lines) + "\n"


def render_markdown(data: dict[str, object]) -> str:
    conditions = data["conditions"]
    lines = ["# 文書言語・語彙観測", "", f"- 実行日時: `{data['generated_at']}`", f"- 拡張子: {', '.join(f'`{value}`' for value in conditions['extensions'])}", f"- 除外: {', '.join(f'`{value}`' for value in conditions['excludes']) or 'なし'}", f"- stop word: {', '.join(f'`{value}`' for value in conditions['stop_words']) or 'なし'}", f"- 上位件数: {conditions['top']}", f"- テーマ候補: 上位{conditions['theme_top']}件、最低 {conditions['theme_min_documents']} 文書・{conditions['theme_min_groups']} 文書群・{conditions['theme_min_ngram']}-gram", f"- 引用行: {conditions['quoted_lines']}; fenced code: {conditions['fenced_code_blocks']}", f"- 正規化: {conditions['normalization']}", f"- 分かち書き: {conditions['tokenizer']}", f"- 注意: {data['interpretation_limit']}", "", "## テーマ候補（要レビュー）", "", data['theme_candidate_method'], "", "| 語句 | n-gram | score | 文書数 | 見出し文書数 | 文書群 | 出現箇所（最大5件） |", "| --- | ---: | ---: | ---: | ---: | --- | --- |"]
    lines.extend(f"| `{row['term']}` | {row['ngram']} | {row['score']} | {row['document_count']} | {row['heading_document_count']} | {', '.join(f'`{value}`' for value in row['groups'])} | {', '.join(f'`{value['file']}:{value['line']}`' for value in row['source_locations'])} |" for row in data["theme_candidates"]) or lines.append("| なし |  |  |  |  |  |  |")
    lines.append("")
    for group in data["groups"]:
        lines.extend([f"## {group['name']}", "", f"- 対象: `{group['directory']}`", f"- ファイル数: {group['file_count']}", f"- token 数: {group['token_count']}", ""])
        for label, key in (("Unigram", "unigrams"), ("Bigram", "bigrams"), ("Trigram", "trigrams")):
            lines.extend([f"### {label}", "", "| 語句 | 件数 | 出現ファイル（最大5件） |", "| --- | ---: | --- |"])
            lines.extend(f"| `{row['term']}` | {row['count']} | {', '.join(f'`{value}`' for value in row['source_files'])} |" for row in group[key]) or lines.append("| なし | 0 |  |")
            lines.append("")
        lines.extend(["### 他の選択文書群より偏っている語句（要レビュー）", "", "| 語句 | n-gram | score | この群の文書率 | 他群の文書率 | 出現箇所（最大5件） |", "| --- | ---: | ---: | ---: | ---: | --- |"])
        rows = data["distinctive_terms"][group["name"]]
        lines.extend(f"| `{row['term']}` | {row['ngram']} | {row['score']} | {row['document_rate']} | {row['other_document_rate']} | {', '.join(f'`{value['file']}:{value['line']}`' for value in row['source_locations'])} |" for row in rows) or lines.append("| なし |  |  |  |  |  |")
        lines.append("")
    return "\n".join(lines)


def render_report(data: dict[str, object], output_format: str) -> str:
    if output_format == "json":
        return json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    return render_markdown(data) if output_format == "md" else render_text(data)


def write_new_report(path: Path, content: str, source_dirs: Sequence[Path]) -> None:
    if any(path.is_relative_to(directory) for directory in source_dirs):
        raise ValueError(f"report must not be placed inside an analyzed source directory: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8", newline="\n") as handle:
        handle.write(content)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    root = Path(args.root).resolve()
    if not root.is_dir():
        print(f"root is not a directory: {root}", file=sys.stderr)
        return 2
    try:
        groups = parse_groups(args.group)
        extensions = {value if value.startswith(".") else "." + value for value in (args.extension or DEFAULT_EXTENSIONS)}
        excludes = [*DEFAULT_EXCLUDES, *(args.exclude or [])]
        data = analyze_documents(root, groups, {value.casefold() for value in extensions}, excludes, read_stop_words(args, root), max(0, args.top), max(0, args.theme_top), max(1, args.theme_min_docs), max(1, args.theme_min_groups), args.theme_min_ngram, args.include_quoted_lines, args.include_code_blocks)
        content = render_report(data, args.format)
        if args.report:
            report = Path(args.report)
            write_new_report(
                (root / report).resolve() if not report.is_absolute() else report.resolve(),
                content,
                [(root / path).resolve() if not path.is_absolute() else path.resolve() for _, path in groups],
            )
    except (OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    sys.stdout.write(content)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
