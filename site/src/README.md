# サイト専用ソース

`site/output/public/` は Cloudflare と GitHub Pages が公開する追跡済み成果物です。

現在は、`public_site_allowlist.json` が許可するリポジトリ正本の `For_AI.md`、`for_ai/`、`Document/`、ライセンス資料、画像から公開サイトを生成します。許可リストにないファイルは公開されません。サイト独自のテンプレート、文章、画像、設定を追加するときはこの `site/src/` に置き、許可リストと `site/scripts/` の生成処理で明示的に公開対象へ追加します。

`site/output/public/` を手で編集しません。`python site/scripts/build_public_site.py --replace` で再生成し、`python site/scripts/validate_public_site.py --site site/output/public` で検査します。
