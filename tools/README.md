# ツール ドキュメント

## ビルドログ解析ツール (`analyze_build_logs.py`)

このツールは `tools/metrics/analyze_build_logs.py` に配置されています。`out/logs/` の build 終了時刻ログと詳細ログを読み取り専用で集計し、ビルド履歴の傾向を確認するための CLI です。

### できること
- `build_end_time.log` / `build_readonly_viewer_end_time.log` から所要時間の件数、最小、中央値、平均、最大、合計を集計します。
- `build_detail_*.log` / `build_readonly_viewer_detail_*.log` から warning / error を集計します。
- warning / error の頻出ファイルと頻出メッセージを上位表示します。
- text / JSON / Markdown で標準出力へ出せます。必要なら `--report` でファイル保存します。

### 使い方

```powershell
python tools/metrics/analyze_build_logs.py
```

```powershell
python tools/metrics/analyze_build_logs.py --format md --report out/reports/build_log_analysis.md
```

```powershell
python tools/metrics/analyze_build_logs.py --format json --top 20 > out/reports/build_log_analysis.json
```

### 主なオプション
- `--root`
  - リポジトリルート。既定ではこのリポジトリを自動解決します
- `--log-dir`
  - 集計対象のログディレクトリ。既定は `out/logs`
- `--format text|json|md`
  - 出力形式
- `--top`
  - 頻出 warning / error の上位件数
- `--report`
  - 追加で保存したい出力ファイルパス

### 注意
- このツールはログを読むだけで、生成物やソースを書き換えません。
- 既定では標準出力だけに結果を出します。ファイル出力は `--report` 指定時だけ行います。

## 公開スナップショット作成ツール (`export_public_snapshot.py`)

実装本体は `tools/dev/export_public_snapshot.py` にあり、日常運用ではルートの `export_public_snapshot.ps1` から呼び出します。allowlist と公開用 `.gitignore` テンプレートを使い、開発リポジトリから公開用リポジトリへ持ち込んでよいファイルだけを別ディレクトリへコピーします。

### 特徴
- コピー元の開発リポジトリは変更しません。
- 出力先が非空ディレクトリの場合は失敗し、既存内容を上書きしません。
- 出力先が開発リポジトリ配下の場合は失敗し、誤って作業ツリーへ公開物を混在させることを防ぎます。
- `__pycache__/`, `.pyc`, `.pyo`, `Thumbs.db`, `Desktop.ini` などの生成キャッシュは既定で除外します。
- 公開用 `.gitignore` は `docs/internal/operations/public_repo_gitignoreテンプレート_2026-07-02.gitignore` から生成します。

### 使い方
ワークスペースのルートディレクトリで以下を実行してください。

```powershell
export_public_snapshot.ps1 --dest C:\tmp\pdf-note-public
```

`--dest` を省略すると GUI のフォルダ選択ダイアログを開きます。端末から入力する場合は `export_public_snapshot.ps1 --select-dest cui` を使います。既定では `docs/internal/operations/public_repo_demo許可リスト_2026-07-02.txt` を読み、そこに列挙されたパスだけをコピーします。`--dry-run` を付けると、コピー予定だけを確認できます。
