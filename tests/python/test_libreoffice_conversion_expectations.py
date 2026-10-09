from __future__ import annotations

import copy
import io
import json
import sys
import tempfile
import subprocess
import zipfile
import struct
import unittest
from pathlib import Path
from contextlib import ExitStack, redirect_stdout
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/libreoffice'))
import libreoffice_conversion_expectations as expectations
import libreoffice_conversion_corpus as corpus
import libreoffice_upstream_conversion_test as upstream
import libreoffice_runtime_removal_trial as removal
import libreoffice_test_clock as clock


class ConversionExpectationsTests(unittest.TestCase):
    def setUp(self):
        # Retain fixtures: this task expressly forbids deleting untracked files.
        directory = ROOT / '.local/repo_resource/tmp'
        directory.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(prefix='refusal_unit_', dir=directory))
        self.source = self.root / 'source'
        self.source.mkdir()
        self.relative = 'module/qa/data/broken.docx'
        self.input = self.source / self.relative
        self.input.parent.mkdir(parents=True)
        self.input.write_bytes(b'deliberately malformed fixture')
        self.evidence = self.source / 'module/qa/test.cxx'
        self.evidence.write_text('CPPUNIT_TEST_FIXTURE(Test, testReject) { /* refusal */ }', encoding='utf-8')
        self.entry = {'path': self.relative, 'sha256': expectations.sha256(self.input),
                      'expectation': expectations.EXPECTATION,
                      'evidence': {'path': 'module/qa/test.cxx', 'sha256': expectations.sha256(self.evidence),
                                   'test': 'testReject', 'assertion': 'reviewed upstream rejection assertion'}}
        self.catalog = {'source_version': 'unit', 'samples': [{'path': self.relative, 'sha256': self.entry['sha256']}]}
        self.metadata = self.root / 'metadata.json'
        self.data = {'version': 1, 'source_version': 'unit', 'sample_count': 1, 'samples': [self.entry]}
        self.save()
        refusal = {'status': 'FAIL', 'failure_kind': 'normal_load_refusal', 'returncode': 1,
                   'diagnostic': expectations.REFUSAL_DIAGNOSTIC, 'output_pdf_exists': False,
                   'input_unchanged': True}
        self.record = {'path': self.relative, 'sha256': self.entry['sha256'],
                       'outcome': 'both_failed', 'error': 'retained conversion failures',
                       'original_unchanged': True, 'runs': {'baseline': copy.deepcopy(refusal),
                                                           'candidate': copy.deepcopy(refusal)}}
        self.runtime = self.root / 'runtime/program/soffice.com'
        self.runtime.parent.mkdir(parents=True)
        self.runtime.write_bytes(b'mock runtime')

    def save(self):
        self.metadata.write_text(json.dumps(self.data), encoding='utf-8')

    def load(self):
        return expectations.load_expectations(self.source, self.catalog, self.metadata)

    def test_expected_refusal_keeps_conversion_failures_and_requires_both_runs(self):
        entry = self.load()[self.relative]
        before = copy.deepcopy(self.record)
        self.assertEqual(expectations.judge(self.record, entry)['kind'], 'EXPECTED_REJECTION')
        self.assertEqual(self.record, before)
        del self.record['runs']['baseline']
        self.assertFalse(expectations.judge(self.record, entry)['passed'])

    def test_crash_timeout_unknown_failure_and_missing_observation_fail(self):
        changes = [{'returncode': 3221226505}, {'failure_kind': 'timeout'},
                   {'diagnostic': 'unclassified load failure'}, {'output_pdf_exists': True},
                   {'input_unchanged': False}, {'returncode': True}, {'returncode': None}]
        for change in changes:
            with self.subTest(change=change):
                record = copy.deepcopy(self.record)
                record['runs']['candidate'].update(change)
                self.assertFalse(expectations.judge(record, self.entry)['passed'])
        for key in ['failure_kind', 'returncode', 'diagnostic', 'output_pdf_exists', 'input_unchanged']:
            record = copy.deepcopy(self.record)
            del record['runs']['candidate'][key]
            self.assertFalse(expectations.judge(record, self.entry)['passed'])

    def test_source_mutation_and_unexpected_success_fail(self):
        self.record['original_unchanged'] = False
        self.assertFalse(expectations.judge(self.record, self.entry)['passed'])
        self.record['original_unchanged'] = True
        self.record['outcome'] = 'both_success_equal'
        self.assertFalse(expectations.judge(self.record, self.entry)['passed'])

    def test_unlisted_failure_cannot_pass_and_normal_quality_is_still_required(self):
        self.assertFalse(expectations.judge(self.record, None)['passed'])
        self.record.update(outcome='both_success_equal', changed=False, error=None)
        for run in self.record['runs'].values():
            run.update(status='SUCCESS', pages=1)
        self.record['comparison'] = {'reference_pages': 1, 'candidate_pages': 1,
                                     'structural_issues': [], 'semantic_issues': [],
                                     'reference_only_pdf_fonts': [], 'candidate_only_pdf_fonts': [],
                                     'pages': [{'page_size_equal': True, 'same_dimensions': True,
                                                'text_similarity': 1.0, 'different_pixels': 0}]}
        self.assertTrue(expectations.judge(self.record, None)['passed'])
        page = self.record['comparison']['pages'][0]
        page['different_pixels'] = 1
        self.assertFalse(expectations.judge(self.record, None)['passed'])
        page['different_pixels'] = 0
        self.record['comparison']['semantic_issues'] = ['image_counts_per_page']
        self.assertFalse(expectations.judge(self.record, None)['passed'])
        self.record['comparison']['semantic_issues'] = []
        self.record['runs']['baseline']['pages'] = 0
        self.assertFalse(expectations.judge(self.record, None)['passed'])
        self.record['runs']['baseline']['pages'] = 1
        self.record['outcome'] = 'both_success_different'
        self.assertFalse(expectations.judge(self.record, None)['passed'])

    def test_input_and_evidence_hash_changes_or_missing_files_fail(self):
        self.input.write_bytes(b'changed')
        with self.assertRaises(ValueError):
            self.load()
        self.input.write_bytes(b'deliberately malformed fixture')
        self.evidence.write_text('changed source', encoding='utf-8')
        with self.assertRaises(ValueError):
            self.load()
        self.data['samples'][0]['evidence']['path'] = 'module/qa/missing.cxx'
        self.save()
        with self.assertRaises(FileNotFoundError):
            self.load()

    def test_duplicate_unknown_unsafe_or_unsupported_entries_fail(self):
        variants = [dict(self.entry, path='../outside.docx'), dict(self.entry, path='C:/outside.docx'),
                    dict(self.entry, path='unknown.docx'), dict(self.entry, expectation='any-failure-is-pass'),
                    dict(self.entry, sha256='0' * 64)]
        for entry in variants:
            with self.subTest(entry=entry):
                self.data['samples'] = [entry]
                self.save()
                with self.assertRaises(ValueError):
                    self.load()
        self.data['samples'] = [self.entry, self.entry]
        self.data['sample_count'] = 2
        self.save()
        with self.assertRaises(ValueError):
            self.load()

    def test_empty_metadata_version_mismatch_and_missing_test_declaration_fail(self):
        self.data.update(samples=[], sample_count=0)
        self.save()
        with self.assertRaises(ValueError):
            self.load()
        self.data.update(samples=[self.entry], sample_count=1, source_version='different')
        self.save()
        with self.assertRaises(ValueError):
            self.load()
        self.data['source_version'] = 'unit'
        self.entry['evidence']['test'] = 'testMissing'
        self.save()
        with self.assertRaises(ValueError):
            self.load()

    def test_mismatched_result_hash_is_not_expected_refusal(self):
        self.record['sha256'] = '0' * 64
        with self.assertRaises(ValueError):
            expectations.judge(self.record, self.entry)

    def test_directory_refusal_requires_pinned_runner_and_explicit_scoped_input(self):
        relative = 'module/qa/data/fail/broken.docx'
        path = self.source / relative
        path.parent.mkdir()
        path.write_bytes(self.input.read_bytes())
        self.entry['path'] = relative
        self.catalog['samples'][0]['path'] = relative
        self.evidence.write_text('CPPUNIT_TEST(testReject);', encoding='utf-8')
        helper = self.source / 'module/qa/runner.cxx'
        helper.write_text('reviewed expected-failure test runner', encoding='utf-8')
        self.entry['evidence'].update(kind='directory-refusal', scope='module/qa/data/fail',
                                      sha256=expectations.sha256(self.evidence),
                                      support=[{'path': 'module/qa/runner.cxx', 'sha256': expectations.sha256(helper)}])
        self.save()
        self.assertIn(relative, self.load())
        helper.write_text('changed expected behavior', encoding='utf-8')
        with self.assertRaises(ValueError):
            self.load()
        self.entry['evidence']['support'] = []
        self.save()
        with self.assertRaises(ValueError):
            self.load()
        self.entry['evidence']['scope'] = 'module/qa/data/pass'
        self.save()
        with self.assertRaises(ValueError):
            self.load()

    def formal_report(self, refusal=None, *, paired=False, fixed=False, receipt_change=None):
        normal = self.source / 'module/qa/data/normal.docx'
        normal.write_bytes(b'mock normal input')
        catalog = corpus.build_catalog(self.source, 'unit')
        catalog_path = self.root / 'catalog.json'
        catalog_path.write_text(json.dumps(catalog), encoding='utf-8')
        # Pair mode executes converters concurrently; mock their output with
        # prebuilt bytes so no PDF engine is used from conversion threads.
        with upstream.quality.fitz.open() as document:
            document.new_page()
            pdf_bytes = document.tobytes()

        def convert(runtime, source, output, profile, timeout, **kwargs):
            if fixed:
                envelope = kwargs['clock_control']
                control = envelope['control']
                role = output.parent.name
                pin = control['runtime_pins'][role]
                data = {'pid': 42, 'role': role, 'returncode': 1 if source.read_bytes() == b'deliberately malformed fixture' else 0,
                        'control_sha256': envelope['sha256'], 'unix_time': control['unix_time'],
                        'helper_sha256': control['helper_sha256'], 'executable_sha256': pin['executable_sha256'],
                        'sal_sha256': pin['sal_sha256'], 'exceptions_forwarded': 0,
                        **{key: control[key] for key in ('system_dll', 'system_dll_sha256', 'functions')},
                        **{key: True for key in ('clock_applied', 'patched_before_initial_breakpoint', 'executable_disk_unchanged',
                                               'sal_disk_unchanged', 'system_dll_disk_unchanged')}}
                if receipt_change:
                    receipt_change(data)
                receipt = output.parent / 'clock_receipt_0.json'
                receipt.write_text(json.dumps(data), encoding='utf-8')
                kwargs['clock_observations'].append({'path': str(receipt), 'sha256': clock.digest(receipt), 'data': data})
            if source.read_bytes() == b'deliberately malformed fixture':
                if refusal:
                    return refusal(runtime, source, output, profile, timeout, **kwargs)
                raise upstream.smoke.NormalLoadRefusal(source, 1)
            pdf = output / 'source.pdf'
            pdf.write_bytes(pdf_bytes)
            return pdf

        with ExitStack() as stack:
            stack.enter_context(mock.patch.object(upstream.smoke, 'convert_one', side_effect=convert))
            stack.enter_context(redirect_stdout(io.StringIO()))
            if fixed:
                stack.enter_context(mock.patch.object(clock, 'METADATA', self.clock_metadata()))
            report = upstream.run_test(self.source, self.runtime, self.root / 'result',
                                       baseline_soffice=self.runtime, manifest_path=catalog_path,
                                       acceptance=True, expectations_path=self.metadata, paired=paired,
                                       fixed_time=1767323045 if fixed else None)
        return report, catalog_path

    def clock_metadata(self):
        path = self.root / 'clock_metadata.json'
        if not path.exists():
            self.runtime.with_suffix('.bin').write_bytes(b'mock bin')
            self.runtime.with_name('sal3.dll').write_bytes(b'mock sal')
            data = json.loads(clock.METADATA.read_text(encoding='utf-8'))
            data['source_version'] = 'unit'
            for entry in data['source_evidence']:
                target = self.source / entry['path']
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text('mock reviewed source', encoding='utf-8')
                entry['sha256'] = clock.digest(target)
            path.write_text(json.dumps(data), encoding='utf-8')
        return path

    def trial_passed(self, report, catalog):
        return removal.upstream_report_passed(report, acceptance=True, catalog_path=catalog,
                                              expectations_path=self.metadata)

    def test_formal_acceptance_and_trial_keep_refusal_failure_scores(self):
        report, catalog = self.formal_report()
        self.assertTrue(report['summary']['acceptance_passed'])
        self.assertEqual(report['summary']['errors'], 1)
        self.assertEqual(report['summary']['validated_documents'], 2)
        self.assertEqual(report['summary']['expected_rejections'], 1)
        self.assertEqual(report['scores']['runtimes']['candidate']['failure'], 1)
        self.assertEqual(report['scores']['runtimes']['candidate']['success_rate'], 0.5)
        self.assertEqual(report['scores']['compared_documents'], 1)
        self.assertTrue(self.trial_passed(report, catalog))
        original = self.input.read_bytes()
        self.assertEqual(original, b'deliberately malformed fixture')
        for key in ('returncode', 'input_unchanged', 'output_pdf_exists'):
            forged = copy.deepcopy(report)
            del forged['results'][0]['runs']['candidate'][key]
            self.assertFalse(self.trial_passed(forged, catalog))
        forged = copy.deepcopy(report)
        forged['summary']['errors'] = 0
        self.assertFalse(self.trial_passed(forged, catalog))
        forged = copy.deepcopy(report)
        forged['expectations_sha256'] = '0' * 64
        self.assertFalse(self.trial_passed(forged, catalog))

    def test_formal_unclassified_failure_is_not_expected_refusal(self):
        def crash(*args, **kwargs):
            raise RuntimeError('mock crash exit=3221226505')
        report, catalog = self.formal_report(crash)
        self.assertFalse(report['summary']['acceptance_passed'])
        self.assertEqual(report['summary']['validation_failures'], 1)
        self.assertFalse(self.trial_passed(report, catalog))

    def test_expectation_mutation_during_conversion_prevents_final_report(self):
        def mutate(*args, **kwargs):
            self.data['purpose'] = 'changed during run'
            self.save()
            raise upstream.smoke.NormalLoadRefusal(args[1], 1)
        with self.assertRaisesRegex(ValueError, 'Expectation metadata changed'):
            self.formal_report(mutate)
        self.assertFalse((self.root / 'result/report.json').exists())
        self.assertTrue((self.root / 'result/partial.json').exists())

    def test_refusal_source_evidence_mutation_prevents_final_report(self):
        def mutate(*args, **kwargs):
            self.evidence.write_text('changed source', encoding='utf-8')
            raise upstream.smoke.NormalLoadRefusal(args[1], 1)
        with self.assertRaises(ValueError):
            self.formal_report(mutate)
        self.assertFalse((self.root / 'result/report.json').exists())

    def test_mutated_refusal_copy_is_not_a_pass_and_original_is_preserved(self):
        def mutate(*args, **kwargs):
            args[1].write_bytes(b'mutated conversion copy')
            raise upstream.smoke.NormalLoadRefusal(args[1], 1)
        report, catalog = self.formal_report(mutate)
        self.assertFalse(report['summary']['acceptance_passed'])
        self.assertFalse(self.trial_passed(report, catalog))
        self.assertEqual(self.input.read_bytes(), b'deliberately malformed fixture')

    def test_unexpected_pdf_success_on_refusal_input_is_not_a_pass(self):
        def succeed(*args, **kwargs):
            pdf = args[2] / 'source.pdf'
            with upstream.quality.fitz.open() as document:
                document.new_page()
                document.save(pdf)
            return pdf
        report, catalog = self.formal_report(succeed)
        self.assertEqual(report['summary']['errors'], 0)
        self.assertEqual(report['summary']['different_documents'], 0)
        self.assertFalse(report['summary']['acceptance_passed'])
        self.assertFalse(self.trial_passed(report, catalog))

    def test_cli_refusal_observation_distinguishes_crash_and_existing_pdf(self):
        output = self.root / 'cli_output'
        output.mkdir()
        convert = lambda: upstream.smoke.convert_one(self.runtime, self.input, output, self.root / 'profile', 30,
                                                       own_process_tree=True, cleanup_runtime_pycache=False)
        result = subprocess.CompletedProcess([], 1, expectations.REFUSAL_DIAGNOSTIC + '\n')
        with mock.patch.object(upstream.smoke, 'run_owned_conversion', return_value=result):
            with self.assertRaises(upstream.smoke.NormalLoadRefusal) as caught:
                convert()
        self.assertEqual(caught.exception.returncode, 1)
        result.returncode = 3221226505
        with mock.patch.object(upstream.smoke, 'run_owned_conversion', return_value=result):
            with self.assertRaises(RuntimeError) as caught:
                convert()
        self.assertNotIsInstance(caught.exception, upstream.smoke.NormalLoadRefusal)

        result.returncode = 1
        (output / 'unexpected.PDF').write_bytes(b'incomplete output')
        with mock.patch.object(upstream.smoke, 'run_owned_conversion', return_value=result):
            with self.assertRaises(RuntimeError) as caught:
                convert()
        self.assertNotIsInstance(caught.exception, upstream.smoke.NormalLoadRefusal)

    @unittest.skipUnless(sys.platform == 'win32', 'Win32 immutable input lease')
    def test_paired_formal_report_preserves_refusal_scores_and_snapshots(self):
        report, catalog = self.formal_report(paired=True)
        self.assertTrue(report['summary']['acceptance_passed'])
        self.assertEqual(report['summary']['expected_rejections'], 1)
        self.assertEqual(report['summary']['errors'], 1)
        self.assertTrue(self.trial_passed(report, catalog))
        for item in report['results']:
            for run in item['runs'].values():
                self.assertEqual(corpus.sha256(Path(run['input_snapshot'])), item['conversion_input_sha256'])

    @unittest.skipUnless(sys.platform == 'win32', 'Win32 immutable input lease')
    def test_paired_input_write_attempt_cannot_damage_other_run_or_original(self):
        observed = []
        def attempt_write(runtime, source, output, profile, timeout, **kwargs):
            if output.parent.name == 'baseline':
                source.write_bytes(b'forbidden mutation')
            observed.append(source.read_bytes())
            raise upstream.smoke.NormalLoadRefusal(source, 1)
        report, catalog = self.formal_report(attempt_write, paired=True)
        self.assertFalse(report['summary']['acceptance_passed'])
        self.assertFalse(self.trial_passed(report, catalog))
        self.assertEqual(observed, [b'deliberately malformed fixture'])
        self.assertEqual(self.input.read_bytes(), b'deliberately malformed fixture')
        for item in report['results']:
            self.assertTrue(all(run['input_unchanged'] for run in item['runs'].values()))

    @unittest.skipUnless(sys.platform == 'win32', 'Win32 immutable input lease')
    def test_immutable_input_lease_blocks_writes_and_releases_after_exception(self):
        with self.assertRaisesRegex(RuntimeError, 'probe'):
            with upstream.immutable_input(self.input):
                with self.assertRaises(PermissionError):
                    self.input.write_bytes(b'forbidden mutation')
                self.assertEqual(self.input.read_bytes(), b'deliberately malformed fixture')
                raise RuntimeError('probe')
        self.input.write_bytes(b'lease released; retained fixture')

    def test_paired_rejects_missing_baseline_and_excess_workers_before_output(self):
        for baseline, workers in [(None, 1), (self.runtime, 5)]:
            with self.assertRaisesRegex(ValueError, 'Paired conversions require'):
                upstream.run_test(self.source, self.runtime, self.root / 'forbidden_result',
                                  baseline_soffice=baseline, workers=workers, paired=True)
        self.assertFalse((self.root / 'forbidden_result').exists())

    @unittest.skipUnless(sys.platform == 'win32', 'Owned Win64 clock control')
    def test_fixed_clock_formal_acceptance_and_trial_require_retained_receipts(self):
        report, catalog = self.formal_report(fixed=True)
        self.assertEqual(report['report_version'], 4)
        self.assertTrue(report['summary']['acceptance_passed'])
        self.assertEqual(report['summary']['errors'], 1)
        with mock.patch.object(clock, 'METADATA', self.clock_metadata()):
            self.assertTrue(self.trial_passed(report, catalog))
            for key in ('clock_control_unchanged',):
                forged = copy.deepcopy(report)
                del forged['audits'][key]
                self.assertFalse(self.trial_passed(forged, catalog))
            forged = copy.deepcopy(report)
            del forged['results'][0]['runs']['candidate']['clock_receipts']
            self.assertFalse(self.trial_passed(forged, catalog))
            forged = copy.deepcopy(report)
            forged['report_version'] = 3
            self.assertFalse(self.trial_passed(forged, catalog))
            receipt = Path(report['results'][0]['runs']['candidate']['clock_receipts'][0]['path'])
            receipt.write_text('{}', encoding='utf-8')
            self.assertFalse(self.trial_passed(report, catalog))

    @unittest.skipUnless(sys.platform == 'win32', 'Owned Win64 clock control')
    def test_missing_clock_application_cannot_pass_even_with_equal_pdf_or_normal_refusal(self):
        report, catalog = self.formal_report(fixed=True, receipt_change=lambda data: data.update(clock_applied=False))
        self.assertEqual(report['summary']['validation_failures'], 2)
        self.assertFalse(report['summary']['acceptance_passed'])
        with mock.patch.object(clock, 'METADATA', self.clock_metadata()):
            self.assertFalse(self.trial_passed(report, catalog))

    def clock_runner_fixture(self):
        out = self.root / 'candidate/output'
        out.mkdir(parents=True)
        control = {'unix_time': 1767323045, 'helper_sha256': 'a' * 64,
                   'system_dll': 'mock', 'system_dll_sha256': 'b' * 64, 'functions': [],
                   'runtime_pins': {'candidate': {'executable': str(self.runtime.with_suffix('.bin').resolve()),
                                                'executable_sha256': 'c' * 64, 'sal_sha256': 'd' * 64}}}
        path = self.root / 'clock_control.json'
        path.write_text(json.dumps(control), encoding='utf-8')
        envelope = {'path': str(path), 'sha256': clock.digest(path), 'control': control}
        return out, envelope, []

    def test_clock_crash_is_never_retried_as_profile_restart(self):
        out, envelope, observations = self.clock_runner_fixture()
        def crash(cmd, out, env, timeout):
            receipt = Path(cmd[cmd.index('--receipt') + 1])
            receipt.write_text(json.dumps({'returncode': 79}), encoding='utf-8')
            return subprocess.CompletedProcess(cmd, 79, 'crash')
        with mock.patch.object(clock, 'check_receipt'):
            runner = mock.Mock(side_effect=crash)
            result = clock.controlled_conversion([str(self.runtime)], out, {}, 20, envelope, observations, runner)
        self.assertEqual(result.returncode, 79)
        self.assertEqual(runner.call_count, 1)
        self.assertEqual(len(observations), 1)

    def test_clock_launcher_preserves_high_bit_windows_crash_exit_codes(self):
        self.assertEqual(clock.windows_exit_argument(0), 0)
        self.assertEqual(clock.windows_exit_argument(81), 81)
        self.assertEqual(clock.windows_exit_argument(0xe0424242), -532528574)
        self.assertEqual(clock.windows_exit_argument(0xc0000409), -1073740791)
        for value in (-1, 0x100000000, True):
            with self.assertRaises(ValueError):
                clock.windows_exit_argument(value)

    def test_clock_normal_restart_is_bounded_and_uses_one_timeout_budget(self):
        out, envelope, observations = self.clock_runner_fixture()
        budgets = []
        def restart(cmd, out, env, timeout):
            budgets.append(timeout)
            receipt = Path(cmd[cmd.index('--receipt') + 1])
            receipt.write_text(json.dumps({'returncode': 81}), encoding='utf-8')
            return subprocess.CompletedProcess(cmd, 81, '')
        with mock.patch.object(clock, 'check_receipt'), mock.patch.object(clock.time, 'monotonic', side_effect=[0, 2, 4, 6]):
            with self.assertRaisesRegex(ValueError, 'restart limit'):
                clock.controlled_conversion([str(self.runtime)], out, {}, 20, envelope, observations, restart)
        self.assertEqual(budgets, [18, 16, 14])
        self.assertEqual(len(observations), 3)

    def test_clock_missing_receipt_or_timeout_cannot_be_normal_load_refusal(self):
        out, envelope, observations = self.clock_runner_fixture()
        for effect in (subprocess.CompletedProcess([], 1, expectations.REFUSAL_DIAGNOSTIC), subprocess.TimeoutExpired([], 1)):
            runner = mock.Mock(side_effect=effect if isinstance(effect, Exception) else None,
                               return_value=effect if not isinstance(effect, Exception) else None)
            with self.assertRaises((OSError, subprocess.TimeoutExpired)):
                clock.controlled_conversion([str(self.runtime)], out, {}, 20, envelope, observations, runner)
            self.assertEqual(observations, [])

    @unittest.skipUnless(sys.platform == 'win32', 'Owned Win64 clock control')
    def test_clock_receipt_requires_all_pins_application_and_disk_audits(self):
        report, catalog = self.formal_report(fixed=True)
        envelope = report['clock_control']
        data = report['results'][0]['runs']['candidate']['clock_receipts'][0]['data']
        for key in ('system_dll_disk_unchanged', 'sal_disk_unchanged', 'executable_disk_unchanged',
                    'patched_before_initial_breakpoint', 'control_sha256', 'helper_sha256', 'functions', 'returncode'):
            forged = copy.deepcopy(data)
            del forged[key]
            with self.assertRaises(ValueError):
                clock.check_receipt(forged, envelope, 'candidate', data['returncode'])

    def test_fixed_clock_rejects_invalid_time_or_unowned_conversion_before_output(self):
        for fixed, baseline, metadata in [(0, self.runtime, self.metadata), (1767323045, None, self.metadata),
                                           (1767323045, self.runtime, None)]:
            with self.assertRaises(ValueError):
                upstream.run_test(self.source, self.runtime, self.root / 'forbidden_result', baseline_soffice=baseline,
                                  expectations_path=metadata, fixed_time=fixed)
        self.assertFalse((self.root / 'forbidden_result').exists())

    def test_trial_command_forwards_explicit_clock_control(self):
        from types import SimpleNamespace
        args = SimpleNamespace(upstream_source_dir='source', timeout=30, acceptance=True,
                               upstream_group=[], upstream_sample=[], upstream_fixed_time=1767323045)
        command = removal.upstream_command(args, ROOT, self.root, self.root, self.root / 'result')
        self.assertEqual(command[command.index('--fixed-time') + 1], '1767323045')
        self.assertIn('--acceptance', command)

    @unittest.skipUnless(sys.platform == 'win32', 'Owned Win64 clock control')
    def test_clock_source_evidence_change_prevents_final_acceptance_report(self):
        def mutate(runtime, source, output, profile, timeout, **kwargs):
            (self.source / 'tools/source/datetime/systemdatetime.cxx').write_text('changed clock implementation', encoding='utf-8')
            raise upstream.smoke.NormalLoadRefusal(source, 1)
        with self.assertRaises(ValueError):
            self.formal_report(mutate, fixed=True)
        self.assertFalse((self.root / 'result/report.json').exists())
        self.assertTrue((self.root / 'result/partial.json').exists())

    def cli_support(self, crc=False):
        required = expectations.CLI_SUPPORT | ({'package/source/zipapi/XUnbufferedStream.cxx'} if crc else set())
        result = []
        for relative in sorted(required):
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('mock reviewed no-interaction refusal source', encoding='utf-8')
            result.append({'path': relative, 'sha256': expectations.sha256(path)})
        return result

    def test_repair_required_evidence_includes_cli_chain_and_python_qa_declaration(self):
        self.entry['evidence'].update(kind='repair-required', support=self.cli_support())
        self.save()
        self.assertIn(self.relative, self.load())
        self.entry['evidence']['support'].pop()
        self.save()
        with self.assertRaises(ValueError):
            self.load()
        self.entry['evidence']['support'] = self.cli_support()
        python_test = self.source / 'repair_test.py'
        python_test.write_text('class Test:\n    def testReject(self):\n        pass\n', encoding='utf-8')
        self.entry['evidence'].update(path='repair_test.py', sha256=expectations.sha256(python_test))
        self.save()
        self.assertIn(self.relative, self.load())
        dependency = self.source / self.entry['evidence']['support'][0]['path']
        dependency.write_text('changed reviewed source', encoding='utf-8')
        with self.assertRaises(ValueError):
            self.load()

    def test_crc_refusal_requires_exact_invalid_member_and_pinned_official_verification(self):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, 'w', compression=zipfile.ZIP_STORED) as archive:
            archive.writestr('word/header1.xml', b'mock document header')
        valid = buffer.getvalue()
        invalid = bytearray(valid)
        central = invalid.index(b'PK\x01\x02')
        # Preserve member bytes but make both headers claim the same wrong CRC.
        crc = struct.unpack_from('<I', invalid, 14)[0] ^ 1
        struct.pack_into('<I', invalid, 14, crc)
        struct.pack_into('<I', invalid, central + 16, crc)
        self.input.write_bytes(invalid)
        self.entry['sha256'] = expectations.sha256(self.input)
        self.catalog['samples'][0]['sha256'] = self.entry['sha256']
        self.entry['evidence'].update(kind='zip-integrity-refusal', invalid_crc_member='word/header1.xml',
                                      support=self.cli_support(crc=True))
        self.save()
        self.assertIn(self.relative, self.load())
        self.entry['evidence']['invalid_crc_member'] = 'word/document.xml'
        self.save()
        with self.assertRaises(ValueError):
            self.load()
        self.input.write_bytes(valid)
        self.entry['sha256'] = expectations.sha256(self.input)
        self.catalog['samples'][0]['sha256'] = self.entry['sha256']
        self.entry['evidence']['invalid_crc_member'] = 'word/header1.xml'
        self.save()
        with self.assertRaises(ValueError):
            self.load()


if __name__ == '__main__':
    unittest.main()
