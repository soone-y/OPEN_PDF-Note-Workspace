#!/usr/bin/env python3
"""
render_human_docs.py

GitHub Pages デプロイ用の静的サイトツリー (_site/) 内にある人間用ドキュメント
(README.md, Document/*.md) に対して、外部通信ゼロのブラウザ閲覧用スタイルと
超軽量インラインレンダラースクリプトを付加します。

AI 向け文書 (For_AI.md, for_ai/*) は変更せず、完全な Raw テキストのまま保護します。
"""

import sys
from pathlib import Path

# ブラウザ閲覧時に適用される外部通信ゼロのスタイルとレンダラースクリプト
HUMAN_DOC_ENHANCER = """
<!-- HUMAN_DOC_VIEWER_START -->
<style>
  @media screen {
    html {
      background-color: #f8fafc;
      color: #0f172a;
    }
    body {
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
      max-width: 860px;
      margin: 0 auto;
      padding: 32px 24px;
      line-height: 1.75;
      background-color: #ffffff;
      box-shadow: 0 4px 20px rgba(0, 0, 0, 0.06);
      border: 1px solid #e2e8f0;
      border-radius: 8px;
    }
    h1 {
      font-size: 1.85em;
      border-bottom: 2px solid #0284c7;
      padding-bottom: 8px;
      color: #0369a1;
      margin-top: 0;
      margin-bottom: 16px;
    }
    h2 {
      font-size: 1.4em;
      border-bottom: 1px solid #cbd5e1;
      padding-bottom: 6px;
      color: #0f172a;
      margin-top: 1.8em;
      margin-bottom: 12px;
    }
    h3 {
      font-size: 1.15em;
      color: #334155;
      margin-top: 1.4em;
      margin-bottom: 8px;
    }
    p {
      margin-bottom: 1em;
    }
    a {
      color: #0284c7;
      text-decoration: none;
      font-weight: 500;
    }
    a:hover {
      text-decoration: underline;
      color: #0369a1;
    }
    code {
      background-color: #f1f5f9;
      color: #0f172a;
      padding: 2px 6px;
      border-radius: 4px;
      font-size: 0.9em;
      font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
      border: 1px solid #e2e8f0;
    }
    pre {
      background-color: #0f172a;
      color: #f8fafc;
      padding: 16px;
      border-radius: 8px;
      overflow-x: auto;
      margin: 1.2em 0;
    }
    pre code {
      background-color: transparent;
      color: inherit;
      padding: 0;
      border: none;
    }
    blockquote {
      border-left: 4px solid #0284c7;
      background-color: #f0f9ff;
      margin: 1.2em 0;
      padding: 12px 16px;
      color: #0369a1;
      border-radius: 0 6px 6px 0;
    }
    blockquote p {
      margin: 0;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      margin: 1.2em 0;
      font-size: 0.95em;
    }
    th, td {
      border: 1px solid #cbd5e1;
      padding: 10px 14px;
      text-align: left;
    }
    th {
      background-color: #f1f5f9;
      font-weight: 600;
      color: #0f172a;
    }
    tr:nth-child(even) {
      background-color: #f8fafc;
    }
    img {
      max-width: 100%;
      height: auto;
      border-radius: 6px;
      margin: 1em 0;
    }
    hr {
      border: none;
      border-top: 1px dashed #cbd5e1;
      margin: 2em 0;
    }
    ul, ol {
      padding-left: 1.6em;
      margin-bottom: 1em;
    }
    li {
      margin-bottom: 0.35em;
    }
    .doc-nav-top {
      margin-bottom: 24px;
      padding: 10px 14px;
      background-color: #f1f5f9;
      border-radius: 6px;
      font-size: 0.9em;
      display: flex;
      align-items: center;
      gap: 12px;
    }
    .doc-nav-top a {
      color: #0284c7;
      text-decoration: none;
    }
  }
</style>
<script>
(function() {
  if (typeof window === 'undefined' || typeof document === 'undefined') return;
  document.addEventListener('DOMContentLoaded', function() {
    var body = document.body;
    if (!body || body.getAttribute('data-doc-enhanced')) return;
    body.setAttribute('data-doc-enhanced', 'true');

    // ブラウザがプレーンテキスト表示を行っている場合、簡易HTMLへと動的に見た目を向上
    var rawText = body.innerText || body.textContent;
    if (!rawText || rawText.indexOf('# ') === -1) return;

    // トップナビゲーションバーの挿入
    var nav = document.createElement('div');
    nav.className = 'doc-nav-top';
    nav.innerHTML = '<a href="/OPEN_PDF-Note-Workspace/index.html">&laquo; ドキュメントポータルへ戻る</a> | <a href="/OPEN_PDF-Note-Workspace/README.md">README</a> | <a href="/OPEN_PDF-Note-Workspace/Document/Index.md">文書案内</a>';
    body.insertBefore(nav, body.firstChild);
  });
})();
</script>
<!-- HUMAN_DOC_VIEWER_END -->
"""

def enhance_human_document(file_path: Path) -> bool:
    """人間用ドキュメントファイルに閲覧用拡張ブロックを適用します。"""
    try:
        content = file_path.read_text(encoding="utf-8")
        if "HUMAN_DOC_VIEWER_START" in content:
            return False  # 適用済み

        # 末尾に視覚拡張ブロックを追加
        enhanced_content = content.rstrip() + "\n\n" + HUMAN_DOC_ENHANCER.strip() + "\n"
        file_path.write_text(enhanced_content, encoding="utf-8")
        return True
    except Exception as e:
        print(f"Error enhancing {file_path}: {e}", file=sys.stderr)
        return False

def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: python render_human_docs.py <site_directory>", file=sys.stderr)
        return 1

    site_dir = Path(sys.argv[1]).resolve()
    if not site_dir.exists() or not site_dir.is_dir():
        print(f"Directory not found: {site_dir}", file=sys.stderr)
        return 1

    enhanced_count = 0

    # README.md の処理
    readme_path = site_dir / "README.md"
    if readme_path.exists():
        if enhance_human_document(readme_path):
            enhanced_count += 1

    # Document/*.md の処理
    doc_dir = site_dir / "Document"
    if doc_dir.exists() and doc_dir.is_dir():
        for doc_file in doc_dir.glob("*.md"):
            if enhance_human_document(doc_file):
                enhanced_count += 1

    print(f"Successfully enhanced {enhanced_count} human documentation files in {site_dir}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
