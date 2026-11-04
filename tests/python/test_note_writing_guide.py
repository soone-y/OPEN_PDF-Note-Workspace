"""Keep the local user reference complete and usable in either release locale."""
from pathlib import Path
import importlib.util
import re
import tempfile
import unittest
from tools.release_checks import release_locale_content_gate as release

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("guide_portal_gate", ROOT / "site/github/scripts/validate_public_site.py")
assert SPEC and SPEC.loader
portal = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(portal)


class WritingGuideTests(unittest.TestCase):
    def guides(self):
        for locale in ("ja", "en"):
            yield locale, (ROOT / "docs" / locale / "Note_Writing_Guide.md").read_text(encoding="utf-8")

    def test_all_custom_style_aliases_and_math_symbols_are_documented(self):
        parser = (ROOT / "src/note/note_parser.cpp").read_text(encoding="utf-8")
        style = parser.split("bool ParseStyleOpenTag(", 1)[1].split("void AppendStyleSpan(", 1)[0]
        aliases = set(re.findall(r'lowered == L"([a-z]+)"', style))
        math = (ROOT / "src/math/math_render.cpp").read_text(encoding="utf-8")
        mapping = math.split("const wchar_t* MapLatexCommand(", 1)[1].split("bool ConsumeLatexCommand(", 1)[0]
        symbols = set(re.findall(r'name == L"([A-Za-z]+)"', mapping))
        self.assertGreater(len(aliases), 15)
        self.assertGreater(len(symbols), 70)
        for locale, guide in self.guides():
            with self.subTest(locale=locale):
                for word in aliases | symbols:
                    self.assertRegex(guide, r"\b" + re.escape(word) + r"\b", word)
                for form in ("\\frac", "\\dfrac", "\\tfrac", "\\sqrt", "\\binom", "\\left", "\\right",
                             "\\bar", "\\overline", "\\hat", "\\widehat", "\\vec", "\\dot", "\\ddot",
                             "pmatrix", "bmatrix", "Bmatrix", "vmatrix", "Vmatrix", "cases", "aligned", "align",
                             "<math", "</math>", "display=inline", "display=block", "title=", "hicont"):
                    self.assertIn(form, guide)

    def test_each_locale_is_indexed_and_resolves_local_links(self):
        for locale, guide in self.guides():
            directory = ROOT / "docs" / locale
            with self.subTest(locale=locale):
                for index in ("README.md", "CLRO_Note_Format.md", "Help_Reference.md"):
                    self.assertIn("(Note_Writing_Guide.md)", (directory / index).read_text(encoding="utf-8"))
                # Examples inside fences are source text, not broken release links.
                fence = None
                prose = []
                for line in guide.splitlines():
                    marker = re.match(r"^ {0,3}(`{3,}|~{3,})", line)
                    if marker:
                        run = marker.group(1)
                        if fence is None:
                            fence = run
                        elif run[0] == fence[0] and len(run) >= len(fence):
                            fence = None
                        continue
                    if fence is None:
                        prose.append(line)
                self.assertIsNone(fence, "Writing guide ends with an unclosed example fence")
                links = re.findall(r"\[[^\]]*\]\(([^)]+)\)", "\n".join(prose))
                self.assertGreaterEqual(len(links), 3)
                for link in links:
                    # Page fragments identify a heading, not part of its filename.
                    # The generated-site gate validates the fragment itself.
                    self.assertTrue((directory / link.split("#", 1)[0]).is_file(), link)
                self.assertIn("[x]", guide)
                self.assertIn("| :--- | :---: | ---: |", guide)
                self.assertIn("![", guide)
                if locale == "en":
                    self.assertIn("| English | Japanese aliases (Unicode escapes) | RGB |", guide)
                    self.assertIsNone(release.JAPANESE.search(guide), "English guide must satisfy the actual release locale gate")

    def test_unicode_alias_reference_preserves_the_japanese_names(self):
        guides = dict(self.guides())
        english = guides["en"].split("### Named colors", 1)[1].split("Pale colors", 1)[0]
        japanese = guides["ja"].split("### 色名で指定する", 1)[1].split("薄色は", 1)[0]
        def rows(section):
            return {line.split("|")[1].strip(): line.split("|")[2].strip()
                    for line in section.splitlines() if line.startswith("| `") and line.count("|") == 4}
        actual, expected = rows(english), rows(japanese)
        self.assertEqual(set(actual), set(expected))
        self.assertGreater(len(actual), 20)
        for name in actual:
            decoded = {bytes(value.strip("`"), "ascii").decode("unicode_escape") for value in actual[name].split(" / ")}
            self.assertEqual(decoded, set(expected[name].split("・")), name)
        self.assertIn("not escape syntax for note input", guides["en"])

    def test_fenced_examples_are_literal_but_real_broken_links_still_fail(self):
        parent = ROOT / "out/tests/guide_document_checks"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as temporary:
            directory = Path(temporary)
            doc = directory / "guide.md"
            doc.write_text(
                "````text\n```\n[example](missing-example.md)\n```\n````\n"
                "~~~text\n![example](missing-image.png)\n~~~\n"
                "[real](missing-real.md)\n", encoding="utf-8")
            for check in (lambda: release.validate_markdown_links(directory, directory),
                          lambda: self.portal_link_errors(directory)):
                errors = check()
                self.assertEqual(len(errors), 1, errors)
                self.assertIn("missing-real.md", errors[0])
            for locale, guide in self.guides():
                with self.subTest(locale=locale):
                    doc.write_text(guide, encoding="utf-8")
                    for target in ("CLRO_Note_Format.md", "Help_Reference.md", "Read_Only_Viewer.md"):
                        (directory / target).write_text("# Local reference\n", encoding="utf-8")
                    self.assertEqual(release.validate_markdown_links(directory, directory), [])
                    self.assertEqual(self.portal_link_errors(directory), [])

    def portal_link_errors(self, directory):
        errors = []
        portal.validate_local_links(directory, errors)
        return errors


if __name__ == "__main__":
    unittest.main()
