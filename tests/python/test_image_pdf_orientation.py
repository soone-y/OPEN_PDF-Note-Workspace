"""Render the image converter's placement matrix using the shipped PDFium DLL."""
import ctypes as C
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ImagePdfOrientationTests(unittest.TestCase):
    def test_four_corners_survive_pdf_save_and_reload(self):
        import fitz

        source = (ROOT / "src/file_output/file_output.cpp").read_text(encoding="utf-8")
        conversion = source.split("static bool ConvertImageToPdfFile(", 1)[1].split(
            "static bool ExportPdfDocumentWithParts(", 1)[0]
        matrix = re.findall(r"FPDFImageObj_SetMatrix\(object,\s*([^)]*)\)", conversion)
        self.assertEqual(len(matrix), 1, "expected the converter's image placement matrix")
        arguments = [value.strip() for value in matrix[0].split(",")]
        self.assertEqual(len(arguments), 6)

        pdf = C.CDLL(str(ROOT / "third_party/pdfium/bin/pdfium.dll"))

        def bind(name, result, *parameters):
            function = getattr(pdf, name)
            function.restype = result
            function.argtypes = list(parameters)
            return function

        pointer, integer, number = C.c_void_p, C.c_int, C.c_double
        initialize = bind("FPDF_InitLibrary", None)
        destroy = bind("FPDF_DestroyLibrary", None)
        new_document = bind("FPDF_CreateNewDocument", pointer)
        close_document = bind("FPDF_CloseDocument", None, pointer)
        new_page = bind("FPDFPage_New", pointer, pointer, integer, number, number)
        close_page = bind("FPDF_ClosePage", None, pointer)
        new_image = bind("FPDFPageObj_NewImageObj", pointer, pointer)
        new_bitmap = bind("FPDFBitmap_CreateEx", pointer, integer, integer, integer, pointer, integer)
        close_bitmap = bind("FPDFBitmap_Destroy", None, pointer)
        set_bitmap = bind("FPDFImageObj_SetBitmap", integer, C.POINTER(pointer), integer, pointer, pointer)
        set_matrix = bind("FPDFImageObj_SetMatrix", integer, pointer, *([number] * 6))
        insert = bind("FPDFPage_InsertObject", None, pointer, pointer)
        generate = bind("FPDFPage_GenerateContent", integer, pointer)
        callback_type = C.CFUNCTYPE(integer, pointer, pointer, C.c_ulong)

        class Writer(C.Structure):
            _fields_ = [("version", integer), ("WriteBlock", callback_type)]

        save = bind("FPDF_SaveAsCopy", integer, pointer, C.POINTER(Writer), C.c_ulong)
        initialize()
        try:
            for width, height in ((80, 48), (48, 80), (64, 64)):
                with self.subTest(width=width, height=height):
                    # Four distinct quadrants catch vertical/horizontal flips and 180-degree rotation.
                    colors = ((255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0))
                    pixels = bytearray()
                    for y in range(height):
                        for x in range(width):
                            red, green, blue = colors[2 * (y >= height // 2) + (x >= width // 2)]
                            pixels.extend((blue, green, red, 255))
                    buffer = (C.c_ubyte * len(pixels)).from_buffer(pixels)
                    document = new_document()
                    self.assertTrue(document)
                    page = bitmap = None
                    try:
                        page = new_page(document, 0, width, height)
                        self.assertTrue(page)
                        image = new_image(document)
                        self.assertTrue(image)
                        bitmap = new_bitmap(width, height, 4, buffer, width * 4)  # BGRA
                        self.assertTrue(bitmap)
                        page_handle = pointer(page)
                        self.assertTrue(set_bitmap(C.byref(page_handle), 1, image, bitmap))
                        values = {"widthPt": width, "heightPt": height,
                                  "-widthPt": -width, "-heightPt": -height}
                        placement = [values[token] if token in values else float(token)
                                     for token in arguments]
                        self.assertTrue(set_matrix(image, *placement))
                        insert(page, image)
                        self.assertTrue(generate(page))
                        output = bytearray()

                        @callback_type
                        def write_block(_writer, data, size):
                            output.extend(C.string_at(data, size))
                            return 1

                        writer = Writer(1, write_block)
                        self.assertTrue(save(document, C.byref(writer), 0))
                        # An independent engine reloads and renders the serialized PDF.
                        with fitz.open(stream=output, filetype="pdf") as reopened:
                            self.assertEqual(len(reopened), 1)
                            rendered = reopened[0].get_pixmap(alpha=False)
                            self.assertEqual((rendered.width, rendered.height), (width, height))
                            corners = [(width // 4, height // 4), (3 * width // 4, height // 4),
                                       (width // 4, 3 * height // 4), (3 * width // 4, 3 * height // 4)]
                            self.assertEqual([rendered.pixel(x, y) for x, y in corners], list(colors))
                    finally:
                        if bitmap:
                            close_bitmap(bitmap)
                        if page:
                            close_page(page)
                        close_document(document)
        finally:
            destroy()


if __name__ == "__main__":
    unittest.main()
