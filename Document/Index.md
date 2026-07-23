# PDF Note Workspace 文書案内

対象アプリ版: 0.8.60

このフォルダは、利用者と開発者が目的別に読める文書の入口です。初めての方は、上から順に必要な文書だけ読めば使い始められます。

初めて使う場合は、まず [How_to_Setup.md](How_to_Setup.md) を読んで ZIP を展開し、その後 [How_to_Use.md](How_to_Use.md) を読んでください。

配布フォルダ内の EXE、DLL、`pdf_workspace_setup.json` は一緒に置いたまま使い、起動場所を変えたい場合はショートカットを作成してください。

## 通常版と Lite版の選び方

配布物には、Office ファイルを PDF に変換できる通常版と、変換 runtime を含めない Lite版があります。PDF 閲覧、注釈、ノート、保存・復元はどちらも同じです。

| 選ぶ版 | 向いている用途 | Office ファイルの扱い |
| --- | --- | --- |
| 通常版 | `.docx` / `.pptx` をアプリ内で PDF にして使いたい | 同梱 LibreOffice でローカル変換する。変換は試験的なため、結果を確認する |
| Lite版 | PDF を既に用意している、または配布サイズを小さくしたい | Office-to-PDF 変換は行わない。PDF を用意してから取り込む |

Lite版はウィンドウ名に `Lite` と表示されます。Lite版へ DOCX/PPTX をドロップしても変換・取込みはせず、Lite版では使えないことを表示します。Microsoft Office やオンライン変換サービスは、どちらの版でも使用しません。

## ファイル名が英語である理由

`How_to_Use.md` のような英語のファイル名は、Windows のフォルダ、ZIP 配布、開発用ツールでも同じ名前で確実に扱えるようにするための識別名です。読むために英語を理解する必要はありません。文書の見出しと本文は日本語です。

`How_to` は「使い方」の意味です。ファイル名は変更せず、表の日本語の案内から目的の文書を開いてください。

## 利用者向け

| 文書 | 読む場面 |
| --- | --- |
| [How_to_Setup.md](How_to_Setup.md) | ZIP の展開、ショートカット、関連付け、更新、削除を行いたい |
| [How_to_Use.md](How_to_Use.md) | 起動、ワークスペース、ノート作成、基本保存を知りたい |
| [What_is_File_Formats.md](What_is_File_Formats.md) | `.clro`、`.md`、`.txt`、`.clrop` の違いを知りたい |
| [How_to_Save_and_Recovery.md](How_to_Save_and_Recovery.md) | 保存、stage、バックアップ、復元を理解したい |
| [How_to_Troubleshoot.md](How_to_Troubleshoot.md) | 困ったときの一次対応と、解決のための知識を得たい |

アプリ内では「ヘルプ」から同じ主題をカテゴリ別に確認できます。ヘルプは外部サイトへ接続しません。

## 開発者向け

| 文書 | 読む場面 |
| --- | --- |
| [How_to_Build.md](How_to_Build.md) | ソースからビルド、テスト、release 作成を行いたい |

設計・調査・運用上の詳細は `docs/internal/` 配下にあります。通常の利用者が読む必要はありません。

## ライセンス

- リポジトリでは、ルートの `LICENSE.md`、`LICENSES_INDEX.md`、`THIRD_PARTY_NOTICES.md` を参照してください。
- 配布物では、同じファイルを `docs/` フォルダに同梱しています。
