# .clrop 開発案内

## 現在の開発方針

**現在の保存形式と機能拡張の中心は `.clrop` version 2（v2）。version 1（v1）は既存機能・旧データとの互換性を保守する対象とする。** 新しい注釈機能や保存項目はv2を前提に設計する。v1の対応は維持し、新規拡張の中心にはしない。

アプリの保存入口 `bridge.cpp` はv2を書き出す。v1は読み込み時に互換性を解決し、保存時にv2へ移行する。開いただけで原本を書き換えない。低水準のcodecに残るv1対応を、現在の保存先や新機能の実装先と取り違えないこと。

開発時は、次の入口から実装と検査を確認する。

- 利用者向けの形式説明: [日本語](../../docs/ja/CLROP_Annotation_Format.md) / [English](../../docs/en/CLROP_Annotation_Format.md)
- 注釈の表示方式・v2拡張の設計: [注釈ツール設計](../../docs/internal/annotation_ツール設計_2026-04-21.md)
- 保存モデルと版の初期値: [types.h](types.h)。JSONの読込・保存: [json.cpp](json.cpp) / [json_direct_parser.inc](json_direct_parser.inc)。アプリ注釈との変換・保存入口: [bridge.cpp](bridge.cpp)
- 既存の関連検査: [JSON・stage往復検査](../../tests/scripts/run_clrop_json_direct_parse_tests.ps1) / [ファイル安全性検査](../../tests/scripts/run_clrop_file_safety_tests.ps1)。実行の要否は作業内容と[開発と検査](../../.agents/development-checks.md)に従って判断する。

v2でも、表示方式の省略値は注釈種別と項目ごとに異なる。`marker-text` の `marker_display_mode` 省略は乗算、`shape` / `marker-free` の `blend_mode` 省略は通常の半透明合成を意味する。v1の方式未指定テキストマーカーは従来表示を保持する。保存項目を追加・変更するときは、版別の省略値、旧データの表示保持、stageとUndo/Redoの往復を合わせて扱う。

## 以下の初期計画について

以下はv1開発開始時の計画を残した記録であり、現行仕様ではない。「1固定」「フェーズ」「後で接続」などの記述は当時の計画として読む。現在の開発には上記の方針と参照先を使う。

## Scope
- Store/load PDF注釈を`.clrop`(JSON)で扱う小モジュール。`clro`同様に`src/clrop/`に閉じ、UI/Win32に依存しない。
- 目的: `g_annots` をファイル往復できるようにし、PDFと同じ場所に`.clrop`として保存する。

## Format (clrop v1)
- 拡張子`.clrop`、中身JSON。ルート: `version:int`(1固定), `pdf_id:{path,size,page_count,page_sizes_pt|page_size_rule,sha256}`, `pages:[{page:int,items:[...]}]`.
- `pdf_id.page_size_rule` はページサイズ圧縮用の任意フィールド。`{default_pt:[w,h], exceptions:[[page,w,h],...]}` を持ち、既定サイズから外れるページだけを列挙する。
- 保存時は `page_size_rule` のほうが短くなる場合にそれを優先し、読込時は `page_sizes_pt` / `page_size_rule` の両方を受ける。
- item共通: `type`, `created`, `updated` (ISO8601), 配列順がZ順。
  - `text` / `math`: `bbox:[x,y,w,h]`, `pt`, `font`, `background_assist_mode(optional: inherit|off|auto|inverted)`, `border`, `color`, `content`, `lines(optional)`。省略された旧データは `inherit` として読む。`writing_mode(optional: horizontal|vertical_rl)` は `text` のみ。
  - `bbox_endpoints:[x1,y1,x2,y2]` は text / math / shape で書き出す任意のv1拡張。`bbox` は旧readerとの互換用に維持し、端点を差で復元すると精度を失う場合だけこのフィールドを優先する。旧ファイル、またはこの任意フィールドが不正なファイルは `bbox` から従来どおり復元する。
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
