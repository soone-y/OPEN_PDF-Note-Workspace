import json
import importlib.util
import io
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from datetime import date, timedelta
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
GATE = REPO_ROOT / "tools" / "release_checks" / "dependency_security_gate.py"
REVIEW = REPO_ROOT / "tools" / "release_checks" / "dependency_security_review.json"
PDFIUM_NOTICE_PATCH = ("third_party/pdfium", "patches/0001-fix-ijg-notice-link.patch")


class DependencySecurityGateTests(unittest.TestCase):
    def setUp(self) -> None:
        spec = importlib.util.spec_from_file_location("dependency_security_gate", GATE)
        self.gate = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.gate)
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory())).resolve()
        # Keep Git from discovering a parent checkout and skipping fixture paths.
        subprocess.run(["git", "init", "--quiet", str(self.root)], check=True, capture_output=True)
        self.assertIn(PDFIUM_NOTICE_PATCH, self.gate.LOCAL_PATCHES)
        self.enterContext(mock.patch.object(self.gate, "LOCAL_PATCHES", (PDFIUM_NOTICE_PATCH,)))
        self.enterContext(mock.patch.object(self.gate, "REPO_ROOT", self.root))

    def copy_tracked_input(self, relative: Path) -> Path:
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPO_ROOT / relative, target)
        return target

    def install_patch_evidence(self) -> Path:
        vendor, patch = PDFIUM_NOTICE_PATCH
        self.copy_tracked_input(Path(vendor) / patch)
        return self.copy_tracked_input(Path(vendor) / "licenses/libjpeg_turbo.md")

    def test_missing_or_unapplied_local_patch_fails_closed(self) -> None:
        errors = self.gate.validate_local_patches(self.root)
        self.assertEqual(1, len(errors))
        self.assertIn("required local patch missing", errors[0])
        notice = self.install_patch_evidence()
        self.assertEqual([], self.gate.validate_local_patches(self.root))
        notice.write_text(notice.read_text(encoding="utf-8").replace(
            "[README.ijg](libjpeg_turbo.ijg)", "[README.ijg](README.ijg)"), encoding="utf-8")
        errors = self.gate.validate_local_patches(self.root)
        self.assertEqual(1, len(errors))
        self.assertIn("required local patch is not reproducible", errors[0])

    def test_md4c_version_requires_release_and_full_commit(self) -> None:
        path = self.root / "VERSION"
        for baseline in ("0.5.3", "0.6.0", "1.0.0"):
            version = baseline + "+git." + "a" * 40
            path.write_text(version + "\n", encoding="utf-8")
            self.assertEqual(version, self.gate.local_version({"name": "MD4C"}, path))
        for version in ("0.6.0", "latest", "0.6.0+git.7fc1815", "0.6.0+git." + "g" * 40):
            path.write_text(version + "\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                self.gate.local_version({"name": "MD4C"}, path)

    def test_zlib_version_accepts_two_and_three_numeric_components(self) -> None:
        path = self.root / "VERSION"
        component = {"name": "zlib runtime"}
        for version in ("1.4", "1.3.2"):
            path.write_text(version + "\n\nUpstream: https://zlib.net/\n", encoding="utf-8")
            self.assertEqual(version, self.gate.local_version(component, path))
        for version in ("", "1", "1.4beta", "1.3.2.1"):
            path.write_text(version, encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "zlib VERSION is malformed"):
                self.gate.local_version(component, path)

    def fixture_review(self) -> dict:
        self.install_patch_evidence()
        review = self.gate.load_review(REVIEW)
        today = date.today()
        review["reviewed_on"] = today.isoformat()
        review["review_valid_until"] = (today + timedelta(days=1)).isoformat()
        for component in review["components"]:
            self.copy_tracked_input(Path(component["evidence_file"]))
            for artifact in component["artifacts"]:
                path = self.root / artifact["path"]
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(artifact["path"].encode("utf-8"))
                artifact["sha256"] = self.gate.file_sha256(path)
        return review

    def run_gate(self, review: dict, *arguments: str) -> tuple[int, str]:
        path = self.root / "review.json"
        path.write_text(json.dumps(review), encoding="utf-8")
        errors = io.StringIO()
        with mock.patch.object(sys, "argv", [str(GATE), "--review", str(path), *arguments]), \
                redirect_stdout(io.StringIO()), redirect_stderr(errors):
            code = self.gate.main()
        return code, errors.getvalue()

    def test_review_requires_present_and_matching_artifacts(self) -> None:
        review = self.fixture_review()
        code, errors = self.run_gate(review, "--check-record-only")
        self.assertEqual(0, code, errors)
        path = self.root / review["components"][0]["artifacts"][0]["path"]
        path.write_bytes(b"modified artifact")
        code, errors = self.run_gate(review, "--check-record-only")
        self.assertEqual(1, code)
        self.assertIn("reviewed artifact changed", errors)
        path.unlink()
        code, errors = self.run_gate(review, "--check-record-only")
        self.assertEqual(1, code)
        self.assertIn("reviewed artifact is missing", errors)

    def test_blocked_review_fails_closed(self) -> None:
        review = self.fixture_review()
        review["release_decision"] = "blocked"
        review["components"][0]["decision"] = "blocked"
        code, errors = self.run_gate(review)
        self.assertEqual(1, code)
        self.assertIn("blocked this release", errors)

    def test_expired_review_fails_even_in_record_check_mode(self) -> None:
        review = self.fixture_review()
        review["reviewed_on"] = review["review_valid_until"] = (date.today() - timedelta(days=1)).isoformat()
        code, errors = self.run_gate(review, "--check-record-only")
        self.assertEqual(1, code)
        self.assertIn("expired", errors)


if __name__ == "__main__":
    unittest.main()
