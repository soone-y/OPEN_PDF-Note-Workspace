// file: help.cpp
#include "help/help.h"

#include "bridge/view_bridge.h"
#include "core/app_core.h"
#include "core/localization.h"
#include "core/path_safety.h"
#include "clrop/bridge.h"
#include "ui/noop_nav_guard.h"
#include "workspace/workspace_actions.h"

#include <shellapi.h>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
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
    HWND btnFindAll = nullptr;
    HWND edit = nullptr;
    HWND btnOpenDocuments = nullptr;
    HWND btnClose = nullptr;
    std::wstring windowTitle;
    std::wstring bodyText;
    int initialSection = 0;
    int currentSection = 0;
    size_t nextSearchOffset = 0;
    int nextGlobalSearchSection = 0;
    size_t searchMatchIndex = 0;
    bool searchUsesAllSections = false;
    bool ownerRestored = false;
    bool done = false;
};

struct PdfInfoDialogCtx {
    HWND owner = nullptr;
    HWND edit = nullptr;
    HWND btnCopy = nullptr;
    HWND btnCopyPaths = nullptr;
    HWND btnClose = nullptr;
    std::wstring pathClipboardText;
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

static bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty()) return false;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return false;
    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        return false;
    }
    memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!OpenClipboard(owner)) {
        GlobalFree(memory);
        return false;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        CloseClipboard();
        GlobalFree(memory);
        return false;
    }
    CloseClipboard();
    return true;
}

// The in-app help is intentionally a quick, read-only reference rather than a
// second Markdown editor.  Keep the Markdown document as the source of truth,
// then turn its small supported subset into readable plain text for the native
// EDIT control.  Full Markdown layout remains available in readonly_viewer.
static std::wstring RenderHelpMarkdownForDisplay(const std::wstring& markdown) {
    auto renderInline = [](const std::wstring& source) {
        std::wstring out;
        out.reserve(source.size());
        for (size_t i = 0; i < source.size();) {
            if ((source.compare(i, 2, L"**") == 0) ||
                (source.compare(i, 2, L"__") == 0) ||
                (source.compare(i, 2, L"~~") == 0)) {
                i += 2;
                continue;
            }
            if (source[i] == L'`') {
                const size_t close = source.find(L'`', i + 1);
                if (close != std::wstring::npos) {
                    // Inline code in Help_Reference.md names visible buttons,
                    // menu paths, keys, files, and folders.  Give it a compact
                    // control-like shape rather than exposing Markdown syntax.
                    out += L"［";
                    out.append(source, i + 1, close - i - 1);
                    out += L"］";
                    i = close + 1;
                    continue;
                }
                // Keep malformed input visible and guarantee forward progress.
                out.push_back(source[i++]);
                continue;
            }
            out.push_back(source[i++]);
        }
        return out;
    };

    std::wstringstream input(NormalizeNewlines(markdown));
    std::wstring line;
    std::wstring out;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t content = 0;
        while (content < line.size() && line[content] == L'#') ++content;
        const bool isHeading = content > 0 && content < line.size() && line[content] == L' ';
        if (isHeading) {
            while (content < line.size() && line[content] == L' ') ++content;
            out += content == line.size() ? L"" : L"■ ";
            out += renderInline(line.substr(content));
        } else if (line.size() >= 2 && line[0] == L'-' && line[1] == L' ') {
            // Older bundled documents used Markdown list lines.  Keep them
            // readable without adding a second visual marker to explanations.
            out += renderInline(line.substr(2));
        } else {
            out += renderInline(line);
        }
        out += L"\r\n";
    }
    while (out.size() >= 2 && out.compare(out.size() - 2, 2, L"\r\n") == 0) {
        out.resize(out.size() - 2);
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
    std::vector<wchar_t> buffer(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (len > 0 && len >= buffer.size() - 1) {
        buffer.resize(buffer.size() * 2, L'\0');
        len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    if (len == 0) return {};
    return std::filesystem::path(std::wstring(buffer.data(), len)).parent_path();
}

static bool IsSafeBundledHelpPath(const std::filesystem::path& path, bool directory) {
    if (path.empty() || !path.is_absolute() || IsUncPath(path)) return false;
    bool isReparse = false;
    if (!TryIsReparsePointNoFollow(path, isReparse) || isReparse) return false;
    std::error_code ec;
    return (directory ? std::filesystem::is_directory(path, ec)
                      : std::filesystem::is_regular_file(path, ec)) && !ec;
}

static std::optional<std::filesystem::path> FindBundledHelpDocsDir(const std::filesystem::path& exeDir) {
    const wchar_t* const locale = IsEnglishUi() ? L"en" : L"ja";
    const std::filesystem::path candidates[] = {
        exeDir / L"docs",                  // release: <release>/docs
        exeDir.parent_path().parent_path() / L"docs" / locale, // development: <repo>/out/bin
    };
    for (const auto& candidate : candidates) {
        if (IsSafeBundledHelpPath(candidate, true)) return candidate;
    }
    return std::nullopt;
}

static std::optional<std::filesystem::path> FindBundledHelpReferenceDocument(
    const std::filesystem::path& exeDir) {
    const auto docsDir = FindBundledHelpDocsDir(exeDir);
    if (!docsDir) return std::nullopt;
    const std::filesystem::path reference = *docsDir / L"Help_Reference.md";
    if (!IsSafeBundledHelpPath(reference, false)) return std::nullopt;
    return reference;
}

static bool LaunchBundledHelpGuide(HWND owner) {
    const std::filesystem::path exeDir = GetExeDir();
    const std::filesystem::path viewerPath = exeDir / L"readonly_viewer.exe";
    if (!IsSafeBundledHelpPath(viewerPath, false)) return false;

    // Open the actual reference document.  Passing only --folder populated the
    // Viewer tree but deliberately left every document unopened.
    std::wstring parameters;
    if (const auto reference = FindBundledHelpReferenceDocument(exeDir)) {
        parameters = L"--open \"" + reference->wstring() + L"\"";
    }
    const HINSTANCE result = ShellExecuteW(owner, L"open", viewerPath.c_str(), parameters.c_str(),
                                            exeDir.c_str(), SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

static std::wstring OfficeConversionHelpTextJa() {
    if (HasOfficeConversionFeature()) {
        return
            L"■ OfficeファイルをPDFに変換\n"
            L"- この通常版では、.docx と .pptx を同梱LibreOfficeでローカルPDF変換できます。\n"
            L"- メニューの変換機能またはワークスペースへのドロップで使えます。ドロップ時は変換、元ファイルをそのまま取り込む、キャンセルを選べます。\n"
            L"- Microsoft Officeやオンライン変換サービスは使用しません。元のOfficeファイルは変更しません。\n"
            L"- runtimeが見つからない場合はLite版へ切り替わりません。通常版の配布フォルダ一式を復元してください。\n"
            L"- 変換は試験的です。フォント、図形、数式などで見た目が異なることがあるため、PDFの結果を確認してください。\n"
            L"\n";
    }
    return
        L"■ OfficeファイルをPDFに変換（Lite版）\n"
        L"- この版はOffice-to-PDF変換runtimeを含まないLite版です。ウィンドウ名にもLiteと表示されます。\n"
            L"- .docx / .pptx はPDF変換できませんが、元ファイルのまま現在の回次へ取り込めます。PDFを開くには通常版で変換するか、別途PDFを用意してください。\n"
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
        L"編集内容は内部で自動保護し、必要な時だけ原本へ安全に保存します。\n"
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
        L"- 自動作業保護: 変更は原本とは別に内部で保護されます。\n"
        L"- 作業保存: Ctrl+S、保存メニュー、通常終了、または出力の直前に原本へ反映します。\n"
        L"- バックアップ: 原本へ統合する前にバックアップを作成します。\n"
        L"\n"
        L"■ 保存の考え方\n"
        L"- 自動作業保護: 原本へ反映していない編集内容を、内部の復旧用データとして保持します。\n"
        L"- 作業保存: 自動作業保護の内容を確認して原本へ安全に反映する処理です。\n"
        L"- ノートの自動作業保護: 行移動・フォーカス離脱・保存前・6〜20秒無操作で更新します。\n"
        L"- 注釈の自動作業保護: 設定した間隔で更新します。\n"
        L"- 切替時: 原本を書き換えず、自動作業保護だけを更新します。\n"
        L"- 原本へ保存した後の undo/redo 履歴は保持対象外です。PDF位置または最終オープン時刻を戻す場合は復元メニューを使います。バックアップは復元メニューの一覧から復元または個別に削除できます。\n"
        L"\n"
        L"■ ノート形式\n"
        L"- 新規ノートは本ソフトのノート形式である .clro として作成されます。\n"
        L"- .md もノート作成時の拡張子変更から選べますが、既定のノート拡張子ではありません。\n"
        L"- .md と .clro は現在同じ Markdown 系記法、独自 markup、数式記法を扱いますが、拡張子の扱いは同一ではありません。\n"
        L"- .txt はプレーンテキストとして扱われ、装飾や構文解析は行いません。\n"
        L"\n"
        L"■ 推奨される保存手順\n"
        L"1) ノートや注釈を編集する\n"
        L"2) 自動作業保護で途中状態を保持する\n"
        L"3) 区切りのよいタイミングで Ctrl+S または「保存 → 作業保存」を実行する\n"
        L"4) 必要な場合だけ「保存 → 保存状態を確認」で保護中の作業を確認する\n"
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
        L"■ 保存状態の確認\n"
        L"メニュー: 保存 → 保存状態を確認\n"
        L"- 採用: 原本を変えず、保護中の作業を現在の採用状態にします。\n"
        L"- 原本へ保存: 保護中の作業を原本へ反映し、成功時に内部データを整理します。\n"
        L"- 破棄: 保護中の作業だけを削除します。原本は変わりません。\n"
        L"\n"
        L"■ 復元 / バックアップ\n"
        L"メニュー: 復元 → PDF位置 / 最終オープン時刻\n"
        L"- PDF位置と最終オープン時刻は、削除または復元を選べます。\n"
        L"- ファイル最終オープン履歴では、回次ごとの最後に開いたPDF／ノートの記録を削除または復元できます。バックアップは一覧から復元または個別に削除できます。\n"
        L"\n"
        L"復元前に確認してください:\n"
        L"- 保護中のノートや注釈があると、復元後に原本だけ古い状態へ戻ることがあります。\n"
        L"- 必要なら保存メニューの「保存状態を確認」で内容を確認するか、先に Ctrl+S で保存してください。\n"
        L"\n"
        L"■ 保存先の目安\n"
        L"- 自動作業保護: __pdf_note_workspace__/__tmp__/__stage__/\n"
        L"- バックアップ本体: __pdf_note_workspace__/__escape__/backup/.../*.bak\n"
        L"- バックアップ情報: __pdf_note_workspace__/__escape__/backup/.../*.bak.meta.txt\n"
        L"- ノート保存失敗時の退避: __pdf_note_workspace__/__escape__/note_recovery/\n"
        L"- 原子的保存に失敗した一時ファイルの退避先: __pdf_note_workspace__/__escape__/\n"
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
        L"- 設定ファイル: __pdf_note_workspace__/__settings__/tool_shortcuts.json\n"
        L"- 例: { \"key\": \"Ctrl+Alt+7\", \"tool\": \"freehand\" } / { \"key\": \"Numpad7\", \"tool\": \"freehand\" }\n"
        L"- ノート入力中、PDFテキスト編集中、IME変換中は、文字入力を優先します。\n"
        L"\n"
        L"■ 補足\n"
        L"- 一時フォルダ (__pdf_note_workspace__/__tmp__) は、自動保存や中間処理に使います。\n"
        L"- 単位の目安: pt = mm × 72 / 25.4\n";
}

static std::wstring DefaultHelpTextEn() {
    return std::wstring(
        L"■ Overview\n"
        L"This app is a local-only tool for working safely with PDF notes and annotations.\n"
        L"While editing, it protects work internally and updates original files only when needed.\n"
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
        L"- Changes are protected internally before the original files are updated.\n"
        L"- Ctrl+S, Save Work, normal exit, and pre-output saving update the originals.\n"
        L"- A backup is created before integration.\n"
        L"\n"
        L"■ How Saving Works\n"
        L"- Automatic work protection: local recovery data that has not yet been applied to the original files.\n"
        L"- Save Work: safely apply protected work to the original files.\n"
        L"- Note protection updates on line move, focus leave, save, or 6-20s idle.\n"
        L"- Annotation protection updates on the configured background interval.\n"
        L"- Switching documents or root folders only updates protected work; it does not write original files.\n"
        L"- Undo/redo history is not retained after saving to the original files. Use Restore for PDF-position or last-open-time recovery; backups can be restored or individually deleted from the Restore menu.\n"
        L"\n"
        L"■ Note Formats\n"
        L"- New notes are created as .clro, the application's note format, by default.\n"
        L"- .md remains selectable from the extension menu, but it is not the default note extension.\n"
        L"- Both .md and .clro currently support Markdown-style syntax, custom markup, and math syntax, but the extensions are not interchangeable product semantics.\n"
        L"- .txt is treated as plain text without decoration or syntax parsing.\n"
        L"\n"
        L"■ Recommended Flow\n"
        L"1) Edit notes or annotations\n"
        L"2) Let automatic work protection keep intermediate work\n"
        L"3) Use Ctrl+S or Save > Save Work at safe checkpoints\n"
        L"4) Use Save > Review Save Status only when you need to inspect protected work\n"
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
        L"■ Review Save Status\n"
        L"Menu: Save -> Review Save Status\n"
        L"- Activate: mark protected work as current without writing to the original.\n"
        L"- Save to original: apply protected work to the original file and clean it up on success.\n"
        L"- Discard: remove only the protected work. The original file is unchanged.\n"
        L"\n"
        L"■ Restore / backups\n"
        L"Menu: Restore -> PDF Position / Last Open Time\n"
        L"- PDF position and last-open time can be deleted or restored.\n"
        L"- File last-open history can delete or restore each session's last-open PDF and note record. Backups can be restored or individually deleted from their list.\n"
        L"\n"
        L"Check before restoring:\n"
        L"- If protected note or annotation work remains, restoring may move only the original file back.\n"
        L"- Review save status from Save, or save with Ctrl+S if that is your intent.\n"
        L"\n"
        L"■ Typical Locations\n"
        L"- Automatic work protection: __pdf_note_workspace__/__tmp__/__stage__/\n"
        L"- Backup data: __pdf_note_workspace__/__escape__/backup/.../*.bak\n"
        L"- Backup metadata: __pdf_note_workspace__/__escape__/backup/.../*.bak.meta.txt\n"
        L"- Recovery copies for note save failures: __pdf_note_workspace__/__escape__/note_recovery/\n"
        L"- Quarantined temp files from failed atomic writes: __pdf_note_workspace__/__escape__/\n"
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
        L"- Config file: __pdf_note_workspace__/__settings__/tool_shortcuts.json\n"
        L"- Example: { \"key\": \"Ctrl+Alt+7\", \"tool\": \"freehand\" } / { \"key\": \"Numpad7\", \"tool\": \"freehand\" }\n"
        L"- Text input takes priority while editing notes, PDF text boxes, or IME composition.\n"
        L"\n"
        L"■ Notes\n"
        L"- The temporary folder (__pdf_note_workspace__/__tmp__) is used for auto-save and intermediate work.\n"
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
    kHelpSectionSoftware,
    kHelpSectionFiles,
    kHelpSectionMenus,
    kHelpSectionSaving,
    kHelpSectionOutput,
    kHelpSectionView,
    kHelpSectionExtensions,
    kHelpSectionNotes,
    kHelpSectionAnnotations,
    kHelpSectionSettings,
    kHelpSectionShortcuts,
    kHelpSectionTroubleshooting,
    kHelpSectionFullGuide,
    kHelpSectionCount,
};

static const wchar_t* HelpSectionLabelId(int section) {
    static constexpr const wchar_t* kIds[kHelpSectionCount] = {
        L"help.section.start", L"help.section.software", L"help.section.files",
        L"help.section.menus", L"help.section.saving", L"help.section.output",
        L"help.section.view", L"help.section.extensions", L"help.section.notes",
        L"help.section.annotations", L"help.section.settings", L"help.section.shortcuts",
        L"help.section.troubleshooting", L"help.section.full_guide",
    };
    return (section < 0 || section >= kHelpSectionCount) ? L"" : kIds[section];
}

static std::wstring HelpSectionLabel(int section) {
    return localization::Text(HelpSectionLabelId(section));
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

static std::wstring HelpSectionTitle(int section) {
    if (section == kHelpSectionStart) return localization::Text(L"help.title");
    return HelpSectionLabel(section);
}

static std::wstring HelpSectionSubtitle(int section) {
    switch (section) {
    case kHelpSectionStart:
        return localization::Text(L"help.subtitle.start");
    case kHelpSectionSoftware:
        return localization::Text(L"help.subtitle.software");
    case kHelpSectionFiles:
        return localization::Text(L"help.subtitle.files");
    case kHelpSectionMenus:
        return localization::Text(L"help.subtitle.menus");
    case kHelpSectionExtensions:
        return localization::Text(L"help.subtitle.extensions");
    case kHelpSectionSaving:
        return localization::Text(L"help.subtitle.saving");
    case kHelpSectionOutput:
        return localization::Text(L"help.subtitle.output");
    case kHelpSectionView:
        return localization::Text(L"help.subtitle.view");
    case kHelpSectionNotes:
        return localization::Text(L"help.subtitle.notes");
    case kHelpSectionAnnotations:
        return localization::Text(L"help.subtitle.annotations");
    case kHelpSectionSettings:
        return localization::Text(L"help.subtitle.settings");
    case kHelpSectionShortcuts:
        return localization::Text(L"help.subtitle.shortcuts");
    case kHelpSectionTroubleshooting:
        return localization::Text(L"help.subtitle.troubleshooting");
    default:
        return localization::Text(L"help.subtitle.full_guide");
    }
}

static std::optional<std::wstring> LoadHelpReferenceSection(int section) {
    const auto docsDir = FindBundledHelpDocsDir(GetExeDir());
    if (!docsDir) return std::nullopt;
    const auto document = ReadUtf8TextFile(*docsDir / L"Help_Reference.md");
    if (!document) return std::nullopt;
    // The concise distributed reference has six headings while this dialog
    // has fourteen task-oriented categories.  The detailed-guide category is
    // therefore the exact document view; short categories below remain safe
    // fallbacks instead of trying to match unrelated headings.
    if (section != kHelpSectionFullGuide) return std::nullopt;
    return RenderHelpMarkdownForDisplay(*document);
}

static std::wstring HelpSectionBody(int section, bool english) {
    // Help_Reference.md is the Japanese help-content source of truth. The
    // in-code text below is a distribution-damage fallback only.
    if (const auto documented = LoadHelpReferenceSection(section)) return *documented;
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
    case kHelpSectionSoftware:
        return english
            ? L"■ Main application\r\n- Choose a root folder, edit notes and PDF annotations, save, recover, and export.\r\n- It works locally and does not use online conversion or update checks.\r\n\r\n■ Read-only Viewer\r\n- Opens bundled Markdown documents and files for reference without using the file association.\r\n- Use Help > Detailed guide when you need the longer bundled documents.\r\n"
            : L"■ メインソフト\r\n- ルートフォルダを指定し、ノート・PDF注釈を編集、保存、復元、出力します。\r\n- オンライン変換・更新確認・外部通信は使いません。\r\n\r\n■ 読み取り専用Viewer\r\n- 同梱Markdownなどを、関連付けに頼らず参照するための別ソフトです。\r\n- 長い案内は「詳細な案内」から開きます。\r\n";
    case kHelpSectionFiles:
        return english
            ? L"■ Root folder and files\r\n- File menu: choose/reload the root folder; create lectures, sessions, and .clro notes; import files or folders.\r\n- Drop one or more files onto the PDF or note list to copy them into the current session. Folders cannot be dropped. Imported files are sorted automatically by format.\r\n- Operations: rename or move the current PDF/note only after selecting it.\r\n- Organize session files groups supported files in a session; review the result before saving.\r\n\r\n■ Formats\r\n- PDF and .clrop annotations are a pair. .clro/.md are notes; .txt/.csv are plain text.\r\n- UNC paths and reparse-point folders are not used as root folders, so local data stays predictable.\r\n"
            : L"■ ルートフォルダとファイル\r\n- ファイルメニューから、ルートフォルダを指定／再読み込みする、講義・回次・.clroノートを作る、ファイルやフォルダを取り込みます。\r\n- PDF欄またはノート欄へ、1個以上のファイルをドロップすると現在の回次へコピーして取り込みます。フォルダーのドロップは非対応です。取り込み後は形式別に自動分類されます。\r\n- 操作メニューの名前変更・移動は、選択中のPDFまたはノートだけを対象にします。\r\n- 回次のファイル整理は対応形式をまとめます。保存前に結果を確認してください。\r\n\r\n■ 形式\r\n- PDFと.clropは組です。.clro/.mdはノート、.txt/.csvは書式なしテキストです。\r\n- UNCや再解析ポイントのフォルダは、保存先を取り違えないためルートフォルダに使いません。\r\n";
    case kHelpSectionMenus:
        return english
            ? L"■ Menu map\r\n- File: create/import items, choose or reload the root folder, organize files, and open folders in Explorer.\r\n- Edit: undo/redo, rename, or move the current PDF or note.\r\n- View: closing, page navigation, zoom, scrolling, readability, note wrapping, and the left column.\r\n- Save: save the current work to the original or review protected work. Restore contains protected position/history recovery.\r\n- Output: quick PDF and TXT-note output, or the output dialog for PDF, PNG, TXT, Markdown, and HTML.\r\n- Tools: " + localization::Text(L"workspace_memo.title") + L", the read-only viewer, local conversion, and blank-PDF creation.\r\n- Search opens its dialog directly. Settings includes the unified dialog, palette, and presets. Help provides reference and diagnostics.\r\n"
            : L"■ メニューの地図\r\n- ファイル: 作成・取込み、ルートフォルダの指定／再読込み、整理、エクスプローラーで開く操作です。\r\n- 編集: 現在のPDFまたはノートの取り消し／やりなおし、名前変更、移動です。\r\n- 表示: 閉じる、ページ移動、ズーム、スクロール、可視性、ノート折り返し、左カラムです。\r\n- 保存: 作業保存で原本へ安全に保存し、必要な場合に保存状態を確認します。復元には位置・履歴の復元をまとめます。\r\n- 出力: 簡単PDFと簡単ノート（txt）、またはPDF・PNG・txt・Markdown・HTMLを扱う出力ダイアログです。\r\n- ツール: " + localization::Text(L"workspace_memo.title") + L"、閲覧専用ソフト、ローカル変換、白紙PDF作成です。\r\n- 検索はダイアログを直接開きます。設定は統合ダイアログ、パレット、プリセットをまとめます。ヘルプは参照と診断です。\r\n";
    case kHelpSectionExtensions:
        return NormalizeNewlines(CustomExtensionHelpText());
    case kHelpSectionSaving:
        return english
            ? L"■ Safe saving\r\n- Editing does not directly overwrite the original PDF or note.\r\n- Automatic work protection keeps intermediate edits for recovery.\r\n- Ctrl+S, Save > Save Work, normal exit, and pre-output processing create a backup and safely save to the original.\r\n- Document/session switching keeps protected work and does not write originals.\r\n\r\n■ Restore\r\n- Restore > PDF Position and Restore > Last Open Time can delete or restore the corresponding protected state.\r\n- Restore > File Last-Open History can delete or restore each session's last-open PDF and note record. Restore > Backups lists saved backups for restoration or individual deletion.\r\n- Review Save Status before restoring when protected work remains.\r\n\r\n■ Typical locations\r\n- Internal work protection: __pdf_note_workspace__/__tmp__/__stage__/\r\n- Backups: __pdf_note_workspace__/__escape__/backup/\r\n- Note recovery: __pdf_note_workspace__/__escape__/note_recovery/\r\n"
            : L"■ 安全な保存\r\n- 編集中に元の PDF やノートを直接上書きしません。\r\n- 自動作業保護により、編集途中も復旧できるよう保持します。\r\n- Ctrl+S、「保存 > 作業保存」、通常終了、出力の直前には、バックアップを作成してから安全に原本へ保存します。\r\n- ファイル・回次・授業の切替では、保護中の作業を残し、原本は書き換えません。\r\n\r\n■ 復元\r\n- 「復元 > PDF位置」「復元 > 最終オープン時刻」では、対応する保護状態を削除または復元できます。\r\n- 「復元 > ファイル最終オープン履歴」では、回次ごとの最後に開いたPDF／ノートの記録を削除または復元できます。「復元 > バックアップ」では、保存済みバックアップを一覧から復元または個別に削除できます。\r\n- 保護中の作業があるときは、復元前に「保存状態を確認」を開いてください。\r\n\r\n■ 主な保存先\r\n- 内部作業保護: __pdf_note_workspace__/__tmp__/__stage__/\r\n- バックアップ: __pdf_note_workspace__/__escape__/backup/\r\n- ノート復旧: __pdf_note_workspace__/__escape__/note_recovery/\r\n";
    case kHelpSectionOutput:
        return english
            ? L"■ Quick output\r\n- Quick PDF exports the current PDF with annotations. Configure its scale and annotation options in Output Dialog > Save as Quick PDF.\r\n- Quick Note exports the current note as TXT; TXT is the default quick-note format. Configure its text options in Output Dialog > Save as Quick Note. View quick output settings opens a compact summary that can be closed immediately.\r\n\r\n■ Output dialog\r\n- Choose annotated PDF, selected-page PDF, PNG, TXT, Markdown, or HTML. Set a destination folder and output file name, then export that one output directly or add multiple settings to the queue and select Export.\r\n- Select a reservation to change its destination or file name, then choose Update reservation. Export selected runs only that reservation. Output safely integrates current edits first; the open original and duplicate queued destinations are rejected.\r\n- The results dialog can reveal output in Explorer. PDFs can open in the read-only viewer; other formats open in their associated application.\r\n\r\n■ Safety\r\n- Output creates a separate result. Existing output files require explicit overwrite confirmation; protected PDFs can restrict copy and export.\r\n"
            : L"■ 簡単出力\r\n- 簡単PDFは、現在のPDFを注釈入りの別ファイルへ出力します。出力ダイアログで倍率・注釈を設定し「簡単PDFに設定」を選ぶと、その設定を使います。\r\n- 簡単ノートは、現在のノートをtxtで出力します。簡単出力の規定形式はtxtです。出力ダイアログでテキストの設定を保存できます。「簡単出力設定を見る」では、現在の設定をすぐ閉じられる小さなウィンドウで確認できます。\r\n\r\n■ 出力ダイアログ\r\n- 注釈PDF、ページ指定PDF、PNG、txt、Markdown、HTMLを選べます。保存先フォルダと出力ファイル名を指定し、予約がなければその1件を「出力」で実行できます。複数の設定は予約に追加して「出力」でまとめて実行できます。\r\n- 予約を選び、保存先・ファイル名を変更して「予約を更新」できます。「選択を出力」はその予約1件だけを実行します。出力前には現在の変更を安全に統合し、原本と同じ出力先や予約間の重複を防ぎます。\r\n- 出力後の結果一覧から、エクスプローラーで表示できます。PDFは閲覧専用ソフトで開き、その他の形式は対応アプリで開けます。\r\n\r\n■ 安全性\r\n- 出力は別の結果ファイルを作ります。既存の出力先だけは上書きを確認します。保護付きPDFではコピー・出力が制限されます。\r\n";
    case kHelpSectionView:
        return english
            ? L"■ View menu\r\n- Reset/set zoom and use first/previous/next/last/jump-to page for PDF navigation.\r\n- Left column changes workspace navigation visibility. Bottom-right pane function switches between note, headings, math, and status assist immediately for the current run. Status assist has a subtle dedicated header and scrolls vertically when needed. Its Change settings group contains only controls available from this pane, including the page-number and zoom displays drawn over the PDF. These displays can be turned on or off independently. Monitor contains information only. Enabling keyboard controls moves focus directly to the note's normal mode. It does not save the pane choice; reloading the root folder or starting again restores the General settings default.\r\n- Readable low-contrast text improves difficult PDF text; single-page mode and scroll direction change reading behavior.\r\n- Note wrap is unavailable while rendered note mode requires its own layout.\r\n"
            : L"■ 表示メニュー\r\n- 拡大率を初期化／指定し、先頭・前・次・末尾・ページ指定でPDFを移動します。\r\n- 左カラムはワークスペース一覧の表示、右下領域の機能はノート・見出し・数式・状態アシストへこの起動中だけ即時に切り替えます。状態アシストは色で強調せず、控えめな専用見出しでノート欄と区別します。内容が収まらない場合は縦スクロールできます。「変更できる項目」にはPDF上のページ番号表示・PDF上の倍率表示を含む、この欄から操作できる設定だけを置きます。ページ番号と倍率の表示は個別にON/OFFできます。「監視する項目」には確認用の情報だけを置きます。キーボード操作をONにすると、入力先は直ちにノートの通常モードへ移ります。領域の選択は保存されず、ルートフォルダを再読み込みしたとき・次回起動時は設定ダイアログの標準状態に戻ります。\r\n- 低コントラスト文字を可読化、単一ページ、スクロール方向は閲覧方法を変えます。\r\n- ノート折返しは、レンダリング表示のレイアウトが必要な場合には使えません。\r\n";
    case kHelpSectionNotes:
        return english
            ? L"■ Note syntax\r\n- .clro, .md, and .markdown use the Markdown/MD4C route.\r\n- Supported custom markup can be combined with Markdown when needed.\r\n- Math notation is experimental; verify the display before relying on it.\r\n\r\n■ Plain text\r\n- .txt does not parse Markdown or custom markup. It has no rendered display or note-link features.\r\n"
            : L"■ ノート記法\r\n- .clro、.md、.markdown は Markdown/MD4C 経路を使います。\r\n- 必要な場合は、対応する独自 markup を Markdown と組み合わせられます。\r\n- 数式記法は試験的です。重要な内容は表示を確認してください。\r\n\r\n■ プレーンテキスト\r\n- .txt では Markdown や独自 markup を解析しません。レンダリング表示やノートリンクも使いません。\r\n";
    case kHelpSectionAnnotations:
        return english
            ? L"■ Annotation tools\r\n- Select, pan, eraser, and magnifier are interaction modes. Text, marker, pen, line, wave, arrow, and shapes create annotations stored separately in .clrop.\r\n- Annotation settings choose font, width, line style, arrow heads, fills, marker style, and freehand correction.\r\n\r\n■ Colors and palette\r\n- Tools can use their own colors. The user palette supplies reusable colors and a custom color slot.\r\n- Ctrl+Up/Down cycles the current annotation-tool color. Review tool and palette settings before changing a shared color.\r\n"
            : L"■ 注釈ツール\r\n- 選択、移動、消しゴム、拡大鏡は操作モードです。テキスト、マーカー、ペン、直線、波線、矢印、図形は.clropに保存する注釈を作ります。\r\n- 注釈設定では、フォント、線幅、線種、矢印、塗りつぶし、マーカー表示、手書き補正を選びます。\r\n\r\n■ 色とパレット\r\n- ツールごとの色に加え、再利用する色をユーザーパレットとカスタム色で管理します。\r\n- Ctrl+↑／Ctrl+↓は現在の注釈ツール色を切り替えます。共通色を変える前に、ツール設定とパレット設定を確認してください。\r\n";
    case kHelpSectionSettings:
        return english
            ? L"■ Settings categories\r\n- General: display/operation, note behavior, file/save protection, schedules/list order, and developer diagnostics. Choose the bottom-right pane function in Display and Interaction.\r\n- Note: fonts, rendering, wrapping, Vim-style editing, input, and markup-related behavior.\r\n- Annotation: tool appearance and behavior. Markup: supported note markup. Palette: reusable annotation colors.\r\n- Use the section selector at the top of a settings tab to jump directly to a section. It follows the section currently shown while you scroll.\r\n\r\n■ Applying settings\r\n- A settings preset stores the settings set, palette, shortcuts, schedule, and selected/custom themes. It never contains documents and can be loaded in another installation.\r\n- Loading first confirms replacement, keeps a recovery copy, and is not a document backup.\r\n- Some diagnostic log choices apply on the next launch. Settings do not rewrite existing PDF or note originals.\r\n"
            : L"■ 設定のカテゴリ\r\n- 一般設定: 表示と操作、ノート、ファイル／保存、スケジュールと一覧順、開発者向け診断。右下領域の機能は「表示と操作」で選びます。\r\n- ノート設定: フォント、レンダリング、折返し、Vim的編集、入力、記法に関わる挙動。\r\n- 注釈設定: ツールの見た目と動作。markup設定: ノート記法。パレット設定: 再利用する注釈色です。\r\n- 各設定タブの上部にある節の選択欄で、目的の節へ直接移動できます。本文をスクロールすると表示中の節に自動で切り替わります。\r\n\r\n■ 設定の反映\r\n- 設定プリセットは設定、パレット、ショートカット、時間割、選択中／カスタムテーマをまとめて保存します。文書は含まず、別の環境でも読み込めます。\r\n- 読込み前に置換を確認し、既存設定を退避します。文書バックアップには使いません。\r\n- 一部の診断ログ設定は次回起動時に反映されます。設定変更で既存PDFやノート原本を書き換えません。\r\n";
    case kHelpSectionShortcuts:
        return english
            ? L"■ Annotation shortcuts\r\n- Ctrl+Alt+number or a numpad key selects an annotation tool. Ctrl+Alt+arrow cycles tool category/detail; Ctrl+Up/Down cycles the current tool color.\r\n\r\n■ Vim-style note editing\r\n- The note settings choose whether normal mode keeps its structured rendering or shows its caret row as raw text, and whether a click enters insert mode. Insert mode always shows its caret or selection rows as raw text.\r\n- Text input, PDF text boxes, and IME composition take priority; do not assume navigation keys are commands while entering text.\r\n- Fixed annotation navigation keys cannot be replaced by the shortcut editor.\r\n"
            : L"■ 注釈ショートカット\r\n- Ctrl+Alt+数字またはテンキー単独で注釈ツールを切り替えます。Ctrl+Alt+矢印はカテゴリ／詳細種を循環し、Ctrl+↑／Ctrl+↓は現在のツール色を切り替えます。\r\n\r\n■ Vim的なノート編集\r\n- ノート設定では、通常モードを構成表示のままにするか、キャレット行を生テキスト表示にするかと、クリックでインサートモードへ入るかを選べます。インサートモードでは、キャレット行または選択行を常に生テキスト表示にします。\r\n- ノート入力、PDFテキストボックス、IME変換中は文字入力を優先します。入力中に移動キーをコマンドとして扱うとは限りません。\r\n- 注釈の固定循環キーは、ショートカット編集では置き換えられません。\r\n";
    case kHelpSectionTroubleshooting:
        return english
            ? L"■ Start with safe checks\r\n- If a file does not open, confirm its type and local path. If saving fails, keep the original and inspect staged changes, recovery copies, and backups.\r\n- For display or conversion differences, check the produced PDF or output before replacing or sharing anything.\r\n- Protected PDFs follow the rights holder's terms; this app restricts copy and export.\r\n\r\n■ Search words\r\n- Try: open, import, stage, backup, restore, export, color, shortcut, font, conversion, or Viewer.\r\n"
            : L"■ まず安全に確認すること\r\n- 開けないときは形式とローカルパスを確認します。保存に失敗しても原本は残し、stage、復旧コピー、バックアップを確認します。\r\n- 表示や変換結果が異なるときは、置換や共有の前に作成されたPDF・出力を確認します。\r\n- 保護付きPDFは権利者の条件に従ってください。本ソフトではコピー・出力を制限します。\r\n\r\n■ 検索語\r\n- 開く、取込、stage、バックアップ、復元、出力、色、ショートカット、フォント、変換、Viewer を試してください。\r\n";
    default:
        return LoadHelpText();
    }
}

constexpr int kHelpNavId = 1005;
constexpr int kHelpSearchId = 1006;
constexpr int kHelpFindId = 1007;
constexpr int kHelpOpenDocumentsId = 1008;
constexpr int kHelpFindAllId = 1009;
constexpr int kHelpMinWidth = 640;
constexpr int kHelpMinHeight = 460;

static void SetHelpFindButtonMode(HelpDialogCtx* ctx, bool next) {
    if (!ctx || !ctx->btnFind) return;
    SetWindowTextW(ctx->btnFind,
                   localization::Text(next ? L"help.next" : L"help.search").c_str());
}

static void ResetHelpSearch(HelpDialogCtx* ctx) {
    if (!ctx) return;
    ctx->nextSearchOffset = 0;
    ctx->nextGlobalSearchSection = ctx->currentSection;
    ctx->searchMatchIndex = 0;
    ctx->searchUsesAllSections = false;
    SetHelpFindButtonMode(ctx, false);
}

static void UpdateHelpDialogContent(HelpDialogCtx* ctx, bool resetSearch = true) {
    if (!ctx) return;
    const bool english = IsEnglishUi();
    ctx->bodyText = HelpSectionBody(ctx->currentSection, english);
    if (resetSearch) ResetHelpSearch(ctx);
    if (ctx->title) SetWindowTextW(ctx->title, HelpSectionTitle(ctx->currentSection).c_str());
    if (ctx->subtitle) SetWindowTextW(ctx->subtitle, HelpSectionSubtitle(ctx->currentSection).c_str());
    if (ctx->edit) {
        SetWindowTextW(ctx->edit, ctx->bodyText.c_str());
        SendMessageW(ctx->edit, EM_SETSEL, 0, 0);
    }
    if (ctx->btnOpenDocuments) {
        const bool hasReference = static_cast<bool>(FindBundledHelpReferenceDocument(GetExeDir()));
        SetWindowTextW(ctx->btnOpenDocuments,
                       localization::Text(hasReference ? L"help.open_document"
                                                       : L"help.open_viewer").c_str());
        ShowWindow(ctx->btnOpenDocuments,
                   ctx->currentSection == kHelpSectionFullGuide ? SW_SHOW : SW_HIDE);
    }
}

enum class HelpSearchStatus {
    Ready,
    EmptyQuery,
    NoMatch,
    FoundInAll,
    NoMatchInAll,
};

static void ShowHelpSearchStatus(HelpDialogCtx* ctx, HelpSearchStatus status, bool allHelp = false,
                                 size_t currentMatch = 0, size_t totalMatches = 0) {
    if (!ctx || !ctx->subtitle) return;
    std::wstring text = HelpSectionSubtitle(ctx->currentSection);
    if (status == HelpSearchStatus::EmptyQuery) {
        text += localization::Text(allHelp ? L"help.search.enter_all" : L"help.search.enter_section");
    } else if (status == HelpSearchStatus::NoMatch) {
        text += localization::Text(L"help.search.no_match_section");
    } else if (status == HelpSearchStatus::FoundInAll) {
        text += localization::Format(L"help.search.match_all", {
            {L"CURRENT", std::to_wstring(currentMatch)},
            {L"TOTAL", std::to_wstring(totalMatches)},
        });
    } else if (status == HelpSearchStatus::Ready && totalMatches != 0) {
        text += localization::Format(L"help.search.match_section", {
            {L"CURRENT", std::to_wstring(currentMatch)},
            {L"TOTAL", std::to_wstring(totalMatches)},
        });
    } else if (status == HelpSearchStatus::NoMatchInAll) {
        text += localization::Text(L"help.search.no_match_all");
    }
    SetWindowTextW(ctx->subtitle, text.c_str());
}

static size_t CountHelpMatches(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return 0;
    size_t count = 0;
    size_t offset = 0;
    while (true) {
        const size_t position = haystack.find(needle, offset);
        if (position == std::wstring::npos) return count;
        ++count;
        offset = position + needle.size();
    }
}

static size_t CountHelpMatchesInAllSections(const std::wstring& needle) {
    size_t total = 0;
    for (int section = 0; section < kHelpSectionCount; ++section) {
        std::wstring haystack = HelpSectionBody(section, IsEnglishUi());
        std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::towlower);
        total += CountHelpMatches(haystack, needle);
    }
    return total;
}

static void PrepareHelpSearch(HelpDialogCtx* ctx, bool allHelp) {
    if (!ctx || ctx->searchUsesAllSections == allHelp) return;
    ctx->nextSearchOffset = 0;
    ctx->nextGlobalSearchSection = ctx->currentSection;
    ctx->searchMatchIndex = 0;
    ctx->searchUsesAllSections = allHelp;
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
    PrepareHelpSearch(ctx, false);
    std::wstring needle = GetControlText(ctx->search);
    if (needle.empty()) {
        ShowHelpSearchStatus(ctx, HelpSearchStatus::EmptyQuery);
        SetFocus(ctx->search);
        return;
    }
    std::wstring haystack = ctx->bodyText;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::towlower);
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::towlower);
    const size_t totalMatches = CountHelpMatches(haystack, needle);
    size_t pos = haystack.find(needle, ctx->nextSearchOffset);
    if (pos == std::wstring::npos && ctx->nextSearchOffset != 0) pos = haystack.find(needle);
    if (pos == std::wstring::npos) {
        SetHelpFindButtonMode(ctx, false);
        ShowHelpSearchStatus(ctx, HelpSearchStatus::NoMatch);
        SetFocus(ctx->search);
        return;
    }
    ctx->searchMatchIndex = (ctx->searchMatchIndex % totalMatches) + 1;
    SetHelpFindButtonMode(ctx, true);
    ShowHelpSearchStatus(ctx, HelpSearchStatus::Ready, false,
                         ctx->searchMatchIndex, totalMatches);
    ctx->nextSearchOffset = pos + needle.size();
    SendMessageW(ctx->edit, EM_SETSEL, static_cast<WPARAM>(pos),
                 static_cast<LPARAM>(pos + needle.size()));
    SendMessageW(ctx->edit, EM_SCROLLCARET, 0, 0);
    SetFocus(ctx->edit);
}

static void ShowHelpSearchMatch(HelpDialogCtx* ctx, int section, size_t position, size_t length) {
    if (!ctx || !ctx->edit || section < 0 || section >= kHelpSectionCount) return;
    ctx->currentSection = section;
    if (ctx->nav) SendMessageW(ctx->nav, LB_SETCURSEL, section, 0);
    UpdateHelpDialogContent(ctx, false);
    SendMessageW(ctx->edit, EM_SETSEL, static_cast<WPARAM>(position),
                 static_cast<LPARAM>(position + length));
    SendMessageW(ctx->edit, EM_SCROLLCARET, 0, 0);
    SetFocus(ctx->edit);
}

static void FindInAllHelpSections(HelpDialogCtx* ctx) {
    if (!ctx || !ctx->search || !ctx->edit) return;
    PrepareHelpSearch(ctx, true);
    std::wstring needle = GetControlText(ctx->search);
    if (needle.empty()) {
        ShowHelpSearchStatus(ctx, HelpSearchStatus::EmptyQuery, true);
        SetFocus(ctx->search);
        return;
    }
    std::transform(needle.begin(), needle.end(), needle.begin(), ::towlower);
    const size_t totalMatches = CountHelpMatchesInAllSections(needle);
    if (totalMatches == 0) {
        SetHelpFindButtonMode(ctx, false);
        ShowHelpSearchStatus(ctx, HelpSearchStatus::NoMatchInAll, true);
        SetFocus(ctx->search);
        return;
    }
    const int startSection = std::clamp(ctx->nextGlobalSearchSection, 0, kHelpSectionCount - 1);
    const size_t startOffset = ctx->nextSearchOffset;
    for (int step = 0; step < kHelpSectionCount; ++step) {
        const int section = (startSection + step) % kHelpSectionCount;
        std::wstring haystack = HelpSectionBody(section, IsEnglishUi());
        std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::towlower);
        const size_t offset = section == startSection ? startOffset : 0;
        const size_t position = haystack.find(needle, offset);
        if (position == std::wstring::npos) continue;
        ShowHelpSearchMatch(ctx, section, position, needle.size());
        ctx->nextGlobalSearchSection = section;
        ctx->nextSearchOffset = position + needle.size();
        ctx->searchMatchIndex = (ctx->searchMatchIndex % totalMatches) + 1;
        SetHelpFindButtonMode(ctx, true);
        ShowHelpSearchStatus(ctx, HelpSearchStatus::FoundInAll, true,
                             ctx->searchMatchIndex, totalMatches);
        return;
    }
    if (startOffset != 0) {
        std::wstring haystack = HelpSectionBody(startSection, IsEnglishUi());
        std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::towlower);
        const size_t position = haystack.find(needle);
        if (position != std::wstring::npos) {
            ShowHelpSearchMatch(ctx, startSection, position, needle.size());
            ctx->nextGlobalSearchSection = startSection;
            ctx->nextSearchOffset = position + needle.size();
            ctx->searchMatchIndex = (ctx->searchMatchIndex % totalMatches) + 1;
            SetHelpFindButtonMode(ctx, true);
            ShowHelpSearchStatus(ctx, HelpSearchStatus::FoundInAll, true,
                                 ctx->searchMatchIndex, totalMatches);
            return;
        }
    }
    ShowHelpSearchStatus(ctx, HelpSearchStatus::NoMatchInAll, true);
    SetFocus(ctx->search);
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
    const int openDocumentsW = 170;
    const int findW = 82;
    const int findAllW = 112;
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
                                std::max(60, rightW - findW - findAllW - 16), searchH, TRUE);
    const int findX = rightX + std::max(60, rightW - findW - findAllW - 8);
    if (ctx->btnFind) MoveWindow(ctx->btnFind, findX,
                                 pad + titleH + subtitleH + 10, findW, searchH, TRUE);
    if (ctx->btnFindAll) MoveWindow(ctx->btnFindAll, findX + findW + 8,
                                    pad + titleH + subtitleH + 10, findAllW, searchH, TRUE);
    if (ctx->edit) MoveWindow(ctx->edit, rightX, contentY, rightW, btnY - contentY - 8, TRUE);
    if (ctx->btnOpenDocuments) {
        MoveWindow(ctx->btnOpenDocuments, rc.right - pad - btnW - 8 - openDocumentsW, btnY,
                   openDocumentsW, btnH, TRUE);
    }
    if (ctx->btnClose) MoveWindow(ctx->btnClose, rc.right - pad - btnW, btnY, btnW, btnH, TRUE);
}

// Restore the disabled owner while the help window still covers it.  Doing it
// only after DestroyWindow leaves a composition frame where neither window is
// ready to paint, which can briefly reveal the desktop.
static void RestoreHelpOwner(HelpDialogCtx* ctx) {
    if (!ctx || ctx->ownerRestored) return;
    ctx->ownerRestored = true;
    HWND owner = ctx->owner;
    if (!owner || !IsWindow(owner)) return;
    EnableWindow(owner, TRUE);
    if (IsWindowVisible(owner)) {
        RedrawWindow(owner, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

static LRESULT CALLBACK HelpDialogProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<HelpDialogCtx*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<HelpDialogCtx*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        ctx->currentSection = std::clamp(ctx->initialSection, 0, kHelpSectionCount - 1);
        ctx->navLabel = CreateWindowExW(0, L"STATIC", localization::Text(L"help.topics").c_str(),
                                        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hWnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(1000)), g_hInst, nullptr);
        ctx->nav = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL,
                                   0, 0, 0, 0, hWnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHelpNavId)), g_hInst, nullptr);
        for (int section = 0; section < kHelpSectionCount; ++section) {
            SendMessageW(ctx->nav, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(HelpSectionLabel(section).c_str()));
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
        ctx->btnFind = CreateWindowExW(0, L"BUTTON", localization::Text(L"help.search").c_str(),
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                       0, 0, 0, 0, hWnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHelpFindId)), g_hInst, nullptr);
        ctx->btnFindAll = CreateWindowExW(0, L"BUTTON", localization::Text(L"help.search_all").c_str(),
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                          0, 0, 0, 0, hWnd,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHelpFindAllId)),
                                          g_hInst, nullptr);
        ctx->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                                        ES_AUTOVSCROLL | WS_VSCROLL,
                                    0, 0, 0, 0, hWnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(1003)), g_hInst, nullptr);
        ctx->btnOpenDocuments = CreateWindowExW(0, L"BUTTON",
                                                localization::Text(L"help.open_document").c_str(),
                                                WS_CHILD | WS_TABSTOP,
                                                0, 0, 0, 0, hWnd,
                                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHelpOpenDocumentsId)),
                                                g_hInst, nullptr);
        ctx->btnClose = CreateWindowExW(0, L"BUTTON", localization::Text(L"common.close").c_str(),
                                        WS_CHILD | WS_VISIBLE,
                                        0, 0, 0, 0, hWnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), g_hInst, nullptr);
        SetUIFont(ctx->navLabel); SetUIFont(ctx->nav); SetUIFont(ctx->title); SetUIFont(ctx->subtitle);
        SetUIFont(ctx->search); SetUIFont(ctx->btnFind); SetUIFont(ctx->btnFindAll); SetUIFont(ctx->edit);
        SetUIFont(ctx->btnOpenDocuments); SetUIFont(ctx->btnClose);
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
        if (id == kHelpFindAllId) {
            FindInAllHelpSections(ctx);
            return 0;
        }
        if (id == kHelpOpenDocumentsId) {
            OpenBundledHelpGuide(hWnd);
            return 0;
        }
        if (id == kHelpSearchId && HIWORD(wParam) == EN_CHANGE && ctx) {
            ResetHelpSearch(ctx);
            ShowHelpSearchStatus(ctx, HelpSearchStatus::Ready);
            return 0;
        }
        if (id == IDOK || id == IDCANCEL) {
            RestoreHelpOwner(ctx);
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        RestoreHelpOwner(ctx);
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        RestoreHelpOwner(ctx);
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
    constexpr unsigned long kMaxPdfMetadataBytes = 64u * 1024u;
    if (bytes <= sizeof(wchar_t) || bytes > kMaxPdfMetadataBytes ||
        (bytes % sizeof(wchar_t)) != 0) return std::nullopt;
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

static std::wstring BuildPdfInfoText(std::wstring* outPathClipboardText) {
    if (outPathClipboardText) outPathClipboardText->clear();
    const bool isEn = IsEnglishUi();
    std::wstring out;
    auto line = [&](const std::wstring& s) {
        out += s;
        out += L"\r\n";
    };
    auto addPathToClipboard = [&](const std::wstring& label, const std::wstring& path) {
        if (!outPathClipboardText || path.empty()) return;
        if (!outPathClipboardText->empty()) *outPathClipboardText += L"\r\n\r\n";
        *outPathClipboardText += label;
        *outPathClipboardText += L":\r\n";
        *outPathClipboardText += path;
    };
    auto displayName = [](const std::wstring& path) {
        if (path.empty()) return std::wstring(L"-");
        const std::wstring name = std::filesystem::path(path).filename().wstring();
        return name.empty() ? path : name;
    };

    line(isEn ? L"PDF Info" : L"PDF情報");
    line(L"");

    if (g_pdf.kind != DocKind::Pdf || !g_pdf.doc) {
        line(isEn ? L"No PDF is currently open." : L"現在PDFが開かれていません。");
        if (!g_currentNotePath.empty()) {
            line(L"");
            line((isEn ? L"Note file: " : L"ノート: ") + displayName(g_currentNotePath));
            addPathToClipboard(isEn ? L"Note file" : L"ノートファイル", g_currentNotePath);
            if (auto sz = TryFileSize(std::filesystem::path(g_currentNotePath))) {
                line((isEn ? L"Note size: " : L"ノートサイズ: ") + FormatBytes(*sz));
            }
        }
        return out;
    }

    std::unique_lock<std::recursive_mutex> pdfiumLock(g_pdfiumMutex, std::defer_lock);
    if (!pdfiumLock.try_lock()) {
        line(isEn ? L"PDF information is temporarily unavailable while the document is busy."
                  : L"PDFの処理中のため、詳細情報を一時的に取得できません。");
        return out;
    }
    const std::wstring pdfPath = g_pdf.path;
    line((isEn ? L"File: " : L"ファイル: ") + displayName(pdfPath));
    addPathToClipboard(isEn ? L"PDF file" : L"PDFファイル", pdfPath);
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
            line((isEn ? L".clrop: " : L".clrop: ") + displayName(clropPath));
            addPathToClipboard(isEn ? L".clrop file" : L".clropファイル", clropPath);
            if (auto sz = TryFileSize(std::filesystem::path(clropPath))) {
                line((isEn ? L".clrop size: " : L".clrop サイズ: ") + FormatBytes(*sz));
            } else {
                line(isEn ? L".clrop size: (not found)" : L".clrop サイズ: （未作成/見つかりません）");
            }
        }
    }
    if (!g_currentNotePath.empty()) {
        line((isEn ? L"Note file: " : L"ノートファイル: ") + displayName(g_currentNotePath));
        addPathToClipboard(isEn ? L"Note file" : L"ノートファイル", g_currentNotePath);
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

        const std::wstring copyLabel = localization::Text(L"help.f9d8ec40980e");
        const std::wstring copyPathsLabel = localization::Text(L"help.pdf_info.copy_paths");
        const std::wstring closeLabel = localization::Text(L"help.603bc62f3f34");
        ctx->btnClose = CreateWindowExW(0, L"BUTTON", closeLabel.c_str(),
                                        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                        rc.right - pad - btnW, btnY, btnW, btnH, hWnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
                                        g_hInst, nullptr);
        ctx->btnCopy = CreateWindowExW(0, L"BUTTON", copyLabel.c_str(),
                                       WS_CHILD | WS_VISIBLE,
                                       rc.right - pad - btnW * 3 - 16, btnY, btnW, btnH, hWnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(1102)),
                                       g_hInst, nullptr);
        ctx->btnCopyPaths = CreateWindowExW(0, L"BUTTON", copyPathsLabel.c_str(),
                                            WS_CHILD | WS_VISIBLE,
                                            rc.right - pad - btnW * 2 - 8, btnY, btnW, btnH, hWnd,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(1103)),
                                            g_hInst, nullptr);

        SetUIFont(ctx->edit);
        SetUIFont(ctx->btnCopy);
        SetUIFont(ctx->btnCopyPaths);
        SetUIFont(ctx->btnClose);

        const auto text = NormalizeNewlines(BuildPdfInfoText(&ctx->pathClipboardText));
        SetWindowTextW(ctx->edit, text.c_str());
        SendMessageW(ctx->edit, EM_SETSEL, 0, 0);
        if (ctx->btnCopyPaths && ctx->pathClipboardText.empty()) EnableWindow(ctx->btnCopyPaths, FALSE);

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
            MoveWindow(ctx->btnCopy, rc.right - pad - btnW * 3 - 16, btnY, btnW, btnH, TRUE);
        }
        if (ctx->btnCopyPaths) {
            MoveWindow(ctx->btnCopyPaths, rc.right - pad - btnW * 2 - 8, btnY, btnW, btnH, TRUE);
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
        if (id == 1103) {
            if (ctx) CopyTextToClipboard(hWnd, ctx->pathClipboardText);
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
    ctx.windowTitle = localization::Text(L"help.cf15f78613ee");
    ctx.initialSection = initialSection;

    if (owner) EnableWindow(owner, FALSE);
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, ctx.windowTitle.c_str(),
                             WS_CAPTION | WS_POPUPWINDOW | WS_SIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 840, 580,
                             owner, nullptr, g_hInst, &ctx);
    if (!w) {
        RestoreHelpOwner(&ctx);
        return;
    }
    PlaceOwnedPopupAtAppTopLeft(w, owner);

    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);

    MSG msg;
    while (!ctx.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (ui::ConsumeNoOpEdgeNavKeyForMultilineEdit(msg, g_hNoteEdit)) continue;
        if (msg.hwnd == ctx.search && msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            FindInHelpSection(&ctx);
            continue;
        }
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    RestoreHelpOwner(&ctx);
}

void ShowHelpDialog(HWND owner) {
    ShowHelpCenterDialog(owner, kHelpSectionStart);
}

void OpenBundledHelpGuide(HWND owner) {
    const bool hasReference = static_cast<bool>(FindBundledHelpReferenceDocument(GetExeDir()));
    if (LaunchBundledHelpGuide(owner)) {
        ShowSoftNotice(owner, IsEnglishUi()
                                  ? (hasReference ? L"Opening the bundled help document in the read-only viewer."
                                                  : L"Opening the read-only viewer.")
                                  : (hasReference ? L"読み取り専用Viewerで同梱ヘルプ文書を開きます。"
                                                  : L"読み取り専用Viewerを開きます。"));
        return;
    }
    ShowSoftNotice(owner, localization::Text(L"help.e73e13526067"),
                   SoftNoticeKind::Warning);
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
    const std::wstring title = localization::Text(L"help.d42eb6858982");
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, title.c_str(),
                             WS_CAPTION | WS_POPUPWINDOW | WS_SIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 720, 560,
                             owner, nullptr, g_hInst, &ctx);
    if (!w) {
        if (owner) EnableWindow(owner, TRUE);
        return;
    }
    PlaceOwnedPopupAtAppTopLeft(w, owner);

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

void ShowNoteInfoDialog(HWND owner) {
    const std::wstring title = localization::Text(L"help.note_info.title");
    if (g_currentNotePath.empty()) {
        ShowSilentMessageDialog(owner, title, localization::Text(L"help.note_info.no_note"), SoftNoticeKind::Info);
        return;
    }

    const std::filesystem::path path(g_currentNotePath);
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    const std::wstring route = (extension == L".txt" || extension == L".csv") ? L"TXT" : L"Markdown/MD4C";
    const auto fileSize = TryFileSize(path);
    const std::wstring size = fileSize.has_value() ? FormatBytes(*fileSize)
                                                    : localization::Text(L"help.note_info.size_unavailable");
    const bool hasPendingChanges = CurrentNoteEditorIsModified() || g_noteDirty || g_noteNeedsIntegrate;
    const std::wstring message = localization::Format(L"help.note_info.summary", {
        {L"NAME", path.filename().wstring()},
        {L"FORMAT", (extension.empty() ? L"-" : extension) + L" / " + route},
        {L"SIZE", size},
        {L"PENDING", localization::Text(hasPendingChanges ? L"help.note_info.pending_yes"
                                                            : L"help.note_info.pending_no")},
    });
    ShowSilentMessageDialog(owner, title, message, SoftNoticeKind::Info,
                            {{localization::Text(L"help.note_info.path"), g_currentNotePath}});
}

