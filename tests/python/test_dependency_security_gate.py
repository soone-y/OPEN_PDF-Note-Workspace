import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
GATE = REPO_ROOT / "tools" / "release_checks" / "dependency_security_gate.py"
REVIEW = REPO_ROOT / "tools" / "release_checks" / "dependency_security_review.json"


class DependencySecurityGateTests(unittest.TestCase):
    def run_gate(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(GATE), *arguments],
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_checked_in_review_matches_local_artifacts(self) -> None:
        result = self.run_gate("--check-record-only")
        self.assertEqual(0, result.returncode, result.stderr)

    def test_blocked_review_fails_closed(self) -> None:
        review = json.loads(REVIEW.read_text(encoding="utf-8"))
        review["release_decision"] = "blocked"
        review["components"][0]["decision"] = "blocked"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "blocked-review.json"
            path.write_text(json.dumps(review), encoding="utf-8")
            result = self.run_gate("--review", str(path))
        self.assertNotEqual(0, result.returncode)
        self.assertIn("blocked this release", result.stderr)

    def test_expired_review_fails_even_in_record_check_mode(self) -> None:
        review = json.loads(REVIEW.read_text(encoding="utf-8"))
        review["review_valid_until"] = "2026-08-11"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "expired-review.json"
            path.write_text(json.dumps(review), encoding="utf-8")
            result = self.run_gate("--review", str(path), "--check-record-only")
        self.assertNotEqual(0, result.returncode)
        self.assertIn("expired", result.stderr)


if __name__ == "__main__":
    unittest.main()
