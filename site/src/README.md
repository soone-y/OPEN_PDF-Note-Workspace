# サイト専用ソース

`site/output/public/` は Cloudflare と GitHub Pages がビルド時に公開する生成物です。Gitでは追跡しません。

`public_site_allowlist.json` は、Cloudflare用の `introduction` とGitHub Pages用の `documentation_portal` を分けて定義します。`introduction` は紹介ページと紹介用画像だけを、`documentation_portal` はAI資料、利用者向け資料、ライセンス資料、画像を公開します。許可リストにないファイルは公開されません。サイト独自のテンプレート、文章、画像、設定を追加するときはこの `site/src/` に置き、対象プロファイルの許可リストと `site/scripts/` の生成処理で明示的に公開対象へ追加します。

`site/output/public/` を手で編集しません。`python site/scripts/build_public_site.py --replace` で再生成し、`python site/scripts/validate_public_site.py --site site/output/public` で検査します。
