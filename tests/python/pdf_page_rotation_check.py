"""Verify application-generated page rotation exports with independent PDF engines."""
from __future__ import annotations
import argparse
import json
from io import BytesIO
from pathlib import Path

import pymupdf
from pypdf import PdfReader
from PIL import Image, ImageFilter


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def blue_mask(document: pymupdf.Document) -> Image.Image:
    pixmap = document[0].get_pixmap(alpha=False)
    require(pixmap.n == 3, "expected RGB raster")
    data = pixmap.samples
    mask = bytes(255 if data[i + 2] > data[i] + 50 and data[i + 2] > data[i + 1] + 50
                 and data[i] < 100 and data[i + 1] < 150 else 0 for i in range(0, len(data), 3))
    return Image.frombytes("L", (pixmap.width, pixmap.height), mask)


def check(directory: Path) -> None:
    reference = None
    upright_reference = None
    for turn in range(4):
        path = directory / f"rotation_{turn}.pdf"
        data = path.read_bytes()
        sidecar = json.loads((directory / f"rotation_{turn}.clrop").read_text(encoding="utf-8-sig"))
        require("view_rotation" not in json.dumps(sidecar), "temporary view rotation leaked into sidecar")
        items = sidecar["pages"][0]["items"]
        require(len(items) == 4, "missing saved annotation")
        new_text = next(item for item in items if item.get("content") == "New")
        require(new_text.get("content_rotation", 0) == (-turn % 4) * 90, "new text orientation was lost")
        reader = PdfReader(BytesIO(data), strict=True)
        require(len(reader.pages) == 1 and reader.pages[0].rotation == turn * 90, "wrong output page rotation")
        annotations = [ref.get_object() for ref in reader.pages[0]["/Annots"]]
        require(len(annotations) == 4 and [a["/Subtype"] for a in annotations] ==
                ["/FreeText", "/FreeText", "/Stamp", "/Stamp"], "text, math or shape annotation lost")
        for item in items:
            if item.get("id") in ("rotation-math", "rotation-triangle"):
                require(item.get("content_rotation", 0) == (-turn % 4) * 90,
                        "math or triangle paper orientation was lost")
        with pymupdf.open(stream=data, filetype="pdf") as document:
            actual = blue_mask(document)
            require(actual.getbbox() is not None, "oriented appearance is blank")
            if turn == 0:
                reference = actual
            else:
                rotation = {1: Image.Transpose.ROTATE_270, 2: Image.Transpose.ROTATE_180, 3: Image.Transpose.ROTATE_90}[turn]
                expected = reference.transpose(rotation)
                require(actual.size == expected.size, "rotated page dimensions differ")
                # PDF page sizes may be fractional points, whereas raster edges
                # are rounded to whole pixels. A 180-degree pivot can therefore
                # shift antialiasing by one pixel even with identical PDF geometry.
                require(all(abs(x-y) <= 1 for x,y in zip(actual.getbbox(),expected.getbbox())),
                        "paper-anchored text bounds moved beyond raster rounding")
                a, b = actual.tobytes(), expected.tobytes()
                expanded_a = actual.filter(ImageFilter.MaxFilter(3)).tobytes()
                expanded_b = expected.filter(ImageFilter.MaxFilter(3)).tobytes()
                count_a, count_b = sum(x != 0 for x in a), sum(x != 0 for x in b)
                require(count_a > 100 and 0.90 < count_a / count_b < 1.10,
                        "paper-anchored appearance lost ink")
                require(sum(x != 0 and y != 0 for x,y in zip(a,expanded_b)) / count_a > 0.98 and
                        sum(x != 0 and y != 0 for x,y in zip(b,expanded_a)) / count_b > 0.98,
                        "paper-anchored text moved or changed direction")
            pixmap = document[0].get_pixmap(alpha=False)
            rgb = pixmap.samples
            mask = Image.frombytes("L", (pixmap.width,pixmap.height), bytes(
                255 if rgb[i] > 200 and rgb[i+1] < 100 and rgb[i+2] < 100 else 0
                for i in range(0,len(rgb),3)))
            require(mask.getbbox() is not None,"new rotated-view text is blank")
            upright = mask.crop(mask.getbbox())
            if turn == 0:
                upright_reference = upright
            else:
                require(abs(upright.width-upright_reference.width)<=1 and abs(upright.height-upright_reference.height)<=1,
                        "new text was rotated away from the screen orientation")
                w=max(upright.width,upright_reference.width)+2
                h=max(upright.height,upright_reference.height)+2
                a=Image.new("L",(w,h));b=Image.new("L",(w,h))
                a.paste(upright,(1,1));b.paste(upright_reference,(1,1))
                aa,bb=a.tobytes(),b.tobytes()
                ad=a.filter(ImageFilter.MaxFilter(3)).tobytes();bd=b.filter(ImageFilter.MaxFilter(3)).tobytes()
                require(sum(x!=0 and y!=0 for x,y in zip(aa,bd))/sum(x!=0 for x in aa)>0.98 and
                        sum(x!=0 and y!=0 for x,y in zip(bb,ad))/sum(x!=0 for x in bb)>0.98,
                        "new text appearance is not upright after page rotation")
            saved_again = document.tobytes()
        with pymupdf.open(stream=saved_again, filetype="pdf") as reopened:
            require(reopened[0].rotation == turn * 90 and blue_mask(reopened).tobytes() == actual.tobytes(),
                    "independent-engine resave changed appearance or direction")
        require(path.read_bytes() == data, "verification changed the original output")
    print("PASS: PDF page rotation: 4 orientations, FreeText, sidecar orientation, raster placement, independent resave")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    check(parser.parse_args().directory)
