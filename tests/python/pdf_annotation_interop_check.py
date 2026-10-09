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


def check(directory: Path) -> None:
    from pypdf import PdfReader
    import pymupdf

    for name, subtype in (("highlight.pdf", "/Highlight"), ("ink.pdf", "/Ink"),
                          ("freetext.pdf", "/FreeText")):
        path = directory / name
        reader = PdfReader(path, strict=True)
        obj = inspect_structure(reader, subtype)
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
        require(updated_reader.pages[0]["/Annots"][0].get_object()["/Subtype"] == subtype, "subtype changed after external edit")
        with pymupdf.open(stream=updated, filetype="pdf") as doc:
            page = doc[0]
            require(len(list(page.annots())) == 1, "annotation lost after external edit")
            if subtype == "/FreeText":
                require(next(page.annots()).info["content"] == "Edited blue text", "edited text was lost")
            else:
                require(all(abs(x-y)<0.001 for x,y in zip(next(page.annots()).colors["stroke"],(0.1,0.2,0.8))), "edited color was lost")
        print(f"PASS: {name}: indirect reference, external-engine access/edit/reload")
    negative_controls(directory)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    args = parser.parse_args()
    check(args.fixtures)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
