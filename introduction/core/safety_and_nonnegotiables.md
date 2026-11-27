# 安全性と守る方針

## 1. 外部通信ゼロ方針 (No-Network Policy)

### 原則
配布物、アプリ本体、依存ライブラリ、関連ツール、ビルド生成物のすべてにおいて、**一切のネットワーク通信を行わない**設計を義務付けます。

### 適用範囲と無効化措置
- **自動アップデート・更新確認**: アプリ本体には実装しない。同梱の LibreOffice runtime は `--disable-online-update`、`--disable-online-update-mar`、`--disable-extension-update` を指定してビルドし（`third_party/libreoffice/custom_build/communication_free_options.input`）、更新確認のコンポーネントを登録しない。
- **テレメトリ・クラッシュレポート・広告SDK**: 配布物に含めない。
- **オンライン辞書・Web検索・翻訳**: アプリ本体にはこれらの機能がない。LibreOffice runtime は通信ライブラリ（curl）を含めずにビルドし、オンライン文法検査（LanguageTool）のコンポーネントを登録しない。綴り・ハイフネーション・類語の機能は、端末内の辞書を使うものだけが残る。

### 検証
- `tests/scripts/run_repo_checks.ps1` の安全性走査（`Invoke-SafetyScans`）による、ネットワークAPI・URL・音API などの走査。除外は `tests/config/safety_scan_ignore_globs.txt` で管理する。
- バイナリスキャン `tools/release_checks/binary_scan.py` による `WS2_32.dll` / `WININET.dll` などの通信 DLL リンク検出スキャン。

---

## 2. ファイル破壊・情報損失ゼロ (Data Protection & Non-Destructive Storage)

### 原則
異常終了、クラッシュ、電源断、権限エラーなどの異常系が発生した場合でも、ユーザーの PDF 原本やノート、注釈データを破棄・損壊させない設計にします。

### 安全機構
1. **原本非破壊**: PDF 原本は読み取り専用で扱い、注釈は別ファイル `.clrop` に保存。
2. **アトミック書き込み (`atomic_write::AtomicWriteUtf8` / `AtomicWriteBytes`、`src/core/atomic_write.h`)**:
   - 一時ファイル（`.__atomic__.<pid>.<tick>.<n>.tmp`）へ書き込み・検証を完了した後に、元のファイルへアトミック置換。置換できなかった一時ファイルは削除せず隔離する。
3. **stage（内部作業保護、`src/file_output/file_output_stage.cpp`）**:
   - 編集途中の状態を `__pdf_note_workspace__/__unsaved__/` に自動保存し、次回読込時に引き継ぐ。原本へ統合できなかった場合は原本を変更せず、利用者が「保留」「復旧」「終了」から選び、復旧ツール（`--recovery`）で残す内容を確認する。

### 書き込み先
- 配布 ZIP は展開して使う。展開後のアプリフォルダーに置くのは起動設定 `pdf_note_workspace_setup.json` である。ワークスペース設定 `workspace.json` はワークスペース直下、ショートカットなどの設定は `__pdf_note_workspace__/__settings__/` に置く。作業中のPDF・ノート・注釈・自動作業保護・バックアップ・ログは、利用者が選んだワークスペース内の `__pdf_note_workspace__` とその周辺で管理する。
- 出力と「一時パス」として利用者が登録した外部フォルダーは、利用者が明示して選んだ保存先として扱う。AppData、OSの一時フォルダー、起動元の作業フォルダーを暗黙の保存先にせず、書込みに失敗しても別の場所へ自動退避しない。
- Office変換の一時データと子プロセスの設定領域も、選択したワークスペース内の `__pdf_note_workspace__/__tmp__/lo/` に限定する。
- ここで定めるのはファイルとして永続的に書き込むストレージの境界であり、実行中のメモリ使用は対象外とする。

---

## 3. 無音遵守 (Silent UI Policy)

### 原則
アプリから突然の音やビープ音を鳴らさない設計にします。入力エラーや警告は静かな視覚通知（ダイアログやステータス表示）で伝えます。

### 実装ルール
- `PlaySound`、`Beep`、`MessageBeep` の使用を禁止。
- メディアの自動再生を実装しない。
- Windows の「一般の警告音」が鳴る未解消の既知問題については、発生経路を特定して解消する（音量設定変更による回避は認めない）。
