import json
import importlib.util
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
GATE = REPO_ROOT / "tools" / "release_checks" / "dependency_security_gate.py"
REVIEW = REPO_ROOT / "tools" / "release_checks" / "dependency_security_review.json"


class DependencySecurityGateTests(unittest.TestCase):
    def test_missing_or_unapplied_local_patch_fails_closed(self) -> None:
        spec = importlib.util.spec_from_file_location("dependency_security_gate", GATE)
        gate = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gate)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(2, len(gate.validate_local_patches(root)))
            for vendor, patch in gate.LOCAL_PATCHES:
                target = root / vendor
                (target / "patches").mkdir(parents=True)
                shutil.copyfile(REPO_ROOT / vendor / patch, target / patch)
            (root / "third_party/md4c/src").mkdir()
            shutil.copyfile(REPO_ROOT / "third_party/md4c/src/md4c.c", root / "third_party/md4c/src/md4c.c")
            (root / "third_party/pdfium/licenses").mkdir()
            notice = root / "third_party/pdfium/licenses/libjpeg_turbo.md"
            shutil.copyfile(REPO_ROOT / "third_party/pdfium/licenses/libjpeg_turbo.md", notice)
            self.assertEqual([], gate.validate_local_patches(root))
            notice.write_text(notice.read_text(encoding="utf-8").replace(
                "[README.ijg](libjpeg_turbo.ijg)", "[README.ijg](README.ijg)"), encoding="utf-8")
            self.assertEqual(1, len(gate.validate_local_patches(root)))
            md4c = root / "third_party/md4c/src/md4c.c"
            md4c.write_text(md4c.read_text(encoding="utf-8").replace(
                " || (unsigned)(ch) == 0x3000", ""), encoding="utf-8")
            self.assertEqual(2, len(gate.validate_local_patches(root)))

    def test_md4c_version_requires_release_and_full_commit(self) -> None:
        spec = importlib.util.spec_from_file_location("dependency_security_gate", GATE)
        gate = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gate)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "VERSION"
            for baseline in ("0.5.3", "0.6.0", "1.0.0"):
                version = baseline + "+git." + "a" * 40
                path.write_text(version + "\n", encoding="utf-8")
                self.assertEqual(version, gate.local_version({"name": "MD4C"}, path))
            for version in ("0.6.0", "latest", "0.6.0+git.7fc1815", "0.6.0+git." + "g" * 40):
                path.write_text(version + "\n", encoding="utf-8")
                with self.assertRaises(ValueError):
                    gate.local_version({"name": "MD4C"}, path)

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
        review["review_valid_until"] = "2099-01-01"
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
