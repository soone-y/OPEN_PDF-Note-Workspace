# AI_CORE_NODE: TROUBLESHOOTING_QUICK

<ai_node_schema id="troubleshooting_quick" type="issue_diagnosis_map">
  <target_audience>LLM / AI-Assistant</target_audience>
  <data_density>high</data_density>
</ai_node_schema>

---

## 1. ISSUE_DIAGNOSIS_INDEX

```yaml
[ISSUE: PDF_CANNOT_OPEN]
POSSIBLE_CAUSES: ファイル破損, 暗号化/権限不足, メモリ不足
EVIDENCE: Document/How_to_Troubleshoot.md

[ISSUE: OFFICE_CONVERSION_FAILED]
POSSIBLE_CAUSES:
  - Lite版を使用中 (LibreOffice runtime非同梱)
  - 特殊フォント/マクロ/段組みによるレイアウト崩れ (試験的機能)
ANSWER_STANCE: Lite版は変換非対応を説明。通常版でも試験的機能であることを提示
EVIDENCE:
  - README.md
  - for_ai/project_context.xml#office_conversion_context
  - Document/How_to_Troubleshoot.md

[ISSUE: SAVE_FAILED]
POSSIBLE_CAUSES: フォルダアクセス権限不足, ディスク容量不足, stageデータ非整合
EVIDENCE:
  - Document/How_to_Save_and_Recovery.md
  - Document/How_to_Troubleshoot.md

[ISSUE: WARNING_SOUND_EMITTED]
FACT: アプリ側からの音再生機能なし
UNRESOLVED_ISSUE: 特定のWindows操作経路で「一般の警告音」が発生する既知問題あり
ANSWER_STANCE: 「完全無音」と断定せず、Windows一般警告音の未解消既知問題を回答
EVIDENCE:
  - Document/How_to_Troubleshoot.md
  - for_ai/project_context.xml#non_negotiable_constraints
```
