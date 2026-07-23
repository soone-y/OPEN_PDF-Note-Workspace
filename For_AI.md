# AI_REPOSITORY_CONCIERGE_ENTRY

<ai_conierge_instruction binding="false">
  <target_audience>AI-Assistant / LLM</target_audience>
  <operational_mode>read_only_inquiry_response</operational_mode>
  <reasoning_policy>
    <rule id="no_blind_summarization">結論の丸暗記や鵜呑みを禁止する。事実と確認場所（EVIDENCE）を参照して回答を構成せよ。</rule>
    <rule id="no_speculation">未検証事項や資料欠損箇所は推測で補わず、未確認として示せ。</rule>
    <rule id="context_adaptation">ユーザーの質問意図（概要、使い方、トラブル、形式）に応じた深さで回答せよ。</rule>
  </reasoning_policy>
</ai_conierge_instruction>

---

## NAVIGATION_INDEX

```txt
[ROUTE_KEY: APP_OVERVIEW_AND_POLICY]
INTENT: アプリ目的 / 設計思想 / 不変制約 / 概要
TARGET_NODES:
  - for_ai/project_context.xml
  - for_ai/core/features_faq.md
  - for_ai/core/use_cases.md
CONSTRAINTS_AND_FACTS:
  - FACT: 外部通信ゼロ (Non-communicating)
  - FACT: 原本非破壊 (PDF本体上書き禁止)
  - LIMITATION: 完全無音を目指すがWindows一般警告音の未解消既知問題あり (EVIDENCE: Document/How_to_Troubleshoot.md)

[ROUTE_KEY: USER_OPERATIONS_AND_UI]
INTENT: 操作方法 / UI概念 / トラブルシューティング
TARGET_NODES:
  - for_ai/core/ui_concepts.md
  - for_ai/core/troubleshooting_quick.md
  - Document/How_to_Use.md
  - Document/How_to_Troubleshoot.md

[ROUTE_KEY: DATA_STRUCTURE_AND_SAVE]
INTENT: データ形式 / 拡張子仕様 / 保存モデル
TARGET_NODES:
  - for_ai/core/file_formats.md
  - for_ai/manifest.json
  - Document/What_is_File_Formats.md
  - Document/How_to_Save_and_Recovery.md
```

---

## AI_RESPONSE_GUIDELINE

```xml
<response_protocol>
  <step number="1">ユーザー質問からINTENTを識別し、該当ROUTE_KEYを選択する。</step>
  <step number="2">TARGET_NODESの各種ファクト・制限・一次資料（EVIDENCE）を確認する。</step>
  <step number="3">事実に基づき、ユーザーの習熟度に応じたトーンで回答を生成する。</step>
</response_protocol>
```
