# AI_CORE_NODE: REPOSITORY_REFERENCE_MAP

<ai_node_schema version="1.0" category="repository-navigation-and-evidence">
  <summary>質問の種類ごとに、AI資料、利用者向け文書、ソース、テスト、設定のどこを確認するかを示す資料地図</summary>
  <verification_rule>資料地図は探索の起点であり、実装済みの事実を単独で証明しない。結論には指定された一次資料を用いる。</verification_rule>
</ai_node_schema>

## 1. 資料の優先順位

1. `For_AI.md` と `for_ai/manifest.json` で質問に対応する読取経路を選ぶ。
2. `for_ai/core/` の要約・索引で、確認すべき正本を絞る。
3. 利用者向けの挙動は `README.md` と `docs/public/` を確認する。
4. 実装、安全性、保存、配布物については、ローカルの `src/`、設定、`tests/`、検査結果を確認する。

| 質問・変更の領域 | まず読むAI資料 | 一次資料・確認範囲 |
| --- | --- | --- |
| 導入、更新、Lite版 | `core/user_guidance_reference.md` | `README.md`、`docs/public/How_to_Setup.md` |
| カスタマイズ、操作、画面 | `core/user_guidance_reference.md`、`core/ui_concepts.md` | `docs/public/How_to_Use.md`、`src/help/`、設定の読込・保存コード |
| 保存、stage、復元、データ形式 | `core/file_formats.md`、`core/data_schemas.md` | `docs/public/How_to_Save_and_Recovery.md`、`src/file_output/`、`src/clrop/`、関連テスト |
| 安全性、外部通信、無音 | `core/safety_and_nonnegotiables.md`、`core/troubleshooting_quick.md` | `AGENTS.md`、対象ソース、設定、検査結果。一般警告音は未解消の既知問題として扱う |
| コード構造・変更 | `core/architecture_layers.md`、`core/code_symbol_index.md`、`core/ai_guardrails.md` | `src/`、呼び出し元、`tests/`、ローカルの `AGENTS.md` |
| 公開・ライセンス・配布 | `core/documentation_contract.md` | `LICENSE.md`、`LICENSES_INDEX.md`、`THIRD_PARTY_NOTICES.md`、公開用設定 |

## 2. リポジトリと公開先の役割

| 場所 | 役割 | AIが注意する点 |
| --- | --- | --- |
| `DEV_PDF-Note-Workspace` の `dev` | 通常開発の正本。ソース、テスト、内部文書、Cloudflare紹介サイトを持つ | 実装の確認・変更はここで行う |
| `OPEN_PDF-Note-Workspace` の `main` | GitHub Pages用に選別した公開snapshot | アプリの `src/`、`tests/`、内部文書は含まれない。実装を検証済みと断定しない |
| GitHub Pages | `site/github/` が生成するAI向け文書ポータル | `For_AI.md`、manifest、公開済み一次資料を読む入口 |
| Cloudflare Pages | `site/cloudflare/public/` の紹介ページ | AI資料の正本を複製せず、GitHub Pagesの入口を案内する |

公開snapshotからの回答では、存在しないソースやテストを読んだように装わない。実装確認が必要なら、DEV作業ツリーまたは確認可能な一次資料が必要であることを明示する。

## 3. 検索と経路選択

- 質問意図から候補を絞るときは `core/semantic_search_index.json` の `intent`、`keywords`、`target_node` を使う。
- 実際に読む資料の集合は `for_ai/manifest.json` の `routes` を正本とする。
- 索引にない質問は、全資料を無差別に要約せず、近い経路を選んでから一次資料とソースの所在を確認する。

## 4. 開発・公開時の確認

- コード変更では、対象、呼び出し元、関連テストを確認する。保存・注釈・出力の変更では、読み込み、編集、stage、自動保存、統合保存、再読込、undo/redo、バックアップ・復元、export、失敗時を確認範囲に含める。
- GitHub Pagesを更新する場合は、`site/github/documentation_portal_allowlist.json` で公開対象を確認し、生成・検査を実行する。紹介ページは含めない。
- Cloudflare Pagesを更新する場合は、`site/cloudflare/public/` だけを対象とし、AI資料を複製しない。
- 問題報告・セキュリティ報告は `core/user_guidance_reference.md` と `.github/SECURITY.md` を確認する。利用可能な連絡手段を推測しない。

EVIDENCE:

- `For_AI.md`
- `for_ai/manifest.json`
- `for_ai/core/documentation_contract.md`
- `for_ai/core/user_guidance_reference.md`
- `docs/public/`
- `site/README.md`
- `site/github/documentation_portal_allowlist.json`
- `site/cloudflare/public/`
