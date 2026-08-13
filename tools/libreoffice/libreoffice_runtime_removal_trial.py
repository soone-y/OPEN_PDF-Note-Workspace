#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path


PROTECTED_PATHS = frozenset(
    {
        "license.txt",
        "LICENSE.html",
        "NOTICE",
        "program/soffice.com",
        "program/soffice.bin",
        "program/fundamental.ini",
        "program/services.rdb",
        "program/types.rdb",
    }
)

SUMMARY_FIELDS = (
    "paired_documents",
    "converted_documents",
    "conversion_errors",
    "missing_reference_pdfs",
    "structural_issues",
    "semantic_issues",
    "compared_pages",
    "low_text_similarity_pages",
)

RESULT_FIELDS = (
    "reference_pages",
    "candidate_pages",
    "reference_pdf_fonts",
    "candidate_pdf_fonts",
    "reference_only_pdf_fonts",
    "candidate_only_pdf_fonts",
    "reference_semantics",
    "candidate_semantics",
    "semantic_issues",
    "structural_issues",
    "mean_difference_ratio",
    "max_difference_ratio",
    "mean_text_similarity",
)

PAGE_FIELDS = (
    "reference_page_points",
    "candidate_page_points",
    "page_size_equal",
    "reference_text_characters",
    "candidate_text_characters",
    "difference_ratio",
    "text_similarity",
    "same_dimensions",
    "reference_pixels",
    "candidate_pixels",
    "comparison_pixels",
    "different_pixels",
    "difference_bbox",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Copy a LibreOffice runtime, remove selected paths from the copy, and run "
            "the communication gate, conversion smoke test, and optional quality regression."
        )
    )
    parser.add_argument("--runtime-root", required=True)
    parser.add_argument("--input-dir", required=True)
    parser.add_argument("--remove", action="append", default=[])
    parser.add_argument(
        "--remove-list",
        help="UTF-8 text file containing one runtime-relative path per line.",
    )
    parser.add_argument("--baseline-report")
    parser.add_argument("--output", required=True)
    parser.add_argument("--work-root")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--keep-candidate", action="store_true")
    args = parser.parse_args()
    if not args.remove and not args.remove_list:
        parser.error("specify --remove or --remove-list")
    if args.timeout < 1:
        parser.error("--timeout must be positive")
    return args


def normalize_relative(value: str) -> str:
    normalized = value.strip().replace("\\", "/").strip("/")
    if not normalized or normalized.startswith("#"):
        return ""
    path = Path(normalized)
    if path.is_absolute() or ".." in path.parts:
        raise ValueError(f"unsafe runtime-relative path: {value}")
    return path.as_posix()


def collect_removals(values: list[str], list_path: str | None) -> list[str]:
    raw = list(values)
    if list_path:
        raw.extend(Path(list_path).read_text(encoding="utf-8").splitlines())
    removals = sorted({path for value in raw if (path := normalize_relative(value))})
    for removal in removals:
        for protected in PROTECTED_PATHS:
            if removal == protected or protected.startswith(removal + "/"):
                raise ValueError(f"refusing to remove protected runtime path: {removal}")
    return removals


def ensure_outside(path: Path, forbidden_root: Path) -> Path:
    resolved = path.resolve()
    forbidden = forbidden_root.resolve()
    if resolved == forbidden or forbidden in resolved.parents:
        raise ValueError(f"path must be outside source runtime: {resolved}")
    return resolved


def path_size(path: Path) -> int:
    if path.is_file():
        return path.stat().st_size
    if path.is_dir():
        return sum(child.stat().st_size for child in path.rglob("*") if child.is_file())
    return 0


def remove_path(path: Path) -> None:
    if path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def run_command(command: list[str], cwd: Path, timeout: int) -> dict[str, object]:
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
        return {
            "command": command,
            "exit_code": completed.returncode,
            "timed_out": False,
            "output": completed.stdout,
        }
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        return {
            "command": command,
            "exit_code": None,
            "timed_out": True,
            "output": output,
        }


def quality_index(report: dict[str, object]) -> dict[str, dict[str, object]]:
    return {
        str(result["relative_office_file"]): result
        for result in report.get("results", [])
    }


def compare_quality(
    baseline: dict[str, object], candidate: dict[str, object]
) -> list[dict[str, object]]:
    differences: list[dict[str, object]] = []
    baseline_summary = baseline.get("summary", {})
    candidate_summary = candidate.get("summary", {})
    for field in SUMMARY_FIELDS:
        if baseline_summary.get(field) != candidate_summary.get(field):
            differences.append(
                {
                    "scope": "summary",
                    "field": field,
                    "baseline": baseline_summary.get(field),
                    "candidate": candidate_summary.get(field),
                }
            )

    baseline_results = quality_index(baseline)
    candidate_results = quality_index(candidate)
    if set(baseline_results) != set(candidate_results):
        differences.append(
            {
                "scope": "documents",
                "field": "relative_office_file",
                "baseline": sorted(baseline_results),
                "candidate": sorted(candidate_results),
            }
        )
        return differences

    for document in sorted(baseline_results):
        baseline_result = baseline_results[document]
        candidate_result = candidate_results[document]
        for field in RESULT_FIELDS:
            if baseline_result.get(field) != candidate_result.get(field):
                differences.append(
                    {
                        "scope": "document",
                        "document": document,
                        "field": field,
                        "baseline": baseline_result.get(field),
                        "candidate": candidate_result.get(field),
                    }
                )
        baseline_pages = baseline_result.get("pages", [])
        candidate_pages = candidate_result.get("pages", [])
        if len(baseline_pages) != len(candidate_pages):
            differences.append(
                {
                    "scope": "document",
                    "document": document,
                    "field": "page_result_count",
                    "baseline": len(baseline_pages),
                    "candidate": len(candidate_pages),
                }
            )
            continue
        for page_number, (baseline_page, candidate_page) in enumerate(
            zip(baseline_pages, candidate_pages), start=1
        ):
            for field in PAGE_FIELDS:
                if baseline_page.get(field) != candidate_page.get(field):
                    differences.append(
                        {
                            "scope": "page",
                            "document": document,
                            "page": page_number,
                            "field": field,
                            "baseline": baseline_page.get(field),
                            "candidate": candidate_page.get(field),
                        }
                    )
    return differences


def write_json_atomic(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temporary, path)


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parents[2]
    runtime_root = Path(args.runtime_root).resolve()
    input_dir = Path(args.input_dir).resolve()
    output_path = ensure_outside(Path(args.output), runtime_root)
    if not (runtime_root / "program" / "soffice.com").is_file():
        raise SystemExit(f"LibreOffice runtime not found: {runtime_root}")
    if not input_dir.is_dir():
        raise SystemExit(f"fixture directory not found: {input_dir}")

    removals = collect_removals(args.remove, args.remove_list)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    default_work = repo_root / ".local" / "repo_resource" / "tmp" / "libreoffice_removal_trial" / timestamp
    work_root = ensure_outside(Path(args.work_root) if args.work_root else default_work, runtime_root)
    if work_root.exists():
        raise SystemExit(f"work root already exists: {work_root}")
    candidate_root = work_root / "candidate"
    quality_output = work_root / "quality"

    removal_records: list[dict[str, object]] = []
    checks: dict[str, object] = {}
    quality_differences: list[dict[str, object]] = []
    passed = False
    try:
        shutil.copytree(runtime_root, candidate_root)
        for relative in removals:
            candidate_path = candidate_root / Path(relative)
            size = path_size(candidate_path)
            existed = candidate_path.exists()
            if existed:
                remove_path(candidate_path)
            removal_records.append(
                {"path": relative, "existed": existed, "size_bytes": size}
            )

        soffice = candidate_root / "program" / "soffice.com"
        checks["runtime_gate"] = run_command(
            [
                sys.executable,
                str(repo_root / "tools" / "release_checks" / "libreoffice_runtime_gate.py"),
                "--image",
                str(candidate_root),
            ],
            repo_root,
            args.timeout,
        )
        checks["smoke"] = run_command(
            [
                sys.executable,
                str(repo_root / "tools" / "libreoffice" / "libreoffice_smoke_test.py"),
                "--soffice",
                str(soffice),
                "--input-dir",
                str(input_dir),
                "--require-docx",
                "--require-pptx",
                "--timeout",
                str(args.timeout),
            ],
            repo_root,
            args.timeout * 4,
        )

        quality_ok = True
        if args.baseline_report:
            baseline_path = Path(args.baseline_report).resolve()
            baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
            checks["quality"] = run_command(
                [
                    sys.executable,
                    str(
                        repo_root
                        / "tools"
                        / "libreoffice"
                        / "libreoffice_conversion_quality_test.py"
                    ),
                    "--input-dir",
                    str(input_dir),
                    "--soffice",
                    str(soffice),
                    "--output-dir",
                    str(quality_output),
                    "--timeout",
                    str(args.timeout),
                ],
                repo_root,
                args.timeout * 4,
            )
            candidate_report_path = quality_output / "quality_report.json"
            if candidate_report_path.is_file():
                candidate_report = json.loads(candidate_report_path.read_text(encoding="utf-8"))
                quality_differences = compare_quality(baseline, candidate_report)
                quality_ok = not quality_differences
            else:
                quality_differences = [
                    {"scope": "quality", "field": "report", "candidate": "missing"}
                ]
                quality_ok = False

        passed = (
            checks["runtime_gate"]["exit_code"] == 0
            and checks["smoke"]["exit_code"] == 0
            and quality_ok
        )
    finally:
        report = {
            "tool": "libreoffice_runtime_removal_trial",
            "report_version": 1,
            "runtime_root": str(runtime_root),
            "input_dir": str(input_dir),
            "candidate_root": str(candidate_root) if args.keep_candidate else None,
            "kept_candidate": args.keep_candidate,
            "passed": passed,
            "removed_bytes": sum(record["size_bytes"] for record in removal_records),
            "removals": removal_records,
            "checks": checks,
            "quality_differences": quality_differences,
        }
        write_json_atomic(output_path, report)
        if not args.keep_candidate and work_root.exists():
            shutil.rmtree(work_root)

    print(
        f"passed={passed} removed_bytes={report['removed_bytes']} "
        f"quality_differences={len(quality_differences)} report={output_path}"
    )
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
