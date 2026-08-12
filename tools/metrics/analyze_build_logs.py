#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
from collections import Counter
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence


END_TIME_RE = re.compile(r"^(?P<timestamp>[^\t]+)\telapsed_sec=(?P<elapsed>[0-9]+(?:\.[0-9]+)?)$")
GCC_MSG_RE = re.compile(
    r"^(?P<path>[^:\s][^:]*(?:\.[A-Za-z0-9_]+)?):(?P<line>\d+):(?:\d+:)?\s*"
    r"(?P<level>warning|error|fatal error):\s*(?P<message>.*)$",
    re.IGNORECASE,
)
SECTION_HEADER_RE = re.compile(r"^==\s*(?P<label>.+?)\s*==$")
STARTED_RE = re.compile(r"^started:\s*(?P<value>.+)$")
CONFIGURATION_RE = re.compile(r"^configuration:\s*(?P<value>.+)$")


@dataclass
class EndTimeEntry:
    log_kind: str
    source_file: str
    finished_at: str
    elapsed_sec: float


@dataclass
class MessageFinding:
    source_file: str
    log_kind: str
    level: str
    path: str
    line: int
    message: str


@dataclass
class DetailLogSummary:
    source_file: str
    log_kind: str
    label: str
    started_at: str
    configuration: str
    warning_count: int
    error_count: int
    status: str


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze local build history logs under out/logs without modifying repository files."
    )
    parser.add_argument(
        "--root",
        default=str(Path(__file__).resolve().parents[2]),
        help="Repository root. Defaults to this script's repository root.",
    )
    parser.add_argument(
        "--log-dir",
        default="out/logs",
        help="Directory containing build logs. Relative paths are resolved from --root.",
    )
    parser.add_argument(
        "--format",
        choices=("text", "json", "md"),
        default="text",
        help="Output format. Defaults to text.",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=10,
        help="Top-N size for warning/error breakdowns. Defaults to 10.",
    )
    parser.add_argument(
        "--report",
        help="Optional output file. If omitted, the report is written to stdout only.",
    )
    return parser.parse_args(argv)


def repo_relative(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except Exception:
        return path.as_posix()


def parse_end_time_file(path: Path, log_kind: str, root: Path) -> List[EndTimeEntry]:
    entries: List[EndTimeEntry] = []
    if not path.exists():
        return entries
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = END_TIME_RE.match(raw_line.strip())
        if not match:
            continue
        timestamp = match.group("timestamp").strip()
        elapsed = float(match.group("elapsed"))
        entries.append(
            EndTimeEntry(
                log_kind=log_kind,
                source_file=repo_relative(path, root),
                finished_at=timestamp,
                elapsed_sec=elapsed,
            )
        )
    return entries


def classify_detail_log(path: Path) -> str:
    name = path.name
    if name.startswith("build_readonly_viewer_detail_"):
        return "readonly_viewer"
    return "app_or_all"


def parse_detail_log(path: Path, root: Path) -> tuple[DetailLogSummary, List[MessageFinding]]:
    label = ""
    started_at = ""
    configuration = ""
    warnings = 0
    errors = 0
    findings: List[MessageFinding] = []
    rel_path = repo_relative(path, root)
    log_kind = classify_detail_log(path)

    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not label:
            match = SECTION_HEADER_RE.match(raw_line)
            if match:
                label = match.group("label").strip()
                continue
        if not started_at:
            match = STARTED_RE.match(raw_line)
            if match:
                started_at = match.group("value").strip()
                continue
        if not configuration:
            match = CONFIGURATION_RE.match(raw_line)
            if match:
                configuration = match.group("value").strip()
                continue

        msg_match = GCC_MSG_RE.match(raw_line)
        if not msg_match:
            continue
        level = msg_match.group("level").lower()
        normalized_level = "warning" if level == "warning" else "error"
        if normalized_level == "warning":
            warnings += 1
        else:
            errors += 1
        findings.append(
            MessageFinding(
                source_file=rel_path,
                log_kind=log_kind,
                level=normalized_level,
                path=msg_match.group("path").strip(),
                line=int(msg_match.group("line")),
                message=msg_match.group("message").strip(),
            )
        )

    status = "failed" if errors > 0 else "ok"
    return (
        DetailLogSummary(
            source_file=rel_path,
            log_kind=log_kind,
            label=label,
            started_at=started_at,
            configuration=configuration,
            warning_count=warnings,
            error_count=errors,
            status=status,
        ),
        findings,
    )


def build_duration_stats(entries: Iterable[EndTimeEntry]) -> Dict[str, float]:
    durations = [entry.elapsed_sec for entry in entries]
    if not durations:
        return {
            "count": 0,
            "min_sec": 0.0,
            "max_sec": 0.0,
            "avg_sec": 0.0,
            "median_sec": 0.0,
            "total_sec": 0.0,
        }
    return {
        "count": float(len(durations)),
        "min_sec": min(durations),
        "max_sec": max(durations),
        "avg_sec": statistics.fmean(durations),
        "median_sec": statistics.median(durations),
        "total_sec": sum(durations),
    }


def counter_rows(counter: Counter[str], top: int) -> List[Dict[str, object]]:
    rows: List[Dict[str, object]] = []
    for value, count in counter.most_common(max(0, top)):
        rows.append({"value": value, "count": count})
    return rows


def finding_rows(findings: Iterable[MessageFinding], level: str, top: int) -> Dict[str, List[Dict[str, object]]]:
    filtered = [item for item in findings if item.level == level]
    file_counter: Counter[str] = Counter(item.path for item in filtered)
    message_counter: Counter[str] = Counter(item.message for item in filtered)
    return {
        "top_files": counter_rows(file_counter, top),
        "top_messages": counter_rows(message_counter, top),
    }


def analyze_log_directory(log_dir: Path, root: Path, top: int) -> Dict[str, object]:
    end_entries: List[EndTimeEntry] = []
    end_entries.extend(parse_end_time_file(log_dir / "build_end_time.log", "app_or_all", root))
    end_entries.extend(
        parse_end_time_file(log_dir / "build_readonly_viewer_end_time.log", "readonly_viewer", root)
    )

    detail_logs = sorted(
        list(log_dir.glob("build_detail_*.log")) + list(log_dir.glob("build_readonly_viewer_detail_*.log"))
    )
    summaries: List[DetailLogSummary] = []
    findings: List[MessageFinding] = []
    for path in detail_logs:
        summary, summary_findings = parse_detail_log(path, root)
        summaries.append(summary)
        findings.extend(summary_findings)

    warning_total = sum(item.warning_count for item in summaries)
    error_total = sum(item.error_count for item in summaries)
    failed_logs = sum(1 for item in summaries if item.status == "failed")
    ok_logs = sum(1 for item in summaries if item.status == "ok")

    duration_by_kind = {
        "app_or_all": build_duration_stats(entry for entry in end_entries if entry.log_kind == "app_or_all"),
        "readonly_viewer": build_duration_stats(
            entry for entry in end_entries if entry.log_kind == "readonly_viewer"
        ),
        "combined": build_duration_stats(end_entries),
    }

    detail_by_kind: Dict[str, Dict[str, object]] = {}
    for kind in ("app_or_all", "readonly_viewer"):
        selected = [item for item in summaries if item.log_kind == kind]
        detail_by_kind[kind] = {
            "count": len(selected),
            "ok": sum(1 for item in selected if item.status == "ok"),
            "failed": sum(1 for item in selected if item.status == "failed"),
            "warnings": sum(item.warning_count for item in selected),
            "errors": sum(item.error_count for item in selected),
        }

    return {
        "tool": "analyze_build_logs",
        "report_version": 1,
        "root": str(root),
        "log_dir": str(log_dir),
        "generated_at": datetime.now().astimezone().isoformat(),
        "summary": {
            "detail_log_count": len(summaries),
            "ok_detail_logs": ok_logs,
            "failed_detail_logs": failed_logs,
            "warning_total": warning_total,
            "error_total": error_total,
        },
        "duration_stats": duration_by_kind,
        "detail_stats": detail_by_kind,
        "warnings": finding_rows(findings, "warning", top),
        "errors": finding_rows(findings, "error", top),
        "detail_logs": [
            {
                "source_file": item.source_file,
                "log_kind": item.log_kind,
                "label": item.label,
                "started_at": item.started_at,
                "configuration": item.configuration,
                "warning_count": item.warning_count,
                "error_count": item.error_count,
                "status": item.status,
            }
            for item in summaries
        ],
        "end_time_entries": [
            {
                "source_file": item.source_file,
                "log_kind": item.log_kind,
                "finished_at": item.finished_at,
                "elapsed_sec": item.elapsed_sec,
            }
            for item in end_entries
        ],
    }


def format_stat_line(label: str, stats: Dict[str, float]) -> str:
    return (
        f"{label}: count={int(stats['count'])} min={stats['min_sec']:.3f}s "
        f"median={stats['median_sec']:.3f}s avg={stats['avg_sec']:.3f}s "
        f"max={stats['max_sec']:.3f}s total={stats['total_sec']:.3f}s"
    )


def render_text_report(data: Dict[str, object]) -> str:
    summary = data["summary"]
    duration_stats = data["duration_stats"]
    detail_stats = data["detail_stats"]
    lines = [
        "Build Log Analysis",
        f"root: {data['root']}",
        f"log_dir: {data['log_dir']}",
        f"generated_at: {data['generated_at']}",
        "",
        "Summary",
        f"  detail_logs: {summary['detail_log_count']}",
        f"  ok_detail_logs: {summary['ok_detail_logs']}",
        f"  failed_detail_logs: {summary['failed_detail_logs']}",
        f"  warnings: {summary['warning_total']}",
        f"  errors: {summary['error_total']}",
        "",
        "Durations",
        f"  {format_stat_line('combined', duration_stats['combined'])}",
        f"  {format_stat_line('app_or_all', duration_stats['app_or_all'])}",
        f"  {format_stat_line('readonly_viewer', duration_stats['readonly_viewer'])}",
        "",
        "Detail Logs By Kind",
        (
            "  app_or_all: "
            f"count={detail_stats['app_or_all']['count']} ok={detail_stats['app_or_all']['ok']} "
            f"failed={detail_stats['app_or_all']['failed']} warnings={detail_stats['app_or_all']['warnings']} "
            f"errors={detail_stats['app_or_all']['errors']}"
        ),
        (
            "  readonly_viewer: "
            f"count={detail_stats['readonly_viewer']['count']} ok={detail_stats['readonly_viewer']['ok']} "
            f"failed={detail_stats['readonly_viewer']['failed']} warnings={detail_stats['readonly_viewer']['warnings']} "
            f"errors={detail_stats['readonly_viewer']['errors']}"
        ),
        "",
        "Top Warning Files",
    ]
    warning_files = data["warnings"]["top_files"]
    if warning_files:
        lines.extend(f"  - {row['value']} ({row['count']})" for row in warning_files)
    else:
        lines.append("  - none")
    lines.append("")
    lines.append("Top Warning Messages")
    warning_messages = data["warnings"]["top_messages"]
    if warning_messages:
        lines.extend(f"  - {row['value']} ({row['count']})" for row in warning_messages)
    else:
        lines.append("  - none")
    lines.append("")
    lines.append("Top Error Files")
    error_files = data["errors"]["top_files"]
    if error_files:
        lines.extend(f"  - {row['value']} ({row['count']})" for row in error_files)
    else:
        lines.append("  - none")
    lines.append("")
    lines.append("Top Error Messages")
    error_messages = data["errors"]["top_messages"]
    if error_messages:
        lines.extend(f"  - {row['value']} ({row['count']})" for row in error_messages)
    else:
        lines.append("  - none")
    return "\n".join(lines) + "\n"


def render_markdown_report(data: Dict[str, object]) -> str:
    summary = data["summary"]
    duration_stats = data["duration_stats"]
    detail_stats = data["detail_stats"]
    lines = [
        "# Build Log Analysis",
        "",
        f"- root: `{data['root']}`",
        f"- log_dir: `{data['log_dir']}`",
        f"- generated_at: `{data['generated_at']}`",
        "",
        "## Summary",
        "",
        f"- detail logs: {summary['detail_log_count']}",
        f"- ok detail logs: {summary['ok_detail_logs']}",
        f"- failed detail logs: {summary['failed_detail_logs']}",
        f"- warnings: {summary['warning_total']}",
        f"- errors: {summary['error_total']}",
        "",
        "## Durations",
        "",
        f"- `{format_stat_line('combined', duration_stats['combined'])}`",
        f"- `{format_stat_line('app_or_all', duration_stats['app_or_all'])}`",
        f"- `{format_stat_line('readonly_viewer', duration_stats['readonly_viewer'])}`",
        "",
        "## Detail Logs By Kind",
        "",
        (
            f"- `app_or_all`: count={detail_stats['app_or_all']['count']} "
            f"ok={detail_stats['app_or_all']['ok']} failed={detail_stats['app_or_all']['failed']} "
            f"warnings={detail_stats['app_or_all']['warnings']} errors={detail_stats['app_or_all']['errors']}"
        ),
        (
            f"- `readonly_viewer`: count={detail_stats['readonly_viewer']['count']} "
            f"ok={detail_stats['readonly_viewer']['ok']} failed={detail_stats['readonly_viewer']['failed']} "
            f"warnings={detail_stats['readonly_viewer']['warnings']} errors={detail_stats['readonly_viewer']['errors']}"
        ),
        "",
        "## Top Warning Files",
        "",
    ]
    warning_files = data["warnings"]["top_files"]
    if warning_files:
        lines.extend(f"- `{row['value']}` ({row['count']})" for row in warning_files)
    else:
        lines.append("- none")
    lines.extend(["", "## Top Warning Messages", ""])
    warning_messages = data["warnings"]["top_messages"]
    if warning_messages:
        lines.extend(f"- `{row['value']}` ({row['count']})" for row in warning_messages)
    else:
        lines.append("- none")
    lines.extend(["", "## Top Error Files", ""])
    error_files = data["errors"]["top_files"]
    if error_files:
        lines.extend(f"- `{row['value']}` ({row['count']})" for row in error_files)
    else:
        lines.append("- none")
    lines.extend(["", "## Top Error Messages", ""])
    error_messages = data["errors"]["top_messages"]
    if error_messages:
        lines.extend(f"- `{row['value']}` ({row['count']})" for row in error_messages)
    else:
        lines.append("- none")
    lines.append("")
    return "\n".join(lines)


def render_report(data: Dict[str, object], output_format: str) -> str:
    if output_format == "json":
        return json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    if output_format == "md":
        return render_markdown_report(data)
    return render_text_report(data)


def write_report(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    root = Path(args.root).resolve()
    if not root.exists() or not root.is_dir():
        print(f"root is not a directory: {root}", file=sys.stderr)
        return 2

    log_dir = Path(args.log_dir)
    if not log_dir.is_absolute():
        log_dir = root / log_dir
    log_dir = log_dir.resolve()
    if not log_dir.exists() or not log_dir.is_dir():
        print(f"log_dir is not a directory: {log_dir}", file=sys.stderr)
        return 2

    data = analyze_log_directory(log_dir, root, max(0, args.top))
    content = render_report(data, args.format)

    if args.report:
        report_path = Path(args.report)
        if not report_path.is_absolute():
            report_path = root / report_path
        write_report(report_path.resolve(), content)

    sys.stdout.write(content)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
