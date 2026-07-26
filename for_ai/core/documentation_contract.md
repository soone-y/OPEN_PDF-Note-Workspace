# AI_CORE_NODE: DOCUMENTATION_CONTRACT

<ai_node_schema version="1.0" category="documentation-contract">
  <summary>AI向け資料の読み方、根拠の優先順位、公開前検査、誤情報を避ける維持規約</summary>
</ai_node_schema>

## AI の回答規約

1. `For_AI.md` で読取順と根拠の扱いを確認し、`for_ai/manifest.json` の `routes` から質問意図に対応する経路を選ぶ。質問別の経路定義は manifest だけを正本とする。
2. 実装、安全性、保存、配布、性能などの主張は、AI資料の要約だけで断定しない。示されたソース、設定、テスト、検査結果を確認する。
3. 回答では **確認済み事実**、**方針**、**既知の制約**、**推論**、**未確認** を区別する。資料が不足するときは推測で埋めない。
4. コード変更では、対象ファイル、呼び出し元、関連テストを確認してから変更する。存在を確認していないパス、クラス、関数を前提にしない。

## 文書構造の役割

| 層 | 役割 | AI の扱い |
| --- | --- | --- |
| `For_AI.md` | 質問の入口、読取順、根拠の扱い | 最初に読む。質問別の資料一覧は持たない |
| `for_ai/manifest.json` | 質問種別ごとの最小読取集合の唯一の経路定義 | `routes` から質問に合う経路を選び、指定された資料だけを段階的に読む |
| `for_ai/project_context.xml` | 重要制約・回答の枠組み | 一次資料への案内として使う |
| `for_ai/core/*.md` | 用途別の短い要約・索引 | 根拠と適用範囲を確認する |
| `Document/`、ソース、テスト、設定 | 一次資料 | 客観的な結論の根拠にする |

## 文字コード・構造化・画像の規約

- 公開するテキストは UTF-8（BOM の有無は可）で保存し、置換文字 `U+FFFD` を含めない。JSON と XML は構文として妥当でなければならない。
- AI向けの本文は、短い見出し、表、JSON/XML/YAML のような機械的に追いやすい構造を用いる。ただし構造化表現は内容の正確性を保証しない。
- コード索引に書くパスはリポジトリに実在し、シンボル名はそのパスで確認したものだけにする。古くなった索引は削除または「未検証」と明示する。
- 画像は人間向け説明を補助する場合にだけ用い、代替テキストを付ける。AIの根拠は画像だけに置かず、本文または一次資料で同じ事実を示す。

## 公開前の必須確認

GitHub Pagesの文書ポータルは次の順で生成・検査する。`site/github/output/public/` はローカルおよびCIで生成する公開成果物であり、Gitでは追跡せず、手で編集しない。Cloudflareの紹介ページはAI資料を含めず、`site/cloudflare/public/` をそのまま公開する。

```text
python site/github/scripts/build_public_site.py --replace --documentation-portal
python site/github/scripts/validate_public_site.py --site site/github/output/public
```

検査は、公開対象の限定、UTF-8復号、JSON/XML構文、ローカルのMarkdown/HTMLリンク・画像参照、manifestの経路、開発者専用資料の除外を確認する。意味内容や実装との一致は、変更者が一次資料を確認して判断する。サイトの更新は release や `publish.ps1 -Mode Submit` から切り離された手動作業であり、必要なときに上記の生成・検査を明示実行する。
