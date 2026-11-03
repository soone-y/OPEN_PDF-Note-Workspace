"""Static contracts for the introduction's independent feature disclosures."""

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


class FeatureDisclosureTests(unittest.TestCase):
    def test_hero_has_direct_documentation_portal_link(self) -> None:
        html = (REPO_ROOT / "site/cloudflare/public/index.html").read_text(encoding="utf-8-sig")
        actions = html.split('<div class="actions">', 1)[1].split('</div>', 1)[0]
        links = re.findall(r'<a\b[^>]*href="([^"]+)"[^>]*>(.*?)</a>', actions)
        self.assertEqual([href for href, _ in links], [
            '#start', '#fit', 'https://soone-y.github.io/OPEN_PDF-Note-Workspace/',
        ])
        self.assertEqual(links[-1][1], '詳細文書を読む')

    def test_columns_and_bulk_controls_preserve_native_fallback(self) -> None:
        html = (REPO_ROOT / "site/cloudflare/public/index.html").read_text(encoding="utf-8-sig")
        self.assertEqual(html.count('<div class="feature-columns">'), 3)
        self.assertEqual(html.count('<ul class="feature-list">'), 6)
        list_rule = re.search(r"(?m)^\.feature-list\s*\{([^}]+)\}", html)
        self.assertIsNotNone(list_rule)
        self.assertIn("display:block", list_rule.group(1))
        self.assertNotIn("grid-template", list_rule.group(1))
        self.assertRegex(html, r'<div id="feature-controls"[^>]*hidden>')
        self.assertRegex(html, r'<button id="features-toggle"[^>]*type="button"[^>]*aria-controls="feature-catalogue"[^>]*aria-expanded="false"')
        self.assertIn('class="disclosure-icon" aria-hidden="true"', html)
        self.assertNotIn('id="features-expand"', html)
        self.assertNotIn('id="features-collapse"', html)
        self.assertIn('<div id="feature-catalogue">', html)
        self.assertIn('<script id="feature-disclosure-script">', html)

    def test_interaction_colors_match_on_both_sites(self) -> None:
        sources = [
            "site/cloudflare/public/index.html",
            "site/github/index.html",
            "site/github/en/index.html",
            "site/github/scripts/render_human_docs.py",
        ]
        rules = []
        for source in sources:
            text = (REPO_ROOT / source).read_text(encoding="utf-8-sig")
            if source.endswith(".py"):
                text = text.replace("{{", "{").replace("}}", "}")
            css = text.split("/* Shared interaction colors:", 1)[1].split("</style>", 1)[0].strip()
            rules.append(css)
            self.assertIn('--external-link:#1d50dd', css)
            self.assertIn('--opened:#a16635', css)
            self.assertIn('details[data-opened="true"] > summary', css)
            self.assertNotIn('details[open] > summary', css)
            self.assertIn('summary { color:var(--opened); }', css)
            self.assertNotIn('background:var(--opened)', css)
            self.assertNotIn('--opened-ink', css)
            self.assertIn('html[data-contrast="high"] { --action:#00573f; --opened:#9a4e00; }', css)
            self.assertIn('a[data-link-kind="internal"]', css)
        self.assertTrue(all(rule == rules[0] for rule in rules))
        renderer = (REPO_ROOT / sources[-1]).read_text(encoding="utf-8")
        current_rule = re.search(r'\.site-menu nav a\[aria-current="page"\]\s*\{\{([^}]+)', renderer)
        self.assertIsNotNone(current_rule)
        self.assertIn('color: var(--destination)', current_rule.group(1))


if __name__ == "__main__":
    unittest.main()
