"""Copy/check local read-only viewer notices. Does not create or approve a release."""
from __future__ import annotations
import argparse
import hashlib
import json
import uuid
from pathlib import Path

PDFIUM_NOTICES = (
    'abseil.txt', 'agg23.txt', 'dragonbox-Apache2-LLVM.txt', 'dragonbox-Boost.txt',
    'fast_float.txt', 'freetype.txt', 'harfbuzz.txt', 'highway.txt', 'icu.txt',
    'lcms.txt', 'libjpeg_turbo.ijg', 'libjpeg_turbo.md', 'libopenjpeg.txt',
    'libpng.txt', 'llvm-libc.txt', 'pdfium.txt', 'simdutf.txt', 'zlib.txt',
)


def source_files(root: Path, locale: str, variant: str = 'prototype') -> dict[str, Path]:
    guide = 'readonly_viewer_next' if variant == 'prototype' else 'readonly_viewer'
    files = {
        'LICENSE.md': root / 'LICENSE.md',
        'licenses/README.txt': root / f'src/{guide}/licenses_{locale}.txt',
        'licenses/project/LICENSE.md': root / f'docs/{locale}/legal/LICENSE.md',
        'licenses/pdfium/LICENSE': root / 'third_party/pdfium/LICENSE',
        'licenses/md4c/LICENSE.md': root / 'third_party/md4c/LICENSE.md',
    }
    for name in PDFIUM_NOTICES:
        files[f'licenses/pdfium/{name}'] = root / f'third_party/pdfium/licenses/{name}'
    for name in ('gcc/COPYING3.txt', 'gcc/COPYING.RUNTIME.txt', 'winpthreads/COPYING.txt'):
        files[f'licenses/mingw-w64/{name}'] = root / f'third_party/mingw_runtime_licenses/mingw-w64/{name}'
    actual = {p.name for p in (root / 'third_party/pdfium/licenses').iterdir() if p.is_file()}
    if actual != set(PDFIUM_NOTICES):
        raise ValueError('PDFium notice inventory changed or is incomplete; review required')
    return files


def prepare(root: Path, output: Path, locale: str, check: bool, variant: str = 'prototype') -> int:
    if variant not in ('prototype', 'standard'): raise ValueError('Unknown viewer variant')
    root = root.resolve(); output = output.resolve()
    if not output.is_relative_to(root / 'out'):
        raise ValueError('Prototype notice output must stay under this repository\'s out directory')
    sources = source_files(root, locale, variant)
    # Read every mandatory original before touching output. Missing/empty inputs
    # fail the build; originals and an existing bundle remain intact.
    content = {name: source.read_bytes() for name, source in sources.items()}
    for name, data in content.items():
        if not data: raise ValueError(f'Empty required license input: {sources[name]}')
        if not (output / name).resolve().is_relative_to(output):
            raise ValueError(f'Notice destination escapes prototype output: {name}')
    manifest = {'locale': locale, 'scope': f'local-readonly-viewer-{variant}', 'files': {
        name: {'source': sources[name].relative_to(root).as_posix(), 'sha256': hashlib.sha256(data).hexdigest()}
        for name, data in content.items()}}
    manifest_bytes = (json.dumps(manifest, ensure_ascii=False, indent=2) + '\n').encode('utf-8')
    if not check:
        for name, data in {**content, 'licenses/manifest.json': manifest_bytes}.items():
            target = output / name
            target.parent.mkdir(parents=True, exist_ok=True)
            temporary = target.with_name(target.name + '.' + uuid.uuid4().hex + '.license-copy-tmp')
            with temporary.open('xb') as stream:
                stream.write(data)
            temporary.replace(target)
    for name, data in {**content, 'licenses/manifest.json': manifest_bytes}.items():
        target = output / name
        if not target.is_file() or target.read_bytes() != data:
            raise ValueError(f'License missing or hash mismatch: {name}')
    expected = {name for name in content if name.startswith('licenses/')} | {'licenses/manifest.json'}
    actual = {p.relative_to(output).as_posix() for p in (output / 'licenses').rglob('*') if p.is_file()}
    if actual != expected: raise ValueError('Unexpected/stale files in prototype license bundle')
    return len(content)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--locale', choices=('ja', 'en'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--variant', choices=('prototype', 'standard'), default='prototype')
    args = parser.parse_args()
    try:
        count = prepare(Path(__file__).resolve().parents[2], args.output, args.locale, args.check, args.variant)
    except (OSError, ValueError) as error:
        parser.exit(1, f'Read-only viewer license check failed: {error}\n')
    print(f'PASS: viewer license files={count}, locale={args.locale}, variant={args.variant}')


if __name__ == '__main__':
    main()
