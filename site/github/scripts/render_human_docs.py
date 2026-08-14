#!/usr/bin/env python3
"""
render_human_docs.py

公開サイト成果物 (site/github/output/public/) 内の人間用ドキュメント (README.md, docs/public/*.md) から、
ブラウザで直接閲覧できる美しいセルフコンテインド HTML ページ (.html) を自動生成します。

- 人間がアクセスした場合: 美しくデザインされた HTML ドキュメントとして表示。
- AI がアクセスした場合: 生の .md ファイルがそのまま取得可能（AI 可読性 100% 保持）。
- 外部通信ゼロ: 外部 CDN やフォントを一切使用せず、ローカル完結。
"""

import sys
import re
import html
from pathlib import Path

# HTML テンプレート（外部通信ゼロ・落ち着いたライト/ダーク対応デザイン）
HTML_TEMPLATE = """<!DOCTYPE html>
<html lang="ja">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>{title} – PDF Note Workspace</title>
  <style>
    :root {{
      --bg-main: #f6f8f7;
      --card-bg: #ffffff;
      --text-main: #24312f;
      --text-muted: #5d6c68;
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
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
      background-color: var(--bg-main);
      color: var(--text-main);
      line-height: 1.75;
      padding: 24px 12px;
    }}

    .container {{
      max-width: 880px;
      margin: 0 auto;
      background-color: var(--card-bg);
      border: 1px solid var(--border-color);
      border-top: 4px solid var(--note-border);
      border-radius: 12px;
      padding: 36px 28px;
      box-shadow: 0 12px 32px rgba(0, 0, 0, 0.08);
    }}

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

    .site-menu {{ position: relative; }}

    .site-menu summary {{
      display: inline-flex;
      align-items: center;
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
      display: inline-block;
      width: 14px;
      height: 10px;
      margin-right: 7px;
      border-top: 2px solid currentColor;
      border-bottom: 2px solid currentColor;
      position: relative;
    }}
    .menu-icon::after {{
      content: "";
      position: absolute;
      left: 0;
      right: 0;
      top: 3px;
      border-top: 2px solid currentColor;
    }}
    .site-menu summary:hover {{
      background-color: var(--note-bg);
    }}

    .site-menu nav {{
      position: absolute;
      z-index: 1;
      top: calc(100% + 8px);
      left: 0;
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
      color: var(--link);
      font-weight: 700;
    }}
    .menu-link-title {{ display: block; }}
    .menu-link-detail {{
      display: block;
      margin-top: 1px;
      color: var(--text-muted);
      font-size: 0.84em;
      font-weight: 400;
    }}

    .raw-md-link {{
      font-size: 0.82em;
      color: var(--text-muted);
    }}

    .header-tools {{ display: inline-flex; align-items: center; gap: 12px; }}
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
    html[data-contrast="high"] .site-menu nav, html[data-contrast="high"] code, html[data-contrast="high"] th, html[data-contrast="high"] td {{ border-width: 2px; }}
    html[data-contrast="high"] .site-menu nav a:hover, html[data-contrast="high"] .site-menu nav a[aria-current="page"] {{ background-color: #ffffff; outline: 3px solid var(--note-border); outline-offset: -3px; }}

    h1 {{ font-size: 1.85em; color: var(--accent); margin-top: 0; margin-bottom: 16px; line-height: 1.3; }}
    h2 {{ font-size: 1.35em; border-bottom: 1px solid var(--border-color); padding-bottom: 6px; margin-top: 1.8em; margin-bottom: 12px; color: var(--text-main); }}
    h3 {{ font-size: 1.12em; margin-top: 1.4em; margin-bottom: 8px; color: var(--text-muted); }}

    p {{ margin-bottom: 1.1em; word-break: break-word; }}

    a {{ color: var(--link); text-decoration: none; font-weight: 500; text-underline-offset: 3px; }}
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

    blockquote {{
      border-left: 4px solid var(--note-border);
      background-color: var(--note-bg);
      margin: 1.2em 0;
      padding: 12px 16px;
      color: var(--text-main);
      border-radius: 0 6px 6px 0;
    }}

    table {{
      width: 100%;
      border-collapse: collapse;
      margin: 1.2em 0;
      font-size: 0.92em;
      overflow-x: auto;
      display: block;
    }}

    th, td {{
      border: 1px solid var(--border-color);
      padding: 8px 12px;
      text-align: left;
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
  </style>
</head>
<body>

<div class="container">
  <div class="doc-header">
{navigation_html}
    <div class="header-tools">
      <label class="contrast-toggle"><input id="contrast-toggle" type="checkbox"><span>高コントラスト</span></label>
{raw_markdown_html}
    </div>
  </div>

  <main>
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
      apply(toggle.checked);
      try {{ localStorage.setItem(storageKey, String(toggle.checked)); }} catch (_) {{ /* Storage unavailable: keep this page's setting only. */ }}
    }});
  }})();
</script>

</body>
</html>
"""

def simple_markdown_to_html(md_text: str, root_rel: str) -> str:
    """Markdown テキストを HTML に簡易変換します。"""
    lines = md_text.splitlines()
    html_lines = []
    in_code_block = False
    code_block_lines = []
    in_list = False
    in_table = False
    is_mermaid = False
    heading_counts: dict[str, int] = {}

    def heading_id(source: str) -> str:
        normalized = re.sub(r"[`*_]", "", source).strip().lower()
        normalized = re.sub(r"\s+", "-", normalized)
        normalized = re.sub(r"[^\w\-\u3040-\u30ff\u3400-\u9fff]", "", normalized)
        base = normalized or "section"
        count = heading_counts.get(base, 0)
        heading_counts[base] = count + 1
        return base if count == 0 else f"{base}-{count}"

    for line in lines:
        # コードブロック処理
        if line.startswith("```"):
            if in_code_block:
                code_text = html.escape("\n".join(code_block_lines))
                if is_mermaid:
                    html_lines.append(f'<pre style="border: 1px solid var(--accent); background-color: var(--note-bg); color: var(--text-main); font-family: monospace;"><code>{code_text}</code></pre>')
                else:
                    html_lines.append(f"<pre><code>{code_text}</code></pre>")
                code_block_lines = []
                in_code_block = False
                is_mermaid = False
            else:
                if in_list:
                    html_lines.append("</ul>")
                    in_list = False
                if in_table:
                    html_lines.append("</table>")
                    in_table = False
                in_code_block = True
                is_mermaid = "mermaid" in line.lower()
            continue

        if in_code_block:
            code_block_lines.append(line)
            continue

        # リストの閉じ処理
        if in_list and not (line.startswith("- ") or line.startswith("* ") or re.match(r"^\d+\.\s", line)):
            html_lines.append("</ul>")
            in_list = False

        # テーブルの閉じ処理
        if in_table and not line.startswith("|"):
            html_lines.append("</table>")
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
        elif line.startswith("- ") or line.startswith("* "):
            if not in_list:
                html_lines.append("<ul>")
                in_list = True
            item_text = format_inline(line[2:], root_rel)
            html_lines.append(f"<li>{item_text}</li>")
        # テーブル
        elif line.startswith("|"):
            if "---" in line:
                continue  # 区切り行スキップ
            cells = [format_inline(c.strip(), root_rel) for c in line.split("|")[1:-1]]
            if not in_table:
                html_lines.append("<table>")
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
            html_lines.append(f"<p>{format_inline(line, root_rel)}</p>")

    if in_list:
        html_lines.append("</ul>")
    if in_table:
        html_lines.append("</table>")

    return "\n".join(html_lines)

def format_inline(text: str, root_rel: str) -> str:
    """インライン要素（リンク、太字、コード、画像）を変換します。"""
    # エスケープ前に基本処理
    # 画像
    text = re.sub(
        r'!\[(.*?)\]\((.*?)\)',
        lambda m: f'<img src="{m.group(2)}" alt="{html.escape(m.group(1))}"><br><em>{html.escape(m.group(1))}</em>',
        text
    )
    # 公開サイトでHTML化されるローカル Markdown は .html を参照する。
    # 生の Markdown も同じ場所に残し、各HTMLページから直接開ける。
    def replace_link(m):
        label = m.group(1)
        url = m.group(2)
        if "#" in url:
            path_part, anchor_part = url.split("#", 1)
            anchor_str = "#" + anchor_part
        else:
            path_part = url
            anchor_str = ""

        if path_part.endswith(".md") and not path_part.startswith("http"):
            path_part = path_part[:-3] + ".html"

        url = path_part + anchor_str
        return f'<a href="{url}">{label}</a>'

    text = re.sub(r'\[(.*?)\]\((.*?)\)', replace_link, text)
    # 太字
    text = re.sub(r'\*\*(.*?)\*\*', r'<strong>\1</strong>', text)
    # インラインコード
    text = re.sub(r'`(.*?)`', lambda m: f'<code>{html.escape(m.group(1))}</code>', text)
    return text

def navigation_html(*, root_rel: str, rel_path: Path) -> str:
    """現在の項目をハイライトし、リンクの目的を示すポータル用メニューを生成する。"""
    if rel_path.parts[0] == "introduction":
        current_section = "背景・設計・確認資料"
    elif rel_path.parts[:2] == ("docs", "public"):
        current_section = "使い方・セットアップ"
    elif rel_path.name == "README.md":
        current_section = "プロジェクトの概要"
    elif rel_path.name in {"LICENSE.md", "LICENSES_INDEX.md", "THIRD_PARTY_NOTICES.md"}:
        current_section = "ライセンスと第三者通知"
    else:
        current_section = "公開文書"

    portal_entries = (
        ("プロジェクトの概要", "配布物、通常版・Lite版、基本方針", f"{root_rel}README.html", "プロジェクトの概要"),
        ("使い方・セットアップ", "導入・操作・保存・トラブル対処", f"{root_rel}docs/public/Index.html", "使い方・セットアップ"),
        ("背景・設計・確認資料", "設計の考え方、利用判断、根拠と確認範囲", f"{root_rel}introduction/index.html", "背景・設計・確認資料"),
        ("ライセンスと第三者通知", "利用条件と第三者コンポーネント", f"{root_rel}LICENSES_INDEX.html", "ライセンスと第三者通知"),
    )
    outside_entries = (
        ("文書ポータルのトップ", "目的別の入口へ戻る", f"{root_rel}index.html"),
        ("紹介サイト", "ソフトの概要と配布先を見る", "https://pdf-note-workspace.soone-y.com/"),
        ("配布物を入手する", "GitHub Releases を開く", "https://github.com/soone-y/OPEN_PDF-Note-Workspace/releases", None),
        ("GitHub リポジトリ", "ソース、Issue、公開履歴を見る", "https://github.com/soone-y/OPEN_PDF-Note-Workspace", None),
    )
    portal_entry_html = "\n".join(
        f'''        <a href="{href}"{' aria-current="page"' if section == current_section else ''}>
          <span class="menu-link-title">{html.escape(label)}</span>
          <span class="menu-link-detail">{html.escape(detail)}</span>
        </a>'''
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
      <summary aria-label=\"文書メニューを開く\"><span class=\"menu-icon\" aria-hidden=\"true\"></span>文書メニュー</summary>
      <nav aria-label=\"文書メニュー\">
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
    raw_markdown_html = f'''    <div class="raw-md-link">
      <a href="{md_path.name}" target="_blank">Raw Markdown</a>
    </div>'''

    # タイトルの抽出
    title_match = re.search(r"^#\s+(.*)$", content, re.MULTILINE)
    title = title_match.group(1).strip() if title_match else md_path.stem

    body_html = simple_markdown_to_html(content, root_rel)
    full_html = HTML_TEMPLATE.format(
        title=html.escape(title),
        root_rel=root_rel,
        raw_markdown_html=raw_markdown_html,
        navigation_html=navigation_html(root_rel=root_rel, rel_path=rel_path),
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
    for relative_dir in (Path("."), Path("docs/public"), Path("introduction")):
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
                count += 1

    print(f"Successfully generated {count} HTML documentation pages in {site_dir}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
