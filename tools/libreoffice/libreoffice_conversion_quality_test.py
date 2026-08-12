#!/usr/bin/env python3
"""Convert paired Office fixtures and compare them with Microsoft Office reference PDFs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from difflib import SequenceMatcher
from pathlib import Path
from typing import Sequence


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import libreoffice_smoke_test as smoke  # noqa: E402

try:
    import fitz  # type: ignore
    from PIL import Image, ImageChops, ImageDraw, ImageEnhance  # type: ignore
except ImportError as exc:  # pragma: no cover - exercised only on incomplete developer environments
    raise SystemExit(f"PDF visual comparison dependencies are missing: {exc}") from exc


REPORT_VERSION = 1
OFFICE_SUFFIXES = frozenset((".docx", ".pptx"))


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def discover_pairs(input_dir: Path) -> tuple[list[tuple[Path, Path]], list[Path]]:
    pairs: list[tuple[Path, Path]] = []
    missing_references: list[Path] = []
    for office in sorted(input_dir.rglob("*"), key=lambda path: path.as_posix().casefold()):
        if not office.is_file() or office.suffix.casefold() not in OFFICE_SUFFIXES:
            continue
        reference = office.with_suffix(".pdf")
        if reference.is_file():
            pairs.append((office, reference))
        else:
            missing_references.append(office)
    return pairs, missing_references


def normalize_text(value: str) -> str:
    return " ".join(value.split())


def pixmap_to_image(pixmap) -> Image.Image:
    return Image.frombytes("RGB", (pixmap.width, pixmap.height), pixmap.samples)


def difference_metrics(reference: Image.Image, candidate: Image.Image, threshold: int) -> dict[str, object]:
    same_dimensions = reference.size == candidate.size
    comparison_size = (max(reference.width, candidate.width), max(reference.height, candidate.height))
    reference_canvas = Image.new("RGB", comparison_size, "white")
    candidate_canvas = Image.new("RGB", comparison_size, "white")
    reference_canvas.paste(reference, (0, 0))
    candidate_canvas.paste(candidate, (0, 0))
    difference = ImageChops.difference(reference_canvas, candidate_canvas)
    bbox = difference.getbbox()
    if bbox is None:
        different_pixels = 0
    else:
        raw = difference.tobytes()
        different_pixels = sum(1 for offset in range(0, len(raw), 3) if max(raw[offset : offset + 3]) > threshold)
    total_pixels = comparison_size[0] * comparison_size[1]
    return {
        "same_dimensions": same_dimensions,
        "reference_pixels": list(reference.size),
        "candidate_pixels": list(candidate.size),
        "comparison_pixels": list(comparison_size),
        "different_pixels": different_pixels,
        "difference_ratio": different_pixels / total_pixels if total_pixels else 0.0,
        "difference_bbox": list(bbox) if bbox else None,
    }


def normalize_pdf_font_name(name: str) -> str:
    return re.sub(r"^[A-Z]{6}\+", "", name)


def count_rendered_images(page) -> int:
    """Count image occurrences painted on a page, not all shared resources."""
    return len(page.get_image_info(xrefs=True))


def collect_pdf_semantics(document) -> dict[str, object]:
    links: list[dict[str, object]] = []
    image_counts: list[int] = []
    annotation_counts: list[int] = []
    for page_index, page in enumerate(document):
        for link in page.get_links():
            links.append(
                {
                    "source_page": page_index + 1,
                    "kind": int(link.get("kind", 0)),
                    "target_page": int(link["page"]) + 1 if isinstance(link.get("page"), int) else None,
                    "uri": link.get("uri"),
                    "file": link.get("file"),
                }
            )
        image_counts.append(count_rendered_images(page))
        annotations = page.annots()
        annotation_counts.append(sum(1 for _ in annotations) if annotations is not None else 0)
    return {
        "links": links,
        "image_counts_per_page": image_counts,
        "annotation_counts_per_page": annotation_counts,
        "toc": [{"level": row[0], "title": row[1], "page": row[2]} for row in document.get_toc(simple=True)],
    }


def save_review_image(
    reference: Image.Image,
    candidate: Image.Image,
    output: Path,
    title: str,
) -> None:
    width = max(reference.width, candidate.width)
    height = max(reference.height, candidate.height)
    ref_canvas = Image.new("RGB", (width, height), "white")
    cand_canvas = Image.new("RGB", (width, height), "white")
    ref_canvas.paste(reference, (0, 0))
    cand_canvas.paste(candidate, (0, 0))
    difference = ImageChops.difference(ref_canvas, cand_canvas)
    difference = ImageEnhance.Contrast(difference).enhance(4.0)
    label_height = 28
    review = Image.new("RGB", (width * 3, height + label_height), "white")
    review.paste(ref_canvas, (0, label_height))
    review.paste(cand_canvas, (width, label_height))
    review.paste(difference, (width * 2, label_height))
    draw = ImageDraw.Draw(review)
    draw.text((8, 7), f"Reference - {title}", fill="black")
    draw.text((width + 8, 7), "LibreOffice candidate", fill="black")
    draw.text((width * 2 + 8, 7), "Amplified pixel difference", fill="black")
    output.parent.mkdir(parents=True, exist_ok=True)
    review.save(output, format="PNG", optimize=True)


def compare_pdfs(
    reference_pdf: Path,
    candidate_pdf: Path,
    review_dir: Path,
    *,
    dpi: int,
    pixel_threshold: int,
) -> dict[str, object]:
    reference = fitz.open(reference_pdf)
    candidate = fitz.open(candidate_pdf)
    reference_page_count = len(reference)
    candidate_page_count = len(candidate)
    reference_fonts = smoke.extract_pdf_font_names(reference_pdf)
    candidate_fonts = smoke.extract_pdf_font_names(candidate_pdf)
    normalized_reference_fonts = sorted({normalize_pdf_font_name(name) for name in reference_fonts}, key=str.casefold)
    normalized_candidate_fonts = sorted({normalize_pdf_font_name(name) for name in candidate_fonts}, key=str.casefold)
    reference_semantics = collect_pdf_semantics(reference)
    candidate_semantics = collect_pdf_semantics(candidate)
    semantic_issues: list[str] = []
    for key in ("links", "image_counts_per_page", "annotation_counts_per_page", "toc"):
        if reference_semantics[key] != candidate_semantics[key]:
            semantic_issues.append(key)
    scale = dpi / 72.0
    matrix = fitz.Matrix(scale, scale)
    page_results: list[dict[str, object]] = []
    structural_issues: list[str] = []
    if reference_page_count != candidate_page_count:
        structural_issues.append(f"page-count: reference={reference_page_count} candidate={candidate_page_count}")

    for index in range(max(reference_page_count, candidate_page_count)):
        if index >= reference_page_count or index >= candidate_page_count:
            page_results.append(
                {"page": index + 1, "missing_in": "reference" if index >= reference_page_count else "candidate"}
            )
            continue
        ref_page = reference[index]
        cand_page = candidate[index]
        ref_rect = ref_page.rect
        cand_rect = cand_page.rect
        page_size_equal = abs(ref_rect.width - cand_rect.width) < 0.1 and abs(ref_rect.height - cand_rect.height) < 0.1
        if not page_size_equal:
            structural_issues.append(
                f"page-{index + 1}-size: reference={ref_rect.width:.2f}x{ref_rect.height:.2f} "
                f"candidate={cand_rect.width:.2f}x{cand_rect.height:.2f}"
            )
        ref_image = pixmap_to_image(ref_page.get_pixmap(matrix=matrix, alpha=False))
        cand_image = pixmap_to_image(cand_page.get_pixmap(matrix=matrix, alpha=False))
        metrics = difference_metrics(ref_image, cand_image, pixel_threshold)
        ref_text = normalize_text(ref_page.get_text("text"))
        cand_text = normalize_text(cand_page.get_text("text"))
        text_ratio = SequenceMatcher(None, ref_text, cand_text).ratio() if ref_text or cand_text else 1.0
        review_path = review_dir / f"page_{index + 1:03d}.png"
        save_review_image(ref_image, cand_image, review_path, f"page {index + 1}")
        page_results.append(
            {
                "page": index + 1,
                "reference_page_points": [ref_rect.width, ref_rect.height],
                "candidate_page_points": [cand_rect.width, cand_rect.height],
                "page_size_equal": page_size_equal,
                "reference_text_characters": len(ref_text),
                "candidate_text_characters": len(cand_text),
                "text_similarity": text_ratio,
                "review_image": str(review_path),
                **metrics,
            }
        )
    reference.close()
    candidate.close()
    ratios = [float(item["difference_ratio"]) for item in page_results if item.get("difference_ratio") is not None]
    text_ratios = [float(item["text_similarity"]) for item in page_results if item.get("text_similarity") is not None]
    return {
        "reference_pdf": str(reference_pdf),
        "candidate_pdf": str(candidate_pdf),
        "reference_sha256": sha256_file(reference_pdf),
        "candidate_sha256": sha256_file(candidate_pdf),
        "reference_pages": reference_page_count,
        "candidate_pages": candidate_page_count,
        "reference_pdf_fonts": reference_fonts,
        "candidate_pdf_fonts": candidate_fonts,
        "reference_only_pdf_fonts": sorted(
            set(normalized_reference_fonts) - set(normalized_candidate_fonts), key=str.casefold
        ),
        "candidate_only_pdf_fonts": sorted(
            set(normalized_candidate_fonts) - set(normalized_reference_fonts), key=str.casefold
        ),
        "reference_semantics": reference_semantics,
        "candidate_semantics": candidate_semantics,
        "semantic_issues": semantic_issues,
        "structural_issues": structural_issues,
        "mean_difference_ratio": sum(ratios) / len(ratios) if ratios else None,
        "max_difference_ratio": max(ratios) if ratios else None,
        "mean_text_similarity": sum(text_ratios) / len(text_ratios) if text_ratios else None,
        "pages": page_results,
    }


def ensure_output_outside_inputs(output_dir: Path, input_dir: Path) -> None:
    output_resolved = output_dir.resolve()
    input_resolved = input_dir.resolve()
    if output_resolved == input_resolved or input_resolved in output_resolved.parents:
        raise ValueError(f"output directory must not be inside the fixture input directory: {output_resolved}")


def write_json_atomic(path: Path, payload: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + f".{os.getpid()}.tmp")
    try:
        temp.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        os.replace(temp, path)
    finally:
        if temp.exists():
            temp.unlink()


def run_quality_test(
    input_dir: Path,
    soffice: Path,
    output_dir: Path,
    *,
    timeout: int,
    dpi: int,
    pixel_threshold: int,
    isolate_home: bool = True,
) -> dict[str, object]:
    input_dir = input_dir.resolve()
    soffice = soffice.resolve()
    output_dir = output_dir.resolve()
    if not input_dir.is_dir():
        raise FileNotFoundError(f"fixture input directory not found: {input_dir}")
    if not soffice.is_file():
        raise FileNotFoundError(f"soffice.com not found: {soffice}")
    ensure_output_outside_inputs(output_dir, input_dir)
    pairs, missing_references = discover_pairs(input_dir)
    if not pairs:
        raise RuntimeError(f"no paired .docx/.pptx and .pdf fixtures found: {input_dir}")
    if output_dir.exists():
        raise FileExistsError(f"quality output directory already exists: {output_dir}")
    output_dir.mkdir(parents=True)
    work_dir = output_dir / "work"
    work_dir.mkdir()
    results: list[dict[str, object]] = []
    conversion_errors: list[dict[str, str]] = []
    try:
        for index, (office, reference_pdf) in enumerate(pairs):
            relative = office.relative_to(input_dir)
            case_dir = output_dir / "cases" / f"case_{index:03d}"
            staged_dir = work_dir / f"{index:03d}" / "input"
            profile_dir = work_dir / f"{index:03d}" / "profile"
            candidate_dir = case_dir / "candidate"
            for path in (staged_dir, profile_dir, candidate_dir):
                path.mkdir(parents=True, exist_ok=True)
            staged = staged_dir / f"source{office.suffix.casefold()}"
            source_hash_before = sha256_file(office)
            shutil.copy2(office, staged)
            try:
                candidate_pdf = smoke.convert_one(
                    soffice,
                    staged,
                    candidate_dir,
                    profile_dir,
                    timeout,
                    isolate_home=isolate_home,
                    configure_profile_paths=isolate_home,
                )
                source_hash_after = sha256_file(office)
                if source_hash_before != source_hash_after:
                    raise RuntimeError(f"source Office file changed during conversion: {office}")
                comparison = compare_pdfs(
                    reference_pdf,
                    candidate_pdf,
                    case_dir / "review",
                    dpi=dpi,
                    pixel_threshold=pixel_threshold,
                )
                results.append(
                    {
                        "office_file": str(office),
                        "relative_office_file": relative.as_posix(),
                        "office_sha256": source_hash_before,
                        "office_bytes": office.stat().st_size,
                        "source_ooxml_fonts": smoke.extract_ooxml_font_names(office),
                        "reference_bytes": reference_pdf.stat().st_size,
                        "candidate_bytes": candidate_pdf.stat().st_size,
                        **comparison,
                    }
                )
            except Exception as exc:
                conversion_errors.append({"office_file": str(office), "error": str(exc)})
    finally:
        shutil.rmtree(work_dir, ignore_errors=True)
        smoke.cleanup_image_pycache(soffice)

    structural_issue_count = sum(len(item["structural_issues"]) for item in results)
    semantic_issue_count = sum(len(item["semantic_issues"]) for item in results)
    all_page_results = [page for item in results for page in item["pages"]]
    low_text_similarity_pages = [
        {"office_file": item["relative_office_file"], "page": page["page"], "text_similarity": page["text_similarity"]}
        for item in results
        for page in item["pages"]
        if page.get("text_similarity") is not None and float(page["text_similarity"]) < 0.95
    ]
    report = {
        "tool": "libreoffice_conversion_quality_test",
        "report_version": REPORT_VERSION,
        "input_dir": str(input_dir),
        "soffice": str(soffice),
        "output_dir": str(output_dir),
        "dpi": dpi,
        "pixel_threshold": pixel_threshold,
        "isolate_home": isolate_home,
        "summary": {
            "paired_documents": len(pairs),
            "converted_documents": len(results),
            "conversion_errors": len(conversion_errors),
            "missing_reference_pdfs": len(missing_references),
            "structural_issues": structural_issue_count,
            "semantic_issues": semantic_issue_count,
            "compared_pages": len(all_page_results),
            "low_text_similarity_pages": len(low_text_similarity_pages),
        },
        "missing_reference_pdfs": [str(path) for path in missing_references],
        "conversion_errors": conversion_errors,
        "low_text_similarity_pages": low_text_similarity_pages,
        "results": results,
    }
    write_json_atomic(output_dir / "quality_report.json", report)
    return report


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Convert paired DOCX/PPTX fixtures and compare against same-stem Microsoft Office PDFs."
    )
    parser.add_argument("--input-dir", required=True)
    parser.add_argument(
        "--soffice",
        default="third_party/libreoffice/custom_runtime/instdir/program/soffice.com",
    )
    parser.add_argument(
        "--preserve-host-home",
        action="store_true",
        help="Keep host HOME/APPDATA environment for stock LibreOffice compatibility; the LO user profile remains isolated.",
    )
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--dpi", type=int, default=144)
    parser.add_argument(
        "--pixel-threshold",
        type=int,
        default=8,
        help="Ignore per-channel raster differences at or below this value when computing pixel ratios.",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        report = run_quality_test(
            Path(args.input_dir),
            Path(args.soffice),
            Path(args.output_dir),
            timeout=args.timeout,
            dpi=args.dpi,
            pixel_threshold=args.pixel_threshold,
            isolate_home=not args.preserve_host_home,
        )
    except Exception as exc:
        print(f"[NG] Office conversion quality test failed: {exc}", file=sys.stderr)
        return 2
    summary = report["summary"]
    print(
        f"documents={summary['converted_documents']}/{summary['paired_documents']} "
        f"pages={summary['compared_pages']} structural_issues={summary['structural_issues']} "
        f"semantic_issues={summary['semantic_issues']} "
        f"low_text_similarity_pages={summary['low_text_similarity_pages']} "
        f"conversion_errors={summary['conversion_errors']}"
    )
    print(f"report={Path(args.output_dir).resolve() / 'quality_report.json'}")
    return 1 if summary["conversion_errors"] or summary["structural_issues"] or summary["semantic_issues"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
