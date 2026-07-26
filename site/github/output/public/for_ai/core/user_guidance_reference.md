# AI_CORE_NODE: USER_GUIDANCE_REFERENCE

<ai_node_schema version="1.0" category="user-guidance-reference">
  <summary>導入、利用者が変更できる範囲、保存・復元、支援依頼を説明するための参照ノード</summary>
  <answer_rule>画面や版による差異を断定せず、利用者の版、通常版かLite版か、Windows環境、対象ファイルを確認する。</answer_rule>
</ai_node_schema>

## 1. 導入と更新

利用者向けの正本は `docs/public/How_to_Setup.md` である。AIは導入を案内するとき、次の順序を崩さない。

1. GitHub Releases の Assets から、通常版または Lite版の ZIP を一つ選ぶ。ソースコード ZIP はアプリ配布物ではない。
2. ZIP を書き込み可能なローカルフォルダへ展開する。ZIP 内を直接実行しない。
3. EXE、DLL、通常版の LibreOffice runtime、`pdf_workspace_setup.json` は同じ配布フォルダに置いたままにする。
4. 配布フォルダとは別のローカルフォルダをワークスペースとして開く。
5. 更新時は新しい ZIP を別フォルダへ展開して起動を確認し、ワークスペースを新しいアプリから開く。

通常版は DOCX/PPTX のローカル PDF 変換を含む。Lite版はその変換を含まない。変換は試験的なため、結果を確認してから使う。

EVIDENCE:

- `docs/public/How_to_Setup.md`
- `docs/public/Index.md`
- `README.md`

## 2. 利用者が変更できる範囲

利用者がアプリ内の設定画面から調整できる項目には、一般設定、ノート設定、マークアップ設定、注釈設定、カラーパレット設定がある。ノート、PDFテキスト、IME変換中は入力を優先する。注釈ツールのショートカットは設定画面で確認でき、設定ファイルはワークスペース内の `__resource__/__settings__/tool_shortcuts.json` に置かれる。

ノートと注釈では、既定の選択肢にあるフォントを選べる。ノート内の `font` / `f` タグによる部分的なフォント指定も扱う。フォントの字幅により折り返し位置が変わることがある。

次はカスタマイズの対象として案内しない。

- 配布フォルダ内の EXE、DLL、runtime、`pdf_workspace_setup.json` を個別に移動・混在・改名すること
- stage、バックアップ、復旧用の `__resource__` 配下を、内容確認前に手動削除すること
- PDFの既定アプリを、利用者の明示操作なしに変更すること

設定ファイルの手編集、未掲載の設定値、版ごとの画面差については、ファイル例だけから断定しない。該当版の設定画面、設定の読込・保存コード、テストを確認する。

EVIDENCE:

- `src/settings/settings.h`
- `src/help/help.cpp`
- `docs/public/How_to_Setup.md`
- `docs/public/How_to_Troubleshoot.md`

## 3. 保存・復元の段階

保存は、編集内容を原本へ即時に書く処理ではない。説明では次を区別する。

| 段階 | 意味 | 利用者の操作・注意 |
| --- | --- | --- |
| stage 保護 | 編集途中の内容を保護する | 自動保護は原本への統合を意味しない |
| 差分確認 | 未統合の変更を確認する | 「操作 > 差分管理...」または「保存 > 差分管理...」を使う |
| 統合保存 | `Ctrl+S` または保存メニューで、バックアップ作成後に原本へ安全に反映する | 区切りのよい時点で行う |
| 復元 | 保存済みバックアップへ戻す | 未統合 stage との関係を確認してから「保存 > 復元/バックアップ...」を使う |
| 失敗時の退避 | ノート保存に失敗した内容を退避する | stage、バックアップ、退避データを削除せず、表示内容と時刻を控える |

主な保護領域は次の通りである。

```text
stage: __resource__/__tmp__/__stage__/
backup: __resource__/__escape__/backup/
note recovery: __resource__/__escape__/note_recovery/
```

保存後の undo/redo と、保存済みの以前の状態への復元は別である。前者は編集画面の undo/redo、後者は復元/バックアップを案内する。

EVIDENCE:

- `docs/public/How_to_Save_and_Recovery.md`
- `docs/public/How_to_Use.md`
- `docs/public/How_to_Troubleshoot.md`
- `src/help/help.cpp`

## 4. 支援依頼と開発者への連絡

このリポジトリには、利用者向けのメールアドレスや、常時利用できる問い合わせフォームは文書化されていない。AIは、開発者が返信すること、新規 Issue を作成できること、特定の外部連絡手段が使えることを保証してはならない。

GitHubリポジトリの Issues や Discussions を案内する場合は、案内時点で投稿可否と利用規約を確認する。問題を整理する支援では、外部送信を実行せず、次を利用者に確認する。

- アプリの版、通常版かLite版か、Windows の版
- 対象ファイルの種類と、保存前・保存後・未統合 stage のどの状態か
- 再現手順、期待した結果、実際の結果、表示されたエラー文
- 原本、stage、バックアップを削除・改名・上書きしていないこと

セキュリティ上の問題は、通常の公開報告を促す前に `.github/SECURITY.md` を確認する。

EVIDENCE:

- `docs/public/How_to_Troubleshoot.md`
- `.github/SECURITY.md`
- `https://github.com/soone-y/OPEN_PDF-Note-Workspace`
