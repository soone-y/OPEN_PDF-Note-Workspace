from pathlib import Path
import unittest
from unittest import mock

import validate_codebase


class OfficeWorkerStaticContractTests(unittest.TestCase):
    def test_current_owned_worker_contract_passes(self):
        self.assertEqual(validate_codebase.find_runtime_safety_regressions(), [])

    def test_missing_worker_safety_evidence_still_fails(self):
        target = validate_codebase.REPO_ROOT / "src/workspace/workspace_actions.cpp"
        original_read = Path.read_text
        source = original_read(target, encoding="utf-8")
        for required in ("JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE", "AssignProcessToJobObject",
                         "kMaxParallelOfficeConversions = 2", "std::thread worker;",
                         "~OfficeBackgroundJob() { if (worker.joinable()) worker.join(); }",
                         "s_officeBackgroundBatch->active.size() < kMaxParallelOfficeConversions",
                         "raw->worker = std::thread(RunOfficeBackgroundJob, raw);",
                         "raw->workerFailed = true;", "raw->done.store(true, std::memory_order_release)",
                         "job.done.load(std::memory_order_acquire)", "job.worker.join();",
                         "SetTimer(hWnd, kOfficeWorkerPollTimerId,"):
            with self.subTest(missing=required):
                self.assertIn(required, source)
                changed = source.replace(required, "REMOVED_SAFETY_EVIDENCE")
                def read(path, *args, **kwargs):
                    return changed if path == target else original_read(path, *args, **kwargs)
                with mock.patch.object(Path, "read_text", read):
                    errors = validate_codebase.find_runtime_safety_regressions()
                self.assertTrue(any("bounded background workers" in error for error in errors), required)

    def test_detached_worker_is_rejected(self):
        target = validate_codebase.REPO_ROOT / "src/workspace/workspace_actions.cpp"
        original_read = Path.read_text
        changed = original_read(target, encoding="utf-8") + "\nvoid invalid_worker() { worker.detach(); }\n"
        def read(path, *args, **kwargs):
            return changed if path == target else original_read(path, *args, **kwargs)
        with mock.patch.object(Path, "read_text", read):
            errors = validate_codebase.find_runtime_safety_regressions()
        self.assertTrue(any("must retain an owner" in error for error in errors))
