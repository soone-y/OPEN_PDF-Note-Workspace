"""Independent-engine regression checks; never repair the input PDFs.

pypdf and PyMuPDF are local development dependencies, not app dependencies.
Missing modules/fixtures, malformed references and inaccessible annotations
must fail, not become an optional skipped test. Mutations stay in memory.
"""
from __future__ import annotations

import argparse
from io import BytesIO
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def inspect_structure(reader, subtype: str):
    from pypdf.generic import IndirectObject
    require(len(reader.pages) == 1, "expected one fixture page")
    refs = reader.pages[0]["/Annots"]
    require(len(refs) == 1 and isinstance(refs[0], IndirectObject), "direct annotation dictionary")
    obj = refs[0].get_object()
    require(obj["/Subtype"] == subtype and bool(int(obj["/F"]) & 4), "incorrect subtype/print flag")
    if subtype == "/FreeText":
        require("/C" not in obj, "text color must not become a background color")
        da = str(obj["/DA"])
        form = reader.trailer["/Root"]["/AcroForm"]
        require(list(form["/Fields"])==[], "empty output resource dictionary must not introduce form fields")
        fonts = form["/DR"]["/Font"]
        require(da.split()[0] in fonts, "DA font resource is missing")
        font_ref=fonts.raw_get(da.split()[0])
        require(isinstance(font_ref,IndirectObject) and font_ref.idnum>0,
                "DA font has an invalid indirect reference")
        font=font_ref.get_object()
        require(font is not None and font.get("/Type")=="/Font", "DA font resource cannot be resolved")
    return obj


def negative_controls(directory: Path) -> None:
    from pypdf import PdfReader, PdfWriter
    from pypdf.generic import ArrayObject, NameObject, FloatObject, IndirectObject
    # Break each invariant in memory. A successful renderer is not enough:
    # the checker must reject formerly accepted but incompatible documents.
    for kind in ("direct annotation", "colored text background", "missing DA font", "invalid DA font reference", "missing Fields array"):
        writer = PdfWriter()
        writer.clone_document_from_reader(PdfReader(directory / "freetext.pdf"))
        obj = writer.pages[0]["/Annots"][0].get_object()
        if kind == "direct annotation":
            writer.pages[0][NameObject("/Annots")] = ArrayObject([obj])
        elif kind == "colored text background":
            obj[NameObject("/C")] = ArrayObject([FloatObject(0), FloatObject(100 / 255), FloatObject(240 / 255)])
        elif kind == "missing DA font":
            del writer.root_object["/AcroForm"]["/DR"]["/Font"]["/Helv"]
        elif kind == "invalid DA font reference":
            writer.root_object["/AcroForm"]["/DR"]["/Font"][NameObject("/Helv")]=IndirectObject(0,0,writer)
        else:
            del writer.root_object["/AcroForm"]["/Fields"]
        stream = BytesIO()
        writer.write(stream)
        try:
            inspect_structure(PdfReader(BytesIO(stream.getvalue()), strict=True), "/FreeText")
        except (RuntimeError, KeyError):
            print(f"PASS: negative control rejected: {kind}")
        else:
            raise RuntimeError(f"negative control was incorrectly accepted: {kind}")


def check(directory: Path, shapes_only: bool = False) -> None:
    from pypdf import PdfReader
    import pymupdf

    fixtures = [] if shapes_only else [("highlight.pdf", "/Highlight"), ("ink.pdf", "/Ink"),
                                       ("freetext.pdf", "/FreeText")]
    fixtures.extend((f"shape_outline_{i}.pdf", "/Ink") for i in range(8))
    for name, subtype in fixtures:
        path = directory / name
        reader = PdfReader(path, strict=True)
        obj = inspect_structure(reader, subtype)
        shape_runs = obj["/InkList"] if name.startswith("shape_outline_") else None
        if shape_runs is not None:
            require(len(shape_runs) > 10, f"{name}: dashed outline became a solid contour")
        with pymupdf.open(path) as doc:
            page = doc[0]
            annots = list(page.annots())
            require(len(annots) == 1 and annots[0].xref > 0, f"{name}: cannot obtain editable annotation")
            annot = annots[0]
            require(annot.type[1] == subtype[1:], name)
            if subtype == "/FreeText":
                before = page.get_pixmap(clip=annot.rect, alpha=False)
                require(before.pixel(before.width - 3, before.height - 3) == (255, 255, 255), "initial background is not transparent")
                annot.set_info(content="Edited blue text")
                annot.update()
                after = page.get_pixmap(clip=annot.rect, alpha=False)
                # Bottom-right is outside the one-line text. The top-left can
                # contain a glyph after a legitimate editor reflows the text.
                require(after.pixel(after.width - 3, after.height - 3) == (255, 255, 255), "editing filled the background")
                samples = after.samples
                blue = sum(samples[i + 2] > samples[i] + 50 for i in range(0, len(samples), after.n))
                require(20 < blue < after.width * after.height // 2, "edited blue text is blank or background-filled")
            else:
                # Exercise actual color editing and a save/reload in a
                # different PDF engine, rather than only enumerate PDFium types.
                annot.set_colors(stroke=(0.1, 0.2, 0.8))
                annot.update()
            updated = doc.tobytes()
        updated_reader = PdfReader(BytesIO(updated), strict=True)
        if shape_runs is not None:
            edited_runs = updated_reader.pages[0]["/Annots"][0].get_object()["/InkList"]
            # The independent engine serializes float coordinates at a different
            # decimal precision. Preserve every run/vertex within 0.0001 pt.
            require(len(edited_runs) == len(shape_runs) and
                    all(len(a) == len(b) and all(abs(float(x) - float(y)) <= 0.0001
                        for x, y in zip(a, b)) for a, b in zip(shape_runs, edited_runs)),
                    f"{name}: external edit lost dash geometry")
        require(updated_reader.pages[0]["/Annots"][0].get_object()["/Subtype"] == subtype, "subtype changed after external edit")
        with pymupdf.open(stream=updated, filetype="pdf") as doc:
            page = doc[0]
            require(len(list(page.annots())) == 1, "annotation lost after external edit")
            if subtype == "/FreeText":
                require(next(page.annots()).info["content"] == "Edited blue text", "edited text was lost")
            else:
                require(all(abs(x-y)<0.001 for x,y in zip(next(page.annots()).colors["stroke"],(0.1,0.2,0.8))), "edited color was lost")
        print(f"PASS: {name}: indirect reference, external-engine access/edit/reload")
    if shapes_only:
        png = pymupdf.Pixmap(str(directory / "shape_outline.png"))
        samples = png.samples
        blue = sum(samples[i + 2] > samples[i] + 60 and samples[i + 2] > samples[i + 1] + 40
                   for i in range(0, len(samples), png.n))
        require(blue > 100, "app PNG output lost the dashed shape")
    if not shapes_only:
        negative_controls(directory)


def check_merged(path: Path) -> None:
    import pymupdf
    from pypdf import PdfReader
    from pypdf.generic import IndirectObject
    data = path.read_bytes()
    reader = PdfReader(BytesIO(data), strict=True)
    require(len(reader.pages) == 3, "merged fixture page count")
    for index, page in enumerate(reader.pages):
        refs = [ref for ref in page.get("/Annots", [])
                if str(ref.get_object().get("/NM", "")).startswith("export-fixture-")]
        require(len(refs) == (0 if index == 0 else 8), "merged annotation selection")
        require(all(isinstance(ref, IndirectObject) and ref.idnum > 0 for ref in refs), "merged direct annotation")
        if not index:
            continue
        entries = [ref.get_object() for ref in refs]
        marker = next(a for a in entries if a["/NM"] == "export-fixture-0")
        require(abs(float(marker["/QuadPoints"][0]) - (60 if index == 1 else 15)) < 0.01, "merged annotation scale")
        text = next(a for a in entries if a["/NM"] == "export-fixture-2")
        require(text["/Subtype"] == ("/FreeText" if index == 1 else "/Stamp"), "merged text annotation option")
        if index == 1:
            form = reader.trailer["/Root"]["/AcroForm"]
            fonts = form["/DR"]["/Font"]
            font = fonts.raw_get(str(text["/DA"]).split()[0])
            require(list(form["/Fields"]) == [] and isinstance(font, IndirectObject) and font.idnum > 0
                    and font.get_object()["/Type"] == "/Font", "merged DA font resource")
    with pymupdf.open(stream=data, filetype="pdf") as document:
        for index in (1, 2):
            page = document[index]  # Keep the owning page alive while editing.
            annot = next(a for a in page.annots() if a.info["id"] == "export-fixture-0")
            annot.set_info(content="independent merge edit")
            annot.update()
        updated = document.tobytes()
    edited = PdfReader(BytesIO(updated), strict=True)
    with pymupdf.open(stream=updated, filetype="pdf") as document:
        for index in (1, 2):
            entries = [ref.get_object() for ref in edited.pages[index]["/Annots"]]
            require(next(a for a in entries if a.get("/NM") == "export-fixture-0")["/Contents"] == "independent merge edit",
                    "merged external edit was lost")
            page = document[index]
            require(next(a for a in page.annots() if a.info["id"] == "export-fixture-0").info["content"] == "independent merge edit",
                    "merged annotation cannot be reloaded")
    require(path.read_bytes() == data, "merged fixture changed")
    print("PASS: merged PDF: segment options/scale, indirect references, DA fonts, external-engine access/edit/reload")


def check_text_color_overlays(directory: Path) -> None:
    import fitz
    from pypdf import PdfReader
    from pypdf.generic import IndirectObject

    for mode, japanese, rotation, scaled in ((m, j, r, h) for m in (1, 2, 3) for j in (False, True) for r in range(4) for h in (False, True)):
        suffix = ("_jp" if japanese else "") + (f"_r{rotation}" if rotation else "") + ("_half" if scaled else "")
        expected_text = "文字色" if japanese else "Color"
        path = directory / f"text_color_overlay_{mode}{suffix}.pdf"
        data = path.read_bytes()
        reader = PdfReader(BytesIO(data), strict=True)
        page = reader.pages[0]
        require(page.extract_text() == expected_text, "overlay changed original text")
        refs = page["/Annots"]
        require(len(refs) == 1 and isinstance(refs[0], IndirectObject), "overlay indirect annotation")
        annot = refs[0].get_object()
        require(annot["/Subtype"] == "/Stamp", "overlay annotation subtype")
        appearance = annot["/AP"]["/N"].get_object().get_data().decode("ascii")
        first_color = [float(v) for v in next(line for line in appearance.splitlines() if line.endswith(" rg")).split()[:3]]
        expected = ((240, 20, 30) if mode == 1 else (220, 240, 230) if mode == 2 else (80, 140, 200))
        require(all(abs(actual - wanted / 255) < 0.005 for actual, wanted in zip(first_color, expected)),
                "overlay transparent/automatic/specified background mismatch")
        with fitz.open(stream=data, filetype="pdf") as document:
            page = document[0]
            require(page.get_text().strip() == expected_text, "overlay searchable text duplicated")
            before = page.get_pixmap(annots=False).samples
            rendered = page.get_pixmap(annots=True).samples
            red = sum(rendered[i] > rendered[i+1] + 20 and rendered[i] > rendered[i+2] + 20
                      for i in range(0, len(rendered), 3))
            require(red > 0 and rendered != before, f"{path.name}: independent engine cannot render colored glyphs (red pixels={red})")
            overlay = next(page.annots())
            overlay.set_info(content="independent color overlay edit")
            edited = document.tobytes()
        edited_reader = PdfReader(BytesIO(edited), strict=True)
        require(edited_reader.pages[0]["/Annots"][0].get_object()["/Contents"] == "independent color overlay edit",
                "overlay external edit/reload")
        with fitz.open(stream=edited, filetype="pdf") as document:
            require(document[0].get_pixmap(annots=True).samples == rendered, "external edit lost the glyph appearance")
        require(path.read_bytes() == data, "overlay original fixture changed")
    print("PASS: text color overlays: 48/48 cases, 3 modes, Latin/Japanese, 4 rotations, 2 scales, background colors, glyph rendering, preserved text, external edit/reload")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--fixtures", type=Path)
    modes.add_argument("--merged-pdf", type=Path)
    modes.add_argument("--shape-fixtures", type=Path)
    args = parser.parse_args()
    if args.merged_pdf:
        check_merged(args.merged_pdf)
    elif args.shape_fixtures:
        check(args.shape_fixtures, shapes_only=True)
    else:
        check(args.fixtures)
        check_text_color_overlays(args.fixtures)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
