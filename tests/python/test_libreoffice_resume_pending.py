"""Boundary tests retain their fixtures; no automatic deletion of local evidence."""
import io
import sys
from contextlib import redirect_stdout
import unittest
from pathlib import Path
from unittest.mock import patch
from uuid import uuid4

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/libreoffice'))
import libreoffice_resume_pending_test as resume


class ResumeBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.root = ROOT / '.local/repo_resource/tmp' / ('resume_boundary_' + uuid4().hex)
        self.root.mkdir(parents=True)

    def test_free_space_stops_before_new_batch(self):
        with patch.object(resume.shutil, 'disk_usage') as usage:
            usage.return_value.free = 13 * resume.GIB
            with self.assertRaisesRegex(ValueError, 'Insufficient space'):
                resume.free_space(self.root, 14 * resume.GIB)

    def test_output_cannot_overlap_prior_evidence(self):
        for output in (self.root, self.root / 'new', self.root.parent):
            with self.assertRaisesRegex(ValueError, 'overlaps'):
                resume.separate(output, [self.root])

    def test_existing_json_is_not_overwritten(self):
        path = self.root / 'evidence.json'
        resume.write_new(path, {'old': True})
        with self.assertRaises(FileExistsError):
            resume.write_new(path, {'old': False})
        self.assertEqual(resume.read_json(path), {'old': True})

    def test_changed_artifact_fails(self):
        path = self.root / 'source.docx'
        path.write_bytes(b'original')
        old_hash = resume.corpus.sha256(path)
        path.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'hash changed'):
            resume.retained_file(path, self.root, old_hash, {})

    def test_artifact_outside_previous_directory_fails(self):
        sub = self.root / 'previous'
        sub.mkdir()
        outside = self.root / 'outside.pdf'
        outside.write_bytes(b'pdf')
        with self.assertRaisesRegex(ValueError, 'escaped'):
            resume.retained_file(outside, sub, resume.corpus.sha256(outside), {})

    def test_changed_code_pin_fails(self):
        path = self.root / 'helper.py'
        path.write_bytes(b'old')
        pins = {str(path): resume.corpus.sha256(path)}
        path.write_bytes(b'new')
        with self.assertRaisesRegex(ValueError, 'Pinned file changed'):
            resume.unchanged(pins)

    def test_holes_and_partial_json_are_pending_but_completed_failure_is_preserved(self):
        previous = self.root / 'previous'
        previous.mkdir()
        items = [{'path': f'qa/{name}.docx', 'sha256': 'a' * 64, 'groups': ['all']}
                 for name in ('a', 'b', 'c')]
        catalog = {'source_version': 'test', 'samples': items}
        identities = {role: {'soffice': str(self.root / (role + '.com')), 'sha256': {}}
                      for role in ('baseline', 'candidate')}
        for role in identities:
            Path(identities[role]['soffice']).write_bytes(b'fixture')
        control = {'unix_time': 1767323045}
        resume.write_new(previous / 'clock_control.json', control)
        envelope = {'path': str(previous / 'clock_control.json'),
                    'sha256': resume.corpus.sha256(previous / 'clock_control.json'), 'control': control}
        identity = {'report_version': 4, 'mode': 'comparison', 'source_dir': str(self.root),
                    'source_version': 'test', 'runtimes': identities,
                    'controls': {'input_path': 'same absolute path; fresh copy per runtime',
                                 'random': 'SAL_RAND_REPEATABLE=1 (seed 42)',
                                 'profile_configuration_root': 'oor:items',
                                 'conversion_scheduling': 'sequential independent copies',
                                 'expected_results': 'pinned upstream refusals; conversion FAIL scores retained'},
                    'dpi': 144, 'pixel_threshold': 8, 'clock_control': envelope,
                    'catalog_sha256': resume.corpus.sha256(resume.corpus.CATALOG),
                    'fixture_encodings_sha256': resume.corpus.sha256(resume.corpus.ENCODINGS),
                    'expectations_sha256': resume.corpus.sha256(resume.expectations.DEFAULT_METADATA)}
        resume.write_new(previous / 'identity.json', identity)
        # A result at index 1 must not exclude the missing earlier index 0.
        case = previous / 'case_01'
        case.mkdir()
        record = {**items[1], 'index': 1, 'runs': {'baseline': {'status': 'FAIL'},
                  'candidate': {'status': 'FAIL'}}, 'outcome': 'both_failed'}
        resume.write_new(case / 'result.json', record)
        partial = previous / 'case_02'
        partial.mkdir()
        (partial / 'result.json').write_bytes(b'{')
        with patch.object(resume.upstream, 'collect_samples', return_value=(catalog, [])), \
             patch.object(resume.expectations, 'load_expectations', return_value={}), \
             patch.object(resume.corpus, 'fixture_encodings', return_value={}), \
             patch.object(resume.upstream, 'runtime_identity', side_effect=lambda path, full: next(
                 value for value in identities.values() if value['soffice'] == str(path))), \
             patch.object(resume.clock, 'audit'):
            plan = resume.plan_previous([previous])
        self.assertEqual(plan['pending'], ['qa/a.docx', 'qa/c.docx'])
        self.assertEqual(plan['completed'], {'qa/b.docx'})
        self.assertEqual(len(plan['failures']), 1)
        self.assertEqual(resume.read_json(case / 'result.json'), record)

    def exercise_main(self, limit=None, batch_size=1):
        previous = self.root / 'previous'
        previous.mkdir()
        source = self.root / 'source'
        source.mkdir()
        runtimes = {role: self.root / role / 'program/soffice.com' for role in ('baseline', 'candidate')}
        plan = {'source': source, 'runtimes': runtimes, 'identities': {'baseline': {}, 'candidate': {}},
                'catalog': {'samples': [1, 2, 3]}, 'pins': {}, 'completed': {'old.docx'},
                'kinds': {'PDF_EQUAL': 1}, 'failures': [], 'incomplete': [],
                'pending': ['new1.docx', 'new2.docx'], 'fixed_time': 1767323045}
        calls = []

        def convert(source, soffice, output, **options):
            calls.append(options)
            output.mkdir()
            result = {'results': [{'path': name} for name in options['individual']],
                      'summary': {'validation_failures': 0, 'acceptance_passed': False}}
            resume.write_new(output / 'report.json', result)
            return result

        with patch.object(resume, 'plan_previous', return_value=plan), \
             patch.object(resume, 'free_space', return_value=20 * resume.GIB), \
             patch.object(resume.upstream, 'run_test', side_effect=convert), \
             patch.object(resume.upstream, 'collect_samples'), \
             patch.object(resume.upstream, 'runtime_identity', return_value={}), \
             redirect_stdout(io.StringIO()):
            arguments = ['--previous-dir', str(previous), '--output-parent', str(self.root),
                         '--batch-size', str(batch_size)]
            if limit is not None:
                arguments += ['--limit', str(limit)]
            code = resume.main(arguments)
        self.assertEqual(code, 0)
        self.assertTrue(all(call['acceptance'] is False and call['workers'] == 1 for call in calls))
        summaries = list(self.root.glob('pending_*/resume_summary.json'))
        self.assertEqual(len(summaries), 1)
        summary = resume.read_json(summaries[0])
        self.assertIs(summary['acceptance_passed'], False)
        plan_record = resume.read_json(summaries[0].parent / 'resume_plan.json')
        return calls, summary, plan_record

    def test_main_converts_only_pending_and_does_not_claim_full_acceptance(self):
        calls, summary, _ = self.exercise_main()
        self.assertEqual([call['individual'] for call in calls], [['new1.docx'], ['new2.docx']])
        self.assertEqual(summary['remaining_pending'], 0)

    def test_limit_smaller_than_batch_does_not_overrun(self):
        calls, summary, plan = self.exercise_main(limit=1, batch_size=25)
        self.assertEqual([call['individual'] for call in calls], [['new1.docx']])
        self.assertEqual(summary['selected_for_this_run'], 1)
        self.assertEqual(summary['pending_attempted'], 1)
        self.assertEqual(summary['remaining_pending'], 1)
        self.assertEqual(summary['deferred_pending'], 1)
        self.assertEqual(plan['pending_inputs'], ['new1.docx'])

    def test_limit_greater_than_pending_runs_only_available_inputs(self):
        calls, summary, _ = self.exercise_main(limit=100)
        self.assertEqual([call['individual'] for call in calls], [['new1.docx'], ['new2.docx']])
        self.assertEqual(summary['selected_for_this_run'], 2)
        self.assertEqual(summary['deferred_pending'], 0)

    def test_zero_or_negative_limit_fails_before_audit_or_writes(self):
        for limit in (0, -1):
            with patch.object(resume, 'plan_previous') as audit, redirect_stdout(io.StringIO()), \
                 patch('sys.stderr', new=io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    resume.main(['--previous-dir', str(self.root), '--limit', str(limit)])
                self.assertEqual(error.exception.code, 2)
                audit.assert_not_called()


if __name__ == '__main__':
    unittest.main()
