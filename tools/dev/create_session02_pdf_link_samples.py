#!/usr/bin/env python3
"""Create the portable PDF-to-note link samples used by Session 02."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
from tempfile import NamedTemporaryFile

from reportlab.lib.colors import HexColor
from reportlab.lib.pagesizes import A4
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.cidfonts import UnicodeCIDFont
from reportlab.pdfgen.canvas import Canvas


REPO_ROOT = Path(__file__).resolve().parents[2]
LINK_ID = "session-02-pdf-note-link"
MARKER_ID = "session-02-pdf-link-marker"
MARKER_X = A4[0] / 2
MARKER_Y = 430.0
TIMESTAMP = "2026-09-15T00:00:00Z"


SAMPLES = (
    {
        "directory": REPO_ROOT / "release_assets/sample_workspace/ja/01_講義サンプル/第02回_ノート形式",
        "pdf": "PDFリンク練習.pdf",
        "title": "第02回  PDF とノートの往復リンク",
        "lead": "標準ノートの「PDFリンク練習のマーカーを開く」をクリックすると、この位置に移動します。",
        "button_lines": ("中央に表示される青い注釈マーカーをクリックすると、ノートのリンク箇所へ戻ります",),
        "guide_label": "青い注釈マーカーは、この点線の中央に表示されます",
        "footer": "PDF と同名の .clrop は、リンクマーカーを保存するアプリ管理データです。",
        "font": "HeiseiKakuGo-W5",
    },
    {
        "directory": REPO_ROOT / "release_assets/sample_workspace/en/01_Lecture_Samples/Session_02_Note_Formats",
        "pdf": "pdf_link_practice.pdf",
        "title": "Session 02  PDF and Note Round-Trip Link",
        "lead": "Click “Open the PDF link-practice marker” in the standard note to move here.",
        "button_lines": (
            "Click the blue annotation marker that appears in the center",
            "to return to the note link.",
        ),
        "guide_label": "The blue annotation marker appears at the center of this guide.",
        "footer": "The same-named .clrop stores the application-managed link marker.",
        "font": "Helvetica",
    },
)


def atomic_write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with NamedTemporaryFile(dir=path.parent, delete=False) as handle:
        temporary = Path(handle.name)
        try:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        except Exception:
            temporary.unlink(missing_ok=True)
            raise
    try:
        os.replace(temporary, path)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise


def draw_pdf(path: Path, sample: dict[str, Path | str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with NamedTemporaryFile(dir=path.parent, suffix=".pdf", delete=False) as handle:
        temporary = Path(handle.name)
    try:
        canvas = Canvas(str(temporary), pagesize=A4, pageCompression=1, invariant=1)
        font = str(sample["font"])
        if font == "HeiseiKakuGo-W5":
            pdfmetrics.registerFont(UnicodeCIDFont(font))
        canvas.setTitle(str(sample["title"]))
        canvas.setAuthor("PDF Note Workspace")
        canvas.setFillColor(HexColor("#17365D"))
        canvas.setFont(font, 20)
        canvas.drawCentredString(A4[0] / 2, 760, str(sample["title"]))
        canvas.setStrokeColor(HexColor("#1A73E8"))
        canvas.setLineWidth(1.5)
        canvas.line(64, 738, A4[0] - 64, 738)
        canvas.setFillColor(HexColor("#1F2937"))
        canvas.setFont(font, 11)
        canvas.drawCentredString(A4[0] / 2, 685, str(sample["lead"]))
        canvas.setFillColor(HexColor("#F9FAFB"))
        canvas.setStrokeColor(HexColor("#CBD5E1"))
        canvas.roundRect(74, 330, A4[0] - 148, 190, 14, fill=1, stroke=1)
        canvas.setFillColor(HexColor("#1F2937"))
        canvas.setFont(font, 12)
        button_lines = tuple(sample["button_lines"])
        button_y = 485 + (len(button_lines) - 1) * 9
        for line in button_lines:
            canvas.drawCentredString(A4[0] / 2, button_y, str(line))
            button_y -= 18
        # Keep the PDF's target guide neutral: the blue circle is the live .clrop marker.
        canvas.setFillColor(HexColor("#FFFFFF"))
        canvas.setStrokeColor(HexColor("#94A3B8"))
        canvas.setLineWidth(1.25)
        canvas.setDash(3, 3)
        canvas.circle(MARKER_X, MARKER_Y, 35, fill=1, stroke=1)
        canvas.setDash()
        canvas.setFillColor(HexColor("#64748B"))
        canvas.setFont(font, 9)
        canvas.drawCentredString(MARKER_X, MARKER_Y - 53, str(sample["guide_label"]))
        canvas.setFillColor(HexColor("#4B5563"))
        canvas.setFont(font, 9)
        canvas.drawCentredString(A4[0] / 2, 265, str(sample["footer"]))
        canvas.save()
        atomic_write_bytes(path, temporary.read_bytes())
    finally:
        temporary.unlink(missing_ok=True)


def write_clrop(pdf_path: Path) -> None:
    width, height = A4
    payload = {
        "version": 1,
        "pdf_id": {
            "path": pdf_path.name,
            "size": pdf_path.stat().st_size,
            "page_count": 1,
            "page_size_rule": {"default_pt": [width, height], "exceptions": []},
            "sha256": hashlib.sha256(pdf_path.read_bytes()).hexdigest(),
        },
        "pages": [{
            "page": 0,
            "items": [{
                "type": "link-marker",
                "id": MARKER_ID,
                "created": TIMESTAMP,
                "updated": TIMESTAMP,
                "color": "#1A73E8",
                "width": 10,
                "p1": [MARKER_X, MARKER_Y],
                "link_id": LINK_ID,
            }],
        }],
    }
    output = pdf_path.with_suffix(".clrop")
    atomic_write_bytes(output, (json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8"))


def main() -> None:
    for sample in SAMPLES:
        pdf_path = Path(sample["directory"]) / str(sample["pdf"])
        draw_pdf(pdf_path, sample)
        write_clrop(pdf_path)


if __name__ == "__main__":
    main()
