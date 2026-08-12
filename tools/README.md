# ツール ドキュメント

## 文書言語・語彙観測ツール (`analyze_document_language.py`)

`tools/metrics/analyze_document_language.py` は、文書群を混ぜずに unigram / bigram / trigram の頻度と出現ファイルをローカル集計する読み取り専用 CLI です。外部通信は行いません。既定では `docs/internal/`、`docs/public/`、`introduction/` を別グループとして、Markdown とテキスト文書だけを読みます。

テーマ候補は、既定で 2-gram 以上かつ 2文書群・2文書以上に出現する語句だけを対象にします。文書数、見出しでの出現、文書群の広がりで優先度を付け、重複した語順の候補はまとめます。各候補には確認用のファイル名と行番号を添えます。また、各文書群に固有な語句を、他群との文書出現率の差として別表示します。

```powershell
python tools/metrics/analyze_document_language.py --format md
```

利用者向けヘルプだけを個別に見る場合は、対象を明示します。

```powershell
python tools/metrics/analyze_document_language.py --group help=docs/public --format json
```

レポートは既存ファイルを上書きしません。実行条件（対象、拡張子、除外、stop word、正規化・分かち書き方式）は JSON 出力に、主要条件は Markdown 出力に含まれます。

```powershell
python tools/metrics/analyze_document_language.py --format md --top 40 --report out/reports/document_language_2026-08-09.md
```

単一の文書群内でテーマ候補を見たい場合だけ、閾値を下げます。

```powershell
python tools/metrics/analyze_document_language.py --group internal=docs/internal --theme-min-groups 1 --theme-min-docs 3 --format md
```

### 推奨する観測・改善ループ

1. `--format md --report out/reports/...` で、対象・除外・stop word を残した新規レポートを作成します。
2. テーマ候補の行番号を開き、同じ概念か、用語集候補・説明不足・表記ゆれ・単なるテンプレート語かをOHが判定します。
3. テンプレート語、引用、生成物が混じった場合は `--stop-word-file`、`--stop-word`、`--exclude` へ条件を追加します。採用・保留・除外の判断は数値へ戻さず、元文書とともに残します。
4. 同じ対象・条件で再実行し、前回と比較できない条件変更はレポートの所見に明記します。頻度の順位だけでテーマを採用しません。

### 注意

- 既定でライセンス本文、内部レポート、公開記録の生成物を除外します。生成物・引用など、今回さらに除外したい対象は `--exclude "glob"` を繰り返して追加します。
- Markdown の引用行と fenced code block は既定で除外します。必要な場合だけ `--include-quoted-lines` または `--include-code-blocks` を指定します。
- 日本語は外部の形態素解析器を使わず Unicode の文字種連続を分ける簡易方式です。日本語の厳密な語境界を保証しないため、語句候補と出現元を確認して判断してください。
- 頻度は重要度、正確さ、文章品質、作者の評価ではありません。

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

実装本体は `tools/dev/export_public_snapshot.py` にあります。`release.ps1` はこのツールを `public_repo_release_allowlist_2026-07-28.txt` とともに呼び出し、公開用 source snapshot を作ります。allowlist配下でもGit未追跡ファイルがあれば停止し、Git追跡済みの入力だけをコピーします。例外は、専用artifact manifestにパスとSHA-256を固定したGit管理外vendor artifactだけです。単体実行では、対象に応じた専用allowlistと、必要な場合は`--artifact-manifest`を明示指定します。

### 特徴
- コピー元の開発リポジトリは変更しません。
- 出力先が非空ディレクトリの場合は失敗し、既存内容を上書きしません。
- 出力先が開発リポジトリ配下の場合は失敗し、誤って作業ツリーへ公開物を混在させることを防ぎます。
- `__pycache__/`, `.pyc`, `.pyo`, `Thumbs.db`, `Desktop.ini` などの生成キャッシュは既定で除外します。
- 公開用 `.gitignore` は `docs/internal/operations/public_repo_gitignoreテンプレート_2026-07-02.gitignore` から生成します。

`release.ps1` は作成後に `tools/release_checks/public_snapshot_content_gate.py` で個人・ローカル環境情報、秘密情報、内部パス、ログ、archive本体、開発履歴集計がないことを確認します。その後 `tools/release_checks/release_set_integrity_gate.py` を実行し、release set直下の `public_snapshot_manifest.json` にsnapshotの全ファイル・ツリーハッシュ・許可リストのSHA-256を固定します。`publish.ps1 -Mode Verify` はcontent gateを再実行してから、この記録と実snapshotを照合し、通常版・Lite版のZIPが展開版と全ファイル単位で一致することも確認します。

### 使い方
ワークスペースのルートディレクトリで以下を実行してください。

```powershell
export_public_snapshot.ps1 --dest C:\tmp\pdf-note-public --allowlist C:\path\to\explicit_allowlist.txt
```

`--dest` を省略すると GUI のフォルダ選択ダイアログを開きます。端末から入力する場合は `export_public_snapshot.ps1 --select-dest cui` を使います。既定allowlistはありません。`--dry-run` を付けると、コピー予定だけを確認できます。
