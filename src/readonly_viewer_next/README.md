# 読み取り専用ビューアの独立試作

正式側へ採用した新しい読み取り専用ビューアの比較用入口です。実装正本は `../readonly_viewer/` に置き、ここでは同じ実装のクラス・タイトル・設定名を分離して起動します。正式側は既存の `readonly_viewer.exe` とメインソフトの起動先を維持し、通常のビルド・梱包に反映します。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src/readonly_viewer_next/build.ps1 -Locale ja
powershell -NoProfile -ExecutionPolicy Bypass -File src/readonly_viewer_next/run.ps1 -Locale ja
```

ファイルを指定する場合は `run.ps1 -Path "C:\資料\文書.pdf"`。英語試作は両方に `-Locale en` を指定します。ダウンロードや依存の自動取得はありません。ローカルの既存MinGW、PDFium、MD4Cが必要です。

実行ファイルは `out/readonly_viewer_next/<locale>/bin/readonly_viewer_next.exe`。設定とセッションもこの実行ファイルのディレクトリ内に置き、正式版と分離します。

正式版は `scripts/build/build_readonly_viewer.ps1 -Locale ja` で `out/bin/readonly_viewer.exe`、ENは `-Locale en` で `out/bin_en/readonly_viewer.exe` を生成します。既存の `readonly_setup.json` を使用し、比較用設定を取り込みません。版番号・公開・正式配布の受け入れは通常のrelease手順に従います。

実行にはexe、PDFium DLL、MinGWの3 DLLが必要です。持ち運び・配布時は、同じディレクトリの `LICENSE.md` と `licenses/` も保持してください。PDFiumの内包部品、exeへ組み込むMD4C、GCCの実行ライブラリ例外、winpthreadsを含む原文をビルド時に同梱します。「ライセンス」メニューで案内を開き、左の一覧から各原文へ移動できます。外部ブラウザは起動しません。

`tools/dev/prepare_readonly_viewer_next_licenses.py` は文書の欠落・空入力・PDFium通知一覧の変更・同梱後のbyte不一致を失敗として扱い、出所とSHA-256を `licenses/manifest.json` に記録します。再検査は `--locale ja --output out/readonly_viewer_next/ja/bin --check`。正式な公開・配布の受け入れ検査は別途必要です。

左のファイル一覧・目次、上のタブ、右の本文という役割を維持し、読書表示の行幅・余白・行間と、表示切替・検索・倍率操作を改善しました。PDF、Markdown、原文、バイト表示、対応するMermaid図、既存のタブ操作を利用できます。

初回レビューを反映し、上部を62論理px薄くして領域を色で区別しました。表は列境界と折り返しを持つネイティブ表示です。本文と目次をタブごとに準備して再解析を減らし、表示モードごとの読書位置も保持します。

ファイル欄では戻る・上へ移動、現在の文書の選択表示を利用できます。タブはドラッグで並べ替え、別の試作ウィンドウへ移動、またはウィンドウの外で離して分離できます。受け取り先で読めない場合は元のタブを保持します。最後のタブの移動が成功すると、移動元のウィンドウとプロセスを終了します。通常のタブを閉じる操作だけでは自動終了しません。

対応するMermaidを本文内でローカル描画します。未対応の記法は原文を表示します。ブラウザや外部通信、入力由来のスクリプト・RTF/OLEの実行、任意の外部画像の自動取得はありません。

`.clro` はMarkdownノートとして、`.clrop` は注釈情報として表示します。コードブロックの境界判定はメインソフトと共通です。長いバッククォートやチルダで囲んだコード例の図・見出しを誤認せず、閉じ忘れたMermaidも本文を残します。図一覧と別ウィンドウ表示は準備済みの図を再利用します。

PDFは開いた際に幅へ合わせ、文字を読みやすく表示します。PDFの「標準倍率」も現在のページを幅に合わせます。元エンジンの倍率上限内で調整します。

| 操作 | キー |
| --- | --- |
| ファイル / フォルダーを開く | Ctrl+O / Ctrl+Shift+O |
| 本文検索 | Ctrl+F |
| 次 / 前の一致 | F3 / Shift+F3、検索欄のEnter / Shift+Enter |
| 集中表示 | Ctrl+B |
| タブ切替 / 逆順 | Ctrl+Tab / Ctrl+Shift+Tab |
| タブを閉じる / 開き直す | Ctrl+Shift+Q / Ctrl+Shift+T |
| 読書 / 原文 / バイト / 図 | Ctrl+1 / 2 / 3 / 4 |
| 拡大 / 縮小 / 標準倍率 | Ctrl++ / Ctrl+- / Ctrl+0 |
| PDF検索の中止 | Esc |

テキスト検索は現在の表示内容を対象に、大文字小文字を区別せず折り返します。PDFは読み込み済み文書の文字をページ単位で検索し、該当ページの位置へ移動します。画像だけのPDFにOCRは追加していません。図だけの表示では本文検索を無効化します。

検証入口:

```powershell
python tests/python/readonly_viewer_next_app_smoke.py --exe out/readonly_viewer_next/ja/bin/readonly_viewer_next.exe
python tests/python/readonly_viewer_next_app_smoke.py --exe out/bin/readonly_viewer.exe --variant standard --locale ja
```

Pillow・PyMuPDFはこの検査だけで使用し、アプリへ同梱しません。結果と画面キャプチャ、検査所有の新規実行用コピーは `out/tests/readonly_viewer_next/` 内に保存します。自動検査を実IME、実キーボードフォーカス、OSのDPI切替、聴覚による無音確認の代わりにはしません。

構造と移行判断は [試作設計](../../docs/internal/ui_読み取り専用ビューア試作設計_2026-10-11.md) を参照してください。
