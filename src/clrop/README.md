# clrop module plan

## Scope
- Store/load PDF注釈を`.clrop`(JSON)で扱う小モジュール。`clro`同様に`src/clrop/`に閉じ、UI/Win32に依存しない。
- 目的: `g_annots` をファイル往復できるようにし、PDFと同じ場所に`.clrop`として保存する。

## Format (clrop v1)
- 拡張子`.clrop`、中身JSON。ルート: `version:int`(1固定), `pdf_id:{path,size,page_count,page_sizes_pt|page_size_rule,sha256}`, `pages:[{page:int,items:[...]}]`.
- `pdf_id.page_size_rule` はページサイズ圧縮用の任意フィールド。`{default_pt:[w,h], exceptions:[[page,w,h],...]}` を持ち、既定サイズから外れるページだけを列挙する。
- 保存時は `page_size_rule` のほうが短くなる場合にそれを優先し、読込時は `page_sizes_pt` / `page_size_rule` の両方を受ける。
- item共通: `type`, `created`, `updated` (ISO8601), 配列順がZ順。
  - `text`: `bbox:[x,y,w,h]`, `pt`, `font`, `writing_mode(optional: horizontal|vertical_rl)`, `border`, `color`, `content`, `lines(optional)`
  - `marker-text`: `quads:[[x1,y1,...,x4,y4],...]`, `alpha`, `color`
  - `marker-free`: `path:[[x,y],...]`, `width`, `alpha`, `color`
  - `line`: `p1:[x,y]`, `p2:[x,y]`, `width`, `dash:[]`, `color`
  - `freehand`: `path:[[x,y],...]`, `width`, `color`
- 座標はPDFページ座標(pt)。`color`は `#RRGGBB` 文字列。

## PdfId
- 構造: `path`(参考値), `size`(byte), `page_count`, `page_sizes_pt` または `page_size_rule`, `sha256`(ファイル生バイト)。
- 照合: Fast は `size + page_count + 各ページサイズ`、Strong はそれに `sha256` を追加。`path` は警告用の参考。
- 計算: UTF-16→UTF-8正規化後のパス文字列をそのまま格納。ハッシュは小さなユーティリティで計算（外部ライブラリは入れない）。

## API案（フェーズ別に実装）
- フェーズ1: 型とJSONシリアライズ/パースを実装（Win32非依存）。候補ファイル `clrop_types.h/.cpp`, `clrop_json.h/.cpp`, `clrop_hash.h/.cpp`。
  - 関数例: `PdfId ComputePdfId(const std::wstring& path)`, `bool LoadClrop(const std::wstring& path, ClropDoc& out, std::wstring& err)`, `bool SaveClrop(const std::wstring& path, const ClropDoc& doc, std::wstring& err)`.
- フェーズ2: ファイル探索やPDF照合を追加（UI非依存）。`FindAdjacentClrop(pdfPath)`、`ValidatePdfId(onMismatchCallback)` など補助関数を用意。
- フェーズ3: UI接続（`g_annots`⇔ClropDoc変換、自動/手動保存トリガ、確認ダイアログ）を本体に組み込む。
- エラー扱い: bool+エラーメッセージ文字列で返却。パース時はフィールド欠落/型不正でスキップ記録し、致命的な場合は失敗。

## 保存/自動保存ポリシーの前提
- 手動保存: メニューやショートカットに後で接続する。
- 自動保存: 非編集1分経過 or 4分周期で発火、Dirty時のみ保存。成功でDirtyクリア。UI実装はフェーズ3。

## ファイル配置・パッキング
- `.clrop`はPDFやノートと同階層に置く。
- 例: `session/<pdf>.pdf` に対し `session/<pdf>.clrop` を書く。

## 作業方針メモ
- 当面は `src/clrop/` 内で完結する実装を積み上げ、他ファイルへの変更はフェーズ3まで控える。
- 追加依存ライブラリは導入せず、既存のスタイル（標準ライブラリ中心）で書く。
