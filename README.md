# PDF Note Workspace

対象アプリ版: 0.8.52

Windows 向けの PDF 学習ワークスペースです。PDF 閲覧、非破壊注釈、ノート編集を 1 つのアプリにまとめています。

## GitHub から入手する

GitHub のアカウントや Git の知識は必要ありません。リポジトリの「Releases」（または右側の「Releases」欄にある最新版）を開き、使いたい版の **Assets** から ZIP を 1 つダウンロードしてください。画面の表示が異なる場合は、「Releases」の一覧から最新版を開いて **Assets** を展開します。

| 目的 | ダウンロードする ZIP |
| --- | --- |
| `.docx` / `.pptx` をアプリ内で PDF に変換したい | ファイル名に `Lite` が付かない通常版 |
| PDF だけを扱う、または小さい配布物を使いたい | ファイル名に `Lite` が付く Lite版 |

`Source code (zip)` や `Source code (tar.gz)` は開発用のソースコードで、アプリとしては使えません。選んだ ZIP をダウンロードした後は、[GitHub から取得した後のセットアップ手順](Document/How_to_Setup.md#github-から-入手する) に従って展開してください。

## 最初に読む文書

- 利用者向け文書の入口: [Document/Index.md](Document/Index.md)
- ZIP 配布のセットアップ: [Document/How_to_Setup.md](Document/How_to_Setup.md)
- まず使い始める: [Document/How_to_Use.md](Document/How_to_Use.md)
- ソースからビルドする: [Document/How_to_Build.md](Document/How_to_Build.md)

配布物では `docs/README.md` から同じ文書案内を開けます。アプリ内のヘルプも外部通信なしで利用できます。

## 通常版と Lite版

配布物には、Office ファイルを PDF に変換できる通常版と、変換 runtime を含めない Lite版があります。どちらも PDF 閲覧、非破壊注釈、ノート、保存・復元の機能は同じです。

| 選ぶ版 | 向いている用途 | Office ファイルの扱い |
| --- | --- | --- |
| 通常版 | `.docx` / `.pptx` をアプリ内で PDF にしてから扱いたい | 同梱 LibreOffice でローカル変換する。変換は試験的なため、結果を確認する |
| Lite版 | 既に PDF を用意している、または配布サイズを小さくしたい | Office-to-PDF 変換は行わない。PDF を用意してから取り込む |

Lite版はウィンドウ名に `Lite` と表示されます。DOCX/PPTX をドロップしても変換・取込みはせず、Lite版では使えないことを表示します。Microsoft Office やオンライン変換サービスは、どちらの版でも使用しません。詳しい選び方と操作は [How_to_Setup.md](Document/How_to_Setup.md) と [How_to_Use.md](Document/How_to_Use.md) を参照してください。

## 配布フォルダ内のファイルについて

配布フォルダ内の実行ファイル、DLL、`pdf_workspace_setup.json` は、基本的に同じフォルダのまま使ってください。`pdfium.dll` や C++ runtime DLL は実行ファイルが起動時に読み込むため、DLL だけを別フォルダへ移すと起動できなくなることがあります。`pdf_workspace_setup.json` も実行ファイルの場所を基準に読み込まれます。

使いやすい場所から起動したい場合は、配布フォルダの中身を移動せず、`pdf_note_workspace.exe` を右クリックしてショートカットを作成してください。ショートカットはデスクトップやスタートメニューに置けます。展開、ショートカット、更新、削除の手順は [Document/How_to_Setup.md](Document/How_to_Setup.md) を参照してください。

`How_to_*.md` という英語のファイル名は、配布・互換性のための識別名です。文書の内容は日本語で、`How_to` は「使い方」を意味します。詳しくは [文書案内の説明](Document/Index.md#ファイル名が英語である理由) を参照してください。

## 大切な方針

- 外部通信を実装しない
- 元ファイルを破壊しない
- 音を鳴らさない

注釈は PDF 本体と分離して保存し、原本を直接書き換えません。新規ノートには `.clro` を使います。`.md` / `.markdown`、`.txt`、PDF 注釈用 `.clrop` との違いは [Document/How_to_File_Formats.md](Document/How_to_File_Formats.md) を参照してください。

## リポジトリの入口

- `src/`: アプリ本体の C++ / Win32 実装
- `tests/`: 単体・統合・スクリプトテスト
- `scripts/`: ビルド、配布、検証スクリプト
- `Document/`: 利用者・開発者向けの目的別文書
- `docs/internal/`: 開発用の設計、調査、運用文書
- `release_assets/`: 配布用サンプルワークスペースなど

## 全体ビルドと配布

- `./full_build.ps1`: 通常版、Lite版、閲覧専用ビューアを Release 構成でまとめてビルドする入口です。引数なしで実行すると、配布準備済みの実行ファイルを `out/` に作成します。
- `./full_release.ps1`: 上記の全ビルド後、通常版・Lite版の配布フォルダと ZIP、チェックサム、公開スナップショットをまとめて作る入口です。引数なしで実行すると、配布可能な release set をリポジトリ外の `../PDF-Note-ReleaseSet/` に作成します。
- 個別のビルド条件や配布構成は、`scripts/build/build_workspace.ps1`、`scripts/build/build_readonly_viewer.ps1`、`scripts/release/make_release_set.ps1`、`scripts/release/pack_release.ps1` を直接使います。

## ライセンス

- プロジェクト本体: [LICENSE.md](LICENSE.md) (`zlib License`)
- サードパーティ通知: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
- ライセンス索引: [LICENSES_INDEX.md](LICENSES_INDEX.md)
