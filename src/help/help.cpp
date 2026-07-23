// file: help.cpp
#include "help/help.h"

#include "core/app_core.h"
#include "clrop/bridge.h"
#include "ui/noop_nav_guard.h"
#include "workspace/workspace_actions.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#include "fpdf_doc.h"
#include "fpdf_edit.h"
#include "fpdf_transformpage.h"

namespace {

struct HelpDialogCtx {
    HWND owner = nullptr;
    HWND navLabel = nullptr;
    HWND nav = nullptr;
    HWND title = nullptr;
    HWND subtitle = nullptr;
    HWND search = nullptr;
    HWND btnFind = nullptr;
    HWND edit = nullptr;
    HWND btnClose = nullptr;
    std::wstring windowTitle;
    std::wstring bodyText;
    int initialSection = 0;
    int currentSection = 0;
    size_t nextSearchOffset = 0;
    bool done = false;
};

struct PdfInfoDialogCtx {
    HWND owner = nullptr;
    HWND edit = nullptr;
    HWND btnCopy = nullptr;
    HWND btnClose = nullptr;
    bool done = false;
};

static std::wstring NormalizeNewlines(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t ch = s[i];
        if (ch == L'\r') {
            if (i + 1 < s.size() && s[i + 1] == L'\n') {
                ++i;
            }
            out += L"\r\n";
        } else if (ch == L'\n') {
            out += L"\r\n";
        } else {
            out += ch;
        }
    }
    return out;
}

static std::optional<std::wstring> ReadUtf8TextFile(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return std::nullopt;
    std::string data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (data.size() >= 3 &&
        static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data.erase(0, 3);
    }
    return UTF8ToWide(data);
}

static std::filesystem::path GetExeDir() {
    wchar_t exePath[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return std::filesystem::current_path();
    return std::filesystem::path(exePath).parent_path();
}

static std::wstring OfficeConversionHelpTextJa() {
    if (HasOfficeConversionFeature()) {
        return
            L"■ OfficeファイルをPDFに変換\n"
            L"- この通常版では、.docx と .pptx を同梱LibreOfficeでローカルPDF変換できます。\n"
            L"- メニューの変換機能またはワークスペースへのドロップで使えます。ドロップ時は対象を確認してから変換します。\n"
            L"- Microsoft Officeやオンライン変換サービスは使用しません。元のOfficeファイルは変更しません。\n"
            L"- runtimeが見つからない場合はLite版へ切り替わりません。通常版の配布フォルダ一式を復元してください。\n"
            L"- 変換は試験的です。フォント、図形、数式などで見た目が異なることがあるため、PDFの結果を確認してください。\n"
            L"\n";
    }
    return
        L"■ OfficeファイルをPDFに変換（Lite版）\n"
        L"- この版はOffice-to-PDF変換runtimeを含まないLite版です。ウィンドウ名にもLiteと表示されます。\n"
        L"- .docx / .pptx をドロップしても変換・取込みは行いません。PDFを用意してから取り込んでください。\n"
        L"- Officeファイルをアプリ内でPDFにしたい場合は、通常版を使ってください。Microsoft Officeやオンライン変換サービスは使用しません。\n"
        L"\n";
}

static std::wstring OfficeConversionHelpTextEn() {
    if (HasOfficeConversionFeature()) {
        return
            L"■ Convert Office Files To PDF\n"
            L"- This standard edition converts .docx and .pptx to PDF locally with bundled LibreOffice.\n"
            L"- Use the conversion menu or drop files into a workspace. A drop asks for confirmation before conversion.\n"
            L"- It does not use Microsoft Office or an online conversion service, and does not modify the source Office file.\n"
            L"- A missing runtime does not turn this into Lite. Restore the complete standard-edition release folder.\n"
            L"- Conversion is experimental. Check the resulting PDF because fonts, shapes, or equations can differ.\n"
            L"\n";
    }
    return
        L"■ Convert Office Files To PDF (Lite)\n"
        L"- This is the Lite edition, which does not include an Office-to-PDF conversion runtime. Its window title also shows Lite.\n"
        L"- Dropped .docx / .pptx files are not converted or imported. Prepare a PDF before importing it.\n"
        L"- Use the standard edition if you need in-app Office-to-PDF conversion. Microsoft Office and online conversion services are not used.\n"
        L"\n";
}

static std::wstring DefaultHelpTextJa() {
    return std::wstring(
        L"■ 概要\n"
        L"このソフトは、PDF に対するノート作成や注釈編集を安全に扱うためのローカル専用ツールです。\n"
        L"編集中に原本 PDF を直接書き換えず、まずワークスペース内へ保存してから統合します。\n"
        L"\n"
        L"■ 試験的な機能\n"
        L"- 数式の扱い（MathBox 入力、ノート内の数式表示を含む）は試験的です。\n"
        L"- 数式は内容や配置によって、表示崩れ、選択ずれ、レイアウト差が出ることがあります。\n"
        L"- DOCX/PPTX から PDF への変換も試験的です。\n"
        L"- 変換結果は文書や環境によってレイアウト差や変換失敗があり得るため、結果を確認してください。\n"
        L"\n"
    ) + OfficeConversionHelpTextJa() +
        L"■ まず知っておきたいこと\n"
        L"- 原本不変: 編集中は原本 PDF / 元ノートを直接上書きしません。\n"
        L"- ステージ保存: 変更はまず一時領域に保存されます。\n"
        L"- 統合保存: Ctrl+S、終了時、または保存メニューから原本へ反映します。\n"
        L"- バックアップ: 原本へ統合する前にバックアップを作成します。\n"
        L"\n"
        L"■ 保存の考え方\n"
        L"- ステージ: まだ原本へ反映していない編集内容です。\n"
        L"- 統合: ステージを原本へ反映して保存する処理です。\n"
        L"- ノートの自動stage保護: 行移動・フォーカス離脱・保存前・6〜20秒無操作で stage に反映します。\n"
        L"- 注釈の自動stage保護: 設定した間隔で stage に反映します。\n"
        L"- 自動統合: 現在は使っていません。原本反映は Ctrl+S / 保存メニュー / 終了確認で行います。\n"
        L"- 原本へ保存した後の undo/redo 履歴は保持対象外です。保存後に戻したい場合は復元/バックアップを使います。\n"
        L"\n"
        L"■ ノート形式\n"
        L"- 新規ノートは本ソフトのノート形式である .clro として作成されます。\n"
        L"- .md もノート作成時の拡張子変更から選べますが、既定のノート拡張子ではありません。\n"
        L"- .md と .clro は現在同じ Markdown 系記法、独自 markup、数式記法を扱いますが、拡張子の扱いは同一ではありません。\n"
        L"- .txt はプレーンテキストとして扱われ、装飾や構文解析は行いません。\n"
        L"\n"
        L"■ 推奨される保存手順\n"
        L"1) ノートや注釈を編集する\n"
        L"2) 自動stage保護で途中状態を保持する\n"
        L"3) 区切りのよいタイミングで Ctrl+S または保存メニューから統合する\n"
        L"4) 現在のノートだけを保存したい場合は、ノート単体保存を使う\n"
        L"\n"
        L"■ 安全性について\n"
        L"- 原本への保存は、別ファイルへ完全に書き出してから置換する方式です。\n"
        L"- 保存途中で失敗しても、原本が途中書き込みで壊れないようにしています。\n"
        L"- 統合前にバックアップを作成するため、直前状態へ戻しやすくしています。\n"
        L"\n"
        L"■ 著作権と保護付きPDF\n"
        L"- 著作権保護や利用制限が設定されたPDFでは、元の権利者や配布元の指示・利用条件に従ってください。\n"
        L"- このアプリは、保護付きPDFに対するコピー、PDF出力、PNG出力を制限します。\n"
        L"- 必要に応じてパスワード入力で再オープンできますが、利用可否の判断そのものを置き換えるものではありません。\n"
        L"- OS標準OCR、スクリーンショット、他のPDFビューアや外部ツールによる取得・出力は、このアプリでは制御しません。\n"
        L"\n"
        L"■ 差分管理\n"
        L"メニュー: 操作 → 差分管理... / 保存 → 差分管理...\n"
        L"- 採用: 原本を変えず、この stage を現在の採用状態にします。\n"
        L"- 元へ統合: stage を原本へ反映し、成功時に stage を削除します。\n"
        L"- 破棄: stage だけを削除します。原本は変わりません。\n"
        L"\n"
        L"■ 復元 / バックアップ\n"
        L"メニュー: 保存 → 復元/バックアップ...\n"
        L"- [はい] バックアップから復元: 保存済みバックアップを復元します。\n"
        L"- [いいえ] 未統合の差分を統合: 現在採用中の stage を原本へ反映します。\n"
        L"\n"
        L"復元前に確認してください:\n"
        L"- 未統合のノートや注釈があると、復元後に原本だけ古い状態へ戻ることがあります。\n"
        L"- 必要なら差分管理で内容を確認するか、先に Ctrl+S で統合してください。\n"
        L"- 復元対象は .meta.txt で選びます。対象ファイルを確認してから実行してください。\n"
        L"\n"
        L"復元の手順:\n"
        L"1) 保存 → 復元/バックアップ... を開く\n"
        L"2) [はい] バックアップから復元する を選ぶ\n"
        L"3) __resource__/__escape__/backup 配下の .meta.txt を選ぶ\n"
        L"4) 記録された保存先 (dest) へ復元する\n"
        L"5) 現在開いている対象なら画面表示も再読込される\n"
        L"\n"
        L"■ 保存先の目安\n"
        L"- ステージ: __resource__/__tmp__/__stage__/\n"
        L"- バックアップ本体: __resource__/__escape__/backup/.../*.bak\n"
        L"- バックアップ情報: __resource__/__escape__/backup/.../*.bak.meta.txt\n"
        L"- ノート保存失敗時の退避: __resource__/__escape__/note_recovery/\n"
        L"- 原子的保存に失敗した一時ファイルの退避先: __resource__/__escape__/\n"
        L"\n"
        L"■ マークアップ\n"
        L"- font タグを使うと、指定範囲だけフォントを切り替えられます。\n"
        L"- 例: `<font=\"Meiryo\">本文</>` / `<f=\"Meiryo\">本文</>`\n"
        L"- 注釈とノートのフォント選択肢は共通です。\n"
        L"- 選択肢: Meiryo, Yu Gothic, Yu Mincho, Segoe UI, Arial,\n"
        L"  Times New Roman, Georgia, Consolas, Courier New\n"
        L"- 既存データに別のフォント名が含まれていても、読み込み時にその指定を不用意に削除しません。\n"
        L"- フォントファイル自体は追加同梱せず、環境にあるフォントまたは既存の同梱フォント条件に従います。\n"
        L"- フォントにより字幅や見た目サイズが変わるため、折り返し位置が変わることがあります。\n"
        L"\n"
        L"■ 注釈ツールのショートカット\n"
        L"- 注釈ツールは Ctrl+Alt+数字 またはテンキー単独で切り替えられます。\n"
        L"- Ctrl+Alt+← / Ctrl+Alt+→ でカテゴリ、Ctrl+Alt+↑ / Ctrl+Alt+↓ で詳細種を切り替えます（固定）。\n"
        L"- Ctrl+↑ / Ctrl+↓ で現在の注釈ツール色を前後のパレット色へ切り替えます。\n"
        L"- Shift+クリックの逆循環は注釈ツール/オプションボタン上だけで使い、PDF面のShift+クリックはテキストボックス選択を優先します。\n"
        L"- 設定ファイル: __resource__/__settings__/tool_shortcuts.json\n"
        L"- 例: { \"key\": \"Ctrl+Alt+7\", \"tool\": \"freehand\" } / { \"key\": \"Numpad7\", \"tool\": \"freehand\" }\n"
        L"- ノート入力中、PDFテキスト編集中、IME変換中は、文字入力を優先します。\n"
        L"\n"
        L"■ 補足\n"
        L"- 一時フォルダ (__resource__/__tmp__) は、自動保存や中間処理に使います。\n"
        L"- 単位の目安: pt = mm × 72 / 25.4\n";
}

static std::wstring DefaultHelpTextEn() {
    return std::wstring(
        L"■ Overview\n"
        L"This app is a local-only tool for working safely with PDF notes and annotations.\n"
        L"While editing, it avoids modifying the original PDF directly and stages changes first.\n"
        L"\n"
        L"■ Experimental Features\n"
        L"- Math handling, including MathBox input and note-side math rendering, is experimental.\n"
        L"- Depending on the expression and layout, rendering glitches, hit-test offsets, or layout differences may occur.\n"
        L"- DOCX/PPTX to PDF conversion is also experimental.\n"
        L"- Depending on the document and environment, layout differences or conversion failures may occur, so verify the result.\n"
        L"\n"
    ) + OfficeConversionHelpTextEn() +
        L"■ What To Know First\n"
        L"- Original files are not overwritten during editing.\n"
        L"- Changes are first saved into a staged area.\n"
        L"- Ctrl+S, exit, or the Save menu integrates staged changes into the originals.\n"
        L"- A backup is created before integration.\n"
        L"\n"
        L"■ How Saving Works\n"
        L"- Stage: edits saved locally but not yet applied to the original files.\n"
        L"- Integrate: apply staged edits to the original files.\n"
        L"- Note auto-stage protection: stage on line move, focus leave, save, or 6-20s idle.\n"
        L"- Annotation auto-stage protection: stage on the configured background interval.\n"
        L"- Auto-integrate is currently unused. Originals are updated only by Ctrl+S, Save, or exit confirmation.\n"
        L"- Undo/redo history is not retained after saving to the original files. Use Recovery / Backups if you need to return to the previous saved state.\n"
        L"\n"
        L"■ Note Formats\n"
        L"- New notes are created as .clro, the application's note format, by default.\n"
        L"- .md remains selectable from the extension menu, but it is not the default note extension.\n"
        L"- Both .md and .clro currently support Markdown-style syntax, custom markup, and math syntax, but the extensions are not interchangeable product semantics.\n"
        L"- .txt is treated as plain text without decoration or syntax parsing.\n"
        L"\n"
        L"■ Recommended Flow\n"
        L"1) Edit notes or annotations\n"
        L"2) Let auto-stage protection keep intermediate work\n"
        L"3) Integrate with Ctrl+S or the Save menu at safe checkpoints\n"
        L"4) Use note-only save when you want to write only the current note\n"
        L"\n"
        L"■ Safety\n"
        L"- Original files are replaced only after a full temporary write completes.\n"
        L"- If a save fails midway, the original file should remain intact.\n"
        L"- A backup is created before integration so the previous state can be restored.\n"
        L"\n"
        L"■ Copyright And Protected PDFs\n"
        L"- For PDFs with copyright protection or usage restrictions, follow the original rights holder's or distributor's instructions and terms.\n"
        L"- This app restricts copy, PDF export, and PNG export for protected PDFs.\n"
        L"- Reopening with a password is supported when needed, but that does not replace the need to follow the usage terms themselves.\n"
        L"- This app does not control OS OCR, screenshots, other PDF viewers, or external tools.\n"
        L"\n"
        L"■ Diff Manager\n"
        L"Menu: Operations -> Diff Manager... / Save -> Diff Manager...\n"
        L"- Activate: mark a staged version as the current one without writing to the original.\n"
        L"- Integrate: apply the stage to the original file and remove it on success.\n"
        L"- Discard: remove only the stage. The original file is unchanged.\n"
        L"\n"
        L"■ Recovery / Backups\n"
        L"Menu: Save -> Recovery / Backups...\n"
        L"- [Yes] Restore from backup: restore a saved backup.\n"
        L"- [No] Integrate staged diffs: apply the currently adopted stage to the original file.\n"
        L"\n"
        L"Check before restoring:\n"
        L"- If you still have unintegrated note or annotation diffs, restoring may move only the original file back.\n"
        L"- Review Diff Manager first, or integrate with Ctrl+S if that is your intent.\n"
        L"- Choose the correct .meta.txt file for the item you want to restore.\n"
        L"\n"
        L"Restore steps:\n"
        L"1) Open Save -> Recovery / Backups...\n"
        L"2) Choose [Yes] Restore from backup\n"
        L"3) Select a .meta.txt file under __resource__/__escape__/backup\n"
        L"4) Restore to the recorded destination (dest)\n"
        L"5) If the restored target is open, the view reloads\n"
        L"\n"
        L"■ Typical Locations\n"
        L"- Stage: __resource__/__tmp__/__stage__/\n"
        L"- Backup data: __resource__/__escape__/backup/.../*.bak\n"
        L"- Backup metadata: __resource__/__escape__/backup/.../*.bak.meta.txt\n"
        L"- Recovery copies for note save failures: __resource__/__escape__/note_recovery/\n"
        L"- Quarantined temp files from failed atomic writes: __resource__/__escape__/\n"
        L"\n"
        L"■ Markup\n"
        L"- Use the font tag to switch fonts only for a selected range.\n"
        L"- Example: `<font=\"Meiryo\">text</>` / `<f=\"Meiryo\">text</>`\n"
        L"- Annotation and note font selectors use the same list.\n"
        L"- Choices: Meiryo, Yu Gothic, Yu Mincho, Segoe UI, Arial,\n"
        L"  Times New Roman, Georgia, Consolas, Courier New\n"
        L"- Existing data with other font names is not deleted just because it is outside this selector list.\n"
        L"- Font files are not newly bundled here; usage follows the fonts available in the environment or existing bundled-font terms.\n"
        L"- Different fonts can change glyph width and visual size, so line wrapping may shift.\n"
        L"\n"
        L"■ Annotation Tool Shortcuts\n"
        L"- Annotation tools can be switched with Ctrl+Alt+number or a numpad key by itself.\n"
        L"- Ctrl+Alt+Left / Ctrl+Alt+Right changes category; Ctrl+Alt+Up / Ctrl+Alt+Down changes detail (fixed).\n"
        L"- Ctrl+Up / Ctrl+Down cycles the current annotation tool color through the palette.\n"
        L"- Shift+click reverse cycling applies only on annotation tool/option buttons; Shift+click on the PDF surface keeps text-box selection priority.\n"
        L"- Config file: __resource__/__settings__/tool_shortcuts.json\n"
        L"- Example: { \"key\": \"Ctrl+Alt+7\", \"tool\": \"freehand\" } / { \"key\": \"Numpad7\", \"tool\": \"freehand\" }\n"
        L"- Text input takes priority while editing notes, PDF text boxes, or IME composition.\n"
        L"\n"
        L"■ Notes\n"
        L"- The temporary folder (__resource__/__tmp__) is used for auto-save and intermediate work.\n"
        L"- Unit reference: pt = mm x 72 / 25.4\n";
}

static std::wstring LoadHelpText() {
    const bool isEn = IsEnglishUi();
    const std::wstring fileName = isEn ? L"help_en.txt" : L"help_ja.txt";
    std::vector<std::filesystem::path> candidates;
    const auto exeDir = GetExeDir();
    candidates.push_back(exeDir / L"docs" / fileName);
    candidates.push_back(exeDir / fileName);
    candidates.push_back(std::filesystem::current_path() / L"docs" / fileName);
    if (!isEn) {
        candidates.push_back(exeDir / L"開発ドキュメント" / L"ヘルプについて.txt");
        candidates.push_back(std::filesystem::current_path() / L"開発ドキュメント" / L"ヘルプについて.txt");
    }
    for (const auto& path : candidates) {
        if (auto text = ReadUtf8TextFile(path)) {
            return NormalizeNewlines(*text);
        }
    }
    return NormalizeNewlines(isEn ? DefaultHelpTextEn() : DefaultHelpTextJa());
}

static std::wstring CustomExtensionHelpText() {
    if (IsEnglishUi()) {
        return
            L"■ .clro: this application's note extension\n"
            L"- New notes are created as .clro by default. The default file naming rule also ends in .clro.\n"
            L"- A .clro file is ordinary UTF-8 note text. It is not a binary container or an annotation file.\n"
            L"- It uses the Markdown/MD4C note route, including Markdown-style syntax, supported custom markup, math notation, rendered display, and note links.\n"
            L"\n"
            L"■ .md / .markdown: compatible Markdown note extensions\n"
            L"- They use the same Markdown/MD4C route and can be opened or selected when creating a note.\n"
            L"- They are not the default extension for notes created by this application. Existing .md files are not renamed or converted automatically.\n"
            L"\n"
            L"■ What differs internally\n"
            L"- The content parser is the same for .clro and .md.\n"
            L"- The extension still controls the product-level default: initial creation, suggested names, and the default naming rule use .clro.\n"
            L"- File rename, copy, and link operations preserve the selected note's extension rather than silently changing it.\n"
            L"\n"
            L"■ Related extensions\n"
            L"- .txt is a plain-text note route. Markdown parsing, rendered display, and note-link features are disabled.\n"
            L"- .clrop is separate JSON data for PDF annotations. It is not a note file; do not rename it to .clro or edit it as a note.\n"
            L"\n"
            L"Use .clro for notes created and managed primarily in this application. Use .md when interoperability with other Markdown tools is the priority.\n";
    }
    return
        L"■ .clro: 本ソフトのノート拡張子\n"
        L"- 新規ノートは標準で .clro として作成され、既定の命名規則も .clro で終わります。\n"
        L"- .clro は UTF-8 の通常のノート本文です。バイナリ形式でも、注釈ファイルでもありません。\n"
        L"- Markdown/MD4C のノート経路を使い、Markdown 系記法、対応する独自 markup、数式記法、レンダリング表示、ノートリンクを利用できます。\n"
        L"\n"
        L"■ .md / .markdown: 互換 Markdown ノート拡張子\n"
        L"- .clro と同じ Markdown/MD4C 経路を使い、開くこともノート作成時に選ぶこともできます。\n"
        L"- ただし、本ソフトが新規作成するノートの既定拡張子ではありません。既存の .md は自動で改名・変換しません。\n"
        L"\n"
        L"■ 内部的に異なる扱い\n"
        L"- .clro と .md の本文解析器は同じです。\n"
        L"- 一方で、製品上の既定値は拡張子で区別されます。初回作成、候補名、既定の命名規則には .clro を使います。\n"
        L"- リネーム、コピー、リンク先作成では、選んだノートの拡張子を保持し、黙って別の拡張子へ変更しません。\n"
        L"\n"
        L"■ 関連する拡張子\n"
        L"- .txt はプレーンテキスト経路です。Markdown 解析、レンダリング表示、ノートリンク機能は無効です。\n"
        L"- .clrop は PDF 注釈用の別の JSON データです。ノートではないため、.clro に改名したりノートとして編集したりしないでください。\n"
        L"\n"
        L"本ソフトで主に作成・管理するノートには .clro を、他の Markdown ツールとの互換性を優先する場合には .md を使ってください。\n";
}

enum HelpSectionId : int {
    kHelpSectionStart = 0,
    kHelpSectionExtensions,
    kHelpSectionSaving,
    kHelpSectionNotes,
    kHelpSectionShortcuts,
    kHelpSectionPdf,
    kHelpSectionFullGuide,
    kHelpSectionCount,
};

static const wchar_t* HelpSectionLabel(int section, bool english) {
    static constexpr const wchar_t* kJa[kHelpSectionCount] = {
        L"はじめに", L"ノート形式と拡張子", L"保存・復元", L"ノート記法", L"ショートカット", L"PDF と注釈", L"すべての案内",
    };
    static constexpr const wchar_t* kEn[kHelpSectionCount] = {
        L"Getting started", L"Note formats", L"Saving & recovery", L"Note syntax", L"Shortcuts", L"PDF & annotations", L"Full guide",
    };
    if (section < 0 || section >= kHelpSectionCount) return L"";
    return (english ? kEn : kJa)[section];
}

static std::wstring CurrentNoteContextHelpText(bool english) {
    if (g_currentNotePath.empty()) {
        return english ? L"No note is currently open." : L"現在開いているノートはありません。";
    }
    std::filesystem::path path(g_currentNotePath);
    std::wstring ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    std::wstring route = (ext == L".txt" || ext == L".csv")
        ? (english ? L"plain text" : L"プレーンテキスト")
        : (english ? L"Markdown/MD4C" : L"Markdown/MD4C");
    return (english ? L"Current note: " : L"現在のノート: ") + path.filename().wstring() +
           L"\r\n" + (english ? L"Extension and route: " : L"拡張子と経路: ") +
           (ext.empty() ? (english ? L"(none)" : L"（なし）") : ext) + L" / " + route;
}

static std::wstring HelpSectionTitle(int section, bool english) {
    if (section == kHelpSectionStart) return english ? L"Help center" : L"ヘルプセンター";
    return HelpSectionLabel(section, english);
}

static std::wstring HelpSectionSubtitle(int section, bool english) {
    switch (section) {
    case kHelpSectionStart:
        return english ? L"Choose a topic on the left. All help stays on this device."
                       : L"左の項目から必要な案内を選べます。ヘルプはすべてこの端末内で表示します。";
    case kHelpSectionExtensions:
        return english ? L"Choose an extension according to how you create and use the note."
                       : L"ノートの作成方法と使い方に合わせて拡張子を選んでください。";
    case kHelpSectionSaving:
        return english ? L"Edits are staged first, then integrated only when you choose to save."
                       : L"編集内容はまず stage に保護され、保存時にだけ原本へ統合されます。";
    case kHelpSectionNotes:
        return english ? L"Markdown, supported custom markup, and math notation for notes."
                       : L"Markdown、対応する独自 markup、数式記法について説明します。";
    case kHelpSectionShortcuts:
        return english ? L"Keyboard input takes priority while editing text or using IME."
                       : L"ノート入力・テキスト編集中・IME変換中は文字入力を優先します。";
    case kHelpSectionPdf:
        return english ? L"PDF annotations are stored separately from the original PDF."
                       : L"PDF 注釈は元 PDF と分離して保存します。";
    default:
        return english ? L"The complete local help text." : L"従来の詳細なローカルヘルプです。";
    }
}

static std::wstring HelpSectionBody(int section, bool english) {
    switch (section) {
    case kHelpSectionStart:
        if (english) {
            return L"■ Start here\r\n"
                   L"1) Create or open a workspace.\r\n"
                   L"2) Create a .clro note, or open an existing note.\r\n"
                   L"3) Edit freely; changes are protected in a staged area first.\r\n"
                   L"4) Use Ctrl+S or the Save menu when you want to integrate changes.\r\n\r\n"
                   L"■ Current context\r\n" + CurrentNoteContextHelpText(true) +
                   L"\r\n\r\nSelect a category on the left for details. Use the local search box to find text in the current category.";
        }
        return L"■ はじめに\r\n"
               L"1) ワークスペースを作成または開きます。\r\n"
               L"2) .clro ノートを作成するか、既存ノートを開きます。\r\n"
               L"3) 編集中の変更は、まず stage 領域に保護されます。\r\n"
               L"4) 原本へ反映したいタイミングで Ctrl+S または保存メニューを使います。\r\n\r\n"
               L"■ 現在の状態\r\n" + CurrentNoteContextHelpText(false) +
               L"\r\n\r\n左のカテゴリから詳しい説明を選べます。検索欄では現在のカテゴリ内を検索します。";
    case kHelpSectionExtensions:
        return NormalizeNewlines(CustomExtensionHelpText());
    case kHelpSectionSaving:
        return english
            ? L"■ Safe saving\r\n- Editing does not directly overwrite the original PDF or note.\r\n- Auto-stage protection keeps intermediate edits before integration.\r\n- Ctrl+S and the Save menu integrate staged changes after a safe write and backup.\r\n\r\n■ Recovery\r\n- Use Save > Recovery / Backups to restore a saved backup or integrate adopted staged changes.\r\n- Review staged changes before restoring when you have unfinished work.\r\n\r\n■ Typical locations\r\n- Stage: __resource__/__tmp__/__stage__/\r\n- Backups: __resource__/__escape__/backup/\r\n- Note recovery: __resource__/__escape__/note_recovery/\r\n"
            : L"■ 安全な保存\r\n- 編集中に元の PDF やノートを直接上書きしません。\r\n- 自動 stage 保護により、統合前の編集途中も保持します。\r\n- Ctrl+S と保存メニューは、安全な書込みとバックアップ作成の後に stage を原本へ統合します。\r\n\r\n■ 復元\r\n- 「保存 > 復元/バックアップ...」から保存済みバックアップの復元、または採用済み stage の統合を行えます。\r\n- 未完了の作業があるときは、復元前に差分内容を確認してください。\r\n\r\n■ 主な保存先\r\n- stage: __resource__/__tmp__/__stage__/\r\n- バックアップ: __resource__/__escape__/backup/\r\n- ノート復旧: __resource__/__escape__/note_recovery/\r\n";
    case kHelpSectionNotes:
        return english
            ? L"■ Note syntax\r\n- .clro, .md, and .markdown use the Markdown/MD4C route.\r\n- Supported custom markup can be combined with Markdown when needed.\r\n- Math notation is experimental; verify the display before relying on it.\r\n\r\n■ Plain text\r\n- .txt does not parse Markdown or custom markup. It has no rendered display or note-link features.\r\n"
            : L"■ ノート記法\r\n- .clro、.md、.markdown は Markdown/MD4C 経路を使います。\r\n- 必要な場合は、対応する独自 markup を Markdown と組み合わせられます。\r\n- 数式記法は試験的です。重要な内容は表示を確認してください。\r\n\r\n■ プレーンテキスト\r\n- .txt では Markdown や独自 markup を解析しません。レンダリング表示やノートリンクも使いません。\r\n";
    case kHelpSectionShortcuts:
        return english
            ? L"■ Annotation shortcuts\r\n- Ctrl+Alt+number or a numpad key selects an annotation tool.\r\n- Ctrl+Up / Ctrl+Down cycles the current tool color.\r\n\r\n■ Input priority\r\n- Notes, PDF text boxes, and IME composition keep text input priority.\r\n- Use the Settings screens to review shortcuts and note behavior.\r\n"
            : L"■ 注釈ショートカット\r\n- Ctrl+Alt+数字 またはテンキー単独で注釈ツールを切り替えられます。\r\n- Ctrl+↑ / Ctrl+↓ で現在のツール色を切り替えます。\r\n\r\n■ 入力の優先\r\n- ノート、PDF テキストボックス、IME 変換中は文字入力を優先します。\r\n- 設定画面からショートカットやノートの挙動を確認できます。\r\n";
    case kHelpSectionPdf:
        return english
            ? L"■ PDF annotations\r\n- Annotations are stored in a separate .clrop JSON file; the original PDF is not edited directly.\r\n- .clrop is not a note file. Do not rename it to .clro or edit it as a note.\r\n\r\n■ Protected PDFs\r\n- Follow the rights holder's terms for protected PDFs.\r\n- This app restricts copy and export operations for protected PDFs.\r\n"
            : L"■ PDF 注釈\r\n- 注釈は別の .clrop JSON ファイルへ保存され、元 PDF を直接編集しません。\r\n- .clrop はノートではありません。.clro に改名したり、ノートとして編集したりしないでください。\r\n\r\n■ 保護付き PDF\r\n- 保護付き PDF は権利者・配布元の利用条件に従ってください。\r\n- 本ソフトは保護付き PDF のコピー・出力操作を制限します。\r\n";
    default:
        return LoadHelpText();
    }
}

constexpr int kHelpNavId = 1005;
constexpr int kHelpSearchId = 1006;
constexpr int kHelpFindId = 1007;
constexpr int kHelpMinWidth = 640;
constexpr int kHelpMinHeight = 460;

static void UpdateHelpDialogContent(HelpDialogCtx* ctx) {
    if (!ctx) return;
    const bool english = IsEnglishUi();
    ctx->bodyText = HelpSectionBody(ctx->currentSection, english);
    ctx->nextSearchOffset = 0;
    if (ctx->title) SetWindowTextW(ctx->title, HelpSectionTitle(ctx->currentSection, english).c_str());
    if (ctx->subtitle) SetWindowTextW(ctx->subtitle, HelpSectionSubtitle(ctx->currentSection, english).c_str());
    if (ctx->edit) {
        SetWindowTextW(ctx->edit, ctx->bodyText.c_str());
        SendMessageW(ctx->edit, EM_SETSEL, 0, 0);
    }
}

enum class HelpSearchStatus {
    Ready,
    EmptyQuery,
    NoMatch,
};

static void ShowHelpSearchStatus(HelpDialogCtx* ctx, HelpSearchStatus status) {
    if (!ctx || !ctx->subtitle) return;
    const bool english = IsEnglishUi();
    std::wstring text = HelpSectionSubtitle(ctx->currentSection, english);
    if (status == HelpSearchStatus::EmptyQuery) {
        text += english ? L"  |  Enter text to search this category."
                        : L"  |  このカテゴリ内を検索する文字を入力してください。";
    } else if (status == HelpSearchStatus::NoMatch) {
        text += english ? L"  |  No matches in this category."
                        : L"  |  このカテゴリ内には見つかりません。";
    }
    SetWindowTextW(ctx->subtitle, text.c_str());
}

static std::wstring GetControlText(HWND control) {
    const int len = GetWindowTextLengthW(control);
    if (len <= 0) return L"";
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(control, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    return text;
}

static void FindInHelpSection(HelpDialogCtx* ctx) {
    if (!ctx || !ctx->search || !ctx->edit) return;
    std::wstring needle = GetControlText(ctx->search);
    if (needle.empty()) {
        ShowHelpSearchStatus(ctx, HelpSearchStatus::EmptyQuery);
        SetFocus(ctx->search);
        return;
    }
    std::wstring haystack = ctx->bodyText;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::towlower);
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::towlower);
    size_t pos = haystack.find(needle, ctx->nextSearchOffset);
    if (pos == std::wstring::npos && ctx->nextSearchOffset != 0) pos = haystack.find(needle);
    if (pos == std::wstring::npos) {
        ShowHelpSearchStatus(ctx, HelpSearchStatus::NoMatch);
        SetFocus(ctx->search);
        return;
    }
    ShowHelpSearchStatus(ctx, HelpSearchStatus::Ready);
    ctx->nextSearchOffset = pos + needle.size();
    SendMessageW(ctx->edit, EM_SETSEL, static_cast<WPARAM>(pos),
                 static_cast<LPARAM>(pos + needle.size()));
    SendMessageW(ctx->edit, EM_SCROLLCARET, 0, 0);
    SetFocus(ctx->edit);
}

static void LayoutHelpDialog(HWND hWnd, HelpDialogCtx* ctx) {
    if (!ctx) return;
    const int pad = 12;
    const int navW = 190;
    const int gap = 12;
    const int titleH = 24;
    const int subtitleH = 20;
    const int searchH = 26;
    const int btnH = 28;
    const int btnW = 100;
    const int findW = 82;
    RECT rc{};
    GetClientRect(hWnd, &rc);
    const int btnY = rc.bottom - pad - btnH;
    const int rightX = pad + navW + gap;
    const int rightW = std::max(100, static_cast<int>(rc.right) - rightX - pad);
    const int contentY = pad + titleH + subtitleH + 10 + searchH + 8;
    if (ctx->navLabel) MoveWindow(ctx->navLabel, pad, pad, navW, 20, TRUE);
    if (ctx->nav) MoveWindow(ctx->nav, pad, pad + 22, navW, btnY - (pad + 22) - 8, TRUE);
    if (ctx->title) MoveWindow(ctx->title, rightX, pad, rightW, titleH, TRUE);
    if (ctx->subtitle) MoveWindow(ctx->subtitle, rightX, pad + titleH, rightW, subtitleH, TRUE);
    if (ctx->search) MoveWindow(ctx->search, rightX, pad + titleH + subtitleH + 10,
                                std::max(60, rightW - findW - 8), searchH, TRUE);
    if (ctx->btnFind) MoveWindow(ctx->btnFind, rightX + std::max(60, rightW - findW),
                                 pad + titleH + subtitleH + 10, findW, searchH, TRUE);
    if (ctx->edit) MoveWindow(ctx->edit, rightX, contentY, rightW, btnY - contentY - 8, TRUE);
    if (ctx->btnClose) MoveWindow(ctx->btnClose, rc.right - pad - btnW, btnY, btnW, btnH, TRUE);
}

static LRESULT CALLBACK HelpDialogProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<HelpDialogCtx*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<HelpDialogCtx*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        ctx->currentSection = std::clamp(ctx->initialSection, 0, kHelpSectionCount - 1);
        const bool english = IsEnglishUi();
        ctx->navLabel = CreateWindowExW(0, L"STATIC", english ? L"Topics" : L"項目",
                                        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(1000)), g_hInst, nullptr);
        ctx->nav = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL,
                                   0, 0, 0, 0, hWnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHelpNavId)), g_hInst, nullptr);
        for (int section = 0; section < kHelpSectionCount; ++section) {
            SendMessageW(ctx->nav, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(HelpSectionLabel(section, english)));
        }
        SendMessageW(ctx->nav, LB_SETCURSEL, ctx->currentSection, 0);
        ctx->title = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     0, 0, 0, 0, hWnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001)), g_hInst, nullptr);
        ctx->subtitle = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        0, 0, 0, 0, hWnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(1002)), g_hInst, nullptr);
        ctx->search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      0, 0, 0, 0, hWnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHelpSearchId)), g_hInst, nullptr);
        ctx->btnFind = CreateWindowExW(0, L"BUTTON", english ? L"Find next" : L"次を検索",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                       0, 0, 0, 0, hWnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHelpFindId)), g_hInst, nullptr);
        ctx->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                                        ES_AUTOVSCROLL | WS_VSCROLL,
                                    0, 0, 0, 0, hWnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(1003)), g_hInst, nullptr);
        ctx->btnClose = CreateWindowExW(0, L"BUTTON", english ? L"Close" : L"閉じる",
                                        WS_CHILD | WS_VISIBLE,
                                        0, 0, 0, 0, hWnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), g_hInst, nullptr);
        SetUIFont(ctx->navLabel); SetUIFont(ctx->nav); SetUIFont(ctx->title); SetUIFont(ctx->subtitle);
        SetUIFont(ctx->search); SetUIFont(ctx->btnFind); SetUIFont(ctx->edit); SetUIFont(ctx->btnClose);
        LayoutHelpDialog(hWnd, ctx);
        UpdateHelpDialogContent(ctx);
        ApplyThemeToDialog(hWnd);
        return 0;
    }
    case WM_THEMECHANGED:
        ApplyThemeToDialog(hWnd);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* minmax = reinterpret_cast<MINMAXINFO*>(lParam);
        minmax->ptMinTrackSize.x = kHelpMinWidth;
        minmax->ptMinTrackSize.y = kHelpMinHeight;
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{}; GetClientRect(hWnd, &rc);
        HBRUSH bg = g_hThemeWindowBrush ? g_hThemeWindowBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        FillRect(hdc, &rc, bg);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return ThemeCtlColorPanel(reinterpret_cast<HWND>(lParam), reinterpret_cast<HDC>(wParam));
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (DrawThemeButton(dis)) return TRUE;
        break;
    }
    case WM_SIZE:
        LayoutHelpDialog(hWnd, ctx);
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == kHelpNavId && HIWORD(wParam) == LBN_SELCHANGE && ctx && ctx->nav) {
            const int selected = static_cast<int>(SendMessageW(ctx->nav, LB_GETCURSEL, 0, 0));
            if (selected >= 0 && selected < kHelpSectionCount) {
                ctx->currentSection = selected;
                UpdateHelpDialogContent(ctx);
            }
            return 0;
        }
        if (id == kHelpFindId) {
            FindInHelpSection(ctx);
            return 0;
        }
        if (id == kHelpSearchId && HIWORD(wParam) == EN_CHANGE && ctx) {
            ctx->nextSearchOffset = 0;
            ShowHelpSearchStatus(ctx, HelpSearchStatus::Ready);
            return 0;
        }
        if (id == IDOK || id == IDCANCEL) { DestroyWindow(hWnd); return 0; }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        if (ctx) ctx->done = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static std::optional<uintmax_t> TryFileSize(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return std::nullopt;
    auto sz = std::filesystem::file_size(path, ec);
    if (ec) return std::nullopt;
    return sz;
}

static std::wstring FormatWithCommas(uintmax_t v) {
    std::wstring s = std::to_wstring(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(static_cast<size_t>(i), 1, L',');
    }
    return s;
}

static std::wstring FormatBytes(uintmax_t bytes) {
    std::wstringstream ss;
    ss << FormatWithCommas(bytes) << L" bytes";

    const double b = static_cast<double>(bytes);
    const wchar_t* unit = nullptr;
    double value = 0.0;
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        unit = L"GiB";
        value = b / (1024.0 * 1024.0 * 1024.0);
    } else if (bytes >= 1024ull * 1024ull) {
        unit = L"MiB";
        value = b / (1024.0 * 1024.0);
    } else if (bytes >= 1024ull) {
        unit = L"KiB";
        value = b / 1024.0;
    }

    if (unit) {
        ss << L" (" << std::fixed << std::setprecision(2) << value << L" " << unit << L")";
    }
    return ss.str();
}

static std::optional<std::wstring> PdfMetaText(FPDF_DOCUMENT doc, const char* tag) {
    if (!doc || !tag) return std::nullopt;
    std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
    unsigned long bytes = FPDF_GetMetaText(doc, tag, nullptr, 0);
    if (bytes <= sizeof(wchar_t) || (bytes % sizeof(wchar_t)) != 0) return std::nullopt;
    std::wstring buf;
    buf.resize(bytes / sizeof(wchar_t));
    if (!FPDF_GetMetaText(doc, tag, buf.data(), bytes)) return std::nullopt;
    if (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    while (!buf.empty() && (buf.back() == L'\r' || buf.back() == L'\n' || buf.back() == L' ' || buf.back() == L'\t')) {
        buf.pop_back();
    }
    size_t start = 0;
    while (start < buf.size() && (buf[start] == L'\r' || buf[start] == L'\n' || buf[start] == L' ' || buf[start] == L'\t')) {
        ++start;
    }
    if (start > 0) buf.erase(0, start);
    if (buf.empty()) return std::nullopt;
    return buf;
}

static std::wstring FormatPtMm(double wPt, double hPt) {
    const double wMm = wPt * 25.4 / 72.0;
    const double hMm = hPt * 25.4 / 72.0;
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(2)
       << wPt << L" x " << hPt << L" pt  ("
       << wMm << L" x " << hMm << L" mm)";
    return ss.str();
}

static std::wstring FormatPermissionsLine(bool isEn, unsigned long userPerms) {
    if (userPerms == 0xfffffffful) {
        return isEn ? L"Permissions (user): unrestricted" : L"権限（ユーザー）: 制限なし";
    }

    auto yesno = [&](bool ok) { return ok ? (isEn ? L"Yes" : L"可") : (isEn ? L"No" : L"不可"); };
    const bool canPrint = (userPerms & 0x4u) != 0;
    const bool canModify = (userPerms & 0x8u) != 0;
    const bool canCopy = (userPerms & 0x10u) != 0;
    const bool canAnnot = (userPerms & 0x20u) != 0;
    const bool canFill = (userPerms & 0x100u) != 0;
    const bool canCopyAcc = (userPerms & 0x200u) != 0;
    const bool canAssemble = (userPerms & 0x400u) != 0;
    const bool canPrintHq = (userPerms & 0x800u) != 0;

    std::wstringstream ss;
    if (isEn) {
        ss << L"Permissions (user): Print=" << yesno(canPrint)
           << L", Edit=" << yesno(canModify)
           << L", Copy=" << yesno(canCopy)
           << L", Annotate=" << yesno(canAnnot)
           << L", FillForms=" << yesno(canFill)
           << L", Accessibility=" << yesno(canCopyAcc)
           << L", Assemble=" << yesno(canAssemble)
           << L", HQPrint=" << yesno(canPrintHq);
    } else {
        ss << L"権限（ユーザー）: 印刷=" << yesno(canPrint)
           << L", 編集=" << yesno(canModify)
           << L", コピー=" << yesno(canCopy)
           << L", 注釈=" << yesno(canAnnot)
           << L", フォーム入力=" << yesno(canFill)
           << L", アクセシビリティ=" << yesno(canCopyAcc)
           << L", 組み立て=" << yesno(canAssemble)
           << L", 高品質印刷=" << yesno(canPrintHq);
    }
    return ss.str();
}

static std::wstring BuildPdfInfoText() {
    const bool isEn = IsEnglishUi();
    std::wstring out;
    auto line = [&](const std::wstring& s) {
        out += s;
        out += L"\r\n";
    };

    line(isEn ? L"PDF Info" : L"PDF情報");
    line(L"");

    if (g_pdf.kind != DocKind::Pdf || !g_pdf.doc) {
        line(isEn ? L"No PDF is currently open." : L"現在PDFが開かれていません。");
        if (!g_currentNotePath.empty()) {
            line(L"");
            line((isEn ? L"Note file: " : L"ノート: ") + g_currentNotePath);
            if (auto sz = TryFileSize(std::filesystem::path(g_currentNotePath))) {
                line((isEn ? L"Note size: " : L"ノートサイズ: ") + FormatBytes(*sz));
            }
        }
        return out;
    }

    std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
    const std::wstring pdfPath = g_pdf.path;
    line((isEn ? L"File: " : L"ファイル: ") + (pdfPath.empty() ? L"-" : pdfPath));
    if (!pdfPath.empty()) {
        if (auto sz = TryFileSize(std::filesystem::path(pdfPath))) {
            line((isEn ? L"File size: " : L"ファイルサイズ: ") + FormatBytes(*sz));
        }
    }

    const int pageCount = FPDF_GetPageCount(g_pdf.doc);
    line((isEn ? L"Pages: " : L"ページ数: ") + std::to_wstring(pageCount));

    int fileVersion = 0;
    if (FPDF_GetFileVersion(g_pdf.doc, &fileVersion)) {
        std::wstringstream ss;
        ss << (isEn ? L"PDF version: " : L"PDFバージョン: ")
           << std::fixed << std::setprecision(1) << (static_cast<double>(fileVersion) / 10.0);
        line(ss.str());
    }

    double wPt = 0.0, hPt = 0.0;
    if (pageCount > 0 && FPDF_GetPageSizeByIndex(g_pdf.doc, 0, &wPt, &hPt) && wPt > 0.0 && hPt > 0.0) {
        const bool portrait = (hPt >= wPt);
        line(L"");
        line(isEn ? L"Representative page (1):" : L"代表ページ（1）:");
        line((isEn ? L"  MediaBox: " : L"  MediaBox: ") + FormatPtMm(wPt, hPt));
        line((isEn ? L"  Orientation: " : L"  縦横: ") + std::wstring(portrait ? (isEn ? L"Portrait" : L"縦（縦長）") : (isEn ? L"Landscape" : L"横（横長）")));
        {
            const double wh = wPt / hPt;
            const double hw = hPt / wPt;
            std::wstringstream ss;
            ss << std::fixed << std::setprecision(4);
            ss << (isEn ? L"  Aspect (w/h): " : L"  比率 (w/h): ") << wh
               << (isEn ? L"  (h/w: " : L"  (h/w: ") << hw << L")";
            line(ss.str());
        }

        FPDF_PAGE page = FPDF_LoadPage(g_pdf.doc, 0);
        if (page) {
            float left = 0.0f, bottom = 0.0f, right = 0.0f, top = 0.0f;
            if (FPDFPage_GetCropBox(page, &left, &bottom, &right, &top)) {
                const double cw = std::abs(static_cast<double>(right) - static_cast<double>(left));
                const double ch = std::abs(static_cast<double>(top) - static_cast<double>(bottom));
                line((isEn ? L"  CropBox: " : L"  CropBox: ") + FormatPtMm(cw, ch));
            } else {
                line(isEn ? L"  CropBox: (not present)" : L"  CropBox: （なし）");
            }
            int rot = FPDFPage_GetRotation(page);
            line((isEn ? L"  Rotation: " : L"  回転: ") + std::to_wstring(rot * 90) + (isEn ? L" deg" : L" 度"));
            FPDF_ClosePage(page);
        }
    }

    line(L"");
    auto metaOrDash = [&](const char* tag) -> std::wstring {
        if (auto v = PdfMetaText(g_pdf.doc, tag)) return *v;
        return L"-";
    };
    line((isEn ? L"Title: " : L"タイトル: ") + metaOrDash("Title"));
    line((isEn ? L"Author: " : L"作成者(Author): ") + metaOrDash("Author"));
    line((isEn ? L"Creator: " : L"Creator: ") + metaOrDash("Creator"));
    line((isEn ? L"Producer: " : L"Producer: ") + metaOrDash("Producer"));
    line((isEn ? L"CreationDate: " : L"作成日(CreationDate): ") + metaOrDash("CreationDate"));
    line((isEn ? L"ModDate: " : L"更新日(ModDate): ") + metaOrDash("ModDate"));

    const int secRev = FPDF_GetSecurityHandlerRevision(g_pdf.doc);
    line(L"");
    if (secRev < 0) {
        line(isEn ? L"Protection: none" : L"編集保護: なし");
    } else {
        line((isEn ? L"Protection: enabled (security rev: " : L"編集保護: あり（security rev: ") +
             std::to_wstring(secRev) + (isEn ? L")" : L"）"));
    }
    const unsigned long userPerms = FPDF_GetDocUserPermissions(g_pdf.doc);
    line(FormatPermissionsLine(isEn, userPerms));
    {
        std::wstringstream ss;
        ss << (isEn ? L"User permissions raw: 0x" : L"ユーザー権限 raw: 0x")
           << std::hex << std::setw(8) << std::setfill(L'0') << userPerms;
        line(ss.str());
    }

    line(L"");
    if (!pdfPath.empty()) {
        const auto clropPath = clrop_bridge::ClropPathForPdf(pdfPath);
        if (!clropPath.empty()) {
            line((isEn ? L".clrop: " : L".clrop: ") + clropPath);
            if (auto sz = TryFileSize(std::filesystem::path(clropPath))) {
                line((isEn ? L".clrop size: " : L".clrop サイズ: ") + FormatBytes(*sz));
            } else {
                line(isEn ? L".clrop size: (not found)" : L".clrop サイズ: （未作成/見つかりません）");
            }
        }
    }
    if (!g_currentNotePath.empty()) {
        line((isEn ? L"Note file: " : L"ノートファイル: ") + g_currentNotePath);
        if (auto sz = TryFileSize(std::filesystem::path(g_currentNotePath))) {
            line((isEn ? L"Note size: " : L"ノートサイズ: ") + FormatBytes(*sz));
        }
    }

    return out;
}

static LRESULT CALLBACK PdfInfoDialogProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<PdfInfoDialogCtx*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<PdfInfoDialogCtx*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

        const int pad = 12;
        const int btnH = 28;
        const int btnW = 110;
        RECT rc{};
        GetClientRect(hWnd, &rc);
        const int btnY = rc.bottom - pad - btnH;

        ctx->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                                        ES_AUTOVSCROLL | WS_VSCROLL,
                                    pad, pad, rc.right - pad * 2, btnY - pad, hWnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(1101)),
                                    g_hInst, nullptr);

        const wchar_t* copyLabel = IsEnglishUi() ? L"Copy" : L"コピー";
        const wchar_t* closeLabel = IsEnglishUi() ? L"Close" : L"閉じる";
        ctx->btnClose = CreateWindowExW(0, L"BUTTON", closeLabel,
                                        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                        rc.right - pad - btnW, btnY, btnW, btnH, hWnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
                                        g_hInst, nullptr);
        ctx->btnCopy = CreateWindowExW(0, L"BUTTON", copyLabel,
                                       WS_CHILD | WS_VISIBLE,
                                       rc.right - pad - btnW * 2 - 8, btnY, btnW, btnH, hWnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(1102)),
                                       g_hInst, nullptr);

        SetUIFont(ctx->edit);
        SetUIFont(ctx->btnCopy);
        SetUIFont(ctx->btnClose);

        const auto text = NormalizeNewlines(BuildPdfInfoText());
        SetWindowTextW(ctx->edit, text.c_str());
        SendMessageW(ctx->edit, EM_SETSEL, 0, 0);

        ApplyThemeToDialog(hWnd);
        return 0;
    }
    case WM_THEMECHANGED:
        ApplyThemeToDialog(hWnd);
        return 0;
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = g_hThemeWindowBrush ? g_hThemeWindowBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        FillRect(hdc, &rc, bg);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        return ThemeCtlColorPanel(ctl, hdc);
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (DrawThemeButton(dis)) return TRUE;
        break;
    }
    case WM_SIZE: {
        if (!ctx) break;
        const int pad = 12;
        const int btnH = 28;
        const int btnW = 110;
        RECT rc{};
        GetClientRect(hWnd, &rc);
        const int btnY = rc.bottom - pad - btnH;
        if (ctx->edit) {
            MoveWindow(ctx->edit, pad, pad, rc.right - pad * 2, btnY - pad, TRUE);
        }
        if (ctx->btnClose) {
            MoveWindow(ctx->btnClose, rc.right - pad - btnW, btnY, btnW, btnH, TRUE);
        }
        if (ctx->btnCopy) {
            MoveWindow(ctx->btnCopy, rc.right - pad - btnW * 2 - 8, btnY, btnW, btnH, TRUE);
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == 1102) {
            if (ctx && ctx->edit) {
                SendMessageW(ctx->edit, EM_SETSEL, 0, -1);
                SendMessageW(ctx->edit, WM_COPY, 0, 0);
                SendMessageW(ctx->edit, EM_SETSEL, 0, 0);
            }
            return 0;
        }
        if (id == IDOK || id == IDCANCEL) {
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        if (ctx) ctx->done = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace

static void ShowHelpCenterDialog(HWND owner, int initialSection) {
    static bool registered = false;
    static const wchar_t kClass[] = L"HelpDialogClass";
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = HelpDialogProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = g_hThemeWindowBrush ? g_hThemeWindowBrush
                                              : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClass;
        RegisterClassW(&wc);
        registered = true;
    }

    HelpDialogCtx ctx{};
    ctx.owner = owner;
    ctx.windowTitle = IsEnglishUi() ? L"Help" : L"ヘルプ";
    ctx.initialSection = initialSection;

    if (owner) EnableWindow(owner, FALSE);
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, ctx.windowTitle.c_str(),
                             WS_CAPTION | WS_POPUPWINDOW | WS_VISIBLE | WS_SIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 840, 580,
                             owner, nullptr, g_hInst, &ctx);
    if (!w) {
        if (owner) EnableWindow(owner, TRUE);
        return;
    }

    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);

    MSG msg;
    while (!ctx.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (ui::ConsumeNoOpEdgeNavKeyForMultilineEdit(msg, g_hNoteEdit)) continue;
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
}

void ShowHelpDialog(HWND owner) {
    ShowHelpCenterDialog(owner, kHelpSectionStart);
}

void ShowCustomExtensionHelpDialog(HWND owner) {
    ShowHelpCenterDialog(owner, kHelpSectionExtensions);
}

void ShowPdfInfoDialog(HWND owner) {
    static bool registered = false;
    static const wchar_t kClass[] = L"PdfInfoDialogClass";
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = PdfInfoDialogProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = g_hThemeWindowBrush ? g_hThemeWindowBrush
                                              : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClass;
        RegisterClassW(&wc);
        registered = true;
    }

    PdfInfoDialogCtx ctx{};
    ctx.owner = owner;

    if (owner) EnableWindow(owner, FALSE);
    const wchar_t* title = IsEnglishUi() ? L"PDF Info" : L"PDF情報";
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, title,
                             WS_CAPTION | WS_POPUPWINDOW | WS_VISIBLE | WS_SIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 720, 560,
                             owner, nullptr, g_hInst, &ctx);
    if (!w) {
        if (owner) EnableWindow(owner, TRUE);
        return;
    }

    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);

    MSG msg;
    while (!ctx.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (ui::ConsumeNoOpEdgeNavKeyForMultilineEdit(msg, g_hNoteEdit)) continue;
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
}

