#!/usr/bin/env python3
"""Convert compact PowerPoint fixtures into the locale-specific Session 04 PDFs."""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path
from tempfile import NamedTemporaryFile, TemporaryDirectory


REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_DIR = REPO_ROOT / "tests/fixtures/office_conversion"
RUNTIME_SOFFICE = REPO_ROOT / "third_party/libreoffice/custom_runtime/instdir/program/soffice.com"
JAPANESE_SESSION_DIR = REPO_ROOT / "release_assets/sample_workspace/ja/01_講義サンプル/第04回_Office変換"
ENGLISH_SESSION_DIR = REPO_ROOT / "release_assets/sample_workspace/en/01_Lecture_Samples/Session_04_Office_Conversion"
SAMPLES = (
    ("PPTX_機能紹介とネイティブ図表.pptx", JAPANESE_SESSION_DIR / "PPTX_機能紹介とネイティブ図表_変換結果.pdf"),
    ("PPTX_Feature_Overview_and_Native_Charts.pptx", ENGLISH_SESSION_DIR / "feature_overview_and_charts_conversion_result.pdf"),
)


def atomic_publish(source: Path, destination: Path) -> None:
    """Replace a distribution sample only after its candidate PDF is verified."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    with NamedTemporaryFile(dir=destination.parent, suffix=".pdf", delete=False) as handle:
        temporary = Path(handle.name)
        try:
            handle.write(source.read_bytes())
            handle.flush()
            os.fsync(handle.fileno())
        except Exception:
            temporary.unlink(missing_ok=True)
            raise
    try:
        os.replace(temporary, destination)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise


def main() -> None:
    if not RUNTIME_SOFFICE.is_file():
        raise SystemExit(f"LibreOffice soffice.com not found: {RUNTIME_SOFFICE}")

    sys.path.insert(0, str(REPO_ROOT / "tools/libreoffice"))
    import libreoffice_smoke_test as smoke  # pylint: disable=import-outside-toplevel

    temp_parent = REPO_ROOT / ".local/repo_resource/tmp"
    temp_parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(dir=temp_parent, prefix="session04_office_samples_") as temp_name:
        temp_root = Path(temp_name)
        for index, (source_name, destination) in enumerate(SAMPLES):
            source = FIXTURE_DIR / source_name
            if not source.is_file():
                raise FileNotFoundError(f"Office fixture is missing: {source}")
            staged = temp_root / "input" / source_name
            staged.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, staged)
            output_dir = temp_root / "output" / f"{index:02d}"
            profile_dir = temp_root / "profile" / f"{index:02d}"
            output_dir.mkdir(parents=True, exist_ok=True)
            converted = smoke.convert_one(RUNTIME_SOFFICE, staged, output_dir, profile_dir, timeout=180)
            smoke.validate_pdf(converted)
            atomic_publish(converted, destination)
            print(f"published: {source_name} -> {destination.name} ({converted.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
