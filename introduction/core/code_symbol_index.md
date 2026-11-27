# コード参照索引

## 調査の前提

この索引は入口を絞るためのものです。呼び出し元・失敗経路・関連テストを読まずに変更してはいけません。保存、注釈、出力、配布の変更では `introduction/core/documentation_contract.md` を読み、公開されている一次資料と実装を確認すること。

```xml
<symbol_index>
  <category name="atomic_persistence">
    <symbol name="atomic_write::AtomicWriteUtf8 / AtomicWriteBytes" type="function">
      <path>src/core/atomic_write.h</path>
      <description>一時ファイル、置換、失敗時の隔離を扱うアトミック書き込みユーティリティ。</description>
    </symbol>
    <symbol name="clrop_bridge::LoadAnnotations / SaveAnnotations" type="function">
      <path>src/clrop/bridge.h</path>
      <implementation>src/clrop/bridge.cpp</implementation>
      <description>.clrop と Annotation 配列の読込・保存境界。</description>
    </symbol>
    <symbol name="file_output::SaveAnnotationsIfDirty" type="function">
      <path>src/file_output/file_output.h</path>
      <implementation>src/file_output/file_output_stage.cpp</implementation>
      <description>未保存注釈と stage / 統合保存を扱う入口。</description>
    </symbol>
    <symbol name="SaveAnnotationsIfDirty（名前空間なし）" type="function">
      <path>src/pdf_view/annotation_store.cppinc</path>
      <declaration>src/pdf_view/pdf_view.h</declaration>
      <description>PDF表示側の入口。注釈履歴を保存してから file_output::SaveAnnotationsIfDirty を呼ぶ。UI自動化などはこちらを呼ぶ。</description>
    </symbol>
    <symbol name="workspace_layout::Unsaved / Backup / Recovery / Failed" type="function">
      <path>src/core/workspace_layout.h</path>
      <description>__pdf_note_workspace__ 配下の管理フォルダー（__unsaved__、__backup__、__recovery__、__failed__ など）のパスを作る唯一の場所。</description>
    </symbol>
    <symbol name="stage 復旧 / 復旧ツール" type="module">
      <path>src/recovery/stage_recovery.h</path>
      <implementation>src/recovery/standalone_recovery.cpp</implementation>
      <description>統合できなかった stage の引き継ぎと、--recovery で開く復旧ツール。</description>
    </symbol>
    <symbol name=".clrop 書式" type="module">
      <path>src/clrop/json.cpp</path>
      <description>.clrop の JSON 読み書き（version、pdf_id、注釈項目）。</description>
    </symbol>
  </category>
  <category name="pdf_annotation">
    <symbol name="OpenPdfWithAnnotations / LoadAnnotationsForCurrentPdf" type="function">
      <path>src/pdf_view/annotation_store.cppinc</path>
      <description>PDFを開いた後の注釈読込と状態反映の入口。</description>
    </symbol>
    <symbol name="AddMathAnnotationFromText / AddMathAnnotationFromTextAtPoint" type="function">
      <path>src/pdf_view/annotation_edit.cppinc</path>
      <declaration>src/bridge/view_bridge.h</declaration>
      <description>数式注釈を追加する入口。</description>
    </symbol>
  </category>
  <category name="workspace_and_ui">
    <symbol name="WorkspaceConfig / LoadWorkspaceConfig / SaveWorkspaceConfigToFile" type="type-and-functions">
      <path>src/core/workspace_config.h</path>
      <declaration>src/core/app_core.h</declaration>
      <implementation>src/core/app_core.cpp</implementation>
      <description>workspace.json の構成（構造体は workspace_config.h、関数宣言は app_core.h）と読込・保存処理。</description>
    </symbol>
    <symbol name="ShowStageManagerDialog" type="function">
      <path>src/workspace/file_ops_stage_manager.cppinc</path>
      <declaration>src/workspace/file_ops.h</declaration>
      <description>stage一覧・復元操作のダイアログ。</description>
    </symbol>
    <symbol name="wWinMain / window procedure fragments" type="entry-points">
      <path>src/app/bootstrap.cppinc</path>
      <related>src/main.cpp</related>
      <related>src/ui/core/main_window_proc.cppinc</related>
      <description>アプリ起動、メインウィンドウ、コマンド処理を追う出発点。</description>
    </symbol>
  </category>
</symbol_index>
```

索引にないシンボルは、`rg -n "シンボル名" src tests` で候補を絞り、宣言・実装・呼び出し元を確認してから扱うこと。
