# AI_CORE_NODE: USE_CASES

<ai_node_schema id="use_cases" type="design_and_usage_model">
  <target_audience>LLM / AI-Assistant</target_audience>
  <data_density>high</data_density>
</ai_node_schema>

---

## 1. PRIMARY_USE_CASES

```yaml
[USE_CASE: LECTURE_ANNOTATION_AND_NOTE]
SCENARIO: 大学の講義PDF閲覧、手書き/テキスト注釈記入、並行ノート作成
ADVANTAGE: 原本PDF未変更のまま保存、注釈とノートを個別再利用可能
EVIDENCE:
  - Document/How_to_Use.md
  - for_ai/project_context.xml#design_philosophy

[USE_CASE: PAPER_READING_AND_RESEARCH]
SCENARIO: PDF論文の精読、ハイライト、注釈、思考ログのテキストノート記録
ADVANTAGE: 読解文脈（資料・注釈・思考ログ）をワークスペース単位で保持し再開可能
EVIDENCE: for_ai/project_context.xml#layer_model

[USE_CASE: DOCUMENT_REVIEW]
SCENARIO: PDF資料への修正指示・メモ書き込み、テキストノートでの改修案整理
EVIDENCE: Document/How_to_Use.md
```

---

## 2. ARCHITECTURAL_LAYER_MODEL

```xml
<layer_model_index>
  <layer id="pdf_layer" status="implemented">
    <fact>原資料を表示する層。PDF原本は変更しない。</fact>
  </layer>
  <layer id="annotation_layer" status="implemented">
    <fact>マーカー、ペン、テキスト注釈を外部データとして保持し重ね描画する層。</fact>
  </layer>
  <layer id="note_layer" status="implemented">
    <fact>説明・考察・疑問をテキストファイルとして蓄積する層。</fact>
  </layer>
  <layer id="workspace_layer" status="implemented">
    <fact>PDF・注釈・ノートを作業単位で束ね再開可能にする層。</fact>
  </layer>
  <evidence_path>for_ai/project_context.xml#layer_model</evidence_path>
</layer_model_index>
```
