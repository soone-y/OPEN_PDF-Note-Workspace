# MicroTeX比較実験

Windowsのオフスクリーン描画で、現行の `mathrender` とMicroTeXの単体APIへ同一入力を直接渡して比較する開発用ツール。数式本文を読み取り、画像、計測値、比較HTMLを新規の `out/` 配下へ出力する。アプリ内の表示比較は、前処理・フォント・DPIをそろえた実アプリ画像で確認する。実験結果と既知の不足は検証レポートに記録した。

- [2026-11-16の検証レポート](../../../docs/internal/archive/reports/note_MicroTeX比較検証レポート_2026-11-16.md)
- [openmath＋XITSの試描画と容量](../../../docs/internal/archive/reports/note_MicroTeX新版とXITS試描画レポート_2026-11-16.md)

## ローカル入力を準備する

今回使用した公式入力は次の固定commit。取得・展開は開発者が明示して行い、取得先のURLとartifact識別情報だけを送る。通常のアプリbuild、比較ツール、検査から自動取得しない。

| 入力 | 固定commit | 取得ZIPのSHA-256 |
| --- | --- | --- |
| MicroTeX | `0e3707f6dafebb121d98b53c64364d16fefe481d` | `9da82a0d05ff4edec080938570ac86e3024c8d461c2d739a5eb3b6259aa69c1a` |
| tinyxml2 | `321ea883b7190d4e85cae5512a12e5eaa8f8731f`（10.0.0） | `23100b3c4e3b2d528fdf9b490d68b1fd39ad91c5306d804d4fd2c2a5ef959471` |

- [MicroTeXの固定ZIP](https://codeload.github.com/NanoMichael/MicroTeX/zip/0e3707f6dafebb121d98b53c64364d16fefe481d)
- [tinyxml2の固定ZIP](https://codeload.github.com/leethomason/tinyxml2/zip/321ea883b7190d4e85cae5512a12e5eaa8f8731f)

取得物・展開物・利用者の入力コピーはGit管理外の実験領域に保持する。`res` 一式とライセンス原文を保持する。MicroTeX本体と資源のライセンスは個別に確認する。

## 比較プログラムをビルドする

repo rootから実行する。以下は既存の実験領域を使う例。別の領域を使う場合は絶対パスを指定する。

```powershell
$trialRoot = (Resolve-Path -LiteralPath out/microtex_trial_20261116).Path
cmake -S tools/dev/microtex_trial -B "$trialRoot/build" -G Ninja `
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe `
  -DCMAKE_BUILD_TYPE=Release `
  "-DMICROTEX_SOURCE_DIR=$trialRoot/MicroTeX-0e3707f6dafebb121d98b53c64364d16fefe481d" `
  "-DTINYXML2_SOURCE_DIR=$trialRoot/tinyxml2-321ea883b7190d4e85cae5512a12e5eaa8f8731f"
if ($LASTEXITCODE -ne 0) { throw '比較用configure失敗' }
cmake --build "$trialRoot/build" --parallel 4
if ($LASTEXITCODE -ne 0) { throw '比較用build失敗' }
```

確認したcompilerはMSYS2 UCRT64のGCC 15.2.0。NinjaがPATHにある環境を使う。MinGW版はC++ runtimeを静的リンクする。GDI+ backendはbuild領域へコピーして未使用の `comdef.h` includeを除去し、その翻訳単位だけWindowsヘッダーを先に読み込む。取得した公式ソースはそのまま保持する。MSVC構成は未検証。

## 同じ数式を比較する

`input.clro` に今回指定された `6.clro` のコピーを置く。入力の形式は、行全体が `\[` と `\]` の区切りで囲まれた7ブロック。見出しを比較画像の題名に使う。

```powershell
python tools/dev/microtex_trial/compare.py `
  --exe "$trialRoot/build/microtex_trial.exe" `
  --resources "$trialRoot/MicroTeX-0e3707f6dafebb121d98b53c64364d16fefe481d/res" `
  --input "$trialRoot/input.clro" `
  --output "$trialRoot/comparison_new" `
  --font-px 16 24 36 --iterations 30
if ($LASTEXITCODE -ne 0) { throw '比較で未完了または失敗を検出' }
```

出力先には新規ディレクトリを指定する。`comparison.html` から原寸画像と各計測値を確認できる。`summary.json` は全条件の結果、`identity.json` は入力・実行ファイル・比較コード・資源のhashを持つ。式数を変える場合は `--expected-blocks` を合わせる。

現行側は `Parse` と `Measure`、MicroTeX側はstrict parserと `TeXRenderBuilder` で準備する。`first_prepare_ms` はprocess内の初回準備、`prepare_ms` は反復した準備の1回平均、`draw_ms` は準備済み数式の反復描画の1回平均。各条件を独立processで実行し、フォント資源を描画前にwarm upする。初期化は `init_ms` に別記する。描画画像は反復後に一度描き直す。

## 結果と失敗の扱い

`render_completed` はprocessの正常終了、必須計測値、画像ファイルの生成を確認した状態。数式の正確性は原文と画像を目視照合する。`quality_review` は目視確認前の `pending` を保持し、判定は検証レポートへ記す。

子processは30秒、各logは64 KiBを実行中に監視する。予算超過・timeout・取消時は所有する子processを終了して回収する。失敗した条件のlogと途中成果物を保持し、runnerは非ゼロで終了する。既存出力の上書きを拒否し、入力のhashを終了時にも照合する。強制終了した親processからの子回収は未検証。

数百式を含む実ノートのスクロール、DPI変更、PDF注釈の保存・再読込・出力は、アプリへ接続した段階の確認対象となる。

## openmath版とXITSを比較する

確認した新版は `086f4eb740270b28bd0c61a0a359aea9300d61ae`。取得済みの公式ソースを使い、旧版とは別のbuild directoryで構成する。

```powershell
$openmathRoot = (Resolve-Path -LiteralPath out/microtex_openmath_trial_20261116).Path
cmake -S tools/dev/microtex_trial -B "$openmathRoot/build" -G Ninja `
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe `
  -DCMAKE_BUILD_TYPE=Release -DMICROTEX_OPENMATH=ON `
  "-DMICROTEX_SOURCE_DIR=$openmathRoot/MicroTeX-086f4eb740270b28bd0c61a0a359aea9300d61ae"
if ($LASTEXITCODE -ne 0) { throw 'openmath構成失敗' }
cmake --build "$openmathRoot/build" --target microtex_trial --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'openmathビルド失敗' }
```

`--resources` は `res/xits` または必要な資源を置いた明示ディレクトリへ向ける。`--microtex-backend openmath` をrunnerへ渡すと、画像のラベルと実験情報へ版の区別を記録する。ビルドした実行ファイルのbackendと引数を合わせる。

本構成は `GLYPH_RENDER_TYPE=1` で輪郭描画を使用する。付属GDI実装の空の輪郭callbackを `openmath_gdi_path.h` で補う。XITSの `XITSMath-Regular.clm2` を明示パスから読み、contextの寿命に合わせてメモリー上に保持する。これにより、上流のnarrow `fopen` が失敗する日本語資源パスにも対応する。数式はpublicなstrict `Parser` と `RenderBuilder` で準備する。

自動font探索と上流のlogはbuild時に無効化し、特権macroも実行時に無効化する。フォント資源は `XITSMath-Regular.clm2`、`OFL.txt`、`FONTLOG.txt`、`README.txt`、本体の `LICENSE` を残した構成で確認した。日本語はWindowsの文字描画を使う。実用化の残件と試描画の失敗条件は上記レポートに記録した。
