# ビルドと検証

対象アプリ版: 0.8.55

この文書は、ソースからビルドする開発者向けです。配布物を通常利用するだけの場合は [How_to_Use.md](How_to_Use.md) を参照してください。

## 前提

- Windows 10 / 11
- PowerShell
- `g++` が `PATH` にあること
- `third_party/pdfium/lib/pdfium.dll.lib` があること
- `third_party/pdfium/bin/pdfium.dll` があること
- アプリ版番号は `APP_VERSION.txt` で管理すること

依存関係はリポジトリに vendor 配置し、ビルドや実行時に外部通信を行わない方針です。

## ビルド

```powershell
./full_build.ps1
```

`full_build.ps1` は通常版、Lite版、閲覧専用ビューアをすべて Release 構成で作る全体入口です。引数なしの実行で、配布準備済みの実行ファイルを `out/` に作成します。

個別にビルドする場合は、詳細入口を直接使います。

```powershell
./scripts/build/build_workspace.ps1 -Edition Full
./scripts/build/build_workspace.ps1 -Edition Lite
./scripts/build/build_readonly_viewer.ps1
./scripts/build/build_workspace.ps1 -Edition Full -Rebuild
```

`Full` はOffice-to-PDF変換を含む通常版、`Lite` は変換機能を持たないLite版です。`build_readonly_viewer.ps1` は旧 TextViewer を統合した唯一の閲覧専用ビルド対象です。

出力先は、通常版アプリと閲覧専用ビューアが `out/bin/`、Lite版アプリが `out/bin_lite/` です。どちらのアプリも配布時には `pdf_note_workspace.exe` という同じ名前になり、対応する配布フォルダ内にだけ入ります。

## 検証

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_repo_checks.ps1
python tests/python/validate_codebase.py
python -m unittest tests/python/test_python_tools.py
```

`run_repo_checks.ps1` は、`APP_VERSION.txt` と最新ビルドの Full / Lite / 閲覧専用ビューアの `.buildinfo.txt` を比較し、版番号の不一致を検出します。文書に版番号マーカーがあることも確認します。`-SkipBuild` を付けた場合も、既存のビルド成果物についてこの確認を行います。release作成時には、このマーカーが `APP_VERSION.txt` の値へ置換されます。

保存処理を変更した場合は、保存系テストも実行してください。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/scripts/run_atomic_write_tests.ps1
```

## release 作成

```powershell
./full_release.ps1
```

配布時は `out/` を直接渡さず、release set 内に作られる通常版と Lite版のフォルダまたは ZIP を使います。`./full_release.ps1` は常に通常版・Lite版・閲覧専用ビューアを再ビルドしてから、両方の配布物、ZIP、チェックサム、公開スナップショットを作ります。通常版には検証済み LibreOffice conversion runtime を必ず同梱し、Lite版には同梱しません。通常版からruntimeだけを除いた配布物は作れません。通常利用者には、二つの版を混ぜず、用途に応じてどちらか一方を配布してください。

配布名、出力先、同梱物などを個別に調整する場合だけ、`scripts/release/make_release_set.ps1` または `scripts/release/pack_release.ps1` を使います。
