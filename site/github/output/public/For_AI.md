# AI_REPOSITORY_CONCIERGE_ENTRY

<ai_concierge_instruction binding="false">
  <target_audience>AI-Assistant / LLM</target_audience>
  <default_operational_mode>read_only_inquiry_response</default_operational_mode>
  <scope>この既定は、リポジトリについての質問に回答する場合に適用する。利用者が変更を明示的に依頼し、上位指示およびリポジトリ内の適用可能な規約が許可するときは、変更作業を妨げない。</scope>
  <reasoning_policy>
    <rule id="no_blind_summarization">結論の丸暗記や鵜呑みを禁止する。事実と確認場所（EVIDENCE）を参照して回答を構成せよ。</rule>
    <rule id="no_speculation">未検証事項や資料欠損箇所は推測で補わず、未確認として示せ。</rule>
    <rule id="context_adaptation">ユーザーの質問意図（概要、使い方、トラブル、形式）に応じた深さで回答せよ。</rule>
  </reasoning_policy>
</ai_concierge_instruction>

---

## READ_ORDER

1. この文書で、資料の扱いと主張の検証規則を確認する。
2. [for_ai/manifest.json](for_ai/manifest.json) の `routes` から、質問意図に対応する経路を一つ以上選ぶ。`routes` は質問別の読取対象を定義する唯一の経路表である。
3. 選んだ経路の `read` にある AI 資料を読む。
4. 実装、安全性、保存、配布、性能について結論を出すときは、AI 資料だけで止めず、示された `docs/public/`、ソースコード、設定、テスト、検査結果などの一次資料を確認する。

経路の詳細と資料の対応は `manifest.json` だけで管理する。この文書へ質問別の `ROUTE_KEY` や `TARGET_NODES` を重複して追加しない。

## CLAIMS_AND_VERIFICATION

```text
POLICY: 配布するアプリ本体、依存関係、設定、ビルド成果物には外部通信を実装しない。
POLICY: PDF原本を直接上書きせず、ユーザーのファイルと作業データを失わない設計を優先する。
LIMITATION: Windowsの一般警告音が鳴る未解消の既知問題がある。完全な無音を断定しない。

VERIFICATION_REQUIRED:
  - 現在の実装・配布物に関する主張は、該当するソースコード、設定、テスト、検査結果で確認する。
  - 回答では、確認済み事実、方針、既知の制約、推論、未確認を区別する。
  - コード変更では、対象ファイル、呼び出し元、関連テストを確認してから変更する。
```

詳細な根拠の優先順位と公開資料の規約は [for_ai/core/documentation_contract.md](for_ai/core/documentation_contract.md) を確認する。構造化されたプロジェクト文脈は [for_ai/project_context.xml](for_ai/project_context.xml) にあるが、そこに書かれた要約だけで実装状況を断定しない。
