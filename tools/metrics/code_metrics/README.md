# code_metrics

リポジトリの規模を、読み取り専用集計するためのツールです。

## できること

- ディレクトリ構造の要約表示
- ファイル数
- ファイルサイズ合計とディレクトリ別割合
- 行数
- 文字数
- 拡張子別内訳
- 関数定義数の概算
- 型定義数の概算
- `#include` / `import` の参照数
- 空行 / コメント行 / コード行
- TODO / FIXME / NOTE / BUG / HACK の件数
- 最長行長
- 大きいファイル、行数が多いファイル、関数が多いファイルの上位表示
- include/import が多いファイル、TODO が多いファイル、最長行が長いファイルの上位表示
- 精度注意の近似解析
  - 関数宣言候補数と参照回数
  - 変数宣言候補数と参照回数
  - 未使用候補の件数

## 対象モード

- `--scope own`
  - このリポジトリの自作物を中心に集計します
  - 既定では `src/`, `tests/`, `scripts/`, `tools/`, `docs/`, `README.md`, `AGENTS.md` を見ます
  - `.git/`, `.local/`, `out/`, `third_party/mingw_runtime_licenses/`, `third_party/pdfium/` などは除外します
- `--scope all`
  - `.git/` 以外のリポジトリ全体を集計します

## 実行例

```powershell
python tools/metrics/code_metrics/analyze_repo.py
```

```powershell
python tools/metrics/code_metrics/gui.py
```

```powershell
python tools/metrics/code_metrics/trace_symbols.py --symbol RunLibreOfficePdfConversion --string soffice.com
```

```powershell
python tools/metrics/code_metrics/trace_symbols.py --all --include third_party/libreoffice --path libcurl.dll --path sweb.exe
```

```powershell
python tools/libreoffice/libreoffice_reduce.py --phase1 --cache
```

```powershell
python tools/libreoffice/libreoffice_reduce.py --conversion-only --headless-only
```

```powershell
python tools/libreoffice/libreoffice_reduce.py --templates
```

```powershell
python tools/libreoffice/libreoffice_reduce.py --ui-locales-ja-en --authoring-data --stale-registry
```

```powershell
python tools/libreoffice/libreoffice_reduce.py --dictionaries-ja-en
```

```powershell
python tools/libreoffice/libreoffice_reduce.py --program-resources-ja-en
```

```powershell
python tools/libreoffice/libreoffice_reduce.py --scripting-runtime
```

```powershell
python tools/libreoffice/libreoffice_reduce.py --ui-icon-themes
```

```powershell
python tools/libreoffice/libreoffice_reduce.py --database-java
```

```powershell
python tools/libreoffice/libreoffice_reduce.py --calc
```

```powershell
python tools/libreoffice/libreoffice_reduce.py --nonconversion-leftovers
```

```powershell
python tools/libreoffice/libreoffice_reduce.py --phase1 --cache --apply
```

```powershell
python tools/release_checks/binary_scan.py --include third_party/libreoffice/image/program --query winhttp --query libcurl --query minidump
```

```powershell
python tools/release_checks/binary_scan.py --include path/to/binary.exe --format strings --all-strings
```

```powershell
python tools/release_checks/binary_scan.py --include third_party/libreoffice/image/program --imported-dll libcurl.dll
```

```powershell
python tools/libreoffice/libreoffice_source_scan.py --query webdav --query minidump --query update
```

```powershell
python tools/libreoffice/libreoffice_source_scan.py --query extensions/source/update/check --content --content-query UpdateCheck
```

```powershell
python tools/libreoffice/libreoffice_build_env_check.py
```

```powershell
python tools/libreoffice/libreoffice_build_env_check.py --format json
```

```powershell
python tools/metrics/code_metrics/analyze_repo.py --scope all
```

```powershell
python tools/metrics/code_metrics/analyze_repo.py --format json > tools/metrics/code_metrics/out/own_metrics.json
```

```powershell
python tools/metrics/code_metrics/analyze_repo.py --scope all --top-files 20 --top-dirs 15 --max-tree-depth 4
```

## オプション

- `--root`
  - 集計対象ルート。既定はカレントディレクトリ
- `--scope own|all`
  - 自作中心か全体かを切り替えます
- `--format text|json|strings`
  - 人間向けテキストか JSON かを切り替えます
  - `strings` は復元できた文字列だけを 1 行ずつ出力します
- `--top-files`
  - 上位表示するファイル数
- `--top-dirs`
  - 上位表示するディレクトリ数
- `--max-tree-depth`
  - ツリー表示の深さ
- `--include`
  - `own` モードの追加対象
- `--exclude`
  - 追加除外パス

## GUI

- `python tools/metrics/code_metrics/gui.py` でローカル GUI を起動できます
- `Root`, `Scope`, `Format`, `Top Files`, `Top Dirs`, `Tree Depth`, `Include`, `Exclude` を操作して集計できます
- 結果は画面表示に加えて `Save Output` で保存できます
- GUI も外部通信は行わず、対象ファイルを書き換えません

## 注意

- 関数数は確実性重視の簡易カウントです
- 型定義数、依存参照数、コメント行数も簡易集計です
- `approx_` で始まる項目は精度注意の近似解析です
- 近似解析は C/C++ と Python を対象に、正規表現ベースで候補を集計します
- `trace_symbols.py` は C/C++/Python/PowerShell の関数ブロックを近似認識し、関数名・呼び出し候補・任意文字列の出現位置を追跡します
- `trace_symbols.py --all --include third_party/libreoffice` は LibreOffice 配下だけを対象にできます
- `--string` はテキスト内容、`--path` はファイル名/パス名を検索します
- `tools/release_checks/binary_scan.py` は PE import と埋め込み ASCII/UTF-16 文字列を読み取り専用で確認します
- `tools/release_checks/binary_scan.py --format strings --all-strings` は AI/開発者がバイナリ由来の可読部分だけを確認したい場合の軽量出力です
- `tools/libreoffice/libreoffice_source_scan.py` は LibreOffice source archive を展開せず、path と小さな text file の内容を読み取り専用で確認します
- `tools/libreoffice/libreoffice_build_env_check.py` は LibreOffice A経路の自前ビルドに必要なローカルコマンド、Visual Studio 環境変数、source archive の SHA256 を読み取り専用で確認します
- `tools/libreoffice/libreoffice_runtime_dynamic_probe.py` は指定した変換処理中の LibreOffice process module を短周期で観測します。未観測DLLは削除確定ではなく、隔離削除試験へ送る候補です
- `tools/libreoffice/libreoffice_runtime_removal_trial.py` はruntimeを物理コピーし、候補をコピー側だけから削除して、通信ゲート、DOCX/PPTX実変換、任意の品質基準レポートとの完全比較を行います
- `tools/libreoffice/libreoffice_reduce.py` は dry-run が既定です。`--apply` 時は削除前に `.local/repo_resource/tmp/libreoffice_reduction_backup/<timestamp>/` へコピーします
- `tools/libreoffice/libreoffice_reduce.py --conversion-only` は PDF import / wiki publish / solver / standalone launcher 類を対象にします
- `tools/libreoffice/libreoffice_reduce.py --headless-only` は GUI resource / wizard / bundled script / basic macro 類を対象にします
- `tools/libreoffice/libreoffice_reduce.py --templates` は既存文書変換ではなく新規文書作成用の LibreOffice template 類を対象にします
- `tools/libreoffice/libreoffice_reduce.py --ui-locales-ja-en` は UI registry locale resources を日本語/英語だけに絞ります
- `tools/libreoffice/libreoffice_reduce.py --authoring-data` は autotext/autocorr/labels/classification/wordbook などの文書作成・編集支援データを対象にします
- `tools/libreoffice/libreoffice_reduce.py --stale-registry` は削除済み機能や外部 directory sample の registry fragments を対象にします
- `tools/libreoffice/libreoffice_reduce.py --dictionaries-ja-en` は `dict-en` だけを残し、それ以外の dictionary extension を対象にします
- `tools/libreoffice/libreoffice_reduce.py --program-resources-ja-en` は `program/resource` の UI resource locale directories を日本語/英語だけに絞ります
- `tools/libreoffice/libreoffice_reduce.py --scripting-runtime` は LibreOffice Python/UNO scripting runtime を対象にします
- `tools/libreoffice/libreoffice_reduce.py --ui-icon-themes` は headless conversion で不要な UI icon theme archives を対象にします
- `tools/libreoffice/libreoffice_reduce.py --database-java` は Base/database/reportbuilder と Java UNO runtime を対象にします。ただし起動時 import される `jvmfwklo.dll` と `jvmaccesslo.dll` は保持します
- `tools/libreoffice/libreoffice_reduce.py --calc` は Calc/Excel/spreadsheet runtime と UI/config を対象にします。ただし起動時 import される `orcus.dll`, `CoinMP.dll`, `lpsolve55.dll` は保持します
- `tools/libreoffice/libreoffice_reduce.py --nonconversion-leftovers` は PDF import / Presentation Minimizer / Firebird leftovers / writerperfect UI を対象にします
- マクロ、テンプレート、ラムダ、オーバーロード、スコープ解決、シャドーイングなどで誤差が出ます
- C/C++ のテンプレート、マクロ、複雑な宣言は過少/過大計上の可能性があります
- 変数宣言数や使用回数のような厳密解析はまだ対象外です
