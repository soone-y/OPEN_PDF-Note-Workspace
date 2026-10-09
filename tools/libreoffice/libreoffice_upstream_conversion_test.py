#!/usr/bin/env python3
"""Local upstream inputs only; no download or redistribution of sample files.

The caller owns a new output directory. Originals and runtimes are read-only
inputs; each conversion owns an isolated copy/profile under that directory.
Missing inputs, changed hashes and incomplete comparisons fail, retaining evidence.
"""
from __future__ import annotations

import argparse
import csv
import ctypes
import json
import os
import shutil
import sys
import time
from collections import Counter
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor, as_completed
from contextlib import contextmanager
from ctypes import wintypes
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import libreoffice_conversion_quality_test as quality
import libreoffice_smoke_test as smoke
import libreoffice_conversion_corpus as corpus
import libreoffice_conversion_expectations as expectations
import libreoffice_test_clock as test_clock

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST = REPO_ROOT / "tests/config/libreoffice_upstream_conversion_samples.json"


def collect_samples(source_dir: Path, manifest_path: Path = MANIFEST, *,
                    groups: list[str] | None = None, individual: list[str] | None = None,
                    acceptance: bool = False) -> tuple[dict, list[Path]]:
    root = source_dir.resolve(strict=True)
    manifest = corpus.read_catalog(manifest_path)
    if acceptance:
        corpus.verify_inventory(root, manifest)
    entries = corpus.select(manifest, groups or [], individual or [], acceptance=acceptance)
    samples = []
    for item in entries:
        source = (root / item["path"]).resolve(strict=True)
        if not source.is_relative_to(root) or not source.is_file():
            raise ValueError(f"Invalid upstream input: {item['path']}")
        if smoke.sha256_file(source) != item["sha256"]:
            raise ValueError(f"Upstream input hash mismatch: {item['path']}")
        samples.append(source)
    return {**manifest, "samples": entries}, samples


def comparison_changed(comparison: dict) -> bool:
    if not comparison["pages"]:
        raise ValueError("PDF comparison contained no pages")
    return bool(
        comparison["structural_issues"] or comparison["semantic_issues"]
        or comparison["reference_only_pdf_fonts"] or comparison["candidate_only_pdf_fonts"]
        or any(page.get("text_similarity") != 1.0 or page.get("different_pixels", 0)
               for page in comparison["pages"])
    )


def runtime_identity(soffice: Path, *, full: bool = False) -> dict:
    if not soffice.is_file():
        raise ValueError(f"LibreOffice executable not found: {soffice}")
    files = [soffice] + [soffice.parent / name for name in ("soffice.bin", "deployment.dll", "sclo.dll")]
    if full:
        files = sorted(path for path in soffice.parent.parent.rglob("*") if path.is_file())
    return {"soffice": str(soffice), "sha256": {
        path.relative_to(soffice.parent.parent).as_posix(): smoke.sha256_file(path) for path in files if path.is_file()
    }}


def run_case(index: int, item: dict, original: Path, case: Path,
             runtimes: dict[str, Path], timeout: int, encoding: dict | None = None,
             expectation: dict | None = None, validate_expectations: bool = False,
             paired: bool = False, clock_control: dict | None = None) -> dict:
    """Own one new case directory. Run both converters even if either fails.

    Sequential runs receive fresh copies; paired runs share a Win32 read-locked
    copy. Profiles/outputs are independent and failures never damage the other
    input. Exceptions retain evidence and cannot become equality.
    Process workers own conversion trees through smoke.run_owned_conversion.
    """
    if paired:
        return run_paired_case(index, item, original, case, runtimes, timeout, encoding,
                               expectation, validate_expectations, clock_control)
    case.mkdir()
    record = {**item, "index": index, "runs": {}, "error": None, "changed": None,
              "input_preparation": encoding["codec"] if encoding else "copy",
              "conversion_input_sha256": encoding["decoded_sha256"] if encoding else item["sha256"]}
    problems = []
    pdfs = {}
    input_dir = case / "input"
    input_dir.mkdir()
    for label, runtime in runtimes.items():
        started = time.monotonic()
        result = {"status": "FAIL", "error": None, "pages": 0, "input_unchanged": False}
        record["runs"][label] = result
        if clock_control is not None:
            result['clock_receipts'] = []
        run_dir = case / label
        run_dir.mkdir()
        # Recreate independent bytes at the same absolute pathname. File/path
        # fields printed by Office must not differ solely due to baseline vs
        # candidate directory names. Retain each used copy after the run.
        staged = input_dir / ("source" + original.suffix)
        prepared_hash = None
        try:
            prepared_hash = corpus.prepare_input(original, staged, item, encoding)
            out = run_dir / "output"
            out.mkdir()
            clock_options = {'clock_control': clock_control, 'clock_observations': result['clock_receipts']} if clock_control else {}
            pdf = smoke.convert_one(runtime, staged, out, run_dir / "profile", timeout,
                                    cleanup_runtime_pycache=False, own_process_tree=True, repeatable_random=True, **clock_options)
            with quality.fitz.open(pdf) as document:
                if len(document) == 0:
                    raise ValueError("Converted PDF has no pages")
                result["pages"] = len(document)
            if smoke.sha256_file(staged) != prepared_hash:
                raise ValueError("Conversion copy changed during test")
            result.update(status="SUCCESS", pdf=str(pdf), sha256=smoke.sha256_file(pdf))
            pdfs[label] = pdf
        except Exception as exc:
            result["error"] = f"{type(exc).__name__}: {exc}"
            if isinstance(exc, smoke.NormalLoadRefusal):
                result.update(failure_kind="normal_load_refusal", returncode=exc.returncode,
                              diagnostic=exc.diagnostic, output_pdf_exists=exc.output_pdf_exists)
            problems.append(f"{label}: {result['error']}")
        finally:
            result["elapsed_seconds"] = round(time.monotonic() - started, 3)
            if prepared_hash is not None:
                try:
                    result["input_unchanged"] = smoke.sha256_file(staged) == prepared_hash
                    if not result["input_unchanged"]:
                        raise ValueError("Conversion copy changed during test")
                except Exception as exc:
                    problems.append(f"{label} input audit: {type(exc).__name__}: {exc}")
            if staged.exists():
                snapshot = run_dir / staged.name
                staged.rename(snapshot)
                result["input_snapshot"] = str(snapshot)
    return finish_case(record, item, original, case, runtimes, pdfs, problems, expectation, validate_expectations, clock_control)


def finish_case(record: dict, item: dict, original: Path, case: Path, runtimes: dict,
                pdfs: dict, problems: list, expectation: dict | None, validate_expectations: bool,
                clock_control: dict | None = None) -> dict:
    if "baseline" in runtimes:
        if len(pdfs) == 2:
            try:
                comparison = quality.compare_pdfs(pdfs["baseline"], pdfs["candidate"],
                                                  case / "comparison", dpi=144, pixel_threshold=8,
                                                  save_matching_pages=False)
                record["comparison"] = comparison
                record["changed"] = comparison_changed(comparison)
                record["outcome"] = "both_success_different" if record["changed"] else "both_success_equal"
            except Exception as exc:
                problems.append(f"comparison: {type(exc).__name__}: {exc}")
                record["outcome"] = "comparison_failed"
        else:
            record["outcome"] = ("baseline_only_success" if "baseline" in pdfs else
                                 "candidate_only_success" if "candidate" in pdfs else "both_failed")
    else:
        record["outcome"] = "candidate_success" if pdfs else "candidate_failed"
    try:
        record["original_unchanged"] = smoke.sha256_file(original) == item["sha256"]
        if not record["original_unchanged"]:
            raise ValueError("Original input changed during test")
    except Exception as exc:
        record["original_unchanged"] = False
        problems.append(f"source: {type(exc).__name__}: {exc}")
        record["outcome"] = "input_error"
    clock_valid = True
    if clock_control is not None:
        for label, run in record['runs'].items():
            try:
                test_clock.check_run(run, clock_control, label)
            except (KeyError, TypeError, ValueError, OSError) as exc:
                clock_valid = False
                problems.append(f'{label} clock audit: {type(exc).__name__}: {exc}')
    record["error"] = "; ".join(problems) or None
    if validate_expectations:
        record["validation"] = expectations.judge(record, expectation)
        if not clock_valid:
            record['validation'].update(passed=False, kind='UNEXPECTED_RESULT')
    quality.write_json_atomic(case / "result.json", record)
    return record


@contextmanager
def immutable_input(path: Path):
    """Windows read lease: reject writes/deletes until both owned runs exit.

    A failed converter cannot damage the other run's input. This changes only
    sharing permissions on a fresh QA copy, never its bytes or the original.
    """
    if os.name != 'nt':
        raise ValueError('Paired immutable-input comparisons require Windows')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                                  wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.CreateFileW(str(path), 0x80000000, 1, None, 3, 0, None)
    if handle == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        yield
    finally:
        if not kernel.CloseHandle(handle):
            raise ctypes.WinError(ctypes.get_last_error())


def run_paired_case(index: int, item: dict, original: Path, case: Path, runtimes: dict[str, Path],
                    timeout: int, encoding: dict | None, expectation: dict | None,
                    validate_expectations: bool, clock_control: dict | None = None) -> dict:
    """Start independent converter processes together; inspect PDFs serially.

    Read-lock one prepared pathname to preserve File/Path field equality and
    isolate failure. Keep separate profiles, outputs and post-run snapshots.
    Clock and field bytes stay unchanged; all ordinary strict checks apply.
    """
    if set(runtimes) != {'baseline', 'candidate'}:
        raise ValueError('Paired conversions require both runtimes')
    case.mkdir()
    record = {**item, 'index': index, 'runs': {}, 'error': None, 'changed': None,
              'input_preparation': encoding['codec'] if encoding else 'copy',
              'conversion_input_sha256': encoding['decoded_sha256'] if encoding else item['sha256'],
              'input_sharing': 'one immutable Win32 read-locked copy; independent processes/profiles/outputs'}
    input_dir = case / 'input'
    input_dir.mkdir()
    staged = input_dir / ('source' + original.suffix)
    prepared_hash = corpus.prepare_input(original, staged, item, encoding)
    problems, pdfs = [], {}

    def convert(label, runtime):
        started = time.monotonic()
        result = {'status': 'FAIL', 'error': None, 'pages': 0, 'input_unchanged': False}
        if clock_control is not None:
            result['clock_receipts'] = []
        run_dir = case / label
        out = run_dir / 'output'
        out.mkdir(parents=True)
        try:
            clock_options = {'clock_control': clock_control, 'clock_observations': result['clock_receipts']} if clock_control else {}
            pdf = smoke.convert_one(runtime, staged, out, run_dir / 'profile', timeout,
                                    cleanup_runtime_pycache=False, own_process_tree=True, repeatable_random=True, **clock_options)
            result['pdf'] = str(pdf)
        except Exception as exc:
            result['error'] = f'{type(exc).__name__}: {exc}'
            if isinstance(exc, smoke.NormalLoadRefusal):
                result.update(failure_kind='normal_load_refusal', returncode=exc.returncode,
                              diagnostic=exc.diagnostic, output_pdf_exists=exc.output_pdf_exists)
        finally:
            result['elapsed_seconds'] = round(time.monotonic() - started, 3)
            result['input_unchanged'] = smoke.sha256_file(staged) == prepared_hash
        return result

    with immutable_input(staged):
        with ThreadPoolExecutor(max_workers=2) as pool:
            pending = {label: pool.submit(convert, label, runtime) for label, runtime in runtimes.items()}
            record['runs'] = {label: future.result() for label, future in pending.items()}
        # No PDF engine is shared across threads. All converters have exited.
        for label, result in record['runs'].items():
            try:
                if not result['input_unchanged']:
                    raise ValueError('Conversion copy changed during test')
                if result['error'] is None:
                    pdf = Path(result['pdf'])
                    with quality.fitz.open(pdf) as document:
                        if len(document) == 0:
                            raise ValueError('Converted PDF has no pages')
                        result['pages'] = len(document)
                    result.update(status='SUCCESS', sha256=smoke.sha256_file(pdf))
                    pdfs[label] = pdf
            except Exception as exc:
                result['error'] = f'{type(exc).__name__}: {exc}'
            if result['error']:
                problems.append(f"{label}: {result['error']}")
            snapshot = case / label / staged.name
            shutil.copy2(staged, snapshot)
            if smoke.sha256_file(snapshot) != prepared_hash:
                raise ValueError('Retained conversion input hash mismatch')
            result['input_snapshot'] = str(snapshot)
    return finish_case(record, item, original, case, runtimes, pdfs, problems, expectation, validate_expectations, clock_control)


def score_results(results: list[dict], labels: list[str]) -> dict:
    count = len(results)
    comparison_count = sum(item.get("changed") is not None and item.get("error") is None for item in results)
    equal_count = sum(item.get("outcome") == "both_success_equal" and item.get("error") is None for item in results)
    return {"documents": count, "outcomes": dict(Counter(item["outcome"] for item in results)),
            "runtimes": {label: {
                "success": sum(item["runs"][label]["status"] == "SUCCESS" for item in results),
                "failure": sum(item["runs"][label]["status"] != "SUCCESS" for item in results),
                "success_rate": sum(item["runs"][label]["status"] == "SUCCESS" for item in results) / count if count else None,
            } for label in labels},
            "compared_documents": comparison_count, "equal_documents": equal_count,
            "equality_rate": equal_count / comparison_count if comparison_count else None,
            "validated_documents": sum(item.get("validation", {}).get("passed") is True for item in results),
            "expected_rejections": sum(item.get("validation", {}).get("kind") == "EXPECTED_REJECTION" for item in results)}


def write_scores(output: Path, report: dict) -> None:
    results = report["results"]
    labels = list(report["runtimes"])
    report["scores"] = score_results(results, labels)
    report["scores_by_format"] = {suffix: score_results([item for item in results if Path(item["path"]).suffix == suffix], labels)
                                 for suffix in sorted({Path(item["path"]).suffix for item in results})}
    report["scores_by_group"] = {group: score_results([item for item in results if group in item.get("groups", [])], labels)
                                for group in sorted({group for item in results for group in item.get("groups", [])})}
    # Output is a newly owned directory; CSV contains metadata/metrics, no text.
    with (output / "results.csv").open("x", encoding="utf-8-sig", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["path", "sha256", "input_preparation", "conversion_input_sha256", "groups", "outcome", "baseline_status", "candidate_status",
                         "baseline_seconds", "candidate_seconds", "baseline_error", "candidate_error",
                         "baseline_pages", "candidate_pages", "mean_text_similarity", "max_difference_ratio", "error",
                         "validation_kind", "validation_passed"])
        for item in results:
            baseline = item["runs"].get("baseline", {})
            candidate = item["runs"]["candidate"]
            comparison = item.get("comparison", {})
            writer.writerow([item["path"], item["sha256"], item["input_preparation"], item["conversion_input_sha256"],
                             ",".join(item.get("groups", [])), item["outcome"],
                             baseline.get("status"), candidate["status"], baseline.get("elapsed_seconds"), candidate["elapsed_seconds"],
                             baseline.get("error"), candidate.get("error"), baseline.get("pages"), candidate.get("pages"),
                             comparison.get("mean_text_similarity"), comparison.get("max_difference_ratio"), item["error"],
                             item.get("validation", {}).get("kind"), item.get("validation", {}).get("passed")])


def write_checkpoint(path: Path, payload: dict) -> None:
    # A Windows reader/antivirus can briefly deny replacement of the visible
    # progress file. Keep the previous atomic snapshot and retry for <= 0.5 s.
    # Persistent denial still fails the entire run; no alternate output path.
    for attempt in range(11):
        try:
            quality.write_json_atomic(path, payload)
            return
        except PermissionError:
            if attempt == 10:
                raise
            time.sleep(0.05)


def run_test(source_dir: Path, soffice: Path, output_dir: Path, *,
             baseline_soffice: Path | None = None, timeout: int = 180,
             manifest_path: Path = MANIFEST, groups: list[str] | None = None,
             individual: list[str] | None = None, acceptance: bool = False, workers: int = 1,
             expectations_path: Path | None = None, paired: bool = False, fixed_time: int | None = None) -> dict:
    if timeout < 1:
        raise ValueError("Conversion timeout must be positive")
    if workers < 1 or workers > 8:
        raise ValueError("Workers must be between 1 and 8")
    if paired and (os.name != 'nt' or not baseline_soffice or workers > 4):
        raise ValueError('Paired conversions require Windows, baseline and at most 4 workers (8 converter processes)')
    if acceptance and baseline_soffice is None:
        raise ValueError("Acceptance requires a preserved baseline runtime")
    if fixed_time is not None:
        test_clock.verify_platform(fixed_time)
        if not baseline_soffice or not expectations_path:
            raise ValueError('Fixed-time comparison requires both runtimes and formal expectation metadata')
    manifest, samples = collect_samples(source_dir, manifest_path, groups=groups,
                                        individual=individual, acceptance=acceptance)
    encoding_hash = corpus.sha256(corpus.ENCODINGS)
    encodings = corpus.fixture_encodings(source_dir, manifest["samples"])
    expectation_hash = corpus.sha256(expectations_path) if expectations_path else None
    expectation_catalog = (corpus.read_catalog(manifest_path if "catalog_version" in manifest else corpus.CATALOG)
                           if expectations_path else None)
    expected_results = expectations.load_expectations(source_dir, expectation_catalog, expectations_path) if expectations_path else {}
    validate_expectations = bool(expectations_path and baseline_soffice)
    soffice = soffice.resolve()
    baseline_soffice = baseline_soffice.resolve() if baseline_soffice else None
    runtimes = {"candidate": soffice}
    if baseline_soffice:
        runtimes = {"baseline": baseline_soffice, **runtimes}
    identities = {label: runtime_identity(path, full=True) for label, path in runtimes.items()}
    control = test_clock.capture(source_dir, manifest['source_version'], runtimes, fixed_time) if fixed_time is not None else None
    output_dir = output_dir.resolve()
    for protected in [source_dir.resolve(), *(path.parent.parent for path in runtimes.values())]:
        if output_dir.is_relative_to(protected) or protected.is_relative_to(output_dir):
            raise ValueError("Output must be separate from source and runtime directories")
    output_dir.mkdir(parents=True, exist_ok=False)
    clock_control = None
    if control is not None:
        control_path = output_dir / 'clock_control.json'
        quality.write_json_atomic(control_path, control)
        clock_control = {'path': str(control_path), 'sha256': corpus.sha256(control_path), 'control': control}
    report = {"report_version": 4 if control else 3 if validate_expectations else 2, "source_version": manifest["source_version"], "source_dir": str(source_dir.resolve()),
              "catalog_sha256": corpus.sha256(manifest_path), "acceptance": acceptance,
              "fixture_encodings_sha256": encoding_hash,
              "selected_groups": groups or [], "selected_individual": individual or [],
              "mode": "comparison" if baseline_soffice else "conversion-only",
              "controls": {"input_path": "same absolute path; immutable shared copy, individual snapshots" if paired else "same absolute path; fresh copy per runtime",
                           "random": "SAL_RAND_REPEATABLE=1 (seed 42)", "clock": "host clock; not frozen",
                           "profile_configuration_root": "oor:items",
                           "conversion_scheduling": "paired independent processes; immutable shared input" if paired else "sequential independent copies"},
              "dpi": 144, "pixel_threshold": 8, "workers": workers, "runtimes": identities, "results": []}
    if expectations_path:
        report.update(expectations_sha256=expectation_hash,
                      expectations={item["path"]: expected_results[item["path"]]
                                    for item in manifest["samples"] if item["path"] in expected_results})
        report["controls"]["expected_results"] = "pinned upstream refusals; conversion FAIL scores retained"
    if clock_control is not None:
        report['clock_control'] = clock_control
        report['controls']['clock'] = f"owned Win64 process wall-clock fixed at {control['utc']}; host clock and monotonic time unchanged"
    quality.write_json_atomic(output_dir / "identity.json", {key: value for key, value in report.items() if key != "results"})
    errors = differences = pages = 0
    jobs = [(index, item, original, output_dir / f"case_{index:02d}", runtimes, timeout, encodings.get(item["path"]),
             expected_results.get(item["path"]), validate_expectations, paired, clock_control)
            for index, (item, original) in enumerate(zip(manifest["samples"], samples))]

    def consume(records):
        nonlocal errors, differences, pages
        for record in records:
            errors += int(record["error"] is not None)
            differences += int(record["changed"] is True)
            pages += record["runs"]["candidate"].get("pages", 0)
            report["results"].append(record)
            write_checkpoint(output_dir / "partial.json", {
                "status": "INCOMPLETE", "processed_documents": len(report["results"]),
                "expected_documents": len(samples), "errors": errors, "different_documents": differences,
                "last_result": str(output_dir / f"case_{record['index']:02d}" / "result.json")})
            verdict = f" [{record['validation']['kind']}]" if validate_expectations else ""
            print(f"[{len(report['results'])}/{len(samples)}] {record['outcome']}{verdict} {record['path']}", flush=True)

    if workers == 1:
        consume(run_case(*job) for job in jobs)
    else:
        # PDF engines and converters run in separate processes, never threads.
        # Each process owns one case/profile at a time. The parent waits for all
        # bounded active conversions on cancellation, retaining partial evidence.
        with ProcessPoolExecutor(max_workers=workers) as executor:
            futures = [executor.submit(run_case, *job) for job in jobs]
            try:
                consume(future.result() for future in as_completed(futures))
            except BaseException:
                for future in futures:
                    future.cancel()
                raise
    report["results"].sort(key=lambda item: item["index"])
    if corpus.sha256(corpus.ENCODINGS) != encoding_hash:
        raise ValueError("Fixture encoding metadata changed during test")
    corpus.fixture_encodings(source_dir, manifest["samples"])
    if expectations_path:
        if corpus.sha256(expectations_path) != expectation_hash:
            raise ValueError("Expectation metadata changed during test")
        if expectations.load_expectations(source_dir, expectation_catalog, expectations_path) != expected_results:
            raise ValueError("Expectation evidence changed during test")
    if acceptance:
        # Detect additions/deletions and changes to previously completed inputs,
        # not only changes at the moment an individual conversion completed.
        collect_samples(source_dir, manifest_path, acceptance=True)
        if corpus.sha256(manifest_path) != report["catalog_sha256"]:
            raise ValueError("Corpus catalog changed during acceptance test")
    if identities != {label: runtime_identity(path, full=True) for label, path in runtimes.items()}:
        raise ValueError("Runtime identity changed during test")
    if clock_control is not None:
        if corpus.sha256(Path(clock_control['path'])) != clock_control['sha256']:
            raise ValueError('Clock control file changed during test')
        test_clock.audit(control, source_dir, manifest['source_version'])
    if validate_expectations:
        report["audits"] = {"runtime_unchanged": True, "expectation_evidence_unchanged": True,
                            "acceptance_inventory_unchanged": bool(acceptance)}
        if clock_control is not None:
            report['audits']['clock_control_unchanged'] = True
    validation_failures = sum(item["validation"]["passed"] is not True for item in report["results"]) if validate_expectations else errors + differences
    expected_rejections = sum(item.get("validation", {}).get("kind") == "EXPECTED_REJECTION" for item in report["results"])
    comparison_state = ("completed-for-normal-inputs" if validate_expectations and not validation_failures
                        else "completed" if baseline_soffice and not errors else "not-completed")
    report["summary"] = {"expected_documents": len(samples), "completed_documents": len(samples) - errors,
                         "errors": errors, "different_documents": differences, "candidate_pages": pages,
                         "quality_comparison": comparison_state,
                         "acceptance_passed": bool(acceptance and not validation_failures),
                         "status": "FAIL" if validation_failures else "PASS"}
    if validate_expectations:
        report["summary"].update(validated_documents=len(samples) - validation_failures,
                                 validation_failures=validation_failures, expected_rejections=expected_rejections)
    write_scores(output_dir, report)
    quality.write_json_atomic(output_dir / "report.json", report)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", help="Local LibreOffice 26.2.6.3 source root")
    parser.add_argument("--catalog", help="Pinned metadata catalog; defaults to full catalog when selectors/acceptance are used")
    parser.add_argument("--group", action="append", default=[], help="Repeatable group selector; union with individual inputs")
    parser.add_argument("--sample", action="append", default=[], help="Repeatable catalog-relative individual input")
    parser.add_argument("--acceptance", action="store_true", help="Require complete full corpus and baseline comparison; reject subset selectors")
    parser.add_argument("--list-groups", action="store_true")
    parser.add_argument("--preflight", action="store_true", help="Validate selected/full inputs without converting; never acceptance evidence")
    parser.add_argument("--soffice", default=str(REPO_ROOT / "third_party/libreoffice/custom_runtime/instdir/program/soffice.com"))
    parser.add_argument("--baseline-soffice", help="Preserved runtime for regression comparison")
    parser.add_argument("--output-dir")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--workers", type=int, default=1, help="1-8 independent case processes; default serial")
    parser.add_argument("--paired-conversions", action="store_true",
                        help="Windows: start both independent converters together on an immutable copy; at most 4 workers")
    parser.add_argument('--fixed-time', type=int, help='Windows x64 QA only: fixed UNIX seconds in owned converter memory; strict comparison retained')
    args = parser.parse_args(argv)
    manifest_path = Path(args.catalog) if args.catalog else (
        corpus.CATALOG if args.group or args.sample or args.acceptance or args.list_groups else MANIFEST)
    if not args.source_dir and not args.list_groups:
        parser.error("--source-dir is required for input verification/conversion")
    output = Path(args.output_dir) if args.output_dir else (
        REPO_ROOT / ".local/repo_resource/tmp" / ("lo_upstream_" + datetime.now().strftime("%Y%m%d_%H%M%S_%f")))
    try:
        if args.fixed_time is not None:
            test_clock.verify_platform(args.fixed_time)
            if not args.baseline_soffice:
                raise ValueError('Fixed-time comparison requires a baseline runtime')
        if args.paired_conversions and (os.name != 'nt' or not args.baseline_soffice or not 1 <= args.workers <= 4):
            raise ValueError('Paired conversions require Windows, baseline and 1-4 workers')
        if args.list_groups:
            for name, item in corpus.read_catalog(manifest_path)["groups"].items():
                print(f"{name}: {item['count']} {item['description']}")
            return 0
        if args.preflight:
            manifest, samples = collect_samples(Path(args.source_dir), manifest_path,
                                               groups=args.group, individual=args.sample, acceptance=args.acceptance)
            corpus.fixture_encodings(Path(args.source_dir), manifest["samples"])
            expectation_catalog = corpus.read_catalog(manifest_path if "catalog_version" in manifest else corpus.CATALOG)
            expectations.load_expectations(Path(args.source_dir), expectation_catalog)
            if args.fixed_time is not None:
                test_clock.capture(Path(args.source_dir), manifest['source_version'],
                                   {'baseline': Path(args.baseline_soffice).resolve(), 'candidate': Path(args.soffice).resolve()}, args.fixed_time)
            print(f"[PREFLIGHT ONLY] verified_documents={len(samples)} catalog_sha256={corpus.sha256(manifest_path)}")
            return 0
        report = run_test(Path(args.source_dir), Path(args.soffice), output,
                          baseline_soffice=Path(args.baseline_soffice) if args.baseline_soffice else None,
                          timeout=args.timeout, manifest_path=manifest_path,
                          groups=args.group, individual=args.sample, acceptance=args.acceptance, workers=args.workers,
                          expectations_path=expectations.DEFAULT_METADATA, paired=args.paired_conversions, fixed_time=args.fixed_time)
    except Exception as exc:
        print(f"[FAIL] upstream conversion test incomplete: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(report["summary"]))
    print(f"report={output.resolve() / 'report.json'}")
    if not args.baseline_soffice:
        print("[SKIP] PDF quality comparison: baseline runtime not supplied (conversion-only)")
    return 1 if report["summary"]["status"] == "FAIL" else 0


if __name__ == "__main__":
    # QA paths include characters outside the host Windows code page.
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    raise SystemExit(main())
