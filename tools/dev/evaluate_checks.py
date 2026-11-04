"""Evaluate existing check contracts locally; never build, publish, or run the UI suite.

The recorded source scope is conservative, not a complete build dependency graph.
A PASS is evidence for the named evaluation tests only, not release acceptance.
"""
from __future__ import annotations

import argparse
from contextlib import redirect_stderr, redirect_stdout
from datetime import datetime
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import time
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[2]
PROFILES = {
    "failure-contracts": {
        "purpose": "Failure propagation, missing inputs, negative gate cases and diagnostic evidence",
        "module": "test_check_failure_contracts",
    },
    "memo-wiring": {
        "purpose": "Memo routing, test-only hooks and separation from settings workflow",
        "module": "test_workspace_memo_integration",
    },
}
CHECK_ENTRIES = (
    "publish.ps1",
    "tests/scripts/run_repo_checks.ps1",
    "tests/scripts/run_workspace_memo_tests.ps1",
    "tests/scripts/run_settings_workflow_tests.ps1",
    "tests/scripts/run_ui_automation_fault_tests.ps1",
    "tests/python/test_check_failure_contracts.py",
    "tests/python/test_workspace_memo_integration.py",
    "tools/dev/evaluate_checks.py",
)
# Keep one input policy for the small initial set. Expanding evaluation coverage
# requires reviewing both these inputs and the profile's actual assertions.
INPUT_PATTERNS = (
    "publish.ps1",
    "tests/python/*.py", "tests/scripts/*.ps1", "tools/release_checks/*.py",
    "tools/dev/*.py", "src/**/*.cpp", "src/**/*.cppinc", "src/**/*.h",
    "locales/*.json", "scripts/build/*.ps1", "scripts/build/*.json",
    "docs/ja/Help_Reference.md", "docs/en/Help_Reference.md",
)


def git(*args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(ROOT), *args], check=True, capture_output=True,
        encoding="utf-8", errors="strict", timeout=30,
    ).stdout.strip()


def input_snapshot() -> dict[str, str]:
    inputs = {}
    for pattern in INPUT_PATTERNS:
        paths = sorted(path for path in ROOT.glob(pattern) if path.is_file())
        if not paths:
            raise RuntimeError(f"Evaluation input pattern has no files: {pattern}")
        for path in paths:
            inputs[path.relative_to(ROOT).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return dict(sorted(inputs.items()))


def evaluate_suite(suite: unittest.TestSuite, stream) -> dict:
    started = time.monotonic()
    with redirect_stdout(stream), redirect_stderr(stream):
        result = unittest.TextTestRunner(stream=stream, verbosity=2).run(suite)
    if not result.wasSuccessful():
        status = "FAIL"
    elif result.testsRun == 0 or result.skipped or result.expectedFailures:
        status = "INCOMPLETE"
    else:
        status = "PASS"
    return {
        "status": status, "tests_run": result.testsRun,
        "elapsed_seconds": round(time.monotonic() - started, 3),
        "failures": [test.id() for test, _ in result.failures],
        "errors": [test.id() for test, _ in result.errors],
        "skipped": [{"test": test.id(), "reason": reason} for test, reason in result.skipped],
        "expected_failures": [test.id() for test, _ in result.expectedFailures],
        "unexpected_successes": [test.id() for test in result.unexpectedSuccesses],
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--list", action="store_true", help="Show scope and committed update date; do not evaluate")
    action.add_argument("--evaluate", action="store_true", help="Run the selected contract evaluations")
    parser.add_argument("--profile", action="append", choices=PROFILES, help="Repeat to select profiles; default: all")
    args = parser.parse_args(argv)
    profiles = args.profile or list(PROFILES)
    if len(set(profiles)) != len(profiles):
        parser.error("Duplicate evaluation profile")
    metadata = {
        "git_head": git("rev-parse", "HEAD"),
        "last_committed_update": git("log", "-1", "--format=%cI", "--", *INPUT_PATTERNS),
        "working_tree_changes": git("status", "--short", "--untracked-files=all", "--", *INPUT_PATTERNS),
        "input_patterns": INPUT_PATTERNS,
        "entrypoint_updates": [{
            "path": path,
            "last_committed_update": git("log", "-1", "--format=%cI", "--", path) or None,
            "uncommitted_changes": bool(git("status", "--short", "--", path)),
            "sha256": hashlib.sha256((ROOT / path).read_bytes()).hexdigest(),
        } for path in CHECK_ENTRIES],
    }
    print("Check-system evaluation; release acceptance and UI reliability are not evaluated.")
    print(f"Last committed input update: {metadata['last_committed_update']}")
    print(f"Uncommitted input changes: {bool(metadata['working_tree_changes'])}")
    if args.list:
        for item in metadata["entrypoint_updates"]:
            print(f"{item['path']}: updated={item['last_committed_update'] or 'UNCOMMITTED_NEW'}; "
                  f"uncommitted_changes={item['uncommitted_changes']}")
        for name in profiles:
            print(f"{name}: {PROFILES[name]['purpose']} ({PROFILES[name]['module']})")
        print("Evaluation: NOT_RUN")
        return 0

    stamp = datetime.now().astimezone().strftime("%Y%m%d_%H%M%S")
    output = ROOT / "out/tests/check_evaluation" / f"{stamp}_{uuid.uuid4().hex}"
    output.mkdir(parents=True, exist_ok=False)
    report = {
        "format": "check-evaluation-v1", "started_at": datetime.now().astimezone().isoformat(),
        "scope": "Named contract tests only; no product, release or UI repeatability acceptance",
        "python": sys.version, "platform": platform.platform(), **metadata, "profiles": [],
    }
    sys.path.insert(0, str(ROOT / "tests/python"))
    for name in profiles:
        log = output / f"{name}.log"
        item = {"profile": name, **PROFILES[name], "log": str(log)}
        try:
            before = input_snapshot()
            item["input_sha256"] = before
            with log.open("x", encoding="utf-8") as stream:
                suite = unittest.TestLoader().loadTestsFromName(PROFILES[name]["module"])
                item.update(evaluate_suite(suite, stream))
            changed = before != input_snapshot()
            item["inputs_changed_during_evaluation"] = changed
            if changed:
                item["status"] = "INPUT_CHANGED"
        except Exception as error:
            item.update(status="ERROR", error=f"{type(error).__name__}: {error}")
        item["evaluated_at"] = datetime.now().astimezone().isoformat()
        report["profiles"].append(item)
        print(f"{name}: {item['status']} (tests={item.get('tests_run', 0)}); log: {log}")
    report["finished_at"] = datetime.now().astimezone().isoformat()
    report["status"] = "PASS" if all(item["status"] == "PASS" for item in report["profiles"]) else "FAIL"
    path = output / "evaluation.json"
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Evaluation report: {path}")
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
