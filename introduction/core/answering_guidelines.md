# 回答と確認の指針

<ai_node_schema id="ai_guardrails" type="prohibited_patterns_and_rules">
  <target_audience>LLM / AI-Coding-Agent</target_audience>
  <data_density>ultra_high</data_density>
</ai_node_schema>

## 1. AI ガードレール & 絶対禁止パターン (Prohibited Code & Design Patterns)

AI がコード生成や変更提案を行う際に、**絶対に犯してはならないアンチパターンと禁止実装**のルールセットです。

```xml
<prohibited_rules>
  <rule id="no_network_code" severity="CRITICAL">
    <prohibited_pattern>
      WinInet, WinHTTP, Socket API (ws2_32.dll), curl, HTTP/HTTPS URL リテラルの実装
    </prohibited_pattern>
    <reason>最優先要件である「完全ローカル・外部通信ゼロ」を破るため</reason>
  </rule>

  <rule id="no_sound_code" severity="CRITICAL">
    <prohibited_pattern>
      PlaySound, Beep, MessageBeep, Media Foundation 音声再生
    </prohibited_pattern>
    <reason>最優先要件である「無音遵守 (Silent UI)」を破るため</reason>
  </rule>

  <rule id="no_pdf_direct_overwrite" severity="CRITICAL">
    <prohibited_pattern>
      PDF 原本ファイル (.pdf) に対する直接の書き込みオープンや上書き
    </prohibited_pattern>
    <reason>原本非破壊保護に違反するため。注釈は必ず別ファイル .clrop に記述すること</reason>
  </rule>

  <rule id="no_direct_file_replace_without_atomic" severity="HIGH">
    <prohibited_pattern>
      atomic_write::AtomicWriteUtf8 / AtomicWriteBytes を介さない直接ファイル置き換え
    </prohibited_pattern>
    <reason>クラッシュ・電源断時のデータ壊れゼロ（非破壊保存）を担保するため</reason>
  </rule>

  <rule id="no_unregistered_powershell_var" severity="MEDIUM">
    <prohibited_pattern>
      PowerShell スクリプトで Set-StrictMode 環境下での未宣言変数参照
    </prohibited_pattern>
    <reason>ビルド・リリース自動化スクリプトで VariableIsUndefined エラーを発生させるため</reason>
  </rule>
</prohibited_rules>
```
