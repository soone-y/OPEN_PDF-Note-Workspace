"""Offline site-gate regressions, also loaded by the ordinary Python tools suite."""

from __future__ import annotations

import importlib.util
import io
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]


def load_module(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, REPO_ROOT / relative)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load required site validator: {relative}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


introduction = load_module("site_test_introduction", "site/cloudflare/scripts/validate_introduction_site.py")
portal = load_module("site_test_portal", "site/github/scripts/validate_public_site.py")
renderer = load_module("site_test_renderer", "site/github/scripts/render_human_docs.py")
builder = load_module("site_test_builder", "site/github/scripts/build_public_site.py")


class SiteValidationRegressionTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary_root = REPO_ROOT / "out" / "site_validation_tests"
        temporary_root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="case-", dir=temporary_root)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.site = self.root / "introduction"
        shutil.copytree(REPO_ROOT / "site/cloudflare/public", self.site)
        self.portal = self.root / "portal"
        self.portal.mkdir()

    def replace(self, relative: str, old: str, new: str) -> None:
        path = self.site / relative
        text = path.read_text(encoding="utf-8-sig")
        self.assertIn(old, text, f"fixture replacement not applied: {relative}")
        path.write_text(text.replace(old, new), encoding="utf-8")

    def add_html(self, text: str) -> None:
        self.replace("index.html", "</body>", text + "</body>")

    def introduction_errors(self, expected: str) -> None:
        errors = introduction.validate_site(self.site)
        self.assertTrue(any(expected in error for error in errors), errors)

    def portal_errors(self, text: str, expected: str) -> None:
        (self.portal / "index.html").write_text(text, encoding="utf-8")
        errors: list[str] = []
        portal.validate_local_links(self.portal, errors)
        self.assertTrue(any(expected in error for error in errors), errors)

    def test_current_introduction_passes_without_network(self) -> None:
        with mock.patch("socket.socket", side_effect=AssertionError("network forbidden")):
            self.assertEqual(introduction.validate_site(self.site), [])

    def test_portal_document_count_tracks_rendered_sources_in_both_languages(self) -> None:
        for relative in ("README.md", "docs/ja/Guide.md", "docs/en/Guide.md"):
            source = self.portal / relative
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("# Guide", encoding="utf-8")
            source.with_suffix(".html").write_text("<body>Guide</body>", encoding="utf-8")
        (self.portal / "unrendered.md").write_text("# Not rendered", encoding="utf-8")
        (self.portal / "asset.json").write_text("{}", encoding="utf-8")
        pages = [self.portal / relative for relative in ("index.html", "en/index.html")]
        for page in pages:
            page.parent.mkdir(parents=True, exist_ok=True)
            page.write_text(builder.DOCUMENT_COUNT_TOKEN, encoding="utf-8")
        builder.replace_document_count_tokens(self.portal)
        for page in pages:
            self.assertEqual(page.read_text(encoding="utf-8"), "3")
        (self.portal / "unrendered.html").write_text("<body>Guide</body>", encoding="utf-8")
        for page in pages:
            page.write_text(builder.DOCUMENT_COUNT_TOKEN, encoding="utf-8")
        builder.replace_document_count_tokens(self.portal)
        for page in pages:
            self.assertEqual(page.read_text(encoding="utf-8"), "4")

    def test_overview_reference_is_in_the_primary_portal_entries(self) -> None:
        for relative, target in (("index.html", "introduction/project_overview.html"),
                                 ("en/index.html", "../introduction/en/project_overview.html")):
            with self.subTest(page=relative):
                text = (REPO_ROOT / "site/github" / relative).read_text(encoding="utf-8")
                entries = re.search(r'<ul class="entry-list">(.*?)</ul>', text, re.DOTALL).group(1)
                self.assertIn(f'href="{target}"', entries)

    def test_documentation_map_stacks_descriptions_and_paths_without_a_table(self) -> None:
        source = REPO_ROOT / "DOCUMENTATION.md"
        target = self.portal / source.name
        shutil.copyfile(source, target)
        rendered = renderer.convert_md_file_to_html(target, self.portal).read_text(encoding="utf-8")
        body = re.search(r'<main[^>]*>(.*?)</main>', rendered, re.DOTALL).group(1)
        headings = re.findall(r'^#### ', source.read_text(encoding="utf-8"), re.MULTILINE)
        self.assertTrue(headings, "documentation entries must have individual headings")
        self.assertEqual(body.count('<h4 '), len(headings))
        self.assertNotIn('<table>', body)
        self.assertNotIn('table-scroll', body)
        self.assertIn('<p>パス / Path: <code>introduction/core/safety_and_nonnegotiables.md</code></p>', body)
        self.assertIn('href="docs/ja/Using_the_App.html"', body)
        self.assertIn('href="docs/en/Using_the_App.html"', body)
        for entry in re.findall(r'^#### [^\n]+\n(.*?)(?=^#{2,4} |\Z)', source.read_text(encoding="utf-8"), re.MULTILINE | re.DOTALL):
            paragraphs = [part.strip() for part in entry.strip().split("\n\n")]
            descriptions = [part for part in paragraphs if not part.startswith("パス / Path:")]
            self.assertGreaterEqual(len(descriptions), 2, "each catalogue entry needs Japanese and English descriptions")
            self.assertEqual(entry.count("パス / Path:"), 1, "each document has its own source path")

    def portal_jump_fixture(self) -> None:
        expected, _ = portal.allowlisted_portal_paths(REPO_ROOT, portal.ALLOWLIST_PATH)
        for relative in expected:
            if relative.suffix.lower() == ".md":
                page = self.portal / relative.with_suffix(".html")
                page.parent.mkdir(parents=True, exist_ok=True)
                page.write_text("<html></html>", encoding="utf-8")
        for relative in ("index.html", "en/index.html"):
            page = self.portal / relative
            page.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO_ROOT / "site/github" / relative, page)

    def test_each_portal_jump_index_links_every_public_document(self) -> None:
        self.portal_jump_fixture()
        errors: list[str] = []
        portal.validate_portal_jump_index(self.portal, REPO_ROOT, portal.ALLOWLIST_PATH, errors)
        self.assertEqual(errors, [])
        for relative in ("index.html", "en/index.html"):
            text = (self.portal / relative).read_text(encoding="utf-8")
            inventory = text.split('id="all-documents"', 1)[1].split('<script', 1)[0]
            self.assertIn('class="jump-groups"', inventory)
            self.assertNotIn('<code>', inventory, "paths belong in the explanatory catalogue")
            self.assertEqual(inventory.count('<details>'), 4, "one disclosure per major category; no nested disclosure")
            for count, group in re.findall(r'<summary>[^<]+\((\d+)\)</summary>(.*?)</details>', inventory, re.DOTALL):
                self.assertEqual(group.count('<li><a '), int(count), "category count must match its actual links")

    def test_jump_index_cannot_rely_on_navigation_or_the_documentation_guide(self) -> None:
        self.portal_jump_fixture()
        for relative, prefix in (("index.html", ""), ("en/index.html", "../")):
            with self.subTest(page=relative):
                page = self.portal / relative
                original = page.read_text(encoding="utf-8")
                link = f'<a href="{prefix}introduction/project_overview.html">'
                before, inventory = original.split('id="all-documents"', 1)
                page.write_text(before + 'id="all-documents"' + inventory.replace(link, '<a href="#all-documents">'), encoding="utf-8")
                errors: list[str] = []
                portal.validate_portal_jump_index(self.portal, REPO_ROOT, portal.ALLOWLIST_PATH, errors)
                self.assertTrue(any(f"{relative} -> introduction/project_overview.html" in error for error in errors), errors)
                page.write_text(original, encoding="utf-8")

    def test_analytics_is_added_once_to_site_html_without_changing_markdown(self) -> None:
        self.metadata_portal_fixture()
        markdown_before = {path: path.read_bytes() for path in self.portal.rglob("*.md")}
        with mock.patch("socket.socket", side_effect=AssertionError("network forbidden")):
            builder.add_web_analytics(self.portal)
        pages = list(self.portal.rglob("*.html"))
        self.assertEqual(len(pages), 4, "Japanese/English entries and generated documents")
        for path in pages:
            with self.subTest(page=path.relative_to(self.portal)):
                text = path.read_text(encoding="utf-8")
                self.assertEqual(text.count("data-cf-beacon="), 1)
                self.assertIn("src='https://static.cloudflareinsights.com/beacon.min.js'", text)
                self.assertIn("type='module'", text)
                config = re.search(r"data-cf-beacon='([^']+)'", text)
                self.assertEqual(json.loads(config.group(1)), {"token": "385b96f498964281bc0c2a771cebd04f"})
                self.assertLess(text.index("<!-- End Cloudflare Web Analytics -->"), text.index("</body>"))
        self.assertEqual({path: path.read_bytes() for path in markdown_before}, markdown_before)
        self.assertNotIn("data-cf-beacon", renderer.HTML_TEMPLATE, "standalone conversion remains offline")

    def test_analytics_rejects_duplicate_beacons_and_missing_body_end(self) -> None:
        page = self.portal / "index.html"
        for text in ("<html></html>", "<body><script data-cf-beacon='{}'></script></body>"):
            with self.subTest(text=text):
                page.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError):
                    builder.add_web_analytics(self.portal)
                self.assertEqual(page.read_text(encoding="utf-8"), text)

    def metadata_portal_fixture(self) -> None:
        for relative in ("index.html", "en/index.html"):
            path = self.portal / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO_ROOT / "site/github" / relative, path)
        image = self.portal / "docs/images/app_overview.png"
        image.parent.mkdir(parents=True)
        shutil.copyfile(REPO_ROOT / "docs/images/app_overview.png", image)
        for locale in ("ja", "en"):
            path = self.portal / "docs" / locale / "MetadataGuide.md"
            path.parent.mkdir(parents=True)
            path.write_text('# Format `.clro`\n\n同梱リポジトリ版: test\n\n*（画面の説明）*\n\nRead "notes" & PDFs.\n\n## First section\n\nMore text.\n\n## Second section\n', encoding="utf-8")
        for path in self.portal.glob("docs/*/*.md"):
            renderer.convert_md_file_to_html(path, self.portal)

    def test_document_metadata_escapes_text_and_pairs_only_existing_translations(self) -> None:
        self.metadata_portal_fixture()
        with mock.patch("socket.socket", side_effect=AssertionError("network forbidden")):
            errors: list[str] = []
            portal.validate_public_metadata(self.portal, errors)
        self.assertEqual(errors, [])
        text = (self.portal / "docs/ja/MetadataGuide.html").read_text(encoding="utf-8")
        parser = portal.HTMLReferenceParser()
        parser.feed(text)
        self.assertEqual(parser.titles, ["Format .clro – PDF Note Workspace"])
        self.assertEqual(parser.metadata["description"], ['Read "notes" & PDFs.'])
        self.assertNotIn("同梱リポジトリ版", parser.metadata["description"][0])
        self.assertIn("&quot;notes&quot; &amp; PDFs", text)
        path = self.portal / "docs/ja/日本語の資料.md"
        path.write_text("# 資料\n\n説明文。", encoding="utf-8")
        result = renderer.convert_md_file_to_html(path, self.portal).read_text(encoding="utf-8")
        parser = portal.HTMLReferenceParser()
        parser.feed(result)
        self.assertFalse(parser.alternates)
        self.assertIn("%E6%97%A5", parser.canonicals[0])
        self.assertEqual(path.read_text(encoding="utf-8"), "# 資料\n\n説明文。")

    def test_readme_entry_links_prefer_html_and_keep_github_and_local_docs(self) -> None:
        readme = (REPO_ROOT / "README.md").read_text(encoding="utf-8-sig")
        links = re.findall(r'\[([^\]]+)\]\(([^)]+)\)', readme.split('\n\n', 2)[1])
        self.assertEqual(links, [
            ('紹介サイト / Overview', 'https://pdf-note-workspace.soone-y.com/'),
            ('詳細文書・使い方 / Documentation', 'https://soone-y.github.io/OPEN_PDF-Note-Workspace/'),
            ('ダウンロード / Releases', 'https://github.com/soone-y/OPEN_PDF-Note-Workspace/releases'),
        ])
        source = self.portal / "README.md"
        source.write_text(readme, encoding="utf-8")
        rendered = renderer.convert_md_file_to_html(source, self.portal).read_text(encoding="utf-8")
        for locale, label in (('ja', '日本語の文書'), ('en', 'English documentation')):
            pages = f'https://soone-y.github.io/OPEN_PDF-Note-Workspace/docs/{locale}/README.html'
            github = f'https://github.com/soone-y/OPEN_PDF-Note-Workspace/blob/main/docs/{locale}/README.md'
            self.assertIn(f'[{label}]({pages})', readme)
            self.assertIn(github, readme)
            self.assertIn(f'](docs/{locale}/README.md)', readme, 'keep the existing local/package entry')
            self.assertIn(f'href="{pages}"', rendered)
            self.assertIn(f'href="{github}"', rendered, 'GitHub fallback must not be rewritten to HTML')
            self.assertIn(f'href="docs/{locale}/README.html"', rendered)
        parser = portal.HTMLReferenceParser()
        parser.feed(rendered)
        self.assertTrue(parser.metadata['description'][0].startswith('PDFの表示・注釈の追加と、'))
        self.assertNotIn('Overview', parser.metadata['description'][0])

    def test_metadata_skips_link_only_navigation_but_keeps_linked_prose(self) -> None:
        parser = renderer.DocumentSummaryParser()
        parser.feed('<h1>Guide</h1><p><a href="/">Overview</a> · <a href="/docs/">Docs</a></p>'
                    '<p><a href="/ja/">日本語</a>（<a href="/github/">GitHubで読む</a>）</p>'
                    '<p>Read <a href="/notes/">notes</a> beside a PDF.</p>')
        self.assertEqual(parser.paragraphs, ['Read notes beside a PDF.'])

    def test_gfm_callouts_are_single_notes_without_source_markers(self) -> None:
        rendered = renderer.simple_markdown_to_html(
            "# Guide\n\n> [!NOTE]\n> The PDF stays unchanged.\n> Choose a separate output file.\n\n> [!WARNING]\n> Review the output before sharing.\n\nAfter the notes.",
            "",
        )
        self.assertEqual(rendered.count('<aside class="doc-note" role="note">'), 2)
        self.assertNotIn("[!NOTE]", rendered)
        self.assertNotIn("[!WARNING]", rendered)
        self.assertIn("<p>The PDF stays unchanged.</p>", rendered)
        self.assertIn("<p>Choose a separate output file.</p>", rendered)
        self.assertIn("<p>Review the output before sharing.</p>", rendered)
        self.assertIn("<p>After the notes.</p>", rendered)

    def test_gfm_callout_inside_fenced_code_stays_literal(self) -> None:
        rendered = renderer.simple_markdown_to_html("```text\n> [!NOTE]\n> Literal example\n```", "")
        self.assertNotIn('<aside class="doc-note" role="note">', rendered)
        self.assertIn("&gt; [!NOTE]", rendered)

    def test_portal_metadata_missing_duplicate_inconsistent_or_wrong_translation_is_rejected(self) -> None:
        self.metadata_portal_fixture()
        path = self.portal / "index.html"
        original = path.read_text(encoding="utf-8")
        canonical = '<link rel="canonical" href="https://soone-y.github.io/OPEN_PDF-Note-Workspace/">'
        description = re.search(r'  <meta name="description"[^>]*>', original).group(0)
        for old, new, expected in (
            (canonical, "", "canonical"),
            (canonical, canonical + canonical, "canonical"),
            (description, "", "description"),
            (description, description + description, "description"),
            ('property="og:title"', 'property="og:wrong"', "og:title"),
            ('property="og:description"', 'property="og:wrong"', "og:description"),
            ('hreflang="en"', 'hreflang="ja"', "hreflang"),
            ('href="https://soone-y.github.io/OPEN_PDF-Note-Workspace/en/"', 'href="https://example.org/en/"', "hreflang target"),
        ):
            with self.subTest(expected=expected, new=new):
                path.write_text(original.replace(old, new), encoding="utf-8")
                errors: list[str] = []
                portal.validate_public_metadata(self.portal, errors)
                self.assertTrue(any(expected in error for error in errors), errors)
        path.write_text(original, encoding="utf-8")
        translated = self.portal / "en/index.html"
        translated.write_text(translated.read_text(encoding="utf-8").replace('hreflang="ja"', 'hreflang="fr"'), encoding="utf-8")
        errors = []
        portal.validate_public_metadata(self.portal, errors)
        self.assertTrue(any("reciprocal" in error for error in errors), errors)

    def test_paragraph_reading_width_is_assigned_by_role(self) -> None:
        html = (self.site / "index.html").read_text(encoding="utf-8-sig")
        paragraph_rule = re.search(r"(?m)^p\s*\{([^}]+)\}", html)
        self.assertIsNotNone(paragraph_rule)
        self.assertNotIn("max-width", paragraph_rule.group(1))
        self.assertRegex(html, r"\.prose,\.technical > p\s*\{[^}]*max-width:")
        self.assertIn('<p class="prose">', html)
        reader = re.search(r'<section id="reader">(.*?)</section>', html, re.DOTALL)
        self.assertIsNotNone(reader)
        self.assertNotIn('class="prose"', reader.group(1))

    def test_complete_feature_catalogue_is_visible_without_script(self) -> None:
        html = (self.site / "index.html").read_text(encoding="utf-8-sig")
        parser = introduction.IntroductionHTMLParser()
        parser.feed(html)
        self.assertIn("#functions", parser.anchors)
        self.assertNotIn("pdf-output", parser.ids)
        self.assertLess(parser.ids.index("functions"), parser.ids.index("files"))
        section = re.search(r'<section id="functions">(.*?)</section>', html, re.DOTALL)
        self.assertIsNotNone(section)
        self.assertNotIn('<ol class="workflow">', section.group(1))
        self.assertLess(html.index('src="assets/app_overview.png"'), html.index('id="fit"'))
        self.assertNotIn('href="assets/app_overview.png"', html)
        self.assertNotIn("screen-link", html)
        features = re.findall(r'<details class="feature-entry">(.*?)</details>', section.group(1), re.DOTALL)
        expected = {
            "重要な文章にマーカーや下線を付ける", "図や余白へ手書きや図形を加える",
            "PDFへ文字で説明を書き足す", "資料へ数式を書き足す", "PDFとノートを並べて使う",
            "資料とノートをリンクで結ぶ", "元のPDFと注釈を別々に保存する",
            "書き込みを含めてPDFを書き出す", "必要なページだけを取り出す", "PDFの出力を結合する",
            "PDFの1ページを画像にする", "資料と自分の記録から検索する", "授業や回次ごとに資料をまとめる",
            "装飾や数式を含むノートを書く", "ノートを別の形式で書き出す", "Office文書や画像をPDFにする",
            "続きの位置から読み直す", "軽量な閲覧専用ソフトで読む",
        }
        titles = [re.search(r'<summary>(.*?)</summary>', feature, re.DOTALL).group(1) for feature in features]
        self.assertEqual(set(titles), expected)
        self.assertEqual(len(titles), len(expected), "features must not be duplicated or lost")
        for feature in features:
            self.assertRegex(feature, r'<p>\S')
        self.assertIn("PDFの標準注釈として保存", section.group(1))
        self.assertIn("ノート・PDF・注釈は、それぞれ別のファイルとして保存します", html)
        self.assertIn("関連するPDFとノートは一緒に管理してください", html)
        self.assertIn("元のPDFを直接書き換える処理ではありません", section.group(1))

    def test_pdf_merge_description_is_scoped_to_reserved_outputs(self) -> None:
        html = (self.site / "index.html").read_text(encoding="utf-8-sig")
        features = re.findall(r'<details class="feature-entry">(.*?)</details>', html, re.DOTALL)
        merged = [feature for feature in features if "<summary>PDFの出力を結合する</summary>" in feature]
        self.assertEqual(len(merged), 1)
        for phrase in ("注釈PDF", "ページ指定PDF", "2件以上予約", "指定した順序", "一つのPDF", "倍率・注釈の設定を保って結合"):
            self.assertIn(phrase, merged[0])
        parser = introduction.IntroductionHTMLParser()
        parser.feed(html)
        app = next(json.loads(value) for value in parser.json_ld if json.loads(value).get("@type") == "SoftwareApplication")
        self.assertIn("PDF出力の結合", app["featureList"])

    def test_pdf_export_copy_describes_formats_not_temporary_defects(self) -> None:
        html = (self.site / "index.html").read_text(encoding="utf-8-sig")
        section = re.search(r'<section id="functions">(.*?)</section>', html, re.DOTALL)
        self.assertIsNotNone(section)
        text = section.group(1)
        for phrase in ("設定やフォント", "Stamp注釈", "リンクID", "元のPDFと<code>.clrop</code>"):
            self.assertIn(phrase, text)
        self.assertNotIn("文字色変更のPDF出力には対応していません", text)
        self.assertNotIn("出力倍率は1倍に限られます", text)

    def test_export_documentation_distinguishes_save_from_output(self) -> None:
        ja = (REPO_ROOT / "docs/ja/Using_the_App.md").read_text(encoding="utf-8-sig")
        en = (REPO_ROOT / "docs/en/Using_the_App.md").read_text(encoding="utf-8-sig")
        for phrase in ("ノートファイルや `.clrop` は更新", "元のPDFは変更しません", "上書きするか確認"):
            self.assertIn(phrase, ja)
        for phrase in ("updates the note files and `.clrop`", "does not change the original PDF", "asks whether to overwrite"):
            self.assertIn(phrase, en)
        self.assertNotIn("既にあるファイル名には出力できない", ja)
        for locale in ("ja", "en"):
            for name in ("Using_the_App.md", "Help_Reference.md"):
                text = (REPO_ROOT / "docs" / locale / name).read_text(encoding="utf-8-sig")
                for obsolete in ("倍率を変えずに1倍", "PDF出力の倍率を1倍", "export at 1x", "exported at 1x only",
                                 "文字色変更注釈を含む PDF は", "Text recoloring remains unsupported"):
                    self.assertNotIn(obsolete, text)

    def test_feature_catalogue_uses_native_disclosure_without_carousel(self) -> None:
        html = (self.site / "index.html").read_text(encoding="utf-8-sig")
        groups = re.findall(r'<div class="feature-group">(.*?)</ul>\s*</div>', html, re.DOTALL)
        self.assertEqual(len(groups), 3)
        for group in groups:
            self.assertRegex(group, r'<h3>\S.*?</h3>')
            self.assertIn('<ul class="feature-list">', group)
            self.assertIn('<details class="feature-entry">', group)
            self.assertNotIn('<details class="feature-group">', group)
            self.assertNotIn('aria-hidden="true"', group)
        self.assertNotIn('feature-carousel-script', html)
        self.assertNotIn('feature-viewport', html)

    def test_canonical_missing_wrong_or_duplicate_is_rejected(self) -> None:
        old = '<link href="https://pdf-note-workspace.soone-y.com/" rel="canonical"/>'
        original = (self.site / "index.html").read_text(encoding="utf-8-sig")
        for replacement in ("", old + old, old.replace(".soone-y.com/", ".example/")):
            with self.subTest(replacement=replacement):
                (self.site / "index.html").write_text(original, encoding="utf-8")
                self.replace("index.html", old, replacement)
                self.introduction_errors("exactly one canonical")

    def test_meta_indexing_blocks_are_rejected(self) -> None:
        for name, value in (("robots", "NOINDEX, follow"), ("googlebot", "none")):
            with self.subTest(name=name):
                self.replace("index.html", "</head>", f'<meta name="{name}" content="{value}"></head>')
                self.introduction_errors("must not block indexing")

    def test_googlebot_robots_denial_is_rejected(self) -> None:
        self.replace("robots.txt", "Allow: /", "Disallow: /")
        self.introduction_errors("must allow Googlebot")

    def test_commented_sitemap_and_non_link_destinations_are_rejected(self) -> None:
        self.replace("robots.txt", "Sitemap:", "# Sitemap:")
        self.introduction_errors("must declare the canonical sitemap")
        self.replace("index.html", 'href="https://soone-y.github.io/OPEN_PDF-Note-Workspace/"',
                     'data-destination="https://soone-y.github.io/OPEN_PDF-Note-Workspace/"')
        self.introduction_errors("required public destination")

    def test_specific_googlebot_denial_is_rejected(self) -> None:
        path = self.site / "robots.txt"
        path.write_text(path.read_text(encoding="utf-8") + "\nUser-agent: Googlebot\nDisallow: /\n", encoding="utf-8")
        self.introduction_errors("must allow Googlebot")

    def test_training_bot_denial_does_not_block_search(self) -> None:
        content = (self.site / "robots.txt").read_text(encoding="utf-8")
        self.assertIn("User-agent: Google-Extended\nDisallow: /", content)
        self.assertTrue(introduction.googlebot_homepage_allowed(content))
        self.assertEqual(introduction.validate_site(self.site), [])

    def test_googlebot_group_merging_precedence_and_wildcards(self) -> None:
        for content, expected in (
            ("User-agent: *\nDisallow: /\nAllow: /", True),
            ("User-agent: *\nDisallow: /\nAllow: /$", True),
            ("User-agent: *\nAllow: /\nDisallow: /$", False),
            ("User-agent: *\nDisallow: /*", False),
            ("User-agent: *\nDisallow: /*$", False),
            ("User-agent: *\nDisallow: /private/", True),
            ("User-agent: Googlebot\nDisallow: /\nUser-agent: *\nAllow: /", False),
            ("User-agent: Googlebot\nAllow: /\nUser-agent: Googlebot\nDisallow: /$", False),
            ("User-agent: Googlebot\nDisallow: /\nUser-agent: Googlebot/1.2\nAllow: /$", True),
            ("User-agent: Googlebot-News\nDisallow: /\nUser-agent: Google-Extended\nDisallow: /", True),
            ("User-agent: Googlebot\nUser-agent: Otherbot\nDisallow: / # comment", False),
            ("User-agent: Googlebot\nSitemap: https://example.org/sitemap.xml\nUser-agent: Otherbot\nDisallow: /", False),
            ("User-agent: Googlebot*\nDisallow: /", False),
            ("User-agent: Googlebot\nDisallow:", True),
        ):
            with self.subTest(content=content):
                self.assertEqual(introduction.googlebot_homepage_allowed(content), expected)

    def test_x_robots_header_denial_is_rejected(self) -> None:
        self.replace("_headers", "/*\n", "/*\n  X-Robots-Tag: googlebot: noindex\n")
        self.introduction_errors("X-Robots-Tag")

    def test_header_blocks_use_cloudflare_syntax_not_comment_delimiters(self) -> None:
        valid = "/*\n  Content-Signal: " + introduction.CONTENT_SIGNAL + "\n"
        for content, expected in (
            (valid + "*/\n", "invalid _headers URL rule"),
            (valid.replace("  Content-Signal", "Content-Signal"), "invalid _headers URL rule"),
            (valid.replace("/*", "/assets/*"), "policy under /*"),
            ("  Content-Signal: " + introduction.CONTENT_SIGNAL, "invalid _headers header line"),
        ):
            with self.subTest(expected=expected):
                (self.site / "_headers").write_text(content, encoding="utf-8")
                self.introduction_errors(expected)
        (self.site / "_headers").write_text("# comment\n" + valid, encoding="utf-8")
        self.assertEqual(introduction.validate_site(self.site), [])

    def test_malformed_sitemap_returns_failure_not_traceback(self) -> None:
        (self.site / "sitemap.xml").write_text("<urlset><", encoding="utf-8")
        result = subprocess.run(
            [sys.executable, str(REPO_ROOT / "site/cloudflare/scripts/validate_introduction_site.py"),
             "--site", str(self.site)], capture_output=True, text=True, encoding="utf-8", timeout=20,
        )
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("invalid XML", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_sitemap_wrong_namespace_duplicate_and_wrong_loc_are_rejected(self) -> None:
        path = self.site / "sitemap.xml"
        original = path.read_text(encoding="utf-8")
        variants = (
            (original.replace(' xmlns="http://www.sitemaps.org/schemas/sitemap/0.9"', ""), "namespace"),
            (original.replace("</urlset>", original[original.index("  <url>"):original.index("</urlset>")] + "</urlset>"), "only one canonical"),
            (original.replace(introduction.CANONICAL_URL, "https://example.org/"), "only one canonical"),
            (original.replace("</loc>", "</loc><loc>https://example.org/</loc>"), "exactly one loc"),
        )
        for content, expected in variants:
            with self.subTest(expected=expected):
                path.write_text(content, encoding="utf-8")
                self.introduction_errors(expected)

    def test_sitemap_dates_are_validated_not_hardcoded(self) -> None:
        path = self.site / "sitemap.xml"
        original = path.read_text(encoding="utf-8")
        for value in ("2020-01-02", "2020-01-02T03:04:05Z", "2020-01-02T03:04:05+09:00"):
            with self.subTest(value=value):
                path.write_text(re.sub(r"<lastmod>.*?</lastmod>", f"<lastmod>{value}</lastmod>", original), encoding="utf-8")
                self.assertEqual(introduction.validate_site(self.site), [])
        for value in ("2020-02-30", "yesterday", "2020-01-02T03:04:05"):
            with self.subTest(value=value):
                path.write_text(re.sub(r"<lastmod>.*?</lastmod>", f"<lastmod>{value}</lastmod>", original), encoding="utf-8")
                self.introduction_errors("lastmod must be a valid date")
        path.write_text(re.sub(r"\s*<lastmod>.*?</lastmod>", "", original), encoding="utf-8")
        self.introduction_errors("lastmod must be a valid date")

    def test_invalid_json_ld_is_rejected(self) -> None:
        self.replace("index.html", '"featureList": [', '"featureList": invalid [')
        self.introduction_errors("invalid JSON-LD")

    def test_json_ld_fields_and_non_json_constants_are_rejected(self) -> None:
        original = (self.site / "index.html").read_text(encoding="utf-8-sig")
        features = re.search(r'"featureList": \[[^\]]*\]', original)
        self.assertIsNotNone(features)
        for old, new, expected in (
            (features.group(0), '"featureList": []', "featureList"),
            ('"operatingSystem": "Windows x64 (64-bit)"', '"operatingSystem": "macOS"', "must declare Windows"),
            ('"url": "https://pdf-note-workspace.soone-y.com/"', '"url": "https://example.org/"', "must match the canonical"),
            ('"@context": "https://schema.org"', '"@context": "wrong"', "schema.org context"),
            ('"name": "PDF Note Workspace"', '"name": "Another app"', "name and description"),
            ('"inLanguage": "ja"', '"inLanguage": "en"', "language and image"),
            ('"@type": "WebSite"', '"@type": "WebSite", "invalid": NaN', "invalid JSON-LD"),
        ):
            with self.subTest(expected=expected):
                (self.site / "index.html").write_text(original, encoding="utf-8")
                self.replace("index.html", old, new)
                self.introduction_errors(expected)

    def test_json_ld_graph_and_formatting_are_supported(self) -> None:
        parser = introduction.IntroductionHTMLParser()
        parser.feed((self.site / "index.html").read_text(encoding="utf-8-sig"))
        objects = [json.loads(value) for value in parser.json_ld]
        parser.json_ld = [json.dumps({"@context": "https://schema.org", "@graph": objects}, indent=2)]
        errors: list[str] = []
        introduction.validate_structured_data(parser, errors)
        self.assertEqual(errors, [])

    def test_missing_viewport_language_heading_or_alt_is_rejected(self) -> None:
        original = (self.site / "index.html").read_text(encoding="utf-8-sig")
        for old, new, expected in (
            ("width=device-width", "width=1280", "device-width viewport"),
            ('lang="ja"', 'lang="en"', "lang=ja"),
            ("<h1>", "<h2>", "exactly one h1"),
        ):
            with self.subTest(expected=expected):
                (self.site / "index.html").write_text(original, encoding="utf-8")
                self.replace("index.html", old, new)
                self.introduction_errors(expected)
        (self.site / "index.html").write_text(original, encoding="utf-8")
        self.add_html('<img src="assets/app_overview.png">')
        self.introduction_errors("alt attributes")

    def test_introduction_fragment_ids_and_control_targets(self) -> None:
        self.add_html('<div id="test-target"></div><div id="test-target"></div>'
                      '<a href="#missing-target">broken</a><button aria-controls="missing-panel">open</button>')
        self.introduction_errors("duplicate or empty IDs")
        self.introduction_errors("broken HTML fragment")
        self.introduction_errors("broken HTML control")

    def test_introduction_queries_percent_encoding_and_escaped_code(self) -> None:
        self.add_html('<div id="日本語"></div><a href="index.html?mode=test&amp;x=1#%E6%97%A5%E6%9C%AC%E8%AA%9E">link</a>'
                      '<img src="assets/app_overview.png?v=1" alt="">'
                      '<code>&lt;a href="missing.html#missing"&gt;</code>')
        self.assertEqual(introduction.validate_site(self.site), [])

    def test_introduction_encoded_escape_and_unsafe_schemes(self) -> None:
        self.add_html('<a href="%2e%2e/private.html">escape</a><a href="javascript://example.org/">unsafe</a>')
        self.introduction_errors("escapes introduction site")
        self.introduction_errors("unsupported HTML URL scheme")

    def test_malformed_urls_are_reported_by_both_validators(self) -> None:
        self.add_html('<a href="http://[">invalid</a>')
        self.introduction_errors("invalid HTML URL")
        self.portal_errors('<a href="http://[">invalid</a>', "invalid HTML URL")

    def test_portal_fragments_queries_and_escaped_examples(self) -> None:
        (self.portal / "target.html").write_text('<h2 id="日本語">Target</h2>', encoding="utf-8")
        (self.portal / "index.html").write_text(
            '<div id="local"></div><a href="#local">local</a>'
            '<a href="target.html?x=1&amp;y=2#%E6%97%A5%E6%9C%AC%E8%AA%9E">target</a>'
            '<code>&lt;a href="not-a-link.html"&gt;</code>'
            '<a href="//example.org/">external</a>', encoding="utf-8",
        )
        errors: list[str] = []
        portal.validate_local_links(self.portal, errors)
        self.assertEqual(errors, [])

    def test_portal_missing_cross_document_fragment_is_rejected(self) -> None:
        (self.portal / "target.html").write_text('<h2 id="present">Target</h2>', encoding="utf-8")
        self.portal_errors('<a href="target.html#missing">broken</a>', "broken HTML fragment")

    def test_portal_missing_same_document_fragment_is_rejected(self) -> None:
        self.portal_errors('<a href="#missing">broken</a>', "broken HTML fragment")

    def test_portal_duplicate_ids_controls_alt_and_noindex_are_rejected(self) -> None:
        for text, expected in (
            ('<div id="duplicate"></div><div id="duplicate"></div>', "duplicate or empty HTML IDs"),
            ('<button aria-controls="missing">open</button>', "broken HTML control"),
            ('<img src="index.html">', "alt attributes"),
            ('<meta name="googlebot" content="NONE">', "must not block indexing"),
        ):
            with self.subTest(expected=expected):
                self.portal_errors(text, expected)

    def test_portal_encoded_escape_missing_asset_and_unsafe_scheme_are_rejected(self) -> None:
        for text, expected in (
            ('<a href="%2e%2e/private.html">escape</a>', "escapes public site"),
            ('<a href="javascript:alert(1)">unsafe</a>', "unsupported HTML URL scheme"),
            ('<a href="javascript://example.org/">unsafe</a>', "unsupported HTML URL scheme"),
            ('<img src="missing.png" alt="image">', "broken HTML link or asset"),
        ):
            with self.subTest(expected=expected):
                self.portal_errors(text, expected)

    def test_invalid_utf8_is_not_silently_skipped(self) -> None:
        (self.site / "index.html").write_bytes(b"\xff")
        self.introduction_errors("invalid UTF-8")
        (self.portal / "index.html").write_bytes(b"\xff")
        errors = portal.validate_site(self.portal)
        self.assertTrue(any("invalid UTF-8" in error for error in errors), errors)
        self.assertTrue(any("could not complete" in error for error in errors), errors)

    def test_missing_inputs_return_nonzero_exit_codes(self) -> None:
        for validator in (introduction, portal):
            with self.subTest(validator=validator.__name__), redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
                self.assertEqual(validator.main(["--site", str(self.root / "missing")]), 1)

    def test_document_outline_preserves_setup_numbers_without_adding_list_numbers(self) -> None:
        markdown = (REPO_ROOT / "docs/ja/Getting_Started.md").read_text(encoding="utf-8")
        body = renderer.simple_markdown_to_html(markdown, "")
        outline = renderer.document_outline_html(body, "ja")
        links = re.findall(r'<li><a href="#([^"]+)">([^<]+)</a></li>', outline)
        self.assertIn('<ul role="list">', outline)
        self.assertNotIn('<ol', outline)
        self.assertEqual([label for _, label in links], re.findall(r'^## (.+)$', markdown, re.MULTILINE))
        self.assertEqual([label.split('. ', 1)[0] for _, label in links], [str(n) for n in range(8)])
        for identifier, _ in links:
            self.assertIn(f'id="{identifier}"', body)
        css = re.search(r'\.document-outline ul\s*\{\{([^}]+)', renderer.HTML_TEMPLATE)
        self.assertIsNotNone(css)
        self.assertIn('list-style: none', css.group(1))
        self.assertNotIn('display: flex', css.group(1))
        self.assertIn('.document-outline a {{ display: block;', renderer.HTML_TEMPLATE)

    def test_document_outline_does_not_number_unnumbered_headings(self) -> None:
        for locale, title, labels in (
            ('ja', 'この文書の項目', ['資料を開く', '記録を探す']),
            ('en', 'On this page', ['Open files', 'Find notes']),
        ):
            with self.subTest(locale=locale):
                body = renderer.simple_markdown_to_html('# Guide\n\n' + '\n\n'.join('## ' + label for label in labels), '')
                outline = renderer.document_outline_html(body, locale)
                self.assertIn(f'<summary>{title}</summary><ul role="list">', outline)
                self.assertNotIn('<ol', outline)
                self.assertEqual(re.findall(r'<li><a href="#[^"]+">([^<]+)</a></li>', outline), labels)

    def test_japanese_heading_punctuation_matches_markdown_link(self) -> None:
        document = self.portal / "docs/ja/Troubleshooting.md"
        document.parent.mkdir(parents=True)
        document.write_text("# 困ったとき\n\n## ログ・生成物の容量を確認する\n", encoding="utf-8")
        with redirect_stdout(io.StringIO()):
            self.assertEqual(renderer.main([str(self.portal)]), 0)
        rendered = document.with_suffix(".html").read_text(encoding="utf-8")
        self.assertIn('id="ログ生成物の容量を確認する"', rendered)

    def test_statistics_history_is_rendered_and_linked_from_summary(self) -> None:
        history = self.portal / "docs/statistics/HISTORY.md"
        history.parent.mkdir(parents=True)
        history.write_text("# Statistics history\n", encoding="utf-8")
        (self.portal / "STATISTICS.md").write_text("# Statistics\n\n[History](docs/statistics/HISTORY.md)\n", encoding="utf-8")
        with redirect_stdout(io.StringIO()):
            self.assertEqual(renderer.main([str(self.portal)]), 0)
        self.assertTrue(history.with_suffix(".html").is_file())
        self.assertIn('docs/statistics/HISTORY.html', (self.portal / "STATISTICS.html").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
