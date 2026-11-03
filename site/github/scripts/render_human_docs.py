#!/usr/bin/env python3
"""
render_human_docs.py

公開サイト成果物 (site/github/output/public/) 内のMarkdown文書から、
本文・文書ナビゲーション・ページ別メタ情報を持つHTMLページを生成します。

- 元のMarkdownは変更せず、同じ公開先に残します。
- 生成処理は外部へ通信せず、HTMLは外部CDNやWebフォントを使いません。
"""

import sys
import re
import html
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import quote

PUBLIC_BASE_URL = "https://soone-y.github.io/OPEN_PDF-Note-Workspace/"
# Task guides and code-only reference material need a purpose summary rather
# than a build marker, a language-switch sentence, or a single isolated step.
DOCUMENT_DESCRIPTIONS = {
    "docs/ja/Using_the_App.md": "PDF Note Workspaceで資料を開き、PDFに書き込み、ノートを編集する手順。検索、PDF・画像・ノートの出力、保存と復元の案内を確認できます。",
    "docs/en/Using_the_App.md": "Learn how to open materials, annotate PDFs, edit and search notes, and export PDFs, images, and notes in PDF Note Workspace. Includes saving and recovery guidance.",
    "docs/ja/Help_Reference.md": "PDF Note Workspaceの操作・設定を調べる詳細ヘルプ。資料の閲覧、注釈、ノート、検索、保存、出力、キーボード操作を項目別に確認できます。",
    "docs/en/Help_Reference.md": "Detailed help for PDF Note Workspace: viewing materials, annotations, notes, search, saving, export, and keyboard controls, organized by task and setting.",
    "docs/en/Getting_Started.md": "Set up PDF Note Workspace, open the bundled sample workspace, and choose the Standard or Lite edition. Explains application startup, workspace files, and local conversion.",
    "introduction/core/features_and_scenarios.md": "PDF Note Workspaceの主な機能と想定ケースを整理した背景資料。ファイル形式、PC内での処理、音に関する方針と確認事項を、利用者向けの手順と確認根拠への参照とともに掲載しています。",
    "introduction/en/features_and_scenarios.md": "Reference material on PDF Note Workspace features and usage scenarios, including file compatibility, local processing, sound policy, and links to user guides and verification sources.",
    "introduction/core/file_formats.md": "PDF、注釈データ、ノートの役割と保存構造を説明する背景資料。ファイルの組み合わせ、作業データと保存先の境界、利用者向け文書への参照を確認できます。",
    "introduction/en/file_formats.md": "Reference material on PDF, annotation, and note file roles, paired files, working data, and persistence paths in PDF Note Workspace.",
    "introduction/core/ui_concepts.md": "ワークスペース、PDF表示領域、ノート領域、注釈ツールの役割と操作場所を整理した背景資料。画面の用語と利用者向け手順の対応を確認できます。",
    "introduction/core/use_cases.md": "講義資料への記録、論文の精読、文書レビューを想定したPDF Note Workspaceの利用場面と設計の考え方。資料・注釈・ノートの役割と参照先を整理しています。",
    "introduction/core/troubleshooting_quick.md": "PDFの読み込み、Office変換、保存、警告音の問題を調べる確認観点。原因の候補、データを守る対応順序、正本の利用者向け文書への参照を整理しています。",
}

# HTMLテンプレート（ローカルのスタイルと高コントラスト切替）
HTML_TEMPLATE = """<!DOCTYPE html>
<html lang="{language_code}">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>{search_title}</title>
{metadata_html}
  <style>
    :root {{
      --bg-main: #fbfcfa;
      --card-bg: #ffffff;
      --text-main: #24312f;
      --text-muted: #52635b;
      --accent: #00AA7B;
      --link: #1D50DD;
      --accent-hover: #08745b;
      --border-color: #d4deda;
      --code-bg: #f1f5f3;
      --pre-bg: #24312f;
      --pre-text: #f8fbfa;
      --note-bg: #fff4ea;
      --note-border: #FFBD85;
    }}

    @media (prefers-color-scheme: dark) {{
      :root {{
        --bg-main: #18201f;
        --card-bg: #202b29;
        --text-main: #f2f7f5;
        --text-muted: #b5c5c0;
        --accent: #42cda5;
        --link: #9eb2ff;
        --accent-hover: #7fe0c3;
        --border-color: #3a4b47;
        --code-bg: #17211f;
        --pre-bg: #101817;
        --pre-text: #f2f7f5;
        --note-bg: #3b3027;
        --note-border: #ffc99b;
      }}
    }}

    * {{ box-sizing: border-box; margin: 0; padding: 0; }}

    body {{
      font-family: "Yu Gothic UI", Meiryo, system-ui, sans-serif;
      background-color: var(--bg-main);
      color: var(--text-main);
      line-height: 1.75;
      overflow-wrap: anywhere;
      padding: 24px 12px;
    }}

    .container {{
      max-width: 880px;
      margin: 0 auto;
      padding: 36px 28px;
    }}

    main {{ max-width: 42em; overflow-wrap: anywhere; }}
    .skip-link {{ position: absolute; top: 8px; left: 24px; z-index: 2; padding: 8px 16px; background: var(--card-bg); transform: translateY(-150%); }}
    .skip-link:focus {{ transform: none; }}

    .doc-header {{
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      padding-bottom: 14px;
      margin-bottom: 24px;
      border-bottom: 2px solid var(--accent);
      flex-wrap: wrap;
    }}

    .site-menu {{ position: relative; min-width: 0; max-width: 100%; }}

    .site-menu summary {{
      display: inline-flex;
      align-items: center;
      max-width: 100%;
      cursor: pointer;
      list-style: none;
      padding: 6px 12px;
      background-color: var(--code-bg);
      border: 1px solid var(--border-color);
      border-radius: 6px;
      color: var(--text-main);
      font-size: 0.88em;
      font-weight: 600;
    }}

    .site-menu summary::-webkit-details-marker {{ display: none; }}
    .menu-icon {{
      display: inline-flex;
      flex-direction: column;
      justify-content: space-between;
      width: 14px;
      height: 12px;
      margin-right: 7px;
    }}
    .menu-icon-bar {{
      display: block;
      width: 100%;
      height: 2px;
      border-radius: 1px;
      background: currentColor;
    }}
    .site-menu summary:hover {{
      background-color: var(--note-bg);
    }}

    .site-menu nav {{
      position: absolute;
      z-index: 1;
      top: calc(100% + 8px);
      right: 0;
      width: min(360px, calc(100vw - 32px));
      padding: 14px;
      background-color: var(--card-bg);
      border: 1px solid var(--border-color);
      border-radius: 6px;
      box-shadow: 0 8px 20px rgba(0, 0, 0, 0.12);
    }}

    .menu-outside {{
      margin-top: 14px;
      padding: 10px 6px 6px;
      border-top: 3px double var(--border-color);
      background-color: var(--code-bg);
    }}

    .menu-outside a {{
      position: relative;
      padding-right: 30px;
    }}

    .menu-outside a::after {{
      content: "↗";
      position: absolute;
      top: 10px;
      right: 10px;
      color: var(--text-muted);
      font-size: 0.9em;
    }}

    .site-menu nav a {{
      display: block;
      padding: 9px 10px;
      color: var(--text-main);
      font-size: 0.9em;
      text-decoration: none;
      border-radius: 5px;
    }}

    .site-menu nav a:hover {{ background-color: var(--note-bg); }}
    .site-menu nav a[aria-current="page"] {{
      background-color: var(--note-bg);
      color: var(--destination);
      font-weight: 700;
    }}
    .menu-link-title {{ display: block; }}
    .menu-current-label {{
      margin-left: 0.45em;
      color: var(--text-muted);
      font-size: 0.78em;
      font-weight: 600;
    }}
    .menu-link-detail {{
      display: block;
      margin-top: 1px;
      color: var(--text-muted);
      font-size: 0.84em;
      font-weight: 400;
    }}

    .header-tools {{ display: inline-flex; align-items: center; gap: 12px; }}
    .site-menu {{ margin-left: auto; }}
    .language-switch {{ color: var(--link); font-size: 0.82em; font-weight: 600; }}
    .contrast-toggle {{ display: inline-flex; align-items: center; gap: 7px; color: var(--text-muted); font-size: 0.82em; font-weight: 600; cursor: pointer; }}
    .contrast-toggle input {{ inline-size: 15px; block-size: 15px; accent-color: var(--accent); }}

    html[data-contrast="high"] {{
      --bg-main: #ffffff;
      --card-bg: #ffffff;
      --text-main: #111111;
      --text-muted: #2b2b2b;
      --accent: #006b4d;
      --link: #123bb1;
      --accent-hover: #002f22;
      --border-color: #202020;
      --code-bg: #f4f4f4;
      --pre-bg: #000000;
      --pre-text: #ffffff;
      --note-bg: #ffffff;
      --note-border: #9a4e00;
    }}
    html[data-contrast="high"] .container {{ border-width: 2px; box-shadow: none; }}
    html[data-contrast="high"] .site-menu nav, html[data-contrast="high"] code, html[data-contrast="high"] th, html[data-contrast="high"] td, html[data-contrast="high"] .doc-note, html[data-contrast="high"] .doc-details, html[data-contrast="high"] .doc-reference-section {{ border-width: 2px; }}
    html[data-contrast="high"] .site-menu nav a:hover, html[data-contrast="high"] .site-menu nav a[aria-current="page"] {{ background-color: #ffffff; outline: 3px solid var(--note-border); outline-offset: -3px; }}
    html[data-contrast="high"] .flowchart-diagram, html[data-contrast="high"] .flowchart-node {{ border-width: 2px; }}

    h1 {{ font-size: 1.85em; color: var(--text-main); margin-top: 0; margin-bottom: 16px; line-height: 1.45; }}
    h2 {{ font-size: 1.35em; border-bottom: 1px solid var(--border-color); padding-bottom: 6px; margin-top: 1.8em; margin-bottom: 12px; color: var(--text-main); }}
    h3 {{ font-size: 1.12em; margin-top: 1.4em; margin-bottom: 8px; color: var(--text-muted); }}

    p {{ margin-bottom: 1.1em; word-break: break-word; }}

    a {{ color: var(--link); text-decoration: underline; font-weight: 500; text-underline-offset: 3px; }}
    a:hover {{ text-decoration: underline; color: var(--accent-hover); }}
    a:focus-visible {{ outline: 3px solid var(--note-border); outline-offset: 3px; }}

    code {{
      background-color: var(--code-bg);
      color: var(--text-main);
      padding: 2px 6px;
      border-radius: 4px;
      font-size: 0.9em;
      font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
      border: 1px solid var(--border-color);
    }}

    pre {{
      background-color: var(--pre-bg);
      color: var(--pre-text);
      padding: 16px;
      border-radius: 8px;
      overflow-x: auto;
      margin: 1.2em 0;
      font-size: 0.9em;
      line-height: 1.5;
    }}

    pre code {{
      background-color: transparent;
      color: inherit;
      padding: 0;
      border: none;
    }}

    .flowchart-diagram {{
      display: grid;
      gap: 10px;
      margin: 1.2em 0;
      padding: 16px;
      border: 1px solid var(--accent);
      border-radius: 8px;
      background: var(--note-bg);
    }}
    .flowchart-diagram figcaption {{ color: var(--text-muted); font-size: 0.84em; font-weight: 700; }}
    .flowchart-edge {{ display: flex; align-items: center; flex-wrap: wrap; gap: 8px; }}
    .flowchart-node {{
      max-width: min(100%, 320px);
      padding: 8px 12px;
      border: 1px solid var(--border-color);
      border-radius: 6px;
      background: var(--card-bg);
      color: var(--text-main);
      font-size: 0.92em;
      font-weight: 600;
      line-height: 1.55;
    }}
    .flowchart-arrow {{ color: var(--accent); font-size: 1.3em; font-weight: 700; }}
    .flowchart-branch {{ color: var(--text-muted); font-size: 0.82em; font-weight: 600; }}

    blockquote {{
      border-left: 4px solid var(--note-border);
      background-color: var(--note-bg);
      margin: 1.2em 0;
      padding: 12px 16px;
      color: var(--text-main);
      border-radius: 0 6px 6px 0;
    }}

    .document-outline {{
      margin: 0 0 1.8em;
      padding: 0 16px 12px;
      border-left: 4px solid var(--accent);
      background-color: var(--code-bg);
    }}
    .document-outline summary {{ cursor: pointer; padding: 11px 0 5px; color: var(--text-muted); font-size: 0.88em; font-weight: 700; }}
    .document-outline ol {{ display: flex; flex-wrap: wrap; gap: 3px 18px; margin: 0; padding-left: 1.3em; }}
    .document-outline li {{ margin: 0; }}
    .document-outline a {{ font-size: 0.92em; }}

    .doc-note, .doc-details {{
      margin: 1.2em 0;
      padding: 12px 16px;
      border: 1px solid var(--note-border);
      border-radius: 6px;
      background-color: var(--note-bg);
    }}
    .doc-note > :last-child, .doc-details > :last-child {{ margin-bottom: 0; }}
    .doc-details {{ padding-top: 0; }}
    .doc-details summary {{
      cursor: pointer;
      margin: 0 -16px;
      padding: 11px 16px;
      color: var(--text-main);
      font-weight: 700;
    }}
    .doc-details[open] summary {{ border-bottom: 1px solid var(--note-border); margin-bottom: 12px; }}
    .doc-details summary:focus-visible {{ outline: 3px solid var(--link); outline-offset: -3px; }}

    .doc-reference-section {{
      margin: 1em 0;
      border: 1px solid var(--border-color);
      border-radius: 6px;
      background-color: var(--card-bg);
    }}
    .doc-reference-section summary {{
      cursor: pointer;
      padding: 11px 14px;
      color: var(--text-main);
      font-size: 1.12em;
      font-weight: 700;
    }}
    .doc-reference-section[open] summary {{ border-bottom: 1px solid var(--border-color); }}
    .doc-reference-section > :not(summary) {{ margin-left: 14px; margin-right: 14px; }}
    .doc-reference-section > h3:first-of-type {{ margin-top: 1.1em; }}
    .doc-reference-section summary:focus-visible {{ outline: 3px solid var(--link); outline-offset: -3px; }}

    .table-scroll {{
      width: 100%;
      margin: 1.2em 0;
      overflow-x: auto;
    }}

    .table-scroll table {{
      width: 100%;
      border-collapse: collapse;
      margin: 0;
      font-size: 0.92em;
    }}

    th, td {{
      border: 1px solid var(--border-color);
      padding: 8px 12px;
      text-align: left;
      overflow-wrap: normal;
      word-break: normal;
    }}

    th code, td code {{
      white-space: nowrap;
      overflow-wrap: normal;
      word-break: normal;
    }}

    th {{
      background-color: var(--code-bg);
      font-weight: 600;
    }}

    ul, ol {{
      padding-left: 1.6em;
      margin-bottom: 1.1em;
    }}

    li {{ margin-bottom: 0.3em; }}

    img {{
      max-width: 100%;
      height: auto;
      border-radius: 6px;
      margin: 1em 0;
    }}

    hr {{
      border: none;
      border-top: 1px dashed var(--border-color);
      margin: 2em 0;
    }}

    footer {{
      margin-top: 32px;
      padding-top: 16px;
      border-top: 1px dashed var(--border-color);
      text-align: center;
      font-size: 0.82em;
      color: var(--text-muted);
    }}

    @media (max-width: 560px) {{
      body {{ padding: 0; }}
      .container {{ min-height: 100vh; padding: 26px 18px; }}
      .doc-header {{ align-items: flex-start; margin-bottom: 20px; }}
      .header-tools {{ justify-content: flex-start; gap: 8px; }}
      .doc-header, .header-tools {{ flex-wrap: wrap; }}
      .site-menu nav {{ width: min(360px, calc(100vw - 36px)); }}
      h1 {{ font-size: 1.65em; }}
      h2 {{ font-size: 1.24em; }}
      pre {{ padding: 12px; font-size: 0.82em; white-space: pre-wrap; overflow-wrap: anywhere; }}
      th, td {{ padding: 7px 9px; }}
      .flowchart-diagram {{ padding: 12px; }}
      .flowchart-edge {{ align-items: stretch; flex-direction: column; }}
      .flowchart-node {{ max-width: 100%; }}
      .flowchart-arrow {{ align-self: center; transform: rotate(90deg); }}
      footer {{ margin-top: 26px; }}
    }}
/* Shared interaction colors: destinations, local actions, and opened sections. */
:root {{ --action:#007a59; --external-link:#1d50dd; --opened:#a16635; }}
html[data-contrast="high"] {{ --action:#00573f; --opened:#9a4e00; }}
:root a[href] {{ --destination:var(--action); color:var(--destination); }}
:root a:is([href^="http://"],[href^="https://"],[href^="//"]) {{ --destination:var(--external-link); }}
:root a[data-link-kind="internal"] {{ --destination:var(--action); }}
:root a[data-link-kind="external"] {{ --destination:var(--external-link); }}
:root a[href]:focus-visible {{ outline-color:var(--destination); }}
:root summary {{ color:var(--action); }}
:root details > summary:hover {{ color:var(--action); }}
:root :is(summary,button,input):focus-visible {{ outline-color:var(--action); }}
:root input[type="checkbox"] {{ accent-color:var(--action); }}
:root details[data-opened="true"] > summary {{ color:var(--opened); }}
:root .button {{ border-color:var(--destination); }}
:root a.button[href]:hover {{ background:var(--destination); color:#fff; }}
  </style>
</head>
<body>

<a class="skip-link" href="#content">{skip_label}</a>

<div class="container">
  <div class="doc-header">
    <div class="header-tools">
{language_switch_html}
      <label class="contrast-toggle"><input id="contrast-toggle" type="checkbox"><span>{contrast_label}</span></label>
    </div>
{navigation_html}
  </div>

  <main id="content" tabindex="-1">
{content_html}
  </main>

  <footer>
    PDF Note Workspace Documentation &copy; 2026
  </footer>
</div>

<script>
  (() => {{
    const storageKey = "pdf-note-workspace-high-contrast";
    const toggle = document.querySelector("#contrast-toggle");
    const apply = (enabled) => {{
      document.documentElement.dataset.contrast = enabled ? "high" : "";
      toggle.checked = enabled;
    }};
    let saved = null;
    try {{ saved = localStorage.getItem(storageKey); }} catch (_) {{ /* Storage unavailable: keep this page's setting only. */ }}
    apply(saved === "true");
    toggle.addEventListener("change", () => {{
      const left = window.scrollX;
      const top = window.scrollY;
      apply(toggle.checked);
      requestAnimationFrame(() => window.scrollTo(left, top));
      try {{ localStorage.setItem(storageKey, String(toggle.checked)); }} catch (_) {{ /* Storage unavailable: keep this page's setting only. */ }}
    }});

      const revealFragment = () => {{
        if (!window.location || !window.location.hash) return;
        let identifier = "";
        try {{ identifier = decodeURIComponent(window.location.hash.slice(1)); }} catch (_) {{ return; }}
        const target = document.getElementById(identifier);
        if (!target) return;
        for (let parent = target.parentElement; parent; parent = parent.parentElement) {{
          if (parent.tagName === "DETAILS") parent.open = true;
        }}
        requestAnimationFrame(() => target.scrollIntoView({{ block: "start" }}));
      }};
      if (typeof window.addEventListener === "function") window.addEventListener("hashchange", revealFragment);
      revealFragment();
  }})();
</script>

<script id="navigation-state-script">
(() => {{
  // Resolve against the public canonical URL, including during local preview.
  const canonical = document.querySelector('link[rel="canonical"]');
  if (canonical) {{
    const base = new URL(canonical.href);
    document.querySelectorAll('a[href]').forEach(link => {{
      try {{
        const target = new URL(link.getAttribute('href'), base);
        link.dataset.linkKind = target.origin === base.origin ? 'internal' : 'external';
      }} catch (_) {{ /* Leave malformed destinations to the existing link validator. */ }}
    }});
  }}
  document.querySelectorAll('details').forEach(entry => {{
    const summary = entry.querySelector(':scope > summary');
    if (!summary) return;
    summary.addEventListener('click', event => {{
      if (event.target.closest('a,button,input,select,textarea')) return;
      const wasOpen = entry.open;
      const pending = entry.dataset.readPending === 'true';
      const epoch = entry.dataset.readEpoch;
      // Wait for native summary activation; bulk actions invalidate pending work.
      setTimeout(() => {{
        if (event.defaultPrevented || entry.open === wasOpen || entry.dataset.readEpoch !== epoch) return;
        if (entry.open) {{
          entry.dataset.readPending = 'true';
        }} else {{
          if (pending) entry.dataset.opened = 'true';
          delete entry.dataset.readPending;
        }}
      }}, 0);
    }});
    entry.addEventListener('toggle', () => {{
      if (!entry.open) delete entry.dataset.readPending;
    }});
  }});
}})();
</script>
</body>
</html>
"""

def simple_markdown_to_html(md_text: str, root_rel: str) -> str:
    """Return display HTML without changing the Markdown source.

    Numbered steps accept 1–9 ASCII digits. Other text stays a paragraph,
    and fenced code is not interpreted as steps or captions.
    """
    lines = normalize_gfm_callout_blocks(md_text.splitlines())
    html_lines = []
    in_code_block = False
    code_fence = ""
    code_block_lines = []
    list_tag: str | None = None
    in_table = False
    containers: list[str] = []
    is_mermaid = False
    heading_counts: dict[str, int] = {}

    def heading_id(source: str) -> str:
        normalized = re.sub(r"[`*_]", "", source).strip().lower()
        normalized = re.sub(r"\s+", "-", normalized)
        # \w already includes Japanese letters. Broad kana ranges also retain
        # punctuation such as ・, breaking Markdown links that omit punctuation.
        normalized = re.sub(r"[^\w\-]", "", normalized)
        base = normalized or "section"
        count = heading_counts.get(base, 0)
        heading_counts[base] = count + 1
        return base if count == 0 else f"{base}-{count}"

    def close_flow_blocks() -> None:
        nonlocal list_tag, in_table
        if list_tag:
            html_lines.append(f"</{list_tag}>")
            list_tag = None
        if in_table:
            html_lines.append("</table></div>")
            in_table = False

    def append_code_block() -> None:
        code_text = html.escape("\n".join(code_block_lines))
        diagram = render_mermaid_flowchart(code_block_lines) if is_mermaid else None
        html_lines.append(diagram or f"<pre><code>{code_text}</code></pre>")

    for line in lines:
        # A code example may itself contain shorter fences. Only a matching
        # character with at least the opening length closes the literal block.
        if in_code_block:
            closing = re.fullmatch(r" {0,3}(`{3,}|~{3,})\s*", line)
            if closing and closing[1][0] == code_fence[0] and len(closing[1]) >= len(code_fence):
                append_code_block()
                code_block_lines = []
                in_code_block = False
                is_mermaid = False
            else:
                code_block_lines.append(line)
            continue

        opening = re.match(r" {0,3}(`{3,}|~{3,})(.*)$", line)
        if opening:
            close_flow_blocks()
            in_code_block = True
            code_fence = opening[1]
            is_mermaid = opening[2].strip().lower() == "mermaid"
            continue

        # Lightweight containers are intentionally source-readable. `note`
        # keeps essential supplemental content visible; `details` lets the
        # browser collapse optional explanation without removing it from the
        # Markdown that people and assistants can inspect.
        container = re.fullmatch(r"\s{0,3}(:{3,})(?:\s+([A-Za-z][\w-]*)(?:\s+(.*))?)?\s*", line)
        if container:
            _, kind, label = container.groups()
            close_flow_blocks()
            if kind is None:
                if containers:
                    html_lines.append(f"</{containers.pop()}>")
                else:
                    containers.append("aside")
                    html_lines.append('<aside class="doc-note" role="note">')
            elif kind.lower() == "details":
                summary = format_inline((label or "Details").strip(), root_rel)
                containers.append("details")
                html_lines.append(f'<details class="doc-details"><summary>{summary}</summary>')
            else:
                containers.append("aside")
                html_lines.append('<aside class="doc-note" role="note">')
            continue

        # リストの閉じ処理
        numbered_item = re.match(r"^([0-9]{1,9})\.\s+(.*)$", line)
        item_tag = (
            "ol" if numbered_item else
            "ul" if line.startswith(("- ", "* ")) else None
        )
        if list_tag and line.strip() and item_tag != list_tag:
            html_lines.append(f"</{list_tag}>")
            list_tag = None

        # テーブルの閉じ処理
        if in_table and not line.startswith("|"):
            html_lines.append("</table></div>")
            in_table = False

        # 空行
        if not line.strip():
            continue

        # 見出し
        if line.startswith("# "):
            html_lines.append(f'<h1 id="{heading_id(line[2:])}">{format_inline(line[2:], root_rel)}</h1>')
        elif line.startswith("## "):
            html_lines.append(f'<h2 id="{heading_id(line[3:])}">{format_inline(line[3:], root_rel)}</h2>')
        elif line.startswith("### "):
            html_lines.append(f'<h3 id="{heading_id(line[4:])}">{format_inline(line[4:], root_rel)}</h3>')
        elif line.startswith("#### "):
            html_lines.append(f'<h4 id="{heading_id(line[5:])}">{format_inline(line[5:], root_rel)}</h4>')
        # 引用注記 (GFM Callouts)
        elif line.startswith("> "):
            quote_text = format_inline(line[2:], root_rel)
            html_lines.append(f"<blockquote><p>{quote_text}</p></blockquote>")
        # リスト
        elif item_tag:
            if not list_tag:
                first_number = int(numbered_item.group(1)) if numbered_item else 1
                start = f' start="{first_number}"' if first_number != 1 else ""
                html_lines.append(f"<{item_tag}{start}>")
                list_tag = item_tag
            item_text = format_inline(numbered_item.group(2) if numbered_item else line[2:], root_rel)
            html_lines.append(f"<li>{item_text}</li>")
        # テーブル
        elif line.startswith("|"):
            source_cells = [c.strip() for c in line.split("|")[1:-1]]
            if source_cells and all(re.fullmatch(r":?-{3,}:?", c) for c in source_cells):
                continue  # セル全体が区切り指定の行だけをスキップ
            cells = [format_inline(c, root_rel) for c in source_cells]
            if not in_table:
                html_lines.append('<div class="table-scroll"><table>')
                in_table = True
                tag = "th"
            else:
                tag = "td"
            row_html = "".join([f"<{tag}>{c}</{tag}>" for c in cells])
            html_lines.append(f"<tr>{row_html}</tr>")
        # 水平線
        elif line.strip() in ("---", "***", "___"):
            html_lines.append("<hr>")
        # 通常段落
        else:
            # Standalone image captions use emphasis in the source documents.
            caption = re.fullmatch(r"\*([^*]+)\*", line)
            paragraph = (
                f"<em>{format_inline(caption.group(1), root_rel)}</em>"
                if caption else format_inline(line, root_rel)
            )
            html_lines.append(f"<p>{paragraph}</p>")

    if in_code_block:
        append_code_block()
    close_flow_blocks()
    while containers:
        html_lines.append(f"</{containers.pop()}>")

    return "\n".join(html_lines)


def normalize_gfm_callout_blocks(lines: list[str]) -> list[str]:
    """Turn GitHub's quoted callout syntax into the renderer's note container.

    The source form remains readable in Markdown, while the generated page
    presents its content as one continuous note rather than exposing the
    ``[!TYPE]`` marker and separating each quoted line into its own box.
    Code fences are left untouched.
    """
    normalized: list[str] = []
    index = 0
    in_code_fence = False
    code_fence = ""

    while index < len(lines):
        line = lines[index]
        fence = re.match(r" {0,3}(`{3,}|~{3,})", line)
        if fence:
            marker = fence.group(1)
            if not in_code_fence:
                in_code_fence = True
                code_fence = marker
            elif marker[0] == code_fence[0] and len(marker) >= len(code_fence):
                in_code_fence = False
                code_fence = ""
            normalized.append(line)
            index += 1
            continue

        if not in_code_fence and re.fullmatch(
            r"\s*>\s*\[!(?:NOTE|TIP|IMPORTANT|WARNING|CAUTION)\]\s*", line, re.IGNORECASE
        ):
            normalized.append("::: note")
            index += 1
            while index < len(lines):
                quoted = re.fullmatch(r"\s*> ?(.*)", lines[index])
                if quoted is None:
                    break
                normalized.append(quoted.group(1))
                index += 1
            normalized.append(":::")
            continue

        normalized.append(line)
        index += 1

    return normalized


def render_mermaid_flowchart(lines: list[str]) -> str | None:
    """Render basic Mermaid flowchart/graph edges as a local, accessible HTML diagram."""
    content_lines = [line.strip() for line in lines if line.strip()]
    if not content_lines or not re.match(r"(?:flowchart|graph)\s+(?:LR|RL|TD|TB|BT)\b", content_lines[0], re.IGNORECASE):
        return None

    def parse_node(value: str) -> tuple[str, str | None] | None:
        match = re.fullmatch(r"\s*([A-Za-z0-9_]+)(?:\[(.*)\])?\s*", value)
        if not match:
            return None
        node_id, label = match.groups()
        return node_id, label.strip().strip('"') if label is not None else None

    def split_connector(line: str) -> tuple[str, str, str | None] | None:
        """Split the supported Mermaid connectors without treating them as HTML."""
        dotted_end = line.find(".->")
        if dotted_end >= 0:
            dotted_start = line.rfind("-.", 0, dotted_end)
            if dotted_start >= 0:
                return (line[:dotted_start],
                        line[dotted_end + len(".->"):],
                        line[dotted_start + len("-."):dotted_end].strip() or None)

        arrow_end = line.find("->")
        if arrow_end > 0 and line[arrow_end - 1] == "-":
            return line[:arrow_end - 1], line[arrow_end + len("->"):], None
        return None

    edges: list[tuple[str, str, str | None]] = []
    node_labels: dict[str, str] = {}
    for line in content_lines[1:]:
        connector = split_connector(line)
        if connector is None:
            continue
        source_text, target_text, branch_label = connector
        source = parse_node(source_text)
        target = parse_node(target_text)
        if source is not None and target is not None:
            source_id, source_label = source
            target_id, target_label = target
            if source_label is not None:
                node_labels[source_id] = source_label
            if target_label is not None:
                node_labels[target_id] = target_label
            edges.append((source_id, target_id, branch_label))
    if not edges:
        return None

    def node_html(label: str) -> str:
        # Only these exact line-break spellings are admitted as HTML. Escape
        # every other label character before it reaches the generated page.
        parts: list[str] = []
        cursor = 0
        while cursor < len(label):
            tag_start = label.find("<", cursor)
            if tag_start < 0:
                parts.append(html.escape(label[cursor:]))
                break
            parts.append(html.escape(label[cursor:tag_start]))
            lower_tail = label[tag_start:].lower()
            matched_tag = next((tag for tag in ("<br>", "<br/>", "<br />") if lower_tail.startswith(tag)), None)
            if matched_tag is None:
                parts.append("&lt;")
                cursor = tag_start + 1
            else:
                parts.append("<br>")
                cursor = tag_start + len(matched_tag)
        return f'<span class="flowchart-node">{"".join(parts)}</span>'

    edge_html = []
    for source, target, branch_label in edges:
        label_html = f'<span class="flowchart-branch">{html.escape(branch_label)}</span>' if branch_label else ""
        edge_html.append(
            f'<div class="flowchart-edge">{node_html(node_labels.get(source, source))}{label_html}'
            f'<span class="flowchart-arrow" aria-hidden="true">→</span>{node_html(node_labels.get(target, target))}</div>'
        )
    return '<figure class="flowchart-diagram" aria-label="フロー図"><figcaption>フロー図</figcaption>' + "\n".join(edge_html) + "</figure>"


def format_inline(text: str, root_rel: str) -> str:
    """インライン要素（リンク、太字、コード、画像）を変換します。"""
    # Protect code before interpreting Markdown in prose. Matching delimiter
    # lengths also allow examples of backticks inside a longer code span.
    prefix = "\x00code"
    while prefix in text:
        prefix += "\x00"
    codes: dict[str, tuple[str, str]] = {}

    def protect_code(match: re.Match[str]) -> str:
        value = match.group(2)
        if value.startswith(" ") and value.endswith(" ") and value.strip(" "):
            value = value[1:-1]
        token = f"{prefix}{len(codes)}\x00"
        codes[token] = (value, match.group())
        return token

    def restore_codes(value: str, *, source: bool = False, plain: bool = False) -> str:
        for token, (literal, original) in codes.items():
            replacement = original if source else literal if plain else f"<code>{html.escape(literal)}</code>"
            value = value.replace(token, replacement)
        return value

    text = re.sub(r"(?<!`)(`+)(?!`)(.*?)(?<!`)\1(?!`)", protect_code, text)
    # エスケープ前に基本処理
    # 画像
    def replace_image(m):
        label = html.escape(restore_codes(m.group(1), plain=True))
        url = html.escape(restore_codes(m.group(2), source=True))
        return f'<img src="{url}" alt="{label}"><br><em>{label}</em>'

    text = re.sub(
        r'!\[(.*?)\]\((.*?)\)',
        replace_image,
        text
    )
    # 公開サイトでHTML化されるローカル Markdown は .html を参照する。
    # 生の Markdown も同じ場所に残し、各HTMLページから直接開ける。
    def replace_link(m):
        label = m.group(1)
        url = restore_codes(m.group(2), source=True)
        if "#" in url:
            path_part, anchor_part = url.split("#", 1)
            anchor_str = "#" + anchor_part
        else:
            path_part = url
            anchor_str = ""

        if path_part.endswith(".md") and not path_part.startswith("http"):
            path_part = path_part[:-3] + ".html"

        url = path_part + anchor_str
        return f'<a href="{html.escape(url)}">{label}</a>'

    text = re.sub(r'\[(.*?)\]\((.*?)\)', replace_link, text)
    # 太字
    text = re.sub(r'\*\*(.*?)\*\*', r'<strong>\1</strong>', text)
    return restore_codes(text)

def language_counterpart(rel_path: Path, site_dir: Path) -> Path | None:
    """Return only an existing same-document translation in the staged site."""
    introduction_counterparts = {
        Path("introduction/index.md"): Path("introduction/en/index.md"),
        Path("introduction/project_overview.md"): Path("introduction/en/project_overview.md"),
        Path("introduction/core/features_and_scenarios.md"): Path("introduction/en/features_and_scenarios.md"),
        Path("introduction/core/file_formats.md"): Path("introduction/en/file_formats.md"),
    }
    if rel_path.parts[:2] in (("docs", "ja"), ("docs", "en")):
        other_locale = "en" if rel_path.parts[1] == "ja" else "ja"
        counterpart = Path("docs") / other_locale / Path(*rel_path.parts[2:])
    elif rel_path in introduction_counterparts:
        counterpart = introduction_counterparts[rel_path]
    elif rel_path in introduction_counterparts.values():
        counterpart = next(source for source, target in introduction_counterparts.items() if target == rel_path)
    else:
        return None
    return counterpart if (site_dir / counterpart).is_file() else None


def language_switch_html(*, root_rel: str, rel_path: Path, site_dir: Path) -> str:
    """Return a same-document language switch for language-specific docs."""
    counterpart = language_counterpart(rel_path, site_dir)
    if counterpart is None:
        return ""
    current_locale = "en" if rel_path.parts[:2] in (("docs", "en"), ("introduction", "en")) else "ja"
    counterpart_html = counterpart.with_suffix(".html").as_posix()
    label = "English" if current_locale == "ja" else "日本語"
    aria_label = "Switch to English" if current_locale == "ja" else "日本語版へ切り替える"
    return f'      <a class="language-switch" href="{root_rel}{counterpart_html}" aria-label="{aria_label}">{label}</a>'


class DocumentSummaryParser(HTMLParser):
    """Read display text for metadata; source Markdown and body HTML stay unchanged."""

    def __init__(self) -> None:
        super().__init__()
        self.title = ""
        self.paragraphs: list[str] = []
        self._capture = ""
        self._parts: list[str] = []
        self._image = False
        self._emphasis_depth = 0
        self._plain_data = False

    def handle_starttag(self, tag: str, attrs) -> None:
        if tag in ("h1", "p"):
            self._capture = tag
            self._parts = []
            self._image = False
            self._emphasis_depth = 0
            self._plain_data = False
        elif tag == "img" and self._capture:
            self._image = True
        elif tag == "em" and self._capture:
            self._emphasis_depth += 1

    def handle_data(self, data: str) -> None:
        if self._capture:
            self._parts.append(data)
            if data.strip() and self._emphasis_depth == 0:
                self._plain_data = True

    def handle_endtag(self, tag: str) -> None:
        if tag == "em" and self._emphasis_depth:
            self._emphasis_depth -= 1
        if tag != self._capture:
            return
        text = re.sub(r"\s+", " ", "".join(self._parts)).strip()
        if tag == "h1" and not self.title:
            self.title = text
        elif tag == "p" and text and not self._image and self._plain_data:
            if not text.startswith(("同梱リポジトリ版:", "Bundled repository version:", "Repository version:")):
                self.paragraphs.append(text)
        self._capture = ""


class DocumentOutlineParser(HTMLParser):
    """Collect second-level headings for a short, local table of contents."""

    def __init__(self) -> None:
        super().__init__()
        self.items: list[tuple[str, str]] = []
        self._identifier = ""
        self._parts: list[str] = []

    def handle_starttag(self, tag: str, attrs) -> None:
        if tag == "h2":
            attributes = dict(attrs)
            self._identifier = attributes.get("id", "")
            self._parts = []

    def handle_data(self, data: str) -> None:
        if self._identifier:
            self._parts.append(data)

    def handle_endtag(self, tag: str) -> None:
        if tag != "h2" or not self._identifier:
            return
        label = re.sub(r"\s+", " ", "".join(self._parts)).strip()
        if label:
            self.items.append((self._identifier, label))
        self._identifier = ""
        self._parts = []


def document_outline_html(body_html: str, locale: str) -> str:
    """Return a compact map only when a document has multiple main sections."""
    parser = DocumentOutlineParser()
    parser.feed(body_html)
    if len(parser.items) < 2:
        return ""
    title = "On this page" if locale == "en" else "この文書の項目"
    links = "".join(
        f'<li><a href="#{html.escape(identifier, quote=True)}">{html.escape(label)}</a></li>'
        for identifier, label in parser.items
    )
    return f'<details class="document-outline"><summary>{title}</summary><ol>{links}</ol></details>'


def collapse_reference_sections(body_html: str, rel_path: Path) -> str:
    """Turn the two long lookup references into compact, linkable sections.

    The Markdown remains ordinary headings for repository readers and search
    tools. The browser presentation collapses only this reference pair, whose
    primary use is locating one operation or setting rather than linear
    reading. The original heading id moves to the summary so fragment links
    still resolve and the page script can reveal its parent details element.
    """
    if rel_path not in (Path("docs/ja/Help_Reference.md"), Path("docs/en/Help_Reference.md")):
        return body_html
    sections = re.split(r'(?=<h2 id="[^"]+">)', body_html)
    if len(sections) < 2:
        return body_html
    converted = [sections[0]]
    heading = re.compile(r'^<h2 id="([^"]+)">(.*?)</h2>')
    for index, section in enumerate(sections[1:]):
        match = heading.match(section)
        if match is None:
            converted.append(section)
            continue
        identifier, label = match.groups()
        content = section[match.end():]
        open_attribute = " open" if index == 0 else ""
        converted.append(
            f'<details class="doc-reference-section"{open_attribute}>'
            f'<summary id="{identifier}" role="heading" aria-level="2">{label}</summary>{content}</details>'
        )
    return "\n".join(converted)


def document_metadata(body_html: str, rel_path: Path, site_dir: Path, locale: str) -> tuple[str, str]:
    """Build escaped head metadata from local display text and known public paths.

    No network requests or source writes occur. Missing translations are omitted;
    a document without prose uses its own title as the description.
    """
    summary = DocumentSummaryParser()
    summary.feed(body_html)
    title = summary.title or rel_path.stem
    search_title = title if "pdf note workspace" in title.lower() else f"{title} – PDF Note Workspace"
    description = summary.paragraphs[0] if summary.paragraphs else title
    description = DOCUMENT_DESCRIPTIONS.get(rel_path.as_posix(), description)
    canonical = PUBLIC_BASE_URL + quote(rel_path.with_suffix(".html").as_posix())
    tags = [f'  <link rel="canonical" href="{canonical}">']
    counterpart = language_counterpart(rel_path, site_dir)
    if counterpart is not None:
        for language, target in ((locale, rel_path), ("ja" if locale == "en" else "en", counterpart)):
            url = PUBLIC_BASE_URL + quote(target.with_suffix(".html").as_posix())
            tags.append(f'  <link rel="alternate" hreflang="{language}" href="{url}">')
    # Shared product screenshot already belongs to this site's public allowlist.
    image = PUBLIC_BASE_URL + "docs/images/app_overview.png"
    image_alt = (
        "PDF Note Workspace showing a PDF, notes, a material list, and annotation controls in one window"
        if locale == "en" else "PDFとノート、資料一覧、注釈の操作を同じ画面に表示したPDF Note Workspaceの画面"
    )
    values = (
        ("name", "description", description),
        ("property", "og:type", "article"),
        ("property", "og:site_name", "PDF Note Workspace Documentation"),
        ("property", "og:locale", "en_US" if locale == "en" else "ja_JP"),
        ("property", "og:url", canonical),
        ("property", "og:title", search_title),
        ("property", "og:description", description),
        ("property", "og:image", image),
        ("property", "og:image:alt", image_alt),
        ("name", "twitter:card", "summary_large_image"),
        ("name", "twitter:title", search_title),
        ("name", "twitter:description", description),
        ("name", "twitter:image", image),
        ("name", "twitter:image:alt", image_alt),
    )
    tags.extend(f'  <meta {kind}="{name}" content="{html.escape(value, quote=True)}">' for kind, name, value in values)
    return html.escape(search_title), "\n".join(tags)


def navigation_html(*, root_rel: str, rel_path: Path) -> str:
    """Render a one-language navigation menu and highlight the current section."""
    locale = (
        rel_path.parts[1]
        if rel_path.parts[:2] in (("docs", "ja"), ("docs", "en"), ("introduction", "en"))
        else "ja"
    )
    if rel_path.parts[0] == "introduction":
        current_section = "背景・設計・確認資料"
    elif rel_path.parts[:2] == ("docs", "en"):
        current_section = "User documentation"
    elif rel_path.parts[:2] == ("docs", "ja"):
        current_section = "使い方・セットアップ"
    elif rel_path.name == "README.md":
        current_section = "プロジェクトの概要"
    elif rel_path.name in {"LICENSE.md", "LICENSES_INDEX.md", "THIRD_PARTY_NOTICES.md"}:
        current_section = "ライセンスと第三者通知"
    else:
        current_section = "公開文書"

    if locale == "en":
        portal_entries = (
            ("Documentation portal", "Return to the English-language entry point", f"{root_rel}en/index.html", "Documentation portal"),
            ("Project overview", "Packages, Standard and Lite editions, and core policies", f"{root_rel}README.html", "Project overview"),
            ("User documentation", "Setup, use, saving and recovery, file formats, and troubleshooting", f"{root_rel}docs/en/README.html", "User documentation"),
            ("Background, design, and verification", "Selected English material plus Japanese reference documents", f"{root_rel}introduction/en/index.html", "Background, design, and verification"),
            ("Licenses and third-party notices", "Terms and third-party components", f"{root_rel}LICENSES_INDEX.html", "Licenses and third-party notices"),
        )
        outside_entries = (
            ("Product site", "Read an overview and find downloads", "https://pdf-note-workspace.soone-y.com/"),
            ("Get the release", "Open GitHub Releases", "https://github.com/soone-y/OPEN_PDF-Note-Workspace/releases", None),
            ("GitHub repository", "Browse source, issues, and public history", "https://github.com/soone-y/OPEN_PDF-Note-Workspace", None),
        )
        menu_label = "Documentation menu"
        menu_aria_label = "Open documentation menu"
    else:
        portal_entries = (
            ("文書ポータルのトップ", "目的別の入口へ戻る", f"{root_rel}index.html", "文書ポータル"),
            ("プロジェクトの概要", "配布物、通常版・Lite版、基本方針", f"{root_rel}README.html", "プロジェクトの概要"),
            ("日本語の文書", "導入・操作・保存・トラブル対処", f"{root_rel}docs/ja/README.html", "使い方・セットアップ"),
            ("背景・設計・確認資料", "設計の考え方、利用判断、根拠と確認範囲", f"{root_rel}introduction/index.html", "背景・設計・確認資料"),
            ("ライセンスと第三者通知", "利用条件と第三者コンポーネント", f"{root_rel}LICENSES_INDEX.html", "ライセンスと第三者通知"),
        )
        outside_entries = (
            ("紹介サイト", "ソフトの概要と配布先を見る", "https://pdf-note-workspace.soone-y.com/"),
            ("配布物を入手する", "GitHub Releases を開く", "https://github.com/soone-y/OPEN_PDF-Note-Workspace/releases", None),
            ("GitHub リポジトリ", "ソース、Issue、公開履歴を見る", "https://github.com/soone-y/OPEN_PDF-Note-Workspace", None),
        )
        menu_label = "文書メニュー"
        menu_aria_label = "文書メニューを開く"
    current_document_label = "(Current document)" if locale == "en" else "（現在の文書）"

    def render_portal_entry(label: str, detail: str, href: str, section: str) -> str:
        is_current = section == current_section
        current_marker = (
            f'<span class="menu-current-label">{current_document_label}</span>'
            if is_current else ""
        )
        return f'''        <a href="{href}"{' aria-current="page"' if is_current else ''}>
          <span class="menu-link-title">{html.escape(label)}{current_marker}</span>
          <span class="menu-link-detail">{html.escape(detail)}</span>
        </a>'''

    portal_entry_html = "\n".join(
        render_portal_entry(label, detail, href, section)
        for label, detail, href, section in portal_entries
    )
    outside_entry_html = "\n".join(
        f'''        <a href="{href}">
          <span class="menu-link-title">{html.escape(label)}</span>
          <span class="menu-link-detail">{html.escape(detail)}</span>
        </a>'''
        for label, detail, href, *_ in outside_entries
    )
    return f"""    <details class=\"site-menu\">
      <summary aria-label=\"{menu_aria_label}\"><span class=\"menu-icon\" aria-hidden=\"true\"><span class=\"menu-icon-bar\"></span><span class=\"menu-icon-bar\"></span><span class=\"menu-icon-bar\"></span></span>{menu_label}</summary>
      <nav aria-label=\"{menu_label}\">
        <div class=\"menu-links\">
{portal_entry_html}
        </div>
        <div class=\"menu-outside\">
{outside_entry_html}
        </div>
      </nav>
    </details>"""


def convert_md_file_to_html(md_path: Path, site_dir: Path) -> Path:
    """Markdown ファイルを読み込み、対応する HTML ファイルを生成します。"""
    content = md_path.read_text(encoding="utf-8").lstrip("\ufeff")
    rel_path = md_path.relative_to(site_dir)
    depth = len(rel_path.parts) - 1
    root_rel = "../" * depth if depth > 0 else "./"
    # タイトルの抽出
    title_match = re.search(r"^#\s+(.*)$", content, re.MULTILINE)
    title = title_match.group(1).strip() if title_match else md_path.stem

    body_html = simple_markdown_to_html(content, root_rel)
    language_code = (
        rel_path.parts[1]
        if rel_path.parts[:2] in (("docs", "ja"), ("docs", "en"), ("introduction", "en"))
        else "ja"
    )
    # Summarize document prose before adding generated navigation labels.
    search_title, metadata_html = document_metadata(body_html, rel_path, site_dir, language_code)
    outline_html = document_outline_html(body_html, language_code)
    if outline_html:
        body_html = re.sub(r"(</h1>)", r"\1\n" + outline_html, body_html, count=1)
    body_html = collapse_reference_sections(body_html, rel_path)
    full_html = HTML_TEMPLATE.format(
        title=html.escape(title),
        search_title=search_title,
        metadata_html=metadata_html,
        language_code=language_code,
        contrast_label="High contrast" if language_code == "en" else "高コントラスト",
        skip_label="Skip to content" if language_code == "en" else "本文へ移動",
        root_rel=root_rel,
        navigation_html=navigation_html(root_rel=root_rel, rel_path=rel_path),
        language_switch_html=language_switch_html(root_rel=root_rel, rel_path=rel_path, site_dir=site_dir),
        content_html=body_html
    )

    html_path = md_path.with_suffix(".html")
    html_path.write_text(full_html, encoding="utf-8")
    return html_path

def main(argv: list[str] | None = None) -> int:
    args = argv if argv is not None else sys.argv[1:]
    if len(args) < 1:
        print("Usage: python render_human_docs.py <site_directory>", file=sys.stderr)
        return 1

    site_dir = Path(args[0]).resolve()
    if not site_dir.exists() or not site_dir.is_dir():
        print(f"Directory not found: {site_dir}", file=sys.stderr)
        return 1

    count = 0
    rendered: set[Path] = set()
    for relative_dir in (Path("."), Path("docs/ja"), Path("docs/en"), Path("docs/statistics"), Path("introduction")):
        directory = site_dir / relative_dir
        if not directory.exists() or not directory.is_dir():
            continue
        markdown_files = (
            sorted(site_dir.glob("*.md"))
            if relative_dir == Path(".")
            else sorted(directory.rglob("*.md"))
        )
        for markdown_file in markdown_files:
            if markdown_file.is_file():
                convert_md_file_to_html(markdown_file, site_dir)
                rendered.add(markdown_file.resolve())
                count += 1

    # These common documents live outside the language trees but are linked
    # from DOCUMENTATION.md, so their Markdown links must have HTML targets.
    for markdown_file in (
        site_dir / ".github" / "SECURITY.md",
        site_dir / "docs" / "案内ドキュメントの構成.md",
    ):
        if markdown_file.is_file() and markdown_file.resolve() not in rendered:
            convert_md_file_to_html(markdown_file, site_dir)
            count += 1

    print(f"Successfully generated {count} HTML documentation pages in {site_dir}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
