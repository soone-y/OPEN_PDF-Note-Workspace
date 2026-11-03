"""Local prototype notice inventory, original preservation and failure propagation."""
import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('prototype_licenses', ROOT / 'tools/dev/prepare_readonly_viewer_next_licenses.py')
licenses = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(licenses)


class PrototypeLicenseTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / 'out/tests/readonly_viewer_next_licenses'
        parent.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for locale in ('ja', 'en'):
            for variant in ('prototype', 'standard'):
                for source in licenses.source_files(ROOT, locale, variant).values():
                    destination = self.root / source.relative_to(ROOT)
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(source, destination)
        self.output = self.root / 'out/bin'

    def test_both_locales_preserve_originals(self):
        for locale in ('ja', 'en'):
            for variant in ('prototype', 'standard'):
                output = self.output / locale / variant
                self.assertEqual(licenses.prepare(self.root, output, locale, False, variant), 26)
                self.assertEqual(licenses.prepare(self.root, output, locale, True, variant), 26)
                for name, source in licenses.source_files(self.root, locale, variant).items():
                    self.assertEqual((output / name).read_bytes(), source.read_bytes())

    def test_missing_and_empty_input_leave_existing_bundle_intact(self):
        licenses.prepare(self.root, self.output, 'ja', False)
        before = {p.relative_to(self.output): p.read_bytes() for p in self.output.rglob('*') if p.is_file()}
        source = self.root / 'third_party/md4c/LICENSE.md'
        source.unlink()
        with self.assertRaises(OSError): licenses.prepare(self.root, self.output, 'ja', False)
        source.write_bytes(b'')
        with self.assertRaises(ValueError): licenses.prepare(self.root, self.output, 'ja', False)
        after = {p.relative_to(self.output): p.read_bytes() for p in self.output.rglob('*') if p.is_file()}
        self.assertEqual(before, after)

    def test_notice_inventory_change_fails_before_output(self):
        (self.root / 'third_party/pdfium/licenses/unreviewed.txt').write_text('new component')
        with self.assertRaises(ValueError): licenses.prepare(self.root, self.output, 'ja', False)
        self.assertFalse(self.output.exists())

    def test_missing_or_modified_output_fails_check(self):
        licenses.prepare(self.root, self.output, 'ja', False)
        target = self.output / 'licenses/mingw-w64/gcc/COPYING.RUNTIME.txt'
        target.write_text('incomplete')
        with self.assertRaises(ValueError): licenses.prepare(self.root, self.output, 'ja', True)
        target.unlink()
        with self.assertRaises(ValueError): licenses.prepare(self.root, self.output, 'ja', True)

    def test_wrong_locale_and_stale_notice_fail(self):
        licenses.prepare(self.root, self.output, 'ja', False)
        with self.assertRaises(ValueError): licenses.prepare(self.root, self.output, 'en', True)
        (self.output / 'licenses/stale.txt').write_text('stale')
        with self.assertRaises(ValueError): licenses.prepare(self.root, self.output, 'ja', True)

    def test_output_outside_local_build_area_fails(self):
        with self.assertRaises(ValueError): licenses.prepare(self.root, self.root / 'outside', 'ja', False)
        self.assertFalse((self.root / 'outside').exists())


if __name__ == '__main__':
    unittest.main()
