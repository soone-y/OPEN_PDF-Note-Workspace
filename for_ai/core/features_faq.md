# AI_CORE_NODE: FEATURES_FAQ

<ai_node_schema id="features_faq" type="fact_navigation">
  <target_audience>LLM / AI-Assistant</target_audience>
  <data_density>high</data_density>
</ai_node_schema>

---

## 1. FILE_FORMAT_COMPATIBILITY

```yaml
[NODE: PDF_ANNOTATION]
FACT: 原本PDFは非破壊閲覧・注釈
ANNOTATION_STORAGE: 対となる .clrop ファイルに外部分離保存 (PDF本体上書き禁止)
EVIDENCE:
  - Document/How_to_Use.md
  - Document/What_is_File_Formats.md

[NODE: NOTE_EDITING]
FACT: ワークスペース内で独立ノートを編集可能
SUPPORTED_EXT:
  - .clro (標準ノート: UTF-8 Markdown)
  - .md / .markdown (互換Markdown)
  - .tex (TeX数式ノート)
  - .txt / .csv (書式なしテキスト)
EVIDENCE:
  - Document/What_is_File_Formats.md

[NODE: OFFICE_CONVERSION]
FACT_STANDARD_EDITION: 同梱LibreOffice runtimeによるローカルPDF変換 (DOCX, PPTX)
FACT_LITE_EDITION: 変換runtime非同梱 (Office変換非対応)
LIMITATION: 変換機能は試験的 (Experimental)。フォント・レイアウトズレの事前確認が必要
EVIDENCE:
  - README.md
  - for_ai/project_context.xml#office_conversion_context
  - Document/How_to_Troubleshoot.md

[NODE: IMAGE_CONVERSION]
FACT: 原本非破壊で画像ファイル (.png, .jpg, .jpeg) から 1 ページ PDF を生成・取込み可能
COMPATIBILITY: 通常版・Lite版の両方で利用可能 (完全ローカル処理)
EVIDENCE:
  - Document/How_to_Use.md
  - for_ai/core/file_formats.md
```

---

## 2. SYSTEM_CHARACTERISTICS

```yaml
[NODE: NON_DESTRUCTIVE_PERSISTENCE]
FACT: PDF原本を直接上書き変更しない
MECHANISM: 注釈は外部ファイル (.clrop) に保存
EVIDENCE: Document/How_to_Save_and_Recovery.md

[NODE: WORKSPACE_INTEGRATION]
FACT: PDF、注釈、ノート、関連資料を作業単位として同一ワークスペースで保持
EVIDENCE: for_ai/project_context.xml#design_philosophy

[NODE: LOCAL_ONLY_OPERATIONS]
FACT: ネットワーク通信、クラウド依存、外部データ送信機能は存在しない
EVIDENCE: AGENTS.md, README.md

[NODE: PORTABLE_EXECUTION]
FACT: Windows対応、インストーラー不要のポータブル実行
EVIDENCE: README.md
```

---

## 3. SOUND_BEHAVIOR_AND_KNOWN_ISSUES

```yaml
[NODE: SILENT_OPERATION]
POLICY: アプリ側からの能動的な音声・通知音の再生なし
UNRESOLVED_ISSUE: 特定のWindows操作経路で「一般の警告音」が発生する既知問題あり
ANSWER_STANCE: 「完全無音」と断定せず、Windows一般警告音の既知問題の存在を説明すること
EVIDENCE:
  - Document/How_to_Troubleshoot.md
  - for_ai/project_context.xml#non_negotiable_constraints
```
