"""Tests for the locale catalog generator."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPO_ROOT / "tools" / "localization" / "generate_locale_catalog.py"
PREPARER_PATH = REPO_ROOT / "tools" / "localization" / "prepare_locale_catalog.py"
SPEC = importlib.util.spec_from_file_location("generate_locale_catalog", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
catalog_generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(catalog_generator)


class LocalizationCatalogTests(unittest.TestCase):
    def write_catalog(self, directory: Path, name: str, values: dict[str, str]) -> Path:
        path = directory / name
        path.write_text(json.dumps(values, ensure_ascii=False), encoding="utf-8")
        return path

    def generate(self, directory: Path, locale: str, ja: dict[str, str], localized: dict[str, str]) -> tuple[Path, Path]:
        ja_path = self.write_catalog(directory, "ja.json", ja)
        localized_path = self.write_catalog(directory, f"{locale}.json", localized)
        output = directory / "generated" / "locale_catalog.generated.h"
        report = directory / "generated" / "fallback_report.json"
        result = catalog_generator.main([
            "--locale", locale,
            "--ja", str(ja_path),
            "--localized", str(localized_path),
            "--output", str(output),
            "--report", str(report),
        ])
        self.assertEqual(result, 0)
        return output, report

    def test_english_uses_japanese_when_translation_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output, report = self.generate(
                Path(temporary), "en",
                {"menu.open": "開く", "menu.close": "閉じる"},
                {"menu.open": "Open"},
            )

            generated = output.read_text(encoding="utf-8")
            self.assertIn('id == L"menu.open") return L"Open"', generated)
            self.assertIn('id == L"menu.close") return L"閉じる"', generated)
            self.assertEqual(json.loads(report.read_text(encoding="utf-8"))["fallback_ids"], ["menu.close"])

    def test_unknown_translation_id_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            with self.assertRaisesRegex(ValueError, "absent from Japanese"):
                self.generate(directory, "en", {"menu.open": "開く"}, {"menu.save": "Save"})

    def test_placeholder_mismatch_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            with self.assertRaisesRegex(ValueError, "placeholder mismatch"):
                self.generate(directory, "en", {"status.count": "{COUNT} 件"}, {"status.count": "Items"})

    def test_invalid_id_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            with self.assertRaisesRegex(ValueError, "invalid text ID"):
                self.generate(directory, "en", {"Menu Open": "開く"}, {"Menu Open": "Open"})

    def test_ui_text_field_names_generate_catalog_ids(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output, _ = self.generate(
                Path(temporary), "en", {"ui.app_title": "日本語名"}, {"ui.app_title": "English name"}
            )

            generated = output.read_text(encoding="utf-8")
            self.assertIn('Text(L"ui.app_title")', generated)
            self.assertNotIn('ui.app_\\\\1itle', generated)

    def test_preparer_revalidates_changed_source_and_repairs_changed_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = directory / "src"
            source.mkdir()
            source_file = source / "main.cpp"
            source_file.write_text('localization::Text(L"menu.open");\n', encoding="utf-8")
            ja = self.write_catalog(directory, "ja.json", {"menu.open": "開く"})
            en = self.write_catalog(directory, "en.json", {"menu.open": "Open"})
            output = directory / "generated" / "locale_catalog.generated.h"
            report = directory / "generated" / "fallback_report.json"
            usage_stamp = directory / "generated" / "usage.stamp.json"
            catalog_stamp = directory / "generated" / "catalog.stamp.json"
            command = [
                sys.executable, str(PREPARER_PATH), "--source", str(source), "--ja", str(ja), "--en", str(en),
                "--locale", "en", "--localized", str(en), "--output", str(output), "--report", str(report),
                "--usage-stamp", str(usage_stamp), "--catalog-stamp", str(catalog_stamp),
            ]

            first = subprocess.run(command, check=False, capture_output=True, text=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            second = subprocess.run(command, check=False, capture_output=True, text=True)
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertIn("Locale usage validation cache hit.", second.stdout)
            self.assertIn("Locale catalog cache hit: en", second.stdout)

            output.write_text("damaged\n", encoding="utf-8")
            repaired = subprocess.run(command, check=False, capture_output=True, text=True)
            self.assertEqual(repaired.returncode, 0, repaired.stderr)
            self.assertIn('id == L"menu.open") return L"Open"', output.read_text(encoding="utf-8"))

            source_file.write_text('localization::Text(L"menu.missing");\n', encoding="utf-8")
            invalid = subprocess.run(command, check=False, capture_output=True, text=True)
            self.assertEqual(invalid.returncode, 1)
            self.assertIn("source references ID absent from Japanese catalog: menu.missing", invalid.stderr)


if __name__ == "__main__":
    unittest.main()
