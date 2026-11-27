# ビルドと検証

同梱リポジトリ版: (ZIP配布物ではここにバージョンが記載されます)

この文書は、ソースからビルドする開発者向けです。配布物を通常利用するだけの場合は [Using_the_App.md](Using_the_App.md) を参照してください。

## 前提

- Windows 10 / 11
- PowerShell
- `g++`、`gcc`、`windres` が `PATH` にあること
- Python が `PATH` にあること（ビルド時のロケールカタログ生成と、`release.ps1` の事前検査に使います）
- `third_party/pdfium/lib/pdfium.dll.lib` があること
- `third_party/pdfium/bin/pdfium.dll` があること
- `third_party/` の zlib・md4c・MicroTeX があること（リポジトリに同梱）
- リポジトリ版番号は `REPO_VERSION.txt` で管理すること

依存関係はリポジトリに vendor 配置し、ビルドや実行時に外部通信を行わない方針です。

通常版・Lite版・閲覧専用ビューアーのビルドでは、C++17とC11を明示指定します。コンパイラーの既定の言語規格には依存しません。

## ビルド

```powershell
./build.ps1
```

`build.ps1` は通常版、Lite版、閲覧専用ビューアを Release 構成でビルドします。サンプル付きの動作確認環境を用意するには `./build.ps1 -Run` を実行し、`out/run/pdf_note_workspace.exe`（通常版）または `out/run_lite/pdf_note_workspace.exe`（Lite版）を起動します。`-Locale en` では `out/run_en/` と `out/run_lite_en/` を使います。各フォルダーに閲覧専用ビューアも入ります。

初回は配布と同じ言語・版の `sample_workspace/` を用意し、未設定ならこれを開く設定を作ります。以後のビルドでは、サンプルへの編集や設定の変更を保持します。起動中のアプリにバイナリの更新が必要な場合は、閉じてからもう一度ビルドしてください。

- `./build.ps1 -Clean`: `out/obj*` と `out/bin*` の対象成果物を削除します。`-Run` も付けると起動用バイナリも削除しますが、サンプルと設定は残します。
- `./build.ps1 -HardClean`: `-Clean -Run` の対象に加え、表示された `out/run*/sample_workspace/` を編集内容ごと削除します。対象のサンプルを使うアプリを閉じてから実行してください。設定や別のワークスペースは保持し、次回の `./build.ps1 -Run` でサンプルを初期状態から作ります。

どちらも通常版とLite版が対象です。`-Lite` を付けるとLite版の起動環境だけ、`-AllLocales` を付けると両言語の起動環境が対象になります。`out/run*/` はリポジトリ内での確認用です。通常版のOffice変換はリポジトリ内に検証済みの変換ランタイムがある場合に利用できます。配布物の確認には従来どおりrelease setを使います。

個別にビルドする場合は、詳細入口を直接使います。

```powershell
./scripts/build/build_workspace.ps1 -Edition Full
./scripts/build/build_workspace.ps1 -Edition Lite
./scripts/build/build_readonly_viewer.ps1
./scripts/build/build_workspace.ps1 -Edition Full -Rebuild
```

`Full` はOffice-to-PDF変換を含む通常版、`Lite` は変換機能を持たないLite版です。`build_readonly_viewer.ps1` は旧 TextViewer を統合した、配布用の唯一の閲覧専用ビルド対象です。`src/readonly_viewer_next/build.ps1` は比較用の試作版で、`out/readonly_viewer_next/` に出力し、配布物には含めません。

出力先は、通常版アプリと閲覧専用ビューアが `out/bin/`、Lite版アプリが `out/bin_lite/` です（`-Locale en` では `out/bin_en/` と `out/bin_lite_en/`）。どちらのアプリも配布時には `pdf_note_workspace.exe` という同じ名前になり、対応する配布フォルダ内にだけ入ります。

## 検証

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_repo_checks.ps1
python tests/python/validate_codebase.py
python -m unittest tests/python/test_python_tools.py
```

`run_repo_checks.ps1` は、`REPO_VERSION.txt` と最新ビルドの Full / Lite / 閲覧専用ビューアの `.buildinfo.txt` を比較し、版番号の不一致を検出します。通常の検査で `-SkipBuild` を付けた場合も、既存のビルド成果物についてこの確認を行います。配布版番号はrelease setのmanifest、ZIP、チェックサム、locale別タグとRelease Notesで固定・照合されます。日本語の利用者文書の3行目には「同梱リポジトリ版:」で始まる版番号マーカーを置き、release作成時に `pack_release.ps1` が `REPO_VERSION.txt` の版へ置き換えます。公開サイトではこの行を表示せず、各ページのタイトル下に最終更新の日時と版を表示します。

検査の一覧は `./tests/scripts/run_repo_checks.ps1 -ListSteps` で確認できます。関連するステップだけを実行する例は次のとおりです。複数指定は PowerShell から配列で渡します。部分実行は全体の合格を意味せず、選択した検査の前提や既存成果物は各自で用意してください。追加検査は従来の `-Include...` 指定も必要です。指定したステップが省略された場合は失敗します。全体の通常検査は上記の引数なしのコマンドです（追加検査は別途指定）。

```powershell
./tests/scripts/run_repo_checks.ps1 -OnlyStep 'Text Integrity Gate','Safety Scans'
```

能動コンテンツへの耐性検査は `./tests/scripts/run_repo_checks.ps1 -OnlyStep 'Script Resistance Tests'` で選べます。PDFiumの実入力処理と関連する静的契約を確認します。アプリ全体の脆弱性不在を保証する検査ではありません。

保存処理を変更した場合は、保存系テストも実行してください。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_atomic_write_tests.ps1
```

## release 作成

```powershell
./release.ps1
# 両言語のrelease setを作る場合:
./release.ps1 -AllLocales
```

配布時は `out/` を直接渡さず、release set 内に作られる通常版と Lite版のフォルダまたは ZIP を使います。`./release.ps1` は通常版・Lite版・閲覧専用ビューアをビルドしてから、配布物、ZIP、チェックサム、公開スナップショットを作ります。既定では `-Locale ja` の1言語だけを作り、両言語には `-AllLocales` を付けます。出力先は既定でリポジトリの外（親フォルダーの `PDF-Note-ReleaseSet/`）です。`-Lite` は指定できません。ZIP名には言語とeditionを含め、展開後の最上位フォルダは `PDF-Note-Workspace-<version>` です。Lite版だけを開発用にビルド・実機確認したい場合は `./build.ps1 -Lite` を使います。通常版には検証済み LibreOffice conversion runtime を必ず同梱し、Lite版には同梱しません。通常版からruntimeだけを除いた配布物は作れません。通常利用者には、二つの版を混ぜず、用途に応じてどちらか一方を配布してください。

配布名、出力先、同梱物などを個別に調整する場合だけ、`scripts/release/make_release_set.ps1` または `scripts/release/pack_release.ps1` を使います。

通常版はLibreOffice変換runtimeと表示用の私用フォントを `lo/` にまとめ、フォントを `lo/image/Fonts/` に置きます。Lite版はフォントのみを `libreoffice/image/Fonts/` に同梱します。両版のライセンス資料は `licenses/libreoffice/` に置きます。フォント探索は旧配布構成にも対応し、作成済みのrelease setは元の配置を保持します。
