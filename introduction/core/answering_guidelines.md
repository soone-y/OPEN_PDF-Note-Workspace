# 実装と確認で守る方針

この資料は、実装・変更・評価を行う人が、プロジェクトの重要な方針を確認するための補足です。実際の変更では、対象コード、関連テスト、適用される開発規約を確認してください。

## 1. 避ける実装と設計

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
