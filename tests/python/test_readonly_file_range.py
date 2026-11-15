"""Exact byte arithmetic for huge-file UI, including independent wide-integer oracles."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ReadonlyFileRangeTests(unittest.TestCase):
    def compile_and_run(self, directory):
        exe = Path(directory) / 'range_tests.exe'
        compiled = subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                                  '-I', str(directory), '-I', str(ROOT / 'src'),
                                  str(ROOT / 'tests/unit/readonly_file_range_tests.cpp'),
                                  '-static-libgcc', '-static-libstdc++', '-o', str(exe)],
                                 cwd=ROOT, timeout=60, capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        return subprocess.run([str(exe)], cwd=ROOT, timeout=15, capture_output=True, text=True)

    @unittest.skipUnless(shutil.which('g++'), 'requires the project C++ compiler')
    def test_exact_offsets_overflow_invalid_input_and_caps(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'out/tests', prefix='reader_range_') as directory:
            result = self.compile_and_run(directory)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('Byte-range checks: 16032 PASS', result.stdout)
            self.assertIn('Byte-dump checks: 265 PASS', result.stdout)

    @unittest.skipUnless(shutil.which('g++'), 'requires the project C++ compiler')
    def test_oracle_rejects_overflow_and_missing_read_cap(self):
        source = (ROOT / 'src/readonly_viewer/file_range.h').read_text(encoding='utf-8')
        mutations = {
            'overflow': ('return (size / 1000) * part + (size % 1000) * part / 1000;',
                         'return size * part / 1000;'),
            'uncapped': (' || count > kMaximumBytes', ''),
        }
        for label, (original, replacement) in mutations.items():
            with self.subTest(label=label), tempfile.TemporaryDirectory(dir=ROOT / 'out/tests', prefix='reader_range_negative_') as directory:
                self.assertEqual(source.count(original), 1)
                header = Path(directory) / 'readonly_viewer/file_range.h'
                header.parent.mkdir()
                header.write_text(source.replace(original, replacement), encoding='utf-8')
                result = self.compile_and_run(directory)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Failed byte-range check', result.stderr)


if __name__ == '__main__':
    unittest.main()
