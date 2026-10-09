"""Pinned upstream refusal expectations; never infer expectations from failures.

The caller owns conversion evidence and audits runtime/source immutability.
This module reads metadata and upstream evidence only. Missing, unsafe or changed
evidence raises; crashes, timeouts and incomplete observations cannot pass.
"""
from __future__ import annotations

import hashlib
import json
import re
import zipfile
from pathlib import Path, PurePosixPath

DEFAULT_METADATA = Path(__file__).resolve().parents[2] / 'tests/config/libreoffice_conversion_expectations.json'
EXPECTATION = 'load-refusal-without-repair-or-password'
REFUSAL_DIAGNOSTIC = 'Error: source file could not be loaded'
CLI_SUPPORT = frozenset({'desktop/source/app/dispatchwatcher.cxx', 'filter/source/config/cache/typedetection.cxx',
                         'package/source/zipapi/ZipFile.cxx', 'oox/source/helper/zipstorage.cxx'})


def sha256(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def relative_path(value: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError('Empty expectation path')
    path = PurePosixPath(value)
    if path.is_absolute() or '..' in path.parts or ':' in value or '\\' in value or path.as_posix() != value:
        raise ValueError('Unsafe expectation path')
    return value


def pinned_file(source: Path, relative: str, digest: str) -> Path:
    relative_path(relative)
    if not isinstance(digest, str) or not re.fullmatch('[0-9a-f]{64}', digest):
        raise ValueError('Invalid expectation hash')
    path = (source / relative).resolve(strict=True)
    if not path.is_relative_to(source) or not path.is_file() or sha256(path) != digest:
        raise ValueError(f'Expectation evidence missing or changed: {relative}')
    return path


def load_expectations(source: Path, catalog: dict, metadata: Path = DEFAULT_METADATA) -> dict[str, dict]:
    source = source.resolve(strict=True)
    data = json.loads(metadata.read_text(encoding='utf-8'))
    if data.get('version') != 1 or data.get('source_version') != catalog.get('source_version'):
        raise ValueError('Expectation version does not match catalog')
    entries = data['samples']
    if not entries or data.get('sample_count') != len(entries):
        raise ValueError('Empty or incomplete expectation metadata')
    known = {item['path']: item['sha256'] for item in catalog['samples']}
    result = {}
    checked_evidence = {}
    for entry in entries:
        relative = relative_path(entry['path'])
        if relative in result or known.get(relative) != entry['sha256']:
            raise ValueError('Duplicate, unknown or changed expectation input')
        if entry.get('expectation') != EXPECTATION:
            raise ValueError('Unsupported conversion expectation')
        pinned_file(source, relative, entry['sha256'])
        evidence = entry['evidence']
        key = (relative_path(evidence['path']), evidence['sha256'])
        if key not in checked_evidence:
            checked_evidence[key] = pinned_file(source, *key).read_text(encoding='utf-8')
        test = evidence['test']
        if not isinstance(test, str) or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', test):
            raise ValueError('Invalid upstream test name')
        kind = evidence.get('kind', 'fixture-refusal')
        if kind in ('fixture-refusal', 'repair-required', 'zip-integrity-refusal'):
            declaration = r'CPPUNIT_TEST_FIXTURE\([^,]+,\s*' + re.escape(test) + r'\s*\)'
            if kind == 'repair-required' and evidence['path'].endswith('.py'):
                declaration = r'^\s*def\s+' + re.escape(test) + r'\(self\):'
        elif kind == 'directory-refusal':
            scope = relative_path(evidence['scope'])
            if not scope.endswith('/fail') or not PurePosixPath(relative).is_relative_to(PurePosixPath(scope)):
                raise ValueError('Refusal input outside reviewed failure-test scope')
            declaration = r'CPPUNIT_TEST\(\s*' + re.escape(test) + r'\s*\)'
            support = evidence.get('support', [])
            if not support:
                raise ValueError('Directory refusal requires pinned test-runner evidence')
            for dependency in support:
                pinned_file(source, dependency['path'], dependency['sha256'])
        else:
            raise ValueError('Unsupported upstream refusal evidence')
        if kind in ('repair-required', 'zip-integrity-refusal'):
            required = CLI_SUPPORT | ({'package/source/zipapi/XUnbufferedStream.cxx'} if kind == 'zip-integrity-refusal' else set())
            support = evidence.get('support', [])
            if len(support) != len(required) or {item['path'] for item in support} != required:
                raise ValueError('CLI refusal requires all reviewed source evidence')
            for dependency in support:
                pinned_file(source, dependency['path'], dependency['sha256'])
            if kind == 'zip-integrity-refusal':
                member = relative_path(evidence['invalid_crc_member'])
                with zipfile.ZipFile(source / relative) as archive:
                    if archive.testzip() != member:
                        raise ValueError('Reviewed ZIP CRC mismatch not present')
        if not re.search(declaration, checked_evidence[key], re.MULTILINE):
            raise ValueError('Expected upstream test declaration missing')
        result[relative] = entry
    return result


def judge(record: dict, expectation: dict | None) -> dict:
    """Keep conversion FAIL/error metrics intact; return a separate test verdict."""
    if expectation is None:
        runs = record.get('runs', {})
        comparison = record.get('comparison') or {}
        pages = comparison.get('pages', [])
        passed = (record.get('outcome') == 'both_success_equal' and record.get('error') is None
                  and record.get('changed') is False and record.get('original_unchanged') is True
                  and set(runs) == {'baseline', 'candidate'}
                  and all(run.get('status') == 'SUCCESS' and type(run.get('pages')) is int
                          and run['pages'] > 0 and run.get('input_unchanged') is True for run in runs.values())
                  and bool(pages)
                  and comparison.get('reference_pages') == comparison.get('candidate_pages') == len(pages)
                  and all(run['pages'] == len(pages) for run in runs.values())
                  and all(comparison.get(key) == [] for key in ('structural_issues', 'semantic_issues',
                                                                'reference_only_pdf_fonts', 'candidate_only_pdf_fonts'))
                  and all(page.get('page_size_equal') is True and page.get('same_dimensions') is True
                          and page.get('text_similarity') == 1.0 and type(page.get('different_pixels')) is int
                          and page['different_pixels'] == 0 for page in pages))
        return {'expectation': 'pdf-conversion-and-equality', 'passed': passed,
                'kind': 'PDF_EQUAL' if passed else 'UNEXPECTED_RESULT'}
    if record.get('path') != expectation['path'] or record.get('sha256') != expectation['sha256']:
        raise ValueError('Refusal expectation does not match result')
    runs = record.get('runs', {})
    passed = (record.get('outcome') == 'both_failed'
              and record.get('original_unchanged') is True
              and set(runs) == {'baseline', 'candidate'})
    for run in runs.values():
        passed = passed and (run.get('status') == 'FAIL'
                             and run.get('failure_kind') == 'normal_load_refusal'
                             and type(run.get('returncode')) is int and run['returncode'] in (0, 1)
                             and run.get('diagnostic') == REFUSAL_DIAGNOSTIC
                             and run.get('output_pdf_exists') is False
                             and run.get('input_unchanged') is True)
    return {'expectation': EXPECTATION, 'passed': bool(passed),
            'kind': 'EXPECTED_REJECTION' if passed else 'UNEXPECTED_RESULT',
            'evidence': expectation['evidence']}
