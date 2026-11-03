"""Independent inspection of the two real import/restart/export workflows."""
import sys
import hashlib
import math
from pathlib import Path

from pypdf import PdfReader, PdfWriter
from pypdf.generic import ArrayObject, FloatObject, NameObject


def verify(path: Path) -> None:
    reader = PdfReader(path, strict=True)
    if len(reader.pages) != 1:
        raise ValueError(f"Unexpected page count: {path}")
    annotations = [reference.get_object() for reference in reader.pages[0].get("/Annots", [])]
    # The established export contract maps app lines to editable Ink,
    # preserving their ID and path geometry (not native PDF /Line).
    lines = [item for item in annotations if item.get("/Subtype") == "/Ink" and item.get("/NM") == "workflow-line"]
    if len(annotations) != 1 or len(lines) != 1:
        raise ValueError(f"Missing/duplicated exported workflow line: {path}")
    ink = lines[0].get("/InkList", [])
    if len(ink) != 1 or list(ink[0]) != [10, 10, 40, 40]:
        raise ValueError(f"Changed line path geometry: {path}")
    color = lines[0].get("/C", [])
    if len(color) != 3 or any(not math.isfinite(float(actual)) or abs(float(actual) - expected / 255) > 1e-6
                             for actual, expected in zip(color, (0x12, 0x34, 0x56))):
        raise ValueError(f"Changed line color: {path}")
    border = lines[0].get("/Border", [])
    if len(border) != 3 or border[2] != 2:
        raise ValueError(f"Changed line width: {path}")


def negative_controls(source: Path, directory: Path) -> None:
    directory.mkdir()  # New evidence only; never overwrite previous controls.
    cases = ("missing-annotation", "duplicate-annotation", "changed-geometry", "changed-color", "changed-width", "extra-page")
    for case in cases:
        writer = PdfWriter(clone_from=source)
        page = writer.pages[0]
        annotation = page["/Annots"][0].get_object()
        if case == "missing-annotation":
            del page["/Annots"]
        elif case == "duplicate-annotation":
            page["/Annots"].append(page["/Annots"][0])
        elif case == "changed-geometry":
            annotation["/InkList"][0][0] = FloatObject(11)
        elif case == "changed-color":
            annotation[NameObject("/C")] = ArrayObject([FloatObject(0)] * 3)
        elif case == "changed-width":
            annotation["/Border"][2] = FloatObject(3)
        else:
            writer.add_blank_page(width=100, height=100)
        path = directory / f"{case}.pdf"
        with path.open("xb") as stream:
            writer.write(stream)
        try:
            verify(path)
        except ValueError:
            print(f"[PASS] independent verifier rejects: {case}")
        else:
            raise RuntimeError(f"Verifier incorrectly accepted negative control: {path}")
    try:
        verify(directory / "missing-output.pdf")
    except FileNotFoundError:
        print("[PASS] independent verifier rejects: missing-output")
    else:
        raise RuntimeError("Verifier incorrectly accepted missing output")


def main() -> None:
    root = Path(sys.argv[1]).resolve(strict=True)
    for placement in ("root", "separate"):
        path = root / placement / "出力 空白" / "workflow_export.pdf"
        original_hash = hashlib.sha256(path.read_bytes()).digest()
        verify(path)
        print(f"[PASS] independent workflow PDF page/line/coordinates/color/width: {placement}")
        negative_controls(path, root / placement / "independent-verifier-controls")
        if hashlib.sha256(path.read_bytes()).digest() != original_hash:
            raise RuntimeError(f"Independent verification changed the app output: {path}")


if __name__ == "__main__":
    main()
