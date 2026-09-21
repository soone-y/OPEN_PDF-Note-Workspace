# データ構造

## 1. 注釈保存ファイル (`.clrop` - Classroom PDF Annotation)

`.clrop` は、PDF 本体を直接書き換えずに、注釈を別保存する version 1 の JSON ファイルです。名前は Classroom に由来する `.clro` の PDF 注釈版を表します。PDFを確認する情報と、ページごとの注釈を持ちます。

### 全体構造 (Root JSON Schema)

```json
{
  "version": 1,
  "pdf_id": {
    "path": "lecture.pdf",
    "size": 123456,
    "page_count": 12,
    "sha256": "..."
  },
  "pages": [
    {
      "page": 0,
      "items": [
        {
          "type": "text",
          "content": "annotation text"
        }
      ]
    }
  ]
}
```

これは構造を説明するための簡略例です。注釈の種類ごとに位置、線、色、文字、数式、リンク先、図形などの項目が変わります。

利用者向けの扱いと、PDFと一緒に移動する理由は [`.clrop` PDF注釈データ](../../docs/ja/CLROP_Annotation_Format.md) を参照してください。

---

## 2. アプリ標準ノート (`.clro` - Classroom Note)

`.clro` は、Classroom に由来する、本ソフトが新しく作る標準ノートの拡張子です。中身はUTF-8のテキストで、`.md` / `.markdown` と同じ Markdown/MD4C のノート経路で扱います。

- **役割**: 新規作成、候補名、既定の命名規則に使う本ソフト中心のノートです。
- **記法**: 見出し、箇条書き、強調、リンクなどの Markdown 系記法に加え、対応する独自 markup、数式、ノートリンクを扱います。
- **互換性**: 一般的な Markdown は他ソフトでも読めることがありますが、本ソフトが対応していない Markdown の全機能や、GitHubと同じ表示を保証するものではありません。

利用者向けの記法、`.md` との使い分け、他ソフトで開く場合の注意は [`.clro` ノート形式](../../docs/ja/CLRO_Note_Format.md) を参照してください。

---

## 3. アプリ設定ファイル (`pdf_workspace_setup.json`)

配布物およびワークスペースで環境設定を管理する JSON ファイルです。

```json
{
  "version": 1,
  "ui": {
    "theme": "light",
    "sidebar_width": 280,
    "last_workspace_dir": ""
  },
  "annotation_defaults": {
    "pen_color": "#FF0000",
    "pen_thickness": 2.0,
    "highlight_color": "#FFFF0040"
  },
  "safety": {
    "no_network_strict": true,
    "silent_warning_policy": true,
    "atomic_save": true
  }
}
```
