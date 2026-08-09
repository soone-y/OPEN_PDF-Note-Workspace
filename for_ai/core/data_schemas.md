# AI_CORE_NODE: DATA_SCHEMAS

<ai_node_schema version="1.0" category="data-schemas">
  <summary>.clrop (JSON注釈データ), .clro (標準ノート), pdf_workspace_setup.json (設定) のデータモデル・スキーマ仕様</summary>
</ai_node_schema>

## 1. 注釈保存ファイル (`.clrop` - ClassRoom PDF Annotation)

`.clrop` は、PDF 本体を一切傷つけずに注釈・手書きペン・テキスト・図形を別保存するための標準 JSON フォーマットです。

### 全体構造 (Root JSON Schema)

```json
{
  "version": 1,
  "app_version": "0.9.003",
  "target_pdf_hash": "a1b2c3d4...",
  "page_count": 12,
  "pages": [
    {
      "page_index": 0,
      "annotations": [
        {
          "id": "annot-uuid-1",
          "type": "ink",
          "color": "#FF0000",
          "opacity": 1.0,
          "thickness": 2.5,
          "points": [
            { "x": 100.5, "y": 200.0 },
            { "x": 105.0, "y": 202.5 }
          ]
        },
        {
          "id": "annot-uuid-2",
          "type": "text",
          "color": "#0000FF",
          "font_size": 14,
          "bounds": { "x": 50, "y": 80, "w": 200, "h": 100 },
          "content": "ここに注釈メモ"
        }
      ]
    }
  ]
}
```

### 注釈オブジェクトプロパティ (`AnnotationObject`)

| フィールド | 型 | 説明 |
| :--- | :--- | :--- |
| `id` | string | 注釈ごとの固有識別子 (UUID v4) |
| `type` | string | `ink` (フリーハンド), `highlight` (マーカー), `text` (テキスト), `rect` (矩形), `line` (直線), `math` (TeX数式) |
| `color` | string | HEX カラーコード (`#RRGGBB` または `#RRGGBBAA`) |
| `thickness` | number | 線の太さ（ピクセル） |
| `opacity` | number | 不透明度 (`0.0` 〜 `1.0`) |
| `points` | array | `ink` / `line` の場合の座標配列 `[ {x, y}, ... ]` |
| `bounds` | object | `text` / `rect` の描画領域 `{x, y, w, h}` |
| `content` | string | `text` や `math` の内容テキスト |

---

## 2. アプリ標準ノート (`.clro` - ClassRoom Note)

`.clro` は、Markdown をハイパーセットとしてサポートする標準テキストノートです。

- **フォーマット**: UTF-8 エンコーディングのテキストファイル（BOMなし）。
- **Markdown 互換性**: 標準的な GFM (GitHub Flavored Markdown) をそのまま開くことが可能。
- **拡張機能**: TeX 数式ブロック (`$$ ... $$`), Mermaid ダイアグラム (` ```mermaid ... ``` `) のローカル表示に対応。

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
