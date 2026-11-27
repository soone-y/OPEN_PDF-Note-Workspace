"""Run the Python Tool Tests suite by group and verify each group's declared inputs.

The suite is the union of the TestCase classes that ``python -m unittest`` collects
from ``tests/python/test_python_tools.py`` and ``tests/python/test_dependency_security_gate.py``.
``tests/config/python_tool_test_groups.json`` assigns classes to selectable groups;
classes it does not list belong to the always-run ``core`` group, so the groups
always cover the complete suite.

With ``--results`` the runner records, per test, what it depended on: Git-tracked
files the test process opened or passed to a child process, tracked directories it
listed, repository modules it imported, and the Python functions it executed
(``sys.monitoring``). CI keeps these records like a build's dependency files and
skips a heavy test only when a diff cannot affect it (see .github/ci/selection.py).
Child processes are not observed; their inputs are the manifest groups' ``paths``.
"""
from __future__ import annotations

import argparse
import fnmatch
import importlib
import inspect
import json
import os
import subprocess
import sys
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST = REPO_ROOT / "tests/config/python_tool_test_groups.json"
SUITE_MODULES = ("tests.python.test_python_tools", "tests.python.test_dependency_security_gate")
CORE = "core"


def load_manifest(path: Path = MANIFEST) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != "pnw-python-tool-test-groups-v1":
        raise ValueError("unexpected python tool test group manifest schema")
    groups = data.get("groups")
    if not isinstance(groups, dict) or not groups or CORE in groups:
        raise ValueError("manifest groups must be a non-empty object without 'core'")
    for key in ("all_groups_paths", "core_paths"):
        if not isinstance(data.get(key), list) or not all(isinstance(x, str) and x for x in data[key]):
            raise ValueError(f"manifest {key} must be a list of globs")
    seen: set[str] = set()
    for name, group in groups.items():
        if not (isinstance(name, str) and name.replace("_", "").isalnum() and name.islower()):
            raise ValueError(f"unsafe group name: {name!r}")
        for key in ("classes", "paths"):
            values = group.get(key) if isinstance(group, dict) else None
            if not isinstance(values, list) or not values or not all(isinstance(x, str) and x for x in values):
                raise ValueError(f"group {name} needs a non-empty {key} list")
        for cls in group["classes"]:
            if cls in seen:
                raise ValueError(f"class assigned twice: {cls}")
            seen.add(cls)
    return data


def group_names(manifest: dict) -> list[str]:
    return [CORE, *manifest["groups"]]


def matches(path: str, globs: list[str]) -> bool:
    """Match a repository-relative POSIX path; '**/' also matches zero directories."""
    for pattern in globs:
        if fnmatch.fnmatchcase(path, pattern):
            return True
        if "**/" in pattern and fnmatch.fnmatchcase(path, pattern.replace("**/", "")):
            return True
    return False


def class_key(cls: type) -> str:
    source = Path(inspect.getfile(cls)).resolve().relative_to(REPO_ROOT).as_posix()
    return f"{source}::{cls.__name__}"


def tracked_files() -> dict[str, str]:
    output = subprocess.check_output(["git", "ls-files", "-z"], cwd=REPO_ROOT, timeout=60)
    files = output.decode("utf-8").removesuffix("\0").split("\0")
    if not files or files == [""]:
        raise RuntimeError("git ls-files returned no tracked files")
    return {os.path.normcase(str(REPO_ROOT / name)): name for name in files}


class Recorder:
    def __init__(self, tracked: dict[str, str]):
        self.tracked = tracked
        self.tracked_dirs = {os.path.normcase(os.path.dirname(k)): os.path.dirname(v)
                             for k, v in tracked.items()}
        self.phase = "load"
        self.reads: dict[str, set[str]] = {}
        self.commands: dict[str, list[str]] = {}
        # Per-test evidence; "test" is None between tests (class fixtures), which
        # is recorded per group as shared input of every test in that group.
        self.test: str | None = None
        self.per_test: dict[str, dict] = {}

    def _bucket(self) -> dict:
        key = self.test or f"<shared:{self.phase}>"
        return self.per_test.setdefault(key, {"group": self.phase, "reads": set(), "dirs": set(),
                                               "funcs": set(), "spawn": False, "seconds": 0.0})

    def _resolve(self, value: object) -> str | None:
        if isinstance(value, bytes):
            value = os.fsdecode(value)
        if not isinstance(value, (str, os.PathLike)):
            return None
        try:
            return os.path.normcase(os.path.abspath(os.fspath(value)))
        except (TypeError, ValueError, OSError):
            return None

    def _note(self, value: object) -> None:
        key = self._resolve(value)
        name = self.tracked.get(key) if key else None
        if name is not None:
            self.reads.setdefault(self.phase, set()).add(name)
            if self.phase not in ("load", "after"):
                self._bucket()["reads"].add(name)

    def _note_dir(self, value: object) -> None:
        key = self._resolve(value if value is not None else ".")
        name = self.tracked_dirs.get(key) if key else None
        if name is not None and self.phase not in ("load", "after"):
            self._bucket()["dirs"].add(name)

    def on_py_start(self, code, offset):  # sys.monitoring PY_START callback
        key = os.path.normcase(code.co_filename)
        name = self.tracked.get(key)
        if name is not None and self.phase not in ("load", "after"):
            self._bucket()["funcs"].add(f"{name}::{code.co_qualname}")
        return sys.monitoring.DISABLE

    def note_modules(self, names: set[str]) -> None:
        """Record module sources: a cached .pyc import never opens the tracked .py."""
        for name in names:
            module = sys.modules.get(name)
            source = getattr(module, "__file__", None) if module is not None else None
            if source:
                self._note(source)

    def hook(self, event: str, args: tuple) -> None:
        if event == "open" and args:
            self._note(args[0])
        elif event in ("os.listdir", "os.scandir") and args:
            self._note_dir(args[0])
        elif event == "subprocess.Popen" and len(args) >= 2:
            argv = args[1]
            items = [argv] if isinstance(argv, (str, bytes, os.PathLike)) else list(argv or ())
            text = [os.fsdecode(x) if isinstance(x, bytes) else str(x) for x in items]
            self.commands.setdefault(self.phase, []).append(" ".join(text)[:300])
            if self.phase not in ("load", "after"):
                self._bucket()["spawn"] = True
            for item in items:
                self._note(item)


class RecordingResult(unittest.TextTestResult):
    """Attribute evidence, elapsed time and the outcome to the running test."""
    recorder: Recorder | None = None

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.outcomes: dict[str, str] = {}

    def _mark(self, test, status: str) -> None:
        case = getattr(test, "test_case", test)  # subTest results belong to their test
        if isinstance(case, unittest.TestCase) and not isinstance(case, unittest.case._SubTest):
            key = test_id(case)
            if self.outcomes.get(key) != "FAIL":
                self.outcomes[key] = status

    def startTest(self, test):
        self._mark(test, "PASS")
        recorder = self.recorder
        if recorder is not None:
            recorder.test = test_id(test)
            recorder._bucket()
            if hasattr(sys, "monitoring"):
                sys.monitoring.restart_events()
        self._started = time.perf_counter()
        super().startTest(test)

    def stopTest(self, test):
        super().stopTest(test)
        recorder = self.recorder
        if recorder is not None and recorder.test is not None:
            recorder._bucket()["seconds"] += time.perf_counter() - self._started
            recorder.test = None

    def addFailure(self, test, err):
        self._mark(test, "FAIL")
        super().addFailure(test, err)

    def addError(self, test, err):
        self._mark(test, "FAIL")
        super().addError(test, err)

    def addSubTest(self, test, subtest, err):
        if err is not None:
            self._mark(test, "FAIL")
        super().addSubTest(test, subtest, err)

    def addUnexpectedSuccess(self, test):
        self._mark(test, "FAIL")
        super().addUnexpectedSuccess(test)

    def addSkip(self, test, reason):
        # A unittest skip proves nothing; the test is not recorded as passing.
        self._mark(test, "SKIPPED")
        super().addSkip(test, reason)


def test_id(test: unittest.TestCase) -> str:
    return f"{class_key(type(test))}.{test._testMethodName}"


def collect_classes(manifest: dict) -> dict[str, list[type]]:
    loader = unittest.defaultTestLoader
    classes: dict[str, type] = {}
    for module_name in SUITE_MODULES:
        module = importlib.import_module(module_name)
        for suite in loader.loadTestsFromModule(module):
            tests = list(suite)
            if tests:
                cls = type(tests[0])
                classes.setdefault(class_key(cls), cls)
    assigned = {cls: name for name, group in manifest["groups"].items() for cls in group["classes"]}
    missing = sorted(set(assigned) - set(classes))
    if missing:
        raise ValueError("manifest lists classes that the suite no longer defines: " + ", ".join(missing))
    result: dict[str, list[type]] = {name: [] for name in group_names(manifest)}
    for key in sorted(classes):
        result[assigned.get(key, CORE)].append(classes[key])
    return result


def record_of(recorder: Recorder, test: str, group: str) -> dict:
    item = recorder.per_test.get(test)
    shared = recorder.per_test.get(f"<shared:{group}>")  # class fixtures of the group
    parts = [x for x in (item, shared) if x]
    return {"group": group, "seconds": round(item["seconds"], 3) if item else 0.0,
            "spawn": any(x["spawn"] for x in parts),
            "reads": sorted(set().union(*(x["reads"] for x in parts))),
            "dirs": sorted(set().union(*(x["dirs"] for x in parts))),
            "funcs": sorted(set().union(*(x["funcs"] for x in parts)))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--group", action="append", default=[], help="group to run (repeatable)")
    parser.add_argument("--list", action="store_true", help="print group names and classes")
    parser.add_argument("--list-tests", action="store_true", help="print every test id with its group")
    parser.add_argument("--skip-file", help='JSON {"skip": [test ids]} that CI proved unaffected')
    parser.add_argument("--results", help="write per-test outcomes and recorded inputs as JSON")
    args = parser.parse_args()

    manifest = load_manifest()
    names = group_names(manifest)
    unknown = [g for g in args.group if g not in names]
    if unknown:
        print("ERROR: unknown python tool test group: " + ", ".join(unknown), file=sys.stderr)
        return 2
    selected = args.group or names
    skip: set[str] = set()
    if args.skip_file:
        data = json.loads(Path(args.skip_file).read_text(encoding="utf-8"))
        if not isinstance(data, dict) or not isinstance(data.get("skip"), list):
            print("ERROR: invalid python tool test skip file", file=sys.stderr)
            return 2
        skip = set(data["skip"])

    os.chdir(REPO_ROOT)
    if str(REPO_ROOT) not in sys.path:
        sys.path.insert(0, str(REPO_ROOT))
    recorder = Recorder(tracked_files())
    sys.addaudithook(recorder.hook)
    before_load = set(sys.modules)
    classes = collect_classes(manifest)
    recorder.note_modules(set(sys.modules) - before_load)
    loader = unittest.defaultTestLoader

    if args.list or args.list_tests:
        for name in names:
            if args.list:
                print(f"{name}: {len(classes[name])} classes")
            for cls in classes[name]:
                if args.list:
                    print(f"  {class_key(cls)}")
                else:
                    for test in loader.loadTestsFromTestCase(cls):
                        print(f"{name}\t{test_id(test)}")
        return 0

    if args.results:
        RecordingResult.recorder = recorder
        if hasattr(sys, "monitoring"):
            tool = sys.monitoring.COVERAGE_ID
            sys.monitoring.use_tool_id(tool, "pnw-python-tool-groups")
            sys.monitoring.register_callback(tool, sys.monitoring.events.PY_START, recorder.on_py_start)
            sys.monitoring.set_events(tool, sys.monitoring.events.PY_START)

    failed = False
    outcomes: dict[str, dict[str, str]] = {}
    for name in selected:
        tests = [t for c in classes[name] for t in loader.loadTestsFromTestCase(c)]
        # core always runs in full; CI may only skip tests of the heavy groups.
        planned = [] if name == CORE else [t for t in tests if test_id(t) in skip]
        run = [t for t in tests if t not in planned]
        recorder.phase = name
        before_group = set(sys.modules)
        started = time.perf_counter()
        result = unittest.TextTestRunner(stream=sys.stderr, verbosity=1,
                                         resultclass=RecordingResult).run(unittest.TestSuite(run))
        recorder.note_modules(set(sys.modules) - before_group)
        recorder.phase = "after"
        elapsed = time.perf_counter() - started
        status = dict(result.outcomes)
        for test in run:  # for example setUpClass failed before the test started
            status.setdefault(test_id(test), "FAIL")
        for test in planned:
            status[test_id(test)] = "PLANNED_SKIP"
        outcomes[name] = status
        ok = result.wasSuccessful() and (result.testsRun > 0 or (not run and bool(planned)))
        print(f"[{'PASS' if ok else 'FAIL'}] group {name}: tests={result.testsRun} "
              f"skipped_by_plan={len(planned)} failures={len(result.failures)} "
              f"errors={len(result.errors)} elapsed={elapsed:.1f}s")
        if not tests or (result.testsRun == 0 and run):
            print(f"ERROR: python tool test group {name} ran no tests", file=sys.stderr)
        failed |= not ok

    if args.results:
        data = {"schema": "pnw-python-tool-test-results-v1",
                "load": sorted(recorder.reads.get("load", ())),
                "groups": {name: {"tests": status,
                                  "record": {test: record_of(recorder, test, name)
                                             for test, value in status.items()
                                             if value != "PLANNED_SKIP" and name != CORE}}
                           for name, status in outcomes.items()}}
        Path(args.results).parent.mkdir(parents=True, exist_ok=True)
        Path(args.results).write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
