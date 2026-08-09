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
4. 公開サイト上の回答では、AI資料だけで止めず、示された `docs/public/`、README、ライセンス文書などの公開一次資料を確認する。公開資料に根拠がない実装状況は、未確認として扱う。

経路の詳細と資料の対応は `manifest.json` だけで管理する。この文書へ質問別の `ROUTE_KEY` や `TARGET_NODES` を重複して追加しない。

`for_ai/` は単なる索引ではない。公開可能な事実と設計意図を、質問に答えるためにあらかじめ解釈・整理した回答基盤である。選んだ経路の資料を使って説明を組み立て、実装保証、安全性の網羅性、性能、特定配布物の内容など強い客観的主張が必要な場合だけ、示された一次資料を追加確認する。

`for_ai/` はAI向けに、質問意図・回答の境界・根拠への経路を機械的に追いやすい構造で記述している。人も読める公開文書だが、利用者の操作を案内する文書ではない。操作や導入には `docs/public/` を案内する。

## CLAIMS_AND_VERIFICATION

```text
POLICY: 配布するアプリ本体、依存関係、設定、ビルド成果物には外部通信を実装しない。
POLICY: PDF原本を直接上書きせず、ユーザーのファイルと作業データを失わない設計を優先する。
LIMITATION: Windowsの一般警告音が鳴る未解消の既知問題がある。完全な無音を断定しない。

VERIFICATION_REQUIRED:
  - 公開資料から確認できる事実と、公開資料だけでは確認できない実装状況を区別する。
  - 回答では、確認済み事実、方針、既知の制約、推論、未確認を区別する。
  - この公開文書ポータルは、利用者向けの説明と参照資料である。開発用の変更手順や内部資料を前提にしない。
```

公開資料にない事項は推測で補わず、必要なら利用者に版や状況を確認する。
