#!/usr/bin/env python3
"""Create the Session 05 practice PDF for placing MathBox formulas on a page."""

from __future__ import annotations

import os
from pathlib import Path
from tempfile import NamedTemporaryFile

from reportlab.lib.colors import HexColor
from reportlab.lib.pagesizes import A4
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.cidfonts import UnicodeCIDFont
from reportlab.pdfgen.canvas import Canvas


REPO_ROOT = Path(__file__).resolve().parents[2]
MONO_FONT = "Courier"

SAMPLES = (
    {
        "directory": REPO_ROOT / "release_assets/sample_workspace/ja/01_講義サンプル/第05回_数式表示",
        "pdf": "数式配置練習.pdf",
        "title": "第05回  数式を PDF に置く練習",
        "lead": (
            "「表示 → 右下設定（一時） → MathBox入力」で右下を数式入力欄に切り替えます。",
            "式を入力し、プレビューを下の枠までドラッグして離すと、PDF 上に数式を置けます。",
        ),
        "boxes": (
            ("① 分数", r"\frac{a+b}{2}"),
            ("② 行列", r"\begin{pmatrix} 1 & 0 \\ 0 & 1 \end{pmatrix}"),
            ("③ 場合分け", r"|x| = \begin{cases} x & (x \ge 0) \\ -x & (x < 0) \end{cases}"),
        ),
        "box_hint": "入力例（囲み記号の $ は不要です）",
        "footer": (
            "置いた数式は PDF と同名の .clrop に保存され、PDF 本体は書き換えません。",
            "注釈一覧の「編集」で原文・色・位置を変えられます。",
        ),
        "font": "HeiseiKakuGo-W5",
    },
    {
        "directory": REPO_ROOT / "release_assets/sample_workspace/en/01_Lecture_Samples/Session_05_Math_Display",
        "pdf": "math_placement_practice.pdf",
        "title": "Session 05  Placing Formulas on a PDF",
        "lead": (
            "Select View > Bottom pane (temporary) > MathBox input to turn the bottom pane into a formula input.",
            "Type a formula, then drag its preview onto a box below and release to place it on the PDF.",
        ),
        "boxes": (
            ("1. Fraction", r"\frac{a+b}{2}"),
            ("2. Matrix", r"\begin{pmatrix} 1 & 0 \\ 0 & 1 \end{pmatrix}"),
            ("3. Cases", r"|x| = \begin{cases} x & (x \ge 0) \\ -x & (x < 0) \end{cases}"),
        ),
        "box_hint": "Example input (no $ delimiters needed)",
        "footer": (
            "Placed formulas are saved in the same-named .clrop; the PDF itself is not rewritten.",
            "Use Edit in the annotation list to change the source, color, or position.",
        ),
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


def draw_pdf(path: Path, sample: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with NamedTemporaryFile(dir=path.parent, suffix=".pdf", delete=False) as handle:
        temporary = Path(handle.name)
    try:
        width, _ = A4
        canvas = Canvas(str(temporary), pagesize=A4, pageCompression=1, invariant=1)
        font = str(sample["font"])
        if font == "HeiseiKakuGo-W5":
            pdfmetrics.registerFont(UnicodeCIDFont(font))
        canvas.setTitle(str(sample["title"]))
        canvas.setAuthor("PDF Note Workspace")
        canvas.setFillColor(HexColor("#17365D"))
        canvas.setFont(font, 20)
        canvas.drawCentredString(width / 2, 780, str(sample["title"]))
        canvas.setStrokeColor(HexColor("#1A73E8"))
        canvas.setLineWidth(1.5)
        canvas.line(56, 760, width - 56, 760)
        canvas.setFillColor(HexColor("#1F2937"))
        canvas.setFont(font, 10)
        lead_y = 735
        for line in sample["lead"]:
            canvas.drawCentredString(width / 2, lead_y, line)
            lead_y -= 16
        # Neutral dashed targets: the formulas themselves are live MathBox annotations.
        box_top = 680
        box_height = 150
        gap = 22
        for label, example in sample["boxes"]:
            bottom = box_top - box_height
            canvas.setFillColor(HexColor("#FFFFFF"))
            canvas.setStrokeColor(HexColor("#94A3B8"))
            canvas.setLineWidth(1.25)
            canvas.setDash(4, 3)
            canvas.roundRect(72, bottom, width - 144, box_height, 10, fill=1, stroke=1)
            canvas.setDash()
            canvas.setFillColor(HexColor("#17365D"))
            canvas.setFont(font, 12)
            canvas.drawString(86, box_top - 22, label)
            canvas.setFillColor(HexColor("#64748B"))
            canvas.setFont(font, 8.5)
            canvas.drawString(86, bottom + 26, str(sample["box_hint"]))
            canvas.setFillColor(HexColor("#334155"))
            canvas.setFont(MONO_FONT, 9)
            canvas.drawString(86, bottom + 12, example)
            box_top = bottom - gap
        canvas.setFillColor(HexColor("#4B5563"))
        canvas.setFont(font, 9)
        footer_y = 92
        for line in sample["footer"]:
            canvas.drawCentredString(width / 2, footer_y, line)
            footer_y -= 14
        canvas.save()
        atomic_write_bytes(path, temporary.read_bytes())
    finally:
        temporary.unlink(missing_ok=True)


def main() -> None:
    for sample in SAMPLES:
        draw_pdf(Path(sample["directory"]) / str(sample["pdf"]), sample)


if __name__ == "__main__":
    main()
