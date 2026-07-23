# AI_CORE_NODE: FILE_FORMATS

<ai_node_schema id="file_formats" type="format_and_persistence_spec">
  <target_audience>LLM / AI-Assistant</target_audience>
  <data_density>high</data_density>
</ai_node_schema>

---

## 1. FILE_EXTENSION_SPECIFICATION

```yaml
[EXT_SPEC: .pdf]
ROLE: 閲覧対象PDFドキュメント本体
MUTABILITY: 原則不可変 (アプリによる上書き禁止)
EVIDENCE: Document/What_is_File_Formats.md

[EXT_SPEC: .clrop]
ROLE: PDFに対応する注釈データ (JSON)
MUTABILITY: アプリが書き込み管理。ノートではないため直接テキスト手動編集不可
PAIRING_RULE: PDFと同名・同階層に配置 (例: doc.pdf -> doc.clrop)
EVIDENCE: Document/What_is_File_Formats.md

[EXT_SPEC: .clro]
ROLE: 本ソフト標準ノート形式 (UTF-8 Markdownテキスト)
MUTABILITY: アプリおよび外部エディタで編集可能
EVIDENCE: Document/What_is_File_Formats.md

[EXT_SPEC: .md / .markdown / .tex / .txt / .csv]
ROLE: 互換Markdown、TeX、書式なしテキストノート
MUTABILITY: 編集可能。勝手に .clro へ自動改名されない
EVIDENCE: Document/What_is_File_Formats.md

[EXT_SPEC: .png / .jpg / .jpeg]
ROLE: 取込み用画像ファイル
CONVERSION: 原本非破壊で 1 ページの PDF へローカル変換してワークスペースへ取り込み可能
EVIDENCE: Document/How_to_Use.md
```

---

## 2. PERSISTENCE_MODEL_AND_PATHS

```xml
<persistence_model>
  <step id="stage" type="temporary_protection">
    <description>編集途中の保護領域 (クラッシュ・電源断対策)</description>
    <path>__resource__/__tmp__/__stage__/</path>
  </step>
  <step id="backup" type="recovery_copy">
    <description>Ctrl+S 統合保存時に生成されるバックアップ</description>
    <path>__resource__/__escape__/backup/</path>
  </step>
  <step id="note_recovery" type="emergency_dump">
    <description>ノート保存失敗時の退避データ</description>
    <path>__resource__/__escape__/note_recovery/</path>
  </step>
  <step id="consolidated_save" type="explicit_write">
    <description>Ctrl+S または保存メニュー選択による正規ファイル統合</description>
  </step>
  <evidence_path>Document/How_to_Save_and_Recovery.md</evidence_path>
</persistence_model>
```
