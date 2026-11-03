"""Local synthetic inputs only. Runtime probe is bounded and never uses real secrets."""
from pathlib import Path
import ctypes as C
import hashlib
import re
import shutil
import subprocess
import sys
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[2]


def make_pdf(action: bytes) -> bytes:
    objects = [b"<< /Type /Catalog /Pages 2 0 R /OpenAction 5 0 R /Names << /JavaScript << /Names [(probe) 5 0 R] >> >> >>",
               b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
               b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Contents 4 0 R /AA << /O 5 0 R >> >>",
               b"<< /Length 0 >>\nstream\n\nendstream", action]
    data = b"%PDF-1.7\n"
    offsets = [0]
    for i, obj in enumerate(objects, 1):
        offsets.append(len(data))
        data += str(i).encode() + b" 0 obj\n" + obj + b"\nendobj\n"
    start = len(data)
    data += b"xref\n0 6\n0000000000 65535 f \n"
    data += b"".join(f"{offset:010d} 00000 n \n".encode() for offset in offsets[1:])
    return data + f"trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n{start}\n%%EOF\n".encode()


def pdf_probe():
    # These are the same passive PDFium operations used by the viewers.
    # No form environment, JavaScript platform, shell or action dispatcher.
    dll = C.CDLL(str(ROOT / "third_party/pdfium/bin/pdfium.dll"))
    def bind(name, result, args):
        fn = getattr(dll, name)
        fn.restype, fn.argtypes = result, args
        return fn
    load = bind("FPDF_LoadMemDocument64", C.c_void_p, [C.c_void_p, C.c_size_t, C.c_char_p])
    page_load = bind("FPDF_LoadPage", C.c_void_p, [C.c_void_p, C.c_int])
    count = bind("FPDFDoc_GetJavaScriptActionCount", C.c_int, [C.c_void_p])
    page_close = bind("FPDF_ClosePage", None, [C.c_void_p])
    close = bind("FPDF_CloseDocument", None, [C.c_void_p])
    bitmap_create = bind("FPDFBitmap_Create", C.c_void_p, [C.c_int, C.c_int, C.c_int])
    bitmap_close = bind("FPDFBitmap_Destroy", None, [C.c_void_p])
    render = bind("FPDF_RenderPageBitmap", None, [C.c_void_p, C.c_void_p] + [C.c_int] * 6)
    bind("FPDF_InitLibrary", None, [])()
    try:
        cases = {
            "javascript": b"<< /S /JavaScript /JS (while\\(true\\) {}) >>",
            "launch": b"<< /S /Launch /F (untrusted-script-probe.js) >>",
            "remote": b"<< /S /GoToR /F (synthetic-private-canary.txt) /D [0 /Fit] >>",
            "submit": b"<< /S /SubmitForm /F (synthetic-private-canary.txt) /Flags 0 >>",
        }
        for name, action in cases.items():
            data = make_pdf(action)
            buffer = C.create_string_buffer(data)
            doc = load(buffer, len(data), None)
            if not doc: raise RuntimeError(f"{name}: fixture not loaded")
            try:
                if name == "javascript" and count(doc) != 1:
                    raise RuntimeError("JavaScript fixture was not recognized")
                page = page_load(doc, 0)
                if not page: raise RuntimeError(f"{name}: page not loaded")
                try:
                    bitmap = bitmap_create(32, 32, 0)
                    if not bitmap: raise RuntimeError("bitmap allocation failed")
                    try: render(bitmap, page, 0, 0, 32, 32, 0, 0)
                    finally: bitmap_close(bitmap)
                finally: page_close(page)
            finally: close(doc)
        broken = C.create_string_buffer(b"%PDF-1.7\nnot a document")
        bad = load(broken, len(broken.value), None)
        if bad:
            close(bad)
            raise RuntimeError("Malformed PDF unexpectedly accepted")
        data = make_pdf(b"<< /S /JavaScript /JS () >>")
        buffer = C.create_string_buffer(data)
        recovered = load(buffer, len(data), None)
        if not recovered: raise RuntimeError("Cannot load valid input after failure")
        close(recovered)
    finally:
        bind("FPDF_DestroyLibrary", None, [])()
    print("SCRIPT_RESISTANCE_PDF_PASS cases=4 malformed=1 recovery=1")


class ScriptResistanceTests(unittest.TestCase):
    def test_pdf_engine_and_action_dispatch_stay_disabled(self):
        args = (ROOT / "third_party/pdfium/args.gn").read_text(encoding="utf-8")
        for flag in ("pdf_enable_v8", "pdf_enable_xfa"):
            self.assertRegex(args, rf"(?m)^{flag}\s*=\s*false\s*$")
        forbidden = re.compile(r"\b(?:FORM_Do\w*|FPDFDOC_Do\w*|FPDF_InitFormFillEnvironment|JS_Eval|JsRunScript|eval|system|_wsystem)\s*\(")
        sources = [p for p in (ROOT / "src").rglob("*") if p.suffix in (".cpp", ".cppinc", ".h", ".inc")]
        self.assertGreater(len(sources), 0)
        for p in sources:
            self.assertIsNone(forbidden.search(p.read_text(encoding="utf-8-sig")), str(p))

    def test_pdf_notice_is_passive_and_keeps_user_information(self):
        source = (ROOT / "src/pdf_view/document_io.cppinc").read_text(encoding="utf-8")
        notice = source.split("static std::wstring GetPdfJavaScriptNoticeText(", 1)[1].split("static bool PromptPasswordForPdfOpen", 1)[0]
        for token in ("FPDFDoc_GetJavaScriptActionCount", "FPDFJavaScriptAction_GetName",
                      "FPDFJavaScriptAction_GetScript", "FPDFDoc_CloseJavaScriptAction",
                      "pdf_script_notice::Read", "pdf_script_notice::Preview",
                      "pdf.javascript_notice.alternative", "pdf.javascript_notice.other_viewer_caution",
                      "pdf.javascript_notice.pdf_label"):
            self.assertIn(token, notice)
        for token in ("Append", "WriteFile", "ShellExecute", "CreateProcess", "FORM_Do"):
            self.assertNotIn(token, notice)

    def test_notice_reader_bounds_failures_and_literal_display(self):
        compiler = shutil.which("g++")
        self.assertIsNotNone(compiler, "g++ is required for notice boundary checks")
        output = ROOT / "out/tests/script_resistance" / uuid.uuid4().hex
        output.mkdir(parents=True)
        exe = output / ("notice_text.exe" if sys.platform == "win32" else "notice_text")
        log = output / "notice_text.log"
        with log.open("w", encoding="utf-8") as stream:
            build = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                    "-I", str(ROOT / "src"),
                                    str(ROOT / "tests/unit/pdf_script_notice_text_tests.cpp"), "-o", str(exe)],
                                   stdout=stream, stderr=subprocess.STDOUT, timeout=30)
            self.assertEqual(build.returncode, 0, f"compile failed: {log}")
            run = subprocess.run([str(exe)], stdout=stream, stderr=subprocess.STDOUT, timeout=10)
        self.assertEqual(run.returncode, 0, f"notice test failed: {log}")
        self.assertIn("PDF_SCRIPT_NOTICE_TEXT_PASS", log.read_text(encoding="utf-8"))
        print(f"notice text evidence: {log}")

    def test_office_keeps_owned_profile_and_process_boundary(self):
        source = (ROOT / "src/workspace/workspace_actions.cpp").read_text(encoding="utf-8")
        for token in ("QuoteWindowsCommandLineArg(sourceOfficeCopy.wstring())", "-env:UserInstallation=",
                      "CREATE_UNICODE_ENVIRONMENT", "childEnvironment.data()", "AssignProcessToJobObject",
                      "JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE"):
            self.assertIn(token, source)
        self.assertNotRegex(source, r'(?i)L"(?:cmd(?:\.exe)? /c|powershell(?:\.exe)? )')

    def test_pdf_passive_runtime_and_failure_recovery(self):
        if sys.platform != "win32": self.skipTest("Windows PDFium runtime required")
        output = ROOT / "out/tests/script_resistance" / uuid.uuid4().hex
        output.mkdir(parents=True)
        # All canaries are synthetic; no actual private documents are opened.
        canary = output / "synthetic-private-canary.txt"
        canary.write_bytes(b"PRIVATE_CANARY_DO_NOT_COPY")
        fixture = output / "untrusted-script-probe.js"
        fixture.write_text("throw new Error('SCRIPT_PROBE');", encoding="utf-8")
        before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in output.iterdir()}
        log = output / "probe.log"
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run([sys.executable, "-B", str(Path(__file__).resolve()), "--pdf-probe"],
                                    cwd=output, stdout=stream, stderr=subprocess.STDOUT, timeout=20)
        text = log.read_text(encoding="utf-8")
        self.assertEqual(result.returncode, 0, f"PDF probe failed; log: {log}")
        self.assertIn("SCRIPT_RESISTANCE_PDF_PASS cases=4 malformed=1 recovery=1", text)
        self.assertNotIn("PRIVATE_CANARY_DO_NOT_COPY", text)
        self.assertEqual(before, {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in output.iterdir() if p != log})
        print(f"script resistance evidence: {log}")


if __name__ == "__main__":
    if "--pdf-probe" in sys.argv: pdf_probe()
    else: unittest.main()
