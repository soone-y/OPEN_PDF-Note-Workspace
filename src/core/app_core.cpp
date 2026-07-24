#include <richedit.h>
// file: core/app_core.cpp
#include "core/app_core.h"
#include "app/startup_instance.h"
#include "core/font_list.h"
#include "core/atomic_write.h"
#include "core/fault_injection.h"
#include "core/preview_trace.h"
#include "core/ui_notify.h"
#include "core/setup_json_policy.h"
#include "clrop/hash.h"
#include "theme/built_in_theme.h"
#include "workspace/workspace_actions.h"
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <dbghelp.h>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <set>
#include <cctype>
#include <cstdlib>
#include <commdlg.h>
#include <colordlg.h>

namespace {
static std::atomic<long> g_saveOperationCount{0};
static std::atomic<uint64_t> g_editRevision{0};
static std::atomic<bool> g_saveTransactionRunning{false};
static std::atomic<bool> g_saveTransactionQueued{false};
static std::wstring g_pendingStartupNoticeText;
static SoftNoticeKind g_pendingStartupNoticeKind = SoftNoticeKind::Info;
static std::mutex g_appLogMutex;
static std::wstring g_appLogWorkspaceRoot;
static AppDebugLogConfig g_runtimeAppLogConfig{};
static int g_deferredLeftPaneSelectionDepth = 0;
static bool g_deferredLeftPaneSelectionPending = false;
}

static HWND AppCoreDialogOwner(HWND owner) {
    return owner ? owner : g_hMainWnd;
}

static void ShowAppCoreSoftNotice(HWND owner, const std::wstring& text,
                                  SoftNoticeKind kind = SoftNoticeKind::Info) {
    ShowSoftNotice(AppCoreDialogOwner(owner), text, kind);
}

static void ShowAppCoreMessageDialog(HWND owner, const std::wstring& title,
                                     const std::wstring& message, SoftNoticeKind kind) {
    if (IsUiAutomationEnabled()) {
        // Modal recovery notices are correct for an interactive user but would
        // deadlock a non-interactive verification run before it can record its
        // result. Keep the warning observable without blocking the test flow.
        ShowAppCoreSoftNotice(owner, message, kind);
        return;
    }
    ShowSilentMessageDialog(AppCoreDialogOwner(owner), title, message, kind);
}

static void QueuePendingStartupNotice(const std::wstring& text, SoftNoticeKind kind) {
    g_pendingStartupNoticeText = text;
    g_pendingStartupNoticeKind = kind;
}

static bool ConfirmAppCoreYesNo(HWND owner, const std::wstring& title, const std::wstring& message,
                                SoftNoticeKind kind, SilentDialogResult defaultResult,
                                SilentDialogResult escapeResult) {
    SilentDialogOptions options;
    options.title = title;
    options.message = message;
    options.kind = kind;
    options.buttons = SilentDialogButtons::YesNo;
    options.defaultResult = defaultResult;
    options.escapeResult = escapeResult;
    return ShowSilentDialog(AppCoreDialogOwner(owner), options) == SilentDialogResult::Yes;
}

void EnterSaveOperation() {
    g_saveOperationCount.fetch_add(1, std::memory_order_relaxed);
}

void LeaveSaveOperation() {
    long v = g_saveOperationCount.fetch_sub(1, std::memory_order_relaxed) - 1;
    if (v < 0) g_saveOperationCount.store(0, std::memory_order_relaxed);
}

bool IsSaveOperationInProgress() {
    return g_saveOperationCount.load(std::memory_order_relaxed) > 0;
}

void NotifyEditRevisionChanged() {
    g_editRevision.fetch_add(1, std::memory_order_relaxed);
}

uint64_t CurrentEditRevision() {
    return g_editRevision.load(std::memory_order_relaxed);
}

bool TryBeginSaveTransaction(uint64_t* outSnapshotRevision) {
    bool expected = false;
    if (!g_saveTransactionRunning.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        g_saveTransactionQueued.store(true, std::memory_order_release);
        if (outSnapshotRevision) {
            *outSnapshotRevision = g_editRevision.load(std::memory_order_relaxed);
        }
        return false;
    }
    g_saveTransactionQueued.store(false, std::memory_order_release);
    if (outSnapshotRevision) {
        *outSnapshotRevision = g_editRevision.load(std::memory_order_relaxed);
    }
    return true;
}

void RequestQueuedSaveTransaction() {
    g_saveTransactionQueued.store(true, std::memory_order_release);
}

bool ShouldRunQueuedSaveTransaction(uint64_t snapshotRevision) {
    if (g_saveTransactionQueued.exchange(false, std::memory_order_acq_rel)) {
        return true;
    }
    return g_editRevision.load(std::memory_order_relaxed) > snapshotRevision;
}

void EndSaveTransaction() {
    g_saveTransactionQueued.store(false, std::memory_order_release);
    g_saveTransactionRunning.store(false, std::memory_order_release);
}

bool IsSaveTransactionRunning() {
    return g_saveTransactionRunning.load(std::memory_order_acquire);
}

static std::string ColorToHex(COLORREF c);
static std::optional<std::string> ParseJsonStringField(const std::string& json, const std::string& key);
static bool AtomicWriteUtf8WithWorkspaceDirs(const std::filesystem::path& dest,
                                             std::string_view utf8,
                                             const std::filesystem::path& workspaceRoot,
                                             std::wstring* err);
static void WriteThemeObject(std::ostream& os, const std::string& indent, const ThemeColors& theme);
static std::filesystem::path UserPaletteFilePath();
static bool LoadUserPaletteColors(COLORREF* custom, size_t count);
static void SaveUserPaletteColors(const COLORREF* custom, size_t count);
static std::optional<bool> QuerySystemTouchpadInvertVertical();
static void UpdateNoteBgColorFromConfig();

static void AppendSymbol(std::ostream& os, HANDLE process, DWORD64 address) {
    os << "       0x" << std::hex << address << ": ";
    BYTE buffer[sizeof(SYMBOL_INFO) + 256 * sizeof(WCHAR)] = {};
    PSYMBOL_INFO symbol = reinterpret_cast<PSYMBOL_INFO>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    if (SymFromAddr(process, address, &displacement, symbol)) {
        os << symbol->Name << " + 0x" << displacement;
        IMAGEHLP_LINE64 lineInfo{};
        lineInfo.SizeOfStruct = sizeof(lineInfo);
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(process, address, &lineDisplacement, &lineInfo)) {
            os << " (" << lineInfo.FileName << ":" << lineInfo.LineNumber << ")";
        }
    } else {
        os << "(unknown)";
    }
    os << std::dec << "\n";
}

LONG WINAPI CrashLogFilter(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (!IsAppLogEnabled(AppLogKind::Crash)) return EXCEPTION_CONTINUE_SEARCH;
    std::ostringstream oss;
    if (!oss) return EXCEPTION_CONTINUE_SEARCH;
    oss << "Exception 0x" << std::hex << ep->ExceptionRecord->ExceptionCode
        << " at " << ep->ExceptionRecord->ExceptionAddress << "\n";
    void* stack[64];
    constexpr ULONG kStackDepth = static_cast<ULONG>(sizeof(stack) / sizeof(stack[0]));
    ULONG frames = CaptureStackBackTrace(0, kStackDepth, stack, nullptr);
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES);
    if (SymInitialize(process, nullptr, TRUE)) {
        for (ULONG i = 0; i < frames; ++i) {
            AppendSymbol(oss, process, reinterpret_cast<DWORD64>(stack[i]));
        }
        SymCleanup(process);
    } else {
        for (ULONG i = 0; i < frames; ++i) {
            oss << "  0x" << stack[i] << "\n";
        }
    }
    oss << std::dec << "\n";
    AppendAppLogLineUtf8(AppLogKind::Crash, oss.str());
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------
// Globals definition
// ---------------------------------------------------------------------
HINSTANCE g_hInst = nullptr;
HFONT g_hUIFont = nullptr;
HFONT g_hNoteFont = nullptr;
HFONT g_hNoteRenderFont = nullptr;

HWND g_hLectureList = nullptr;
HWND g_hSessionList = nullptr;
HWND g_hPdfList = nullptr;
HWND g_hNoteList = nullptr;
HWND g_hMainWnd = nullptr;
HWND g_hPdfView = nullptr;
HWND g_hPdfToolbar = nullptr;
HWND g_hBtnClearAnn = nullptr;
HWND g_hBtnToggleAnn = nullptr;
HWND g_hBtnNewLecture = nullptr;
HWND g_hBtnNewSession = nullptr;
HWND g_hBtnNewNote = nullptr;
HWND g_hBtnToggleLeftPane = nullptr;
HWND g_hBtnModeSelect = nullptr;
HWND g_hBtnModePan = nullptr;
HWND g_hBtnModeMagnifier = nullptr;
HWND g_hBtnModeMarker = nullptr;
HWND g_hBtnModeMarkerFree = nullptr;
HWND g_hBtnModeMarkerLine = nullptr;
HWND g_hBtnModeMarkerArrow = nullptr;
HWND g_hBtnModeMarkerWave = nullptr;
HWND g_hBtnModeText = nullptr;
HWND g_hBtnModeLine = nullptr;
HWND g_hBtnModeArrow = nullptr;
HWND g_hBtnModeWave = nullptr;
HWND g_hBtnModeFreehand = nullptr;
HWND g_hBtnModeShape = nullptr;
HWND g_hBtnModeEraser = nullptr;
HWND g_hComboFont = nullptr;
HWND g_hComboFontSize = nullptr;
HWND g_hComboFontSizeAlt = nullptr;
HWND g_hRadioFontSizeSlotA = nullptr;
HWND g_hRadioFontSizeSlotB = nullptr;
HWND g_hChkTextReadableBackground = nullptr;
HWND g_hRadioTextReadableBackgroundNormal = nullptr;
HWND g_hRadioTextReadableBackgroundInverted = nullptr;
HWND g_hComboWidth = nullptr;
HWND g_hComboMarkerAlpha = nullptr;
HWND g_hComboAnnotMethod = nullptr;
HWND g_hComboFreehandCorrection = nullptr;
HWND g_hComboMarkerTextStyle = nullptr;
HWND g_hComboLineDashStyle = nullptr;
HWND g_hComboShapeKind = nullptr;
HWND g_hComboShapeGeometry = nullptr;
HWND g_hComboShapeDrawMode = nullptr;
HWND g_hComboMagnifierShape = nullptr;
HWND g_hComboStrokeWidth = nullptr;
HWND g_hAnnotShow = nullptr;
HWND g_hAnnotSettings = nullptr;
HWND g_hAnnotClear = nullptr;
HWND g_hAnnotList = nullptr;
HWND g_hAnnotSummary = nullptr;
HWND g_hNoteEdit = nullptr;
HWND g_hNoteRender = nullptr;
HWND g_hBottomNote = nullptr;
HWND g_hBottomMath = nullptr;
HWND g_hBottomRight = nullptr; // legacy alias
HMENU g_hMainMenu = nullptr;
HMENU g_hBottomPaneMenu = nullptr;
HMENU g_hBottomPaneMenuSettings = nullptr;
HMENU g_hScrollDirectionMenu = nullptr;
HACCEL g_hAccel = nullptr;
HWND g_hChkShortcutHeading1 = nullptr;
HWND g_hChkShortcutHeading2 = nullptr;
HWND g_hShortcutHeadingLevelLabel = nullptr;
HWND g_hBtnShortcutHeadingLevelUp = nullptr;
HWND g_hChkShortcutBack = nullptr;
HWND g_hChkShortcutChar = nullptr;
HWND g_hChkShortcutBold = nullptr;
HWND g_hChkShortcutItalic = nullptr;
HWND g_hChkShortcutStrike = nullptr;
HWND g_hChkShortcutUnderline = nullptr;
HWND g_hChkShortcutLinkDecor = nullptr;
HWND g_hBtnShortcutBackPreview = nullptr;
HWND g_hBtnShortcutCharPreview = nullptr;
HWND g_hBtnShortcutBackPalette = nullptr;
HWND g_hBtnShortcutCharPalette = nullptr;
HWND g_hShortcutIndentLabel = nullptr;
HWND g_hShortcutIndentEdit = nullptr;
HWND g_hShortcutMarginLabel = nullptr;
HWND g_hShortcutMarginEdit = nullptr;
HWND g_hShortcutFontSizeLabel = nullptr;
HWND g_hShortcutFontSizeEdit = nullptr;
HWND g_hShortcutTagEdit = nullptr;
HWND g_hBtnShortcutInput = nullptr;
HWND g_hBtnShortcutPdfLink = nullptr;
HWND g_hBtnNoteAssistBullet = nullptr;
HWND g_hBtnNoteAssistQuote = nullptr;
HWND g_hBtnNoteAssistPageRef = nullptr;
int g_noteShortcutHeadingLevel = 1;
std::vector<HWND> g_colorButtons;
HWND g_hBtnPaletteCustom = nullptr;

PdfViewState g_pdf;
std::recursive_mutex g_pdfiumMutex;
std::wstring g_workspaceRoot;
std::wstring g_currentNotePath;
std::wstring g_currentSessionPath;
bool g_noteNormalMode = false;
DWORD g_noteNormalCaret = 0;
bool g_noteVimModeEnabled = true;
bool g_noteVimCaretLineRawTextVisible = false;
bool g_noteVimClickEntersInsertMode = true;
bool g_noteDirty = false;
bool g_noteNeedsIntegrate = false;
bool g_annotsDirty = false;
bool g_annotsNeedsIntegrate = false;
bool g_annotsRequireStrongValidation = false;
bool g_annotsLoadedPdfIdValid = false;
clrop::PdfId g_annotsLoadedPdfId;
std::vector<MathEntry> g_mathEntries;
std::vector<HighlightRange> g_highlightRanges;
COLORREF g_highlightMarkColor = RGB(255, 245, 200);
COLORREF g_highlightHeadingColor = RGB(220, 240, 255);
int g_markFontPx = 0;
int g_headingFontPx = 0;
std::wstring g_previewNote;
bool g_pdfPreviewEnabled = false;
bool g_pdfPreviewActive = false;
std::wstring g_pdfPreviewOriginalPdfPath;
std::wstring g_pdfPreviewTempPath;
std::vector<Annotation> g_pdfPreviewOriginalAnnotations;
HANDLE g_pdfPreviewSourceFileHandle = INVALID_HANDLE_VALUE;
FPDF_DOCUMENT g_pdfPreviewSourceDoc = nullptr;
BottomPanePin g_bottomPanePin = BottomPanePin::Note;
BottomNoteMode g_bottomNoteMode = BottomNoteMode::Legacy;
NotePlacement g_notePlacement = NotePlacement::Bottom;

void SetHighlightColors(COLORREF mark, COLORREF heading) {
    g_highlightMarkColor = mark;
    g_highlightHeadingColor = heading;
}
std::vector<std::unique_ptr<NodeData>> g_nodeStore;
std::vector<ShortcutItem> g_shortcuts;
std::vector<Annotation> g_annots;
LinkPending g_linkPending;
std::vector<std::wstring> g_lectures;
std::vector<SessionEntry> g_sessions;
std::vector<FileEntry> g_pdfFiles;
std::vector<FileEntry> g_noteFiles;
WorkspaceConfig g_config;
static bool g_workspaceConfigAutoPersistBlocked = false;
static std::wstring g_workspaceConfigAutoPersistBlockedRootKey;
static UiText g_uiJa{
    L"PDF Note Workspace",
    L"メニュー", L"表示", L"保存", L"出力", L"検索", L"操作", L"一時", L"ヘルプ",
    L"ワークスペースを開く...", L"ワークスペース再読み込み", L"ファイルを取り込む...", L"フォルダを授業として取り込む...", L"フォルダを回次として取り込む...", L"PDF/ノートをフォルダに整理...", L"ノートと注釈を原本へ保存・統合 (Ctrl+S)", L"現在のノートだけ原本へ保存", L"復元/バックアップ...", L"保存バックアップを削除...", L"PDFを書き出し...", L"PDFをページ指定で書き出し...", L"PDFをPNGで書き出し...", L"ノートをtxt出力...", L"ノートをマークアップ/Markdown出力...", L"ノートをHTML出力...", L"授業を作成...", L"授業を作成", L"回次作成", L"ノート作成", L"ノートの作成", L"ルートフォルダをエクスプローラーで開く", L"授業フォルダをエクスプローラーで開く", L"終了",
    L"ズームをリセット", L"ズーム指定...", L"最初のページ", L"前のページ", L"次のページ", L"最後のページ", L"ページ指定ジャンプ...", L"PDF位置の記憶をリセット",
    L"右下の表示", L"ノートテキストの延長", L"見出し一覧", L"MathBox入力", L"ノートアシスト", L"ノート折り返し(生表示)",
    L"ヘルプを表示", L"PDF情報...", L"バージョン情報", L"クラッシュ（テスト）",
    L"選択クリア", L"全消し", L"注釈表示切替",
    L"選択", L"パン", L"拡大鏡", L"マーカー", L"フリーハンドマーカー", L"テキスト", L"ライン", L"矢印", L"波線", L"フリーハンド", L"図形", L"消しゴム",
    L"予約スペース",
    L"バージョン情報", L"ソフト情報\n"
    L"ソフト名: PDF Note Workspace\n"
    L"バージョン: {APP_VERSION}\n"
    L"ビルド日時: {BUILD_TIMESTAMP}\n"
    L"ビルド成果物 (SHA-256):\n"
    L"{BUILD_ARTIFACTS}\n"
    L"開発者: Soone-Y\n"
    L"アプリライセンス: zlib License (LICENSE.md 参照)\n"
    L"利用ライブラリ一覧:\n"
    L"  - PDFium\n"
    L"  - MinGW-w64 runtime (libstdc++ / libgcc / winpthreads)\n"
    L"各ライブラリのライセンス:\n"
    L"  - PDFium: パッケージ LICENSE / 同梱 notices (THIRD_PARTY_NOTICES.md 参照)\n"
    L"  - MinGW-w64 runtime: GPLv3 + GCC Runtime Library Exception / MIT + BSD (THIRD_PARTY_NOTICES.md 参照)\n"
    L"\n"
    L"安全性ポリシー:\n"
    L"  - 保存時に原本PDFファイルを直接上書きしません。\n"
    L"外部通信ポリシー:\n"
    L"  - 本ソフトは外部通信機能を持ちません。",
    L"ページ", L"ズーム",
    L"PDFを開けません。パスと権限を確認してください。", L"ノートを開けませんでした。", L"ノートを保存できませんでした。", L"ファイルを取り込めませんでした。", L"同名ファイルがあります。上書きしますか？",
    L"セッションが開かれていません。講義→回次を選択してください。",
    L"ノートを作成できませんでした。書き込み権限とパスを確認してください。",
    L"ノートを作成しました。",
    L"授業を作成", L"授業名", L"授業フォルダを作成できませんでした。", L"同名が存在するため番号を付けました。",
    L"回次ファイルを作成", L"回次名", L"回次フォルダを作成できませんでした。", L"同名の回次が存在します。", L"講義を選択してください。",
    L"設定", L"基本設定...", L"ノート設定...", L"マークアップ設定...", L"注釈設定",
    L"設定プリセットを保存...", L"設定プリセットを読み込み...",
    L"授業スケジュール",
    L"一時外部授業パス追加",
    L"一時外部授業パス削除",
    L"操作",
    L"復元",
    L"削除",
    L"最終オープン時刻をリセット",
    L"セッション最終オープン履歴をリセット",
    L"PDF位置を復元",
    L"最終オープン時刻を復元",
    L"セッション最終オープン履歴を復元",
    L"PDF位置バックアップを削除",
    L"最終オープン時刻バックアップを削除",
    L"セッション最終オープン履歴バックアップを削除",
    L"PDF名を変更...",
    L"ノート名を変更...",
    L"PDF位置を変更...",
    L"ノート位置を変更...",
    L"未統合の差分を確認...",
    L"閲覧専用ビューアで開く",
    L"閲覧専用ビューアを起動",
    L"すべての閲覧専用ビューアを閉じる",
    L"LibreOfficeでDOCX/PPTXをPDFに変換（試験的）...",
    L"白紙PDFを作成...",
    L"スクロール方向",
    L"縦（上→下）",
    L"縦（下→上）",
    L"横（右→左）",
    L"横（左→右）",
    L"1枚送り",
    L"カラーパレット..."
};
static UiText g_uiEn{
    L"PDF Note Workspace",
    L"Menu", L"View", L"Save", L"Export", L"Search", L"Operations", L"Temp", L"Help",
    L"Open Workspace...", L"Reload Workspace", L"Import File...", L"Import Directory as Lecture...", L"Import Directory as Session...", L"Organize PDF/notes into folders...", L"Save note + annotations to original files (Ctrl+S)", L"Save only the current note to its original file", L"Recovery / Backups...", L"Delete Saved File Backup...", L"Export PDF with annotations...", L"Export PDF pages...", L"Export PDF page as PNG...", L"Export note as text...", L"Export note as markup/Markdown...", L"Export note as HTML...", L"New Lecture...", L"New Lecture", L"New Session", L"New Note", L"Create Note", L"Open Root Folder", L"Open Lecture Folder", L"Exit",
    L"Reset Zoom", L"Set Zoom...", L"First Page", L"Previous Page", L"Next Page", L"Last Page", L"Jump to Page...", L"Reset Saved PDF Position",
    L"Bottom-right display", L"Extend note text", L"Heading list", L"MathBox input", L"Note assist", L"Note Wrap (raw mode)",
    L"Help...", L"PDF Info...", L"About", L"Crash (test)",
    L"Clear Select", L"Clear Annots", L"Toggle Annots",
    L"Select", L"Pan", L"Magnifier", L"Marker (text)", L"Marker (free)", L"Text", L"Line", L"Arrow", L"Wave", L"Freehand", L"Shape", L"Eraser",
    L"Reserved pane",
    L"About", L"Software Information\n"
    L"Software Name: PDF Note Workspace\n"
    L"Version: {APP_VERSION}\n"
    L"Build Date/Time: {BUILD_TIMESTAMP}\n"
    L"Build Artifacts (SHA-256):\n"
    L"{BUILD_ARTIFACTS}\n"
    L"Developer: Soone-Y\n"
    L"App License: zlib License (see LICENSE.md)\n"
    L"Third-Party Libraries:\n"
    L"  - PDFium\n"
    L"  - MinGW-w64 runtime (libstdc++ / libgcc / winpthreads)\n"
    L"License for Each Library:\n"
    L"  - PDFium: package LICENSE / bundled notices (see THIRD_PARTY_NOTICES.md)\n"
    L"  - MinGW-w64 runtime: GPLv3 + GCC Runtime Library Exception / MIT + BSD (see THIRD_PARTY_NOTICES.md)\n"
    L"\n"
    L"Safety Policy:\n"
    L"  - The original PDF file is never overwritten directly when saving.\n"
    L"External Communication Policy:\n"
    L"  - This software has no external communication features.",
    L"Page", L"Zoom",
    L"Failed to open PDF. Check path and permissions.", L"Failed to open note file.", L"Failed to save note file.", L"Failed to import file.", L"A file with the same name exists. Overwrite?",
    L"No session is open. Select a lecture/session first.",
    L"Failed to create note. Check write permission and path.",
    L"Note created.",
    L"Create Lecture", L"Lecture name", L"Failed to create lecture directory.", L"Name existed; appended a number.",
    L"Create Session", L"Session name", L"Failed to create session directory.", L"Session already exists.", L"Select a lecture first.",
    L"Settings", L"Preferences...", L"Note Settings...", L"Markup...", L"Annotation Settings...",
    L"Save Settings Preset...", L"Load Settings Preset...",
    L"Lecture Schedule",
    L"Add Temporary External Lecture...",
    L"Remove Temporary External Lecture",
    L"Operations",
    L"Restore",
    L"Delete",
    L"Reset Last Open Times",
    L"Reset Session Last-Open History",
    L"Restore Saved PDF Position",
    L"Restore Last Open Times",
    L"Restore Session Last-Open History",
    L"Delete Saved PDF Position Backup",
    L"Delete Last Open Times Backup",
    L"Delete Session Last-Open History Backup",
    L"Rename PDF...",
    L"Rename Note...",
    L"Move PDF...",
    L"Move Note...",
    L"Review Unintegrated Diffs...",
    L"Open in Read-Only Viewer",
    L"Launch Read-Only Viewer (No File)",
    L"Close All Read-Only Viewers",
    L"Convert DOCX/PPTX to PDF with LibreOffice (Experimental)...",
    L"Create Blank PDF...",
    L"Scroll direction",
    L"Vertical (top to bottom)",
    L"Vertical (bottom to top)",
    L"Horizontal (right to left)",
    L"Horizontal (left to right)",
    L"Single-page mode",
    L"Color Palette..."
};

int g_leftWidth  = kDefaultLeftPane;
int g_rightWidth = kDefaultRightPane;
int g_topHeight  = 520;
int g_leftSplit1 = 0;
int g_leftSplit2 = 0;
bool g_leftPaneCollapsed = false;
bool g_draggingLeft  = false;
bool g_draggingRight = false;
bool g_draggingHoriz = false;
bool g_draggingLeftTop = false;
bool g_draggingLeftMid = false;
POINT g_dragStart{};
int g_leftStart  = 0;
int g_rightStart = 0;
int g_topStart   = 0;
int g_leftSplitStart1 = 0;
int g_leftSplitStart2 = 0;
double g_scrollVelocity = 0.0;
bool g_showAnnots = true;
bool g_readableTextOverlay = false;
bool g_showMathList = true;
ToolMode g_toolMode = ToolMode::Select;
MagnifierShape g_magnifierShape = MagnifierShape::Circle;
ShapeKind g_shapeKind = ShapeKind::Rectangle;
// The first visible Shape-family entry is Stroke > Line. Keep the runtime
// default aligned with that order until a workspace selection is restored.
ShapeDrawMode g_shapeDrawMode = ShapeDrawMode::Outline;
ShapeDetail g_shapeDetail = ShapeDetail::Line;
ShapeToolSelection g_shapeToolSelection{};
ToolMode g_markerGroupMode = ToolMode::MarkerText;
ToolMode g_penGroupMode = ToolMode::Freehand;
ToolMode g_shapeGroupMode = ToolMode::Line;

static std::vector<ToolMode> DefaultAnnotToolModeUiOrder() {
    return {
        ToolMode::Select,
        ToolMode::Pan,
        ToolMode::Magnifier,
        ToolMode::TextBox,
        ToolMode::MarkerText,
        ToolMode::MarkerTextUnderline,
        ToolMode::MarkerTextColor,
        ToolMode::MarkerFree,
        ToolMode::Freehand,
        ToolMode::Line,
        ToolMode::Wave,
        ToolMode::Arrow,
        ToolMode::Shape,
        ToolMode::MarkerLine,
        ToolMode::MarkerWave,
        ToolMode::MarkerArrow,
        ToolMode::Eraser,
    };
}

static const std::vector<AnnotToolFamily>& DefaultAnnotToolFamilyUiOrder() {
    static const std::vector<AnnotToolFamily> order = {
        AnnotToolFamily::Select,
        AnnotToolFamily::Pan,
        AnnotToolFamily::Magnifier,
        AnnotToolFamily::Text,
        AnnotToolFamily::Marker,
        AnnotToolFamily::Pen,
        AnnotToolFamily::Shape,
        AnnotToolFamily::Eraser,
    };
    return order;
}

static std::array<AnnotToolUiState, kToolModeCount> DefaultAnnotToolModeUiStates() {
    std::array<AnnotToolUiState, kToolModeCount> s{};
    s.fill(AnnotToolUiState::Enabled);
    return s;
}

static std::vector<ToolMode> g_annotToolModeUiOrder = DefaultAnnotToolModeUiOrder();
static std::array<AnnotToolUiState, kToolModeCount> g_annotToolModeUiStates = DefaultAnnotToolModeUiStates();
static std::vector<AnnotToolShortcutBinding> g_annotToolShortcuts;
std::vector<COLORREF> g_palette = {
    RGB(255, 140,   0), // orange (default annotation color)
    RGB(255, 255,   0), // marker yellow (doc sample)
    RGB(255,   0,   0), // red
    RGB(  0, 160, 255), // blue
    RGB(  0, 200,   0), // green
    RGB(200,  80, 200), // magenta-ish for variation
    RGB(128, 128, 128), // gray (7th fixed preset)
};
COLORREF g_paletteCustomColor = RGB(0, 0, 0);       // slot 9: last OK color (black default)
COLORREF g_paletteDialogCustomColor = RGB(0, 0, 0); // slot 8: s_customColors[0] snapshot (black default)
COLORREF g_activeColor = g_palette.front();
WNDPROC g_oldNoteProc = nullptr;
std::wstring g_currentLecturePath;
std::wstring g_textFontName;
double g_textFontPt = 14.0;
bool g_textFontUseA4Scale = true;
int g_textFontActiveSizeSlot = 0;
double g_textFontPtSlotA = 14.0;
double g_textFontPtSlotB = 20.0;
bool g_textFontUseA4ScaleSlotA = true;
bool g_textFontUseA4ScaleSlotB = true;
bool g_textBoxReadableBackground = false;
bool g_textBoxReadableBackgroundInverted = false;
SavedToolbarState g_preEditToolbarState;
bool g_lineToolsShareStyle = true;
double g_lineWidthPt = 2.0;
double g_arrowWidthPt = 2.0;
ArrowHead g_arrowHead = ArrowHead::Single;
double g_waveWidthPt = 2.0;
double g_markerFreeWidthPt = 8.0;
double g_markerTextWidthPt = 4.0;
bool g_markerTextUnderline = false;
std::wstring g_lineDashStyle = L"solid";
double g_freehandWidthPt = 2.5;
double g_eraserWidthPt = 4.0;
double g_markerAlpha = kMarkerAlphaDefault;
double g_lineAlpha = kLineAlphaDefault;
double g_arrowAlpha = kLineAlphaDefault;
double g_waveAlpha = kLineAlphaDefault;
double g_freehandAlpha = 1.0;
double g_shapeAlpha = 0.35;
std::wstring g_noteFontName;
double g_noteFontPt = 10.0;
std::wstring g_noteRenderFontName;
std::wstring g_noteRenderJpFontName;
double g_noteRenderFontPt = 10.0;
NoteSystem g_noteSystem = NoteSystem::Legacy;
bool g_noteRenderEnabled = true;
bool g_noteRawOnly = false;
bool g_noteRenderMath = false;
bool g_noteWrapEnabled = true;
bool g_noteGridEnabled = false;
int g_noteGridPitch = 24;
COLORREF g_noteBgColor = RGB(255, 255, 255);
COLORREF g_noteFgColor = RGB(0, 0, 0);
COLORREF g_textColor = RGB(255, 140, 0);
COLORREF g_lineColor = RGB(255, 140, 0);
COLORREF g_arrowColor = RGB(255, 140, 0);
COLORREF g_waveColor = RGB(255, 140, 0);
COLORREF g_freehandColor = RGB(255, 140, 0);
COLORREF g_markerFreeColor = RGB(255, 140, 0);
COLORREF g_markerTextColor = RGB(255, 140, 0);
COLORREF g_shapeColor = RGB(255, 140, 0);
COLORREF g_noteShortcutBackColor = RGB(0xFF, 0xF7, 0xE8);
COLORREF g_noteShortcutTextColor = RGB(0x00, 0xAA, 0x7B);
ThemeColors g_theme;
std::wstring g_themeName = L"00AA7B";
std::vector<ThemeColors> g_themeCatalog;
HBRUSH g_hThemeWindowBrush = nullptr;
HBRUSH g_hThemePanelBrush = nullptr;
HBRUSH g_hThemeNoteBrush = nullptr;
HBRUSH g_hThemeToolbarBrush = nullptr;
HBRUSH g_hThemeMenuBrush = nullptr;

static std::vector<ThemeColors> DefaultThemeCatalog() {
    return theme::MakeBuiltInThemeCatalog();
}

static const ThemeColors* FindThemeByName(const std::vector<ThemeColors>& themes, const std::wstring& name) {
    if (themes.empty()) return nullptr;
    for (const auto& t : themes) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

// Theme storage policy:
// - Unified config: __resource__/__theme__/theme.json (current + catalog)
// - Do not manage per-theme directories under __theme__/<name>/theme.json
static constexpr bool kThemeUseThemeFiles = true;
static constexpr bool kThemeRequireFormatTag = true;
static constexpr std::uint64_t kMaxThemeFileBytes = 512ull * 1024ull;   // DoS guard
static constexpr std::uint64_t kMaxThemeConfigBytes = 256ull * 1024ull; // DoS guard
static constexpr std::uint64_t kMaxThemeManualBytes = 256ull * 1024ull; // DoS guard
static constexpr std::uint64_t kMaxSetupJsonBytes = 256ull * 1024ull;   // DoS guard
static constexpr std::uint64_t kMaxWorkspaceJsonBytes = 512ull * 1024ull; // DoS guard
static constexpr std::uint64_t kMaxPresetJsonBytes = 512ull * 1024ull;    // DoS guard
static constexpr std::uint64_t kMaxUserPaletteBytes = 64ull * 1024ull;    // DoS guard
static constexpr std::uint64_t kMaxUserToolShortcutBytes = 64ull * 1024ull; // DoS guard

static std::filesystem::path ThemeRootPath(const std::wstring& root);

struct VerifiedThemeMeta {
    std::wstring file;           // theme_XXXXXX(.json / _N.json)
    std::uint64_t size = 0;      // bytes
    std::int64_t mtimeMs = 0;    // unix epoch ms
    std::string sha256;          // lowercase hex (optional but recommended)
    std::wstring displayName;    // cached label (en)
    std::wstring displayNameJp;  // cached label (ja)
    COLORREF accent = RGB(0, 0, 0);
    COLORREF noteBg = RGB(255, 255, 255);
};

static std::wstring g_themeCurrentFile;
static std::vector<VerifiedThemeMeta> g_themeVerified;
static std::wstring g_themeLastDisplayEn;
static std::wstring g_themeLastDisplayJp;

static bool IsSafeThemeFileId(const std::wstring& id) {
    if (id.empty()) return false;
    if (id.find(L'\\') != std::wstring::npos) return false;
    if (id.find(L'/') != std::wstring::npos) return false;
    if (id.find(L':') != std::wstring::npos) return false;
    if (id.find(L"..") != std::wstring::npos) return false;
    return true;
}

static std::filesystem::path ThemeFilePathFromId(const std::wstring& root, const std::wstring& id) {
    if (!IsSafeThemeFileId(id)) return {};
    auto dir = ThemeRootPath(root);
    if (dir.empty()) return {};
    return dir / id;
}

static std::int64_t ToUnixEpochMs(std::filesystem::file_time_type ft) {
    using namespace std::chrono;
    auto sysNow = system_clock::now();
    auto fileNow = std::filesystem::file_time_type::clock::now();
    auto sysTime = time_point_cast<milliseconds>(sysNow + (ft - fileNow));
    return static_cast<std::int64_t>(sysTime.time_since_epoch().count());
}

static bool TryGetFileMeta(const std::filesystem::path& p, std::uint64_t* outSize, std::int64_t* outMtimeMs) {
    if (outSize) *outSize = 0;
    if (outMtimeMs) *outMtimeMs = 0;
    if (p.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || ec) return false;
    ec.clear();
    auto sz = std::filesystem::file_size(p, ec);
    if (ec) return false;
    ec.clear();
    auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return false;
    if (outSize) *outSize = static_cast<std::uint64_t>(sz);
    if (outMtimeMs) *outMtimeMs = ToUnixEpochMs(t);
    return true;
}

static bool IsSymlinkPath(const std::filesystem::path& p) {
    if (p.empty()) return false;
    std::error_code ec;
    auto st = std::filesystem::symlink_status(p, ec);
    if (ec) return false;
    return std::filesystem::is_symlink(st);
}

static VerifiedThemeMeta* FindVerifiedMetaByFile(const std::wstring& file) {
    for (auto& v : g_themeVerified) {
        if (v.file == file) return &v;
    }
    return nullptr;
}

static void RemoveVerifiedMetaByFile(const std::wstring& file) {
    g_themeVerified.erase(
        std::remove_if(g_themeVerified.begin(), g_themeVerified.end(),
                       [&](const VerifiedThemeMeta& v) { return v.file == file; }),
        g_themeVerified.end());
}

static std::wstring DefaultThemeFileId() {
    // Default theme = "default" (owner-draw off).
    return L"default";
}

MAYBE_UNUSED static ThemeColors MakeSystemDefaultTone() {
    ThemeColors t;
    t.name = L"default";
    t.nameJp = IsEnglishUi() ? L"Default" : L"デフォルト";

    t.windowBg = GetSysColor(COLOR_WINDOW);
    t.windowText = GetSysColor(COLOR_WINDOWTEXT);
    t.panelBg = GetSysColor(COLOR_WINDOW);
    t.panelText = GetSysColor(COLOR_WINDOWTEXT);
    t.menuBg = GetSysColor(COLOR_MENU);
    t.menuText = GetSysColor(COLOR_MENUTEXT);
    t.menuSelBg = GetSysColor(COLOR_HIGHLIGHT);
    t.menuSelText = GetSysColor(COLOR_HIGHLIGHTTEXT);
    t.toolbarBg = GetSysColor(COLOR_BTNFACE);
    t.toolbarText = GetSysColor(COLOR_BTNTEXT);
    t.buttonBg = GetSysColor(COLOR_BTNFACE);
    t.buttonText = GetSysColor(COLOR_BTNTEXT);
    t.buttonBorder = GetSysColor(COLOR_BTNSHADOW);
    t.buttonHot = GetSysColor(COLOR_BTNHIGHLIGHT);
    t.buttonPressed = GetSysColor(COLOR_3DSHADOW);
    t.splitterBg = GetSysColor(COLOR_BTNFACE);
    t.splitterLine = GetSysColor(COLOR_3DSHADOW);
    t.pdfBg = RGB(210, 210, 210);
    t.pdfPageBg = RGB(255, 255, 255);
    t.noteBg = GetSysColor(COLOR_WINDOW);
    t.noteText = GetSysColor(COLOR_WINDOWTEXT);
    t.selectionBg = GetSysColor(COLOR_HIGHLIGHT);
    t.selectionText = GetSysColor(COLOR_HIGHLIGHTTEXT);
    t.accent = GetSysColor(COLOR_HOTLIGHT);
    return t;
}

static bool UpdateVerifiedMetaLightweight(const std::wstring& workspaceRoot,
                                         const std::wstring& themeFileId,
                                         const ThemeColors& appliedTheme) {
    if (workspaceRoot.empty()) return false;
    if (!IsSafeThemeFileId(themeFileId)) return false;
    auto path = ThemeFilePathFromId(workspaceRoot, themeFileId);
    std::uint64_t sz = 0;
    std::int64_t mt = 0;
    if (!TryGetFileMeta(path, &sz, &mt)) return false;

    VerifiedThemeMeta v;
    v.file = themeFileId;
    v.size = sz;
    v.mtimeMs = mt;
    v.displayName = g_themeLastDisplayEn.empty() ? themeFileId : g_themeLastDisplayEn;
    v.displayNameJp = g_themeLastDisplayJp;
    v.accent = appliedTheme.accent;
    v.noteBg = appliedTheme.noteBg;

    if (auto* existing = FindVerifiedMetaByFile(themeFileId)) {
        // Preserve sha256 if it was already computed.
        v.sha256 = existing->sha256;
        const bool same =
            (existing->size == v.size) &&
            (existing->mtimeMs == v.mtimeMs) &&
            (existing->displayName == v.displayName) &&
            (existing->displayNameJp == v.displayNameJp) &&
            (existing->accent == v.accent) &&
            (existing->noteBg == v.noteBg);
        if (same) return false;
        *existing = std::move(v);
        return true;
    }
    g_themeVerified.push_back(std::move(v));
    return true;
}

static std::filesystem::path ThemeConfigPath(const std::wstring& root) {
    if (root.empty()) return {};
    // Theme catalog / selection config lives under __theme__.
    return std::filesystem::path(root) / L"__resource__" / L"__theme__" / L"theme.json";
}

static std::filesystem::path ThemeRootPath(const std::wstring& root) {
    if (root.empty()) return {};
    return std::filesystem::path(root) / L"__resource__" / L"__theme__";
}

static std::filesystem::path ThemeManualPath(const std::wstring& root) {
    if (root.empty()) return {};
    return ThemeRootPath(root) / L"theme_manual.txt";
}

static bool EnsureThemeManualFile(const std::wstring& root) {
    if (root.empty()) return false;
    auto themeRoot = ThemeRootPath(root);
    if (themeRoot.empty()) return false;
    if (IsSymlinkPath(themeRoot)) return false;

    std::error_code ec;
    std::filesystem::create_directories(themeRoot, ec);
    if (ec) return false;

    auto manual = ThemeManualPath(root);
    if (manual.empty()) return false;

    ec.clear();
    if (std::filesystem::exists(manual, ec) && !ec) {
        if (IsSymlinkPath(manual)) return false;
        ec.clear();
        if (std::filesystem::is_regular_file(manual, ec) && !ec) return true;
        return false;
    }

    // Create only if missing. Do not overwrite user edits.
    std::ostringstream oss;
    oss << "PDF注釈ソフト: テーマの説明 (theme_manual.txt)\n";
    oss << "\n";
    oss << "配置\n";
    oss << "  - テーマフォルダ: __resource__/__theme__/\n";
    oss << "  - テーマファイル: theme_XXXXXX.json (XXXXXX = 基調色の16進6桁)\n";
    oss << "  - カタログ/選択: theme.json\n";
    oss << "\n";
    oss << "テーマの基本思想\n";
    oss << "  - 基調色(00AA7B/FFBD85/1D50DDなど)を選び、カラー傾向を上乗せします。\n";
    oss << "  - カラー傾向は workspace.json の toneVariant で指定します:\n";
    oss << "      pure / guard / emphasis / white / black\n";
    oss << "  - toneVariant は見た目の変換であり、テーマファイル(json)自体は書き換えません。\n";
    oss << "\n";
    oss << "theme.json (キャッシュ)\n";
    oss << "  - current_file: 現在のテーマファイルID (例: theme_00AA7B.json)\n";
    oss << "  - verified[]: 参照済みテーマの軽量キャッシュ(表示名/アクセント等)\n";
    oss << "  - 起動時は負荷を抑えるため verified を優先し、適用時に内容を検証します。\n";
    oss << "\n";
    oss << "theme_XXXXXX.json (フォーマット)\n";
    oss << "  - format: \"pdf_note_theme_v1\"\n";
    oss << "  - tone_id: \"XXXXXX\" (ファイル名のXXXXXXと一致する必要があります)\n";
    oss << "  - name / name_jp\n";
    oss << "  - colors:\n";
    oss << "      windowBg, windowText, panelBg, panelText,\n";
    oss << "      menuBg, menuText, menuSelBg, menuSelText,\n";
    oss << "      toolbarBg, toolbarText,\n";
    oss << "      buttonBg, buttonText, buttonBorder, buttonHot, buttonPressed,\n";
    oss << "      splitterBg, splitterLine,\n";
    oss << "      pdfBg, pdfPageBg,\n";
    oss << "      noteBg, noteText,\n";
    oss << "      selectionBg, selectionText,\n";
    oss << "      accent\n";
    oss << "\n";
    oss << "新テーマを手編集で作る手順\n";
    oss << "  1) __resource__/__theme__/ に theme_XXXXXX.json を作成します。\n";
    oss << "  2) tone_id / accent / ファイル名のXXXXXXを同じ値に揃えます。\n";
    oss << "  3) 必要な色だけ変更して保存し、設定画面からテーマを選択します。\n";
    oss << "  4) うまく読み込めない場合はファイル名形式と16進色コードを確認します。\n";
    oss << "\n";
    oss << "補足コメントの運用\n";
    oss << "  - JSON本体にはコメントを書けません（無効なJSONになります）。\n";
    oss << "  - 補足はこの theme_manual.txt に追記するか、別の .txt / .md に残してください。\n";
    oss << "\n";
    oss << "以下に最小テンプレート例\n";
    oss << "ファイル名: theme_XXXXXX.json\n";
    oss << "{\n";
    oss << "  \"format\": \"pdf_note_theme_v1\",\n";
    oss << "  \"tone_id\": \"00AA7B\",\n";
    oss << "  \"name\": \"00AA7B\",\n";
    oss << "  \"name_jp\": \"00AA7B\",\n";
    oss << "  \"windowBg\": \"#ECFCF6\",\n";
    oss << "  \"windowText\": \"#0E2B1F\",\n";
    oss << "  \"panelBg\": \"#F3FEFA\",\n";
    oss << "  \"panelText\": \"#0E2B1F\",\n";
    oss << "  \"menuBg\": \"#E7FBF4\",\n";
    oss << "  \"menuText\": \"#0E2B1F\",\n";
    oss << "  \"menuSelBg\": \"#BDEEDD\",\n";
    oss << "  \"menuSelText\": \"#083022\",\n";
    oss << "  \"toolbarBg\": \"#ECFCF6\",\n";
    oss << "  \"toolbarText\": \"#0E2B1F\",\n";
    oss << "  \"buttonBg\": \"#F6FFFC\",\n";
    oss << "  \"buttonText\": \"#0E2B1F\",\n";
    oss << "  \"buttonBorder\": \"#8EDCC4\",\n";
    oss << "  \"buttonHot\": \"#D4F7EB\",\n";
    oss << "  \"buttonPressed\": \"#BDEEDD\",\n";
    oss << "  \"splitterBg\": \"#A1E6CF\",\n";
    oss << "  \"splitterLine\": \"#6ADAB9\",\n";
    oss << "  \"pdfBg\": \"#E6F7F0\",\n";
    oss << "  \"pdfPageBg\": \"#FFFFFF\",\n";
    oss << "  \"noteBg\": \"#F1FFFA\",\n";
    oss << "  \"noteText\": \"#0E2B1F\",\n";
    oss << "  \"selectionBg\": \"#BDEEDD\",\n";
    oss << "  \"selectionText\": \"#083022\",\n";
    oss << "  \"accent\": \"#00AA7B\"\n";
    oss << "}\n";
    oss << "\n";
    oss << "安全上の注意\n";
    oss << "  - テーマは外部ファイル読み込みなので、サイズ上限/シンボリックリンク拒否などの安全策があります。\n";
    oss << "  - theme_*.json 以外のファイルはテーマとして扱いません。\n";

    std::string data = oss.str();
    if (data.size() > kMaxThemeManualBytes) return false;
    std::wstring err;
    return AtomicWriteUtf8WithWorkspaceDirs(manual, data, std::filesystem::path(root), &err);
}

static bool TryParseSixHexFromW(const std::wstring& s, unsigned int* out) {
    if (out) *out = 0;
    if (s.size() != 6) return false;
    unsigned int v = 0;
    for (wchar_t ch : s) {
        v <<= 4;
        if (ch >= L'0' && ch <= L'9') v |= static_cast<unsigned int>(ch - L'0');
        else if (ch >= L'a' && ch <= L'f') v |= static_cast<unsigned int>(10 + (ch - L'a'));
        else if (ch >= L'A' && ch <= L'F') v |= static_cast<unsigned int>(10 + (ch - L'A'));
        else return false;
    }
    if (out) *out = v;
    return true;
}

static std::wstring ColorToHexNoHashW(COLORREF c) {
    wchar_t buf[7]{};
    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%06X",
                  (GetRValue(c) << 16) | (GetGValue(c) << 8) | (GetBValue(c)));
    return std::wstring(buf);
}

static bool TryParseThemeIdHexFromFileId(const std::wstring& fileId, std::wstring* outHex6) {
    if (outHex6) outHex6->clear();
    if (fileId.empty()) return false;
    // Accept: theme_XXXXXX.json or theme_XXXXXX_2.json (XXXXXX = 6 hex digits)
    const std::wstring prefix = L"theme_";
    const std::wstring suffix = L".json";
    if (fileId.size() < prefix.size() + 6 + suffix.size()) return false;
    if (fileId.rfind(prefix, 0) != 0) return false;
    if (fileId.size() < suffix.size() || fileId.substr(fileId.size() - suffix.size()) != suffix) return false;
    std::wstring mid = fileId.substr(prefix.size(), fileId.size() - prefix.size() - suffix.size());
    if (mid.size() < 6) return false;
    std::wstring hex = mid.substr(0, 6);
    unsigned int v = 0;
    if (!TryParseSixHexFromW(hex, &v)) return false;
    // If there's a suffix, require it to be _<digits>
    if (mid.size() > 6) {
        if (mid[6] != L'_') return false;
        if (mid.size() == 7) return false;
        for (size_t i = 7; i < mid.size(); ++i) {
            if (mid[i] < L'0' || mid[i] > L'9') return false;
        }
    }
    if (outHex6) {
        for (auto& ch : hex) {
            if (ch >= L'a' && ch <= L'f') ch = static_cast<wchar_t>(ch - L'a' + L'A');
        }
        *outHex6 = hex;
    }
    return true;
}

static bool TryParseThemeOrderKey(const std::wstring& themeIdOrFileId,
                                  unsigned int* outHex,
                                  int* outKind,
                                  int* outSuffix) {
    if (outHex) *outHex = 0;
    if (outKind) *outKind = 2;   // 0: embedded hex, 1: file theme_XXXXXX(_N).json, 2: other
    if (outSuffix) *outSuffix = 0;
    if (themeIdOrFileId.empty()) return false;

    // Embedded: "XXXXXX"
    if (themeIdOrFileId.size() == 6) {
        unsigned int v = 0;
        if (TryParseSixHexFromW(themeIdOrFileId, &v)) {
            if (outHex) *outHex = v;
            if (outKind) *outKind = 0;
            if (outSuffix) *outSuffix = 0;
            return true;
        }
    }

    // File: "theme_XXXXXX.json" or "theme_XXXXXX_2.json"
    std::wstring hex6;
    if (!TryParseThemeIdHexFromFileId(themeIdOrFileId, &hex6) || hex6.empty()) return false;
    unsigned int v = 0;
    if (!TryParseSixHexFromW(hex6, &v)) return false;

    int suffixNum = 0;
    {
        const std::wstring prefix = L"theme_";
        const std::wstring suffix = L".json";
        std::wstring mid = themeIdOrFileId.substr(prefix.size(),
                                                  themeIdOrFileId.size() - prefix.size() - suffix.size());
        if (mid.size() > 6 && mid[6] == L'_') {
            // Parse digits after '_' (validated by TryParseThemeIdHexFromFileId).
            long long acc = 0;
            for (size_t i = 7; i < mid.size(); ++i) {
                acc = acc * 10 + (mid[i] - L'0');
                if (acc > 1000000) break;
            }
            suffixNum = static_cast<int>(std::clamp<long long>(acc, 0, 1000000));
        }
    }

    if (outHex) *outHex = v;
    if (outKind) *outKind = 1;
    if (outSuffix) *outSuffix = suffixNum;
    return true;
}

static void SortThemeCatalogByHex(std::vector<ThemeColors>& catalog) {
    std::stable_sort(catalog.begin(), catalog.end(), [&](const ThemeColors& a, const ThemeColors& b) {
        auto groupOf = [&](const ThemeColors& t) -> int {
            if (t.name == L"default") return 0;  // always first (owner-draw off)
            if (t.name == L"FFFFFF") return 1;   // always second (highest hex)
            unsigned int h = 0;
            int kind = 2;
            int suffix = 0;
            if (TryParseThemeOrderKey(t.name, &h, &kind, &suffix)) return 2; // hex-sort group
            return 3; // other
        };
        int ga = groupOf(a);
        int gb = groupOf(b);
        if (ga != gb) return ga < gb;

        if (ga == 2) {
            unsigned int ah = 0, bh = 0;
            int ak = 2, bk = 2;
            int as = 0, bs = 0;
            (void)TryParseThemeOrderKey(a.name, &ah, &ak, &as);
            (void)TryParseThemeOrderKey(b.name, &bh, &bk, &bs);
            if (ah != bh) return ah > bh;       // descending hex
            if (ak != bk) return ak < bk;       // embedded before file
            if (as != bs) return as < bs;       // _2 after base
        }
        return a.name < b.name;
    });
}

static bool TryParseThemeAccentFromFileName(const std::filesystem::path& path, COLORREF* outAccent) {
    if (outAccent) *outAccent = RGB(0, 0, 0);
    std::wstring hex;
    if (!TryParseThemeIdHexFromFileId(path.filename().wstring(), &hex)) return false;
    unsigned int v = 0;
    if (!TryParseSixHexFromW(hex, &v)) return false;
    if (outAccent) *outAccent = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return true;
}

static std::filesystem::path ThemeFilePathByIdHex(const std::wstring& root, const std::wstring& hex6) {
    auto themeRoot = ThemeRootPath(root);
    if (themeRoot.empty()) return {};
    unsigned int v = 0;
    if (!TryParseSixHexFromW(hex6, &v)) return {};
    std::wstring hex = hex6;
    for (auto& ch : hex) {
        if (ch >= L'a' && ch <= L'f') ch = static_cast<wchar_t>(ch - L'a' + L'A');
    }
    return themeRoot / (L"theme_" + hex + L".json");
}

static std::filesystem::path AllocUniqueThemeFilePathByIdHex(const std::wstring& root, const std::wstring& hex6) {
    auto base = ThemeFilePathByIdHex(root, hex6);
    if (base.empty()) return {};
    std::error_code ec;
    if (!std::filesystem::exists(base, ec) || ec) return base;
    ec.clear();
    std::filesystem::path dir = base.parent_path();
    std::wstring stem = base.stem().wstring(); // theme_XXXXXX
    std::filesystem::path ext = base.extension();
    for (int i = 2; i <= 9999; ++i) {
        std::filesystem::path cand = dir / (stem + L"_" + std::to_wstring(i) + ext.wstring());
        if (!std::filesystem::exists(cand, ec) || ec) return cand;
        ec.clear();
    }
    return base;
}

static bool IsSixHexString(const std::wstring& s) {
    unsigned int v = 0;
    return TryParseSixHexFromW(s, &v);
}

static std::wstring ToLowerAsciiW(std::wstring s) {
    for (auto& ch : s) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return s;
}

static std::wstring ThemeToneIdHexForFile(const ThemeColors& theme) {
    // Built-ins: prefer stable IDs.
    if (theme.name.size() == 6 && IsSixHexString(theme.name)) {
        std::wstring hex = theme.name;
        for (auto& ch : hex) {
            if (ch >= L'a' && ch <= L'f') ch = static_cast<wchar_t>(ch - L'a' + L'A');
        }
        return hex;
    }
    const std::wstring nameLower = ToLowerAsciiW(theme.name);
    if (nameLower == L"white" || theme.nameJp == L"ホワイト") return L"FFFFFF";
    if (nameLower == L"dark" || theme.nameJp == L"ダーク") return L"000000";

    // Custom: use accent color as the tone id.
    return ColorToHexNoHashW(theme.accent);
}

static bool WriteThemeFile(const std::wstring& root, const ThemeColors& theme, std::wstring* outFileId) {
    if (outFileId) outFileId->clear();
    if (!kThemeUseThemeFiles) return false;
    if (root.empty()) return false;
    auto themeRoot = ThemeRootPath(root);
    if (themeRoot.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(themeRoot, ec);
    if (ec) return false;

    const std::wstring toneId = ThemeToneIdHexForFile(theme);
    std::filesystem::path path = AllocUniqueThemeFilePathByIdHex(root, toneId);
    if (path.empty()) return false;

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"format\": \"pdf_note_theme_v1\",\n";
    oss << "  \"tone_id\": \"" << WideToUTF8(toneId) << "\",\n";
    WriteThemeObject(oss, "  ", theme);
    oss << "}\n";
    std::string data = oss.str();
    std::wstring err;
    if (!AtomicWriteUtf8WithWorkspaceDirs(path, data, std::filesystem::path(root), &err)) return false;
    if (outFileId) *outFileId = path.filename().wstring();
    return true;
}

static void WorkspaceSafeDirs(const std::filesystem::path& workspaceRoot,
                              std::filesystem::path* outPreferredTmp,
                              std::filesystem::path* outQuarantineDir) {
    if (outPreferredTmp) outPreferredTmp->clear();
    if (outQuarantineDir) outQuarantineDir->clear();
    if (workspaceRoot.empty()) return;
    std::filesystem::path resource = workspaceRoot / L"__resource__";
    if (outPreferredTmp) *outPreferredTmp = resource / L"__tmp__";
    if (outQuarantineDir) *outQuarantineDir = resource / L"__escape__";
}

static bool AtomicWriteUtf8WithWorkspaceDirs(const std::filesystem::path& dest,
                                             std::string_view utf8,
                                             const std::filesystem::path& workspaceRoot,
                                             std::wstring* err) {
    SaveOperationGuard guard;
    std::filesystem::path preferredTmp;
    std::filesystem::path quarantineDir;
    WorkspaceSafeDirs(workspaceRoot, &preferredTmp, &quarantineDir);
    return atomic_write::AtomicWriteUtf8(dest, utf8, preferredTmp, quarantineDir, err);
}

static void WriteThemeObject(std::ostream& os, const std::string& indent, const ThemeColors& theme) {
    auto writeString = [&](const char* key, const std::string& value, bool trailing = true) {
        os << indent << "\"" << key << "\": \"" << value << "\"";
        if (trailing) os << ",";
        os << "\n";
    };
    auto writeColor = [&](const char* key, COLORREF value, bool trailing = true) {
        writeString(key, ColorToHex(value), trailing);
    };
    writeString("name", WideToUTF8(theme.name));
    const std::wstring& nameJp = theme.nameJp.empty() ? theme.name : theme.nameJp;
    writeString("name_jp", WideToUTF8(nameJp));
    writeColor("windowBg", theme.windowBg);
    writeColor("windowText", theme.windowText);
    writeColor("panelBg", theme.panelBg);
    writeColor("panelText", theme.panelText);
    writeColor("menuBg", theme.menuBg);
    writeColor("menuText", theme.menuText);
    writeColor("menuSelBg", theme.menuSelBg);
    writeColor("menuSelText", theme.menuSelText);
    writeColor("toolbarBg", theme.toolbarBg);
    writeColor("toolbarText", theme.toolbarText);
    writeColor("buttonBg", theme.buttonBg);
    writeColor("buttonText", theme.buttonText);
    writeColor("buttonBorder", theme.buttonBorder);
    writeColor("buttonHot", theme.buttonHot);
    writeColor("buttonPressed", theme.buttonPressed);
    writeColor("splitterBg", theme.splitterBg);
    writeColor("splitterLine", theme.splitterLine);
    writeColor("pdfBg", theme.pdfBg);
    writeColor("pdfPageBg", theme.pdfPageBg);
    writeColor("noteBg", theme.noteBg);
    writeColor("noteText", theme.noteText);
    writeColor("selectionBg", theme.selectionBg);
    writeColor("selectionText", theme.selectionText);
    writeColor("accent", theme.accent, false);
}

static std::string ReadTextFileUtf8(const std::filesystem::path& p);
static bool ParseThemeBlock(const std::string& json, ThemeColors& out);

static bool ReadTextFileUtf8Limited(const std::filesystem::path& p,
                                    std::uint64_t maxBytes,
                                    std::string* out,
                                    std::wstring* outErr) {
    if (out) out->clear();
    if (outErr) outErr->clear();
    if (p.empty()) {
        if (outErr) *outErr = L"invalid path";
        return false;
    }
    if (IsSymlinkPath(p)) {
        if (outErr) *outErr = L"symlink is not allowed";
        return false;
    }
    std::error_code ec;
    auto st = std::filesystem::status(p, ec);
    if (ec || !std::filesystem::is_regular_file(st)) {
        if (outErr) *outErr = L"not a regular file";
        return false;
    }
    ec.clear();
    auto sz = std::filesystem::file_size(p, ec);
    if (ec) {
        if (outErr) *outErr = L"failed to stat file size";
        return false;
    }
    if (static_cast<std::uint64_t>(sz) > maxBytes) {
        if (outErr) *outErr = L"file is too large";
        return false;
    }

    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) {
        if (outErr) *outErr = L"failed to open file";
        return false;
    }
    std::string buf(static_cast<size_t>(sz), '\0');
    if (sz > 0) {
        ifs.read(buf.data(), static_cast<std::streamsize>(sz));
        if (!ifs) {
            if (outErr) *outErr = L"failed to read file";
            return false;
        }
    }
    // Strip UTF-8 BOM if present.
    if (buf.size() >= 3 &&
        static_cast<unsigned char>(buf[0]) == 0xEF &&
        static_cast<unsigned char>(buf[1]) == 0xBB &&
        static_cast<unsigned char>(buf[2]) == 0xBF) {
        buf.erase(0, 3);
    }
    if (out) *out = std::move(buf);
    return true;
}

static bool ReadThemeFile(const std::filesystem::path& path,
                          const std::vector<ThemeColors>& fallbackCatalog,
                          ThemeColors& out,
                          std::wstring* outReason) {
    if (outReason) outReason->clear();
    std::wstring fileToneId;
    if (!TryParseThemeIdHexFromFileId(path.filename().wstring(), &fileToneId)) {
        if (outReason) *outReason = L"ファイル名が theme_XXXXXX.json 形式ではありません。";
        return false;
    }
    std::wstring readErr;
    std::string json;
    if (!ReadTextFileUtf8Limited(path, kMaxThemeFileBytes, &json, &readErr)) {
        if (outReason) *outReason = L"テーマファイルの読み込みに失敗しました。(" + readErr + L")";
        return false;
    }

    // Verify it's our theme file.
    auto fmt = ParseJsonStringField(json, "format");
    if (!fmt) {
        if (kThemeRequireFormatTag) {
            if (outReason) *outReason = L"format が無いため、このソフトのテーマとして扱いません。";
            return false;
        }
    } else if (*fmt != "pdf_note_theme_v1") {
        if (outReason) *outReason = L"format がこのソフトのテーマ形式ではありません。";
        return false;
    }

    auto tone = ParseJsonStringField(json, "tone_id");
    if (!tone) {
        if (outReason) *outReason = L"tone_id が無いため、このソフトのテーマとして扱いません。";
        return false;
    }
    std::wstring toneW = UTF8ToWide(*tone);
    if (!toneW.empty() && toneW[0] == L'#') toneW.erase(0, 1);
    if (toneW.size() != 6 || !IsSixHexString(toneW)) {
        if (outReason) *outReason = L"tone_id が不正です。";
        return false;
    }
    for (auto& ch : toneW) {
        if (ch >= L'a' && ch <= L'f') ch = static_cast<wchar_t>(ch - L'a' + L'A');
    }
    if (toneW != fileToneId) {
        if (outReason) *outReason = L"ファイル名の基調ID(XXXXXX)と tone_id が一致しません。";
        return false;
    }

    ThemeColors t = fallbackCatalog.empty() ? ThemeColors{} : fallbackCatalog.front();
    if (!ParseThemeBlock(json, t)) {
        if (outReason) *outReason = L"テーマJSONの解析に失敗しました。";
        return false;
    }

    if (t.name.empty()) {
        if (outReason) *outReason = L"name が空です。";
        return false;
    }
    if (t.name.size() > 200 || t.nameJp.size() > 200) {
        if (outReason) *outReason = L"name/name_jp が長すぎます。";
        return false;
    }
    out = std::move(t);
    return true;
}

static std::vector<std::wstring> ListThemeFileIds(const std::wstring& root) {
    std::vector<std::wstring> out;
    auto themeRoot = ThemeRootPath(root);
    if (themeRoot.empty()) return out;
    std::error_code ec;
    if (!std::filesystem::exists(themeRoot, ec) || ec) return out;
    for (auto it = std::filesystem::directory_iterator(themeRoot, ec);
         !ec && it != std::filesystem::directory_iterator(); ++it) {
        const auto& entry = *it;
        if (entry.is_symlink(ec)) { ec.clear(); continue; }
        if (!entry.is_regular_file(ec) || ec) { ec.clear(); continue; }
        const auto p = entry.path();
        if (!TryParseThemeAccentFromFileName(p, nullptr)) continue;
        std::wstring id = p.filename().wstring();
        if (!IsSafeThemeFileId(id)) continue;
        out.push_back(std::move(id));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static std::filesystem::path UserPaletteFilePath() {
    if (g_workspaceRoot.empty()) return {};
    return std::filesystem::path(g_workspaceRoot) / L"__resource__" / L"__settings__" / L"user_palette.json";
}

static void FillDefaultUserPaletteColors(COLORREF* custom, size_t count) {
    if (!custom || count == 0) return;
    constexpr COLORREF defaults[] = {
        RGB(255, 140,   0), // [0] Orange (Preset 1)
        RGB(255, 255,   0), // [1] Yellow (Preset 2)
        RGB(255,   0,   0), // [2] Red    (Preset 3)
        RGB(  0, 160, 255), // [3] Blue   (Preset 4)
        RGB(  0, 200,   0), // [4] Green  (Preset 5)
        RGB(200,  80, 200), // [5] Magenta(Preset 6)
        RGB(128, 128, 128), // [6] Gray   (Preset 7)
        RGB(  0,   0,   0), // [7] Last OK selected color (black default)
        RGB(  0,   0,   0), // [8] Picker Custom #1 (Dynamic Slot 8 candidate, black default)
        RGB(  0,   0,   0), // [9] Picker Custom #2 (black default)
        RGB(255, 255, 255), // [10] Picker Custom #3 (white)
        RGB(255, 255, 255), // [11] Picker Custom #4 (white)
        RGB(255, 255, 255), // [12] Picker Custom #5 (white)
        RGB(255, 255, 255), // [13] Picker Custom #6 (white)
        RGB(255, 255, 255), // [14] Picker Custom #7 (white)
        RGB(255, 255, 255), // [15] Picker Custom #8 (white)
        RGB(255, 255, 255), // [16] Picker Custom #9 (white)
        RGB(255, 255, 255), // [17] Picker Custom #10 (white)
        RGB(255, 255, 255), // [18] Picker Custom #11 (white)
        RGB(255, 255, 255), // [19] Picker Custom #12 (white)
        RGB(255, 255, 255), // [20] Picker Custom #13 (white)
        RGB(255, 255, 255), // [21] Picker Custom #14 (white)
        RGB(255, 255, 255), // [22] Picker Custom #15 (white)
        RGB(255, 255, 255), // [23] Picker Custom #16 (white)
    };
    for (size_t i = 0; i < count; ++i) {
        custom[i] = defaults[std::min(i, std::size(defaults) - 1)];
    }
}

// Build the runtime palette from fixed presets + two dynamic slots.
// Dynamic slot 8: g_paletteDialogCustomColor (s_customColors[0] snapshot).
// Dynamic slot 9: g_paletteCustomColor (last OK-returned color).
// Either dynamic slot is omitted when it exactly duplicates any earlier entry.
static void BuildAndApplyRuntimePalette(const COLORREF* presets, size_t presetCount) {
    const std::vector<COLORREF> prevPalette = g_palette;
    const COLORREF prevActive = g_activeColor;

    std::vector<COLORREF> result;
    result.reserve(presetCount + 2);

    // Add presets without duplicates.
    for (size_t i = 0; i < presetCount; ++i) {
        bool dup = false;
        for (const COLORREF& c : result) { if (c == presets[i]) { dup = true; break; } }
        if (!dup) result.push_back(presets[i]);
    }

    // Slot 8: dialog custom color — append only if not already in presets.
    bool dialogInPresets = false;
    for (const COLORREF& c : result) { if (c == g_paletteDialogCustomColor) { dialogInPresets = true; break; } }
    if (!dialogInPresets) result.push_back(g_paletteDialogCustomColor);

    // Slot 9: last OK color — append only if distinct from all prior entries.
    bool okInPrior = false;
    for (const COLORREF& c : result) { if (c == g_paletteCustomColor) { okInPrior = true; break; } }
    if (!okInPrior) result.push_back(g_paletteCustomColor);

    g_palette = result;

    // Preserve the active color if it is still present; otherwise keep it as-is
    // (the color may still be used even if not displayed as a button).
    (void)prevPalette;
    (void)prevActive;
}

static void ResetRuntimePaletteToDefault() {
    COLORREF presets[] = {
        RGB(255, 140,   0), // orange
        RGB(255, 255,   0), // yellow
        RGB(255,   0,   0), // red
        RGB(  0, 160, 255), // blue
        RGB(  0, 200,   0), // green
        RGB(200,  80, 200), // magenta
        RGB(128, 128, 128), // gray (7th preset)
    };
    static_assert(std::size(presets) == kPresetPaletteSlotCount, "preset count mismatch");
    BuildAndApplyRuntimePalette(presets, std::size(presets));
}

static bool ParseHexColorToken(const std::string& value, COLORREF& out) {
    std::string hex = value;
    if (hex.size() == 7 && hex[0] == '#') hex = hex.substr(1);
    if (hex.size() != 6) return false;
    unsigned int v = 0;
    std::stringstream ss;
    ss << std::hex << hex;
    ss >> v;
    out = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return true;
}

static bool LoadUserPaletteColors(COLORREF* custom, size_t count) {
    if (!custom || count == 0) return false;
    FillDefaultUserPaletteColors(custom, count);

    std::vector<std::filesystem::path> candidates;
    candidates.push_back(UserPaletteFilePath());

    std::string buf;
    std::wstring readErr;
    bool loaded = false;
    for (const auto& path : candidates) {
        if (path.empty()) continue;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) continue;
        if (ReadTextFileUtf8Limited(path, kMaxUserPaletteBytes, &buf, &readErr) && !buf.empty()) {
            loaded = true;
            break;
        }
    }
    if (!loaded) return false;

    std::regex re("\"(#?[0-9A-Fa-f]{6})\"");
    size_t idx = 0;
    for (auto it = std::sregex_iterator(buf.begin(), buf.end(), re);
         it != std::sregex_iterator() && idx < count; ++it) {
        COLORREF c{};
        if (ParseHexColorToken((*it)[1].str(), c)) {
            custom[idx++] = c;
        }
    }
    return idx > 0;
}

static void SaveUserPaletteColors(const COLORREF* custom, size_t count) {
    if (!custom || count == 0) return;
    std::ostringstream oss;
    oss << "{\n  \"colors\": [";
    for (size_t i = 0; i < count; ++i) {
        if (i) oss << ", ";
        oss << "\"" << ColorToHex(custom[i]) << "\"";
    }
    oss << "]\n}\n";
    std::string data = oss.str();
    std::wstring err;

    auto primary = UserPaletteFilePath();
    if (primary.empty()) return;

    std::vector<std::filesystem::path> targets;
    targets.push_back(primary);

    for (const auto& path : targets) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) continue;
        AtomicWriteUtf8WithWorkspaceDirs(path, data, std::filesystem::path(g_workspaceRoot), &err);
    }
}

static void UpdateThemeBrushes() {
    if (g_hThemeWindowBrush) DeleteObject(g_hThemeWindowBrush);
    if (g_hThemePanelBrush) DeleteObject(g_hThemePanelBrush);
    if (g_hThemeNoteBrush) DeleteObject(g_hThemeNoteBrush);
    if (g_hThemeToolbarBrush) DeleteObject(g_hThemeToolbarBrush);
    if (g_hThemeMenuBrush) DeleteObject(g_hThemeMenuBrush);
    g_hThemeWindowBrush = CreateSolidBrush(g_theme.windowBg);
    g_hThemePanelBrush = CreateSolidBrush(g_theme.panelBg);
    g_hThemeNoteBrush = CreateSolidBrush(g_noteBgColor);
    g_hThemeToolbarBrush = CreateSolidBrush(g_theme.toolbarBg);
    g_hThemeMenuBrush = CreateSolidBrush(g_theme.menuBg);
}

static void WriteThemeConfig(const std::filesystem::path& path,
                             const std::wstring& workspaceRoot,
                             const std::wstring& current,
                             const std::vector<ThemeColors>& themes) {
    (void)themes;
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return;
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"format\": \"pdf_note_theme_config_v2\",\n";
    oss << "  \"current_file\": \"" << WideToUTF8(current) << "\",\n";
    oss << "  \"verified\": [\n";
    for (size_t i = 0; i < g_themeVerified.size(); ++i) {
        const auto& v = g_themeVerified[i];
        oss << "    {\n";
        oss << "      \"file\": \"" << WideToUTF8(v.file) << "\",\n";
        oss << "      \"size\": " << static_cast<unsigned long long>(v.size) << ",\n";
        oss << "      \"mtime_ms\": " << static_cast<long long>(v.mtimeMs) << ",\n";
        oss << "      \"sha256\": \"" << v.sha256 << "\",\n";
        oss << "      \"display\": \"" << WideToUTF8(v.displayName) << "\",\n";
        oss << "      \"display_jp\": \"" << WideToUTF8(v.displayNameJp) << "\",\n";
        oss << "      \"accent\": \"" << ColorToHex(v.accent) << "\",\n";
        oss << "      \"noteBg\": \"" << ColorToHex(v.noteBg) << "\"\n";
        oss << "    }" << (i + 1 < g_themeVerified.size() ? "," : "") << "\n";
    }
    oss << "  ]\n";
    oss << "}\n";
    std::string data = oss.str();
    std::wstring err;
    AtomicWriteUtf8WithWorkspaceDirs(path, data, std::filesystem::path(workspaceRoot), &err);
}

void SetPaletteCustomColor(COLORREF color) {
    g_paletteCustomColor = color;
    if (!g_palette.empty()) {
        g_palette.back() = color;
    }
}

bool PickColorDialog(HWND owner, COLORREF initial, COLORREF* outColor, bool trackDialogCustom) {
    if (!outColor) return false;
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = owner;
    cc.rgbResult = initial;

    // Load full 24-slot palette from JSON (or defaults).
    // Indices 8..23 correspond 1-to-1 with s_customColors[0..15].
    COLORREF custom[kToolPaletteCommandSlotCapacity]{};
    LoadUserPaletteColors(custom, std::size(custom));

    COLORREF s_customColors[16];
    for (size_t i = 0; i < 16; ++i) {
        s_customColors[i] = custom[kPickerCustomColorStartSlotIndex + i];
    }

    cc.lpCustColors = s_customColors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    bool ok = ChooseColorW(&cc);

    // Save modified custom colors (s_customColors[0..15]) back to JSON indices 8..23
    for (size_t i = 0; i < 16; ++i) {
        custom[kPickerCustomColorStartSlotIndex + i] = s_customColors[i];
    }
    g_paletteDialogCustomColor = s_customColors[0]; // JSON index 8 / Picker #1

    if (ok) {
        *outColor = cc.rgbResult;
        g_paletteCustomColor = cc.rgbResult;
        custom[kLastOkColorSlotIndex] = cc.rgbResult; // JSON index 7 / Last OK
    }

    // Persist full 24 slots to JSON and rebuild runtime palette.
    SaveUserPaletteColors(custom, std::size(custom));
    BuildAndApplyRuntimePalette(custom, static_cast<size_t>(kPresetPaletteSlotCount));
    PersistConfig();

    return ok;
}

void SyncUserPaletteToRuntime() {
    COLORREF custom[kToolPaletteCommandSlotCapacity]{};
    LoadUserPaletteColors(custom, std::size(custom));
    // Index 7 = Last OK color, Index 8 = Picker Custom #1
    g_paletteCustomColor       = custom[static_cast<size_t>(kLastOkColorSlotIndex)];
    g_paletteDialogCustomColor = custom[static_cast<size_t>(kPickerCustomColorStartSlotIndex)];
    BuildAndApplyRuntimePalette(custom, static_cast<size_t>(kPresetPaletteSlotCount));
}

void LoadUserPaletteColorsForSettings(COLORREF* custom, size_t count) {
    LoadUserPaletteColors(custom, count);
}

void SaveUserPaletteColorsForSettings(const COLORREF* custom, size_t count) {
    SaveUserPaletteColors(custom, count);
    if (!custom || count == 0) return;

    if (count > static_cast<size_t>(kLastOkColorSlotIndex)) {
        g_paletteCustomColor = custom[kLastOkColorSlotIndex];
    }
    if (count > static_cast<size_t>(kPickerCustomColorStartSlotIndex)) {
        g_paletteDialogCustomColor = custom[kPickerCustomColorStartSlotIndex];
    }

    BuildAndApplyRuntimePalette(custom, static_cast<size_t>(kPresetPaletteSlotCount));
    PersistConfig();
}

static int RuntimePaletteIndexForColor(COLORREF color) {
    for (size_t i = 0; i < g_palette.size(); ++i) {
        if (g_palette[i] == color) return static_cast<int>(i);
    }
    return -1;
}

bool ApplyActivePaletteColorStep(HWND hwnd, int direction, bool focusPdfView) {
    if (!ToolbarHasColorOptions(g_toolMode) || g_palette.empty()) return false;
    direction = (direction < 0) ? -1 : 1;

    const COLORREF current = ToolColorForMode(g_toolMode);
    int idx = RuntimePaletteIndexForColor(current);
    if (idx < 0) {
        idx = (direction > 0) ? -1 : 0;
    }
    const int count = static_cast<int>(g_palette.size());
    const int next = (idx + direction + count) % count;
    g_activeColor = g_palette[static_cast<size_t>(next)];
    StoreToolColorForMode(g_toolMode, g_activeColor);
    PersistConfig();
    if (g_hPdfToolbar) {
        RedrawWindow(g_hPdfToolbar, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    if (hwnd) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    if (focusPdfView && g_hPdfView) {
        SetFocus(g_hPdfView);
    }
    return true;
}

// ---------------------------------------------------------------------
// Utility helpers (moved from original)
// ---------------------------------------------------------------------
std::string WideToUTF8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        s.data(), len, nullptr, nullptr);
    return s;
}

std::wstring UTF8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                  nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        w.data(), len);
    return w;
}

namespace {
constexpr std::uint64_t kMaxBuildInfoManifestBytes = 64 * 1024;

struct BuildInfoArtifactEntry {
    std::wstring fileName;
    std::wstring sha256;
};

struct BuildInfoManifestData {
    std::wstring version;
    std::wstring buildTimestamp;
    std::vector<BuildInfoArtifactEntry> artifacts;
};

static std::wstring TrimAsciiWhitespaceWide(std::wstring_view text) {
    size_t start = 0;
    while (start < text.size()) {
        wchar_t ch = text[start];
        if (ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n') break;
        ++start;
    }
    size_t end = text.size();
    while (end > start) {
        wchar_t ch = text[end - 1];
        if (ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n') break;
        --end;
    }
    return std::wstring(text.substr(start, end - start));
}

static void ReplaceAllInPlace(std::wstring* text, std::wstring_view needle, std::wstring_view replacement) {
    if (!text || needle.empty()) return;
    size_t pos = 0;
    while ((pos = text->find(needle, pos)) != std::wstring::npos) {
        text->replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

static std::vector<std::wstring> SplitTabFields(std::wstring_view line) {
    std::vector<std::wstring> fields;
    size_t start = 0;
    while (start <= line.size()) {
        size_t tab = line.find(L'\t', start);
        if (tab == std::wstring_view::npos) {
            fields.emplace_back(line.substr(start));
            break;
        }
        fields.emplace_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return fields;
}

static std::filesystem::path CurrentExecutablePathForBuildInfo() {
    std::vector<wchar_t> buffer(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (len > 0 && len >= buffer.size() - 1) {
        buffer.resize(buffer.size() * 2, L'\0');
        len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    if (len == 0) {
        std::error_code ec;
        std::filesystem::path cwd = std::filesystem::current_path(ec);
        if (ec) return std::filesystem::path(L"pdf_note_workspace.exe");
        return cwd / L"pdf_note_workspace.exe";
    }
    return std::filesystem::path(std::wstring(buffer.data(), len));
}

static BuildInfoManifestData LoadBuildInfoManifestData() {
    BuildInfoManifestData data;
    const std::filesystem::path exePath = CurrentExecutablePathForBuildInfo();
    std::filesystem::path manifestPath = exePath;
    manifestPath += L".buildinfo.txt";

    std::wstring readErr;
    std::string utf8;
    if (!ReadTextFileUtf8Limited(manifestPath, kMaxBuildInfoManifestBytes, &utf8, &readErr)) {
        return data;
    }

    std::wistringstream stream(UTF8ToWide(utf8));
    std::wstring line;
    bool formatOk = false;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        const std::wstring trimmedLine = TrimAsciiWhitespaceWide(line);
        if (trimmedLine.empty()) continue;

        const std::vector<std::wstring> fields = SplitTabFields(trimmedLine);
        if (fields.empty()) continue;
        if (fields[0] == L"format" && fields.size() >= 2) {
            formatOk = (fields[1] == L"pdf-note-build-info-v1");
            continue;
        }
        if (!formatOk) {
            continue;
        }
        if (fields[0] == L"version" && fields.size() >= 2) {
            data.version = TrimAsciiWhitespaceWide(fields[1]);
            continue;
        }
        if (fields[0] == L"build_timestamp" && fields.size() >= 2) {
            data.buildTimestamp = TrimAsciiWhitespaceWide(fields[1]);
            continue;
        }
        if (fields[0] == L"artifact" && fields.size() >= 3) {
            BuildInfoArtifactEntry entry;
            entry.fileName = TrimAsciiWhitespaceWide(fields[1]);
            entry.sha256 = TrimAsciiWhitespaceWide(fields[2]);
            if (!entry.fileName.empty() && !entry.sha256.empty()) {
                data.artifacts.push_back(std::move(entry));
            }
        }
    }

    if (!formatOk) {
        return BuildInfoManifestData{};
    }
    return data;
}

static std::wstring BuildArtifactDigestText(const BuildInfoManifestData& data, bool englishUi) {
    if (data.artifacts.empty()) {
        return englishUi ? L"  - (build manifest unavailable)" : L"  - (build manifest を読めませんでした)";
    }

    std::wstring text;
    for (size_t i = 0; i < data.artifacts.size(); ++i) {
        const auto& artifact = data.artifacts[i];
        text += L"  - ";
        text += artifact.fileName;
        text += L": ";
        text += artifact.sha256;
        if (i + 1 < data.artifacts.size()) {
            text += L"\n";
        }
    }
    return text;
}

const wchar_t* AppLogFileName(AppLogKind kind) {
    switch (kind) {
    case AppLogKind::PreviewTrace: return L"preview_trace.log";
    case AppLogKind::SwitchTiming: return L"switch_timing.log";
    case AppLogKind::Crash: return L"crash.log";
    case AppLogKind::StartupWatchdog: return L"startup_watchdog.log";
    default: return L"app.log";
    }
}

bool RuntimeAppLogEnabledLocked(AppLogKind kind) {
    switch (kind) {
    case AppLogKind::PreviewTrace: return g_runtimeAppLogConfig.previewTrace;
    case AppLogKind::SwitchTiming: return g_runtimeAppLogConfig.switchTiming;
    case AppLogKind::Crash: return g_runtimeAppLogConfig.crash;
    case AppLogKind::StartupWatchdog: return g_runtimeAppLogConfig.startupWatchdog;
    default: return false;
    }
}

std::wstring AppLogTimestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[64]{};
    swprintf(buf, std::size(buf), L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

std::filesystem::path AppLogPathLocked(AppLogKind kind) {
    std::filesystem::path base;
    if (!g_appLogWorkspaceRoot.empty()) {
        base = std::filesystem::path(g_appLogWorkspaceRoot);
    } else {
        std::error_code ec;
        base = std::filesystem::current_path(ec);
        if (ec) base.clear();
    }
    if (base.empty()) {
        return std::filesystem::path(L"__resource__") / L"__log__" / AppLogFileName(kind);
    }
    return base / L"__resource__" / L"__log__" / AppLogFileName(kind);
}

void AppendAppLogLineUtf8Locked(AppLogKind kind, const std::string& line) {
    if (!RuntimeAppLogEnabledLocked(kind) || line.empty()) return;
    try {
        fault_injection::MaybeThrow(L"app_log_before_write");
    } catch (...) {
        // Diagnostic logging must never interrupt the user's operation.
        return;
    }
    std::filesystem::path path = AppLogPathLocked(kind);
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream os(path, std::ios::binary | std::ios::app);
    if (!os) return;
    std::string payload = WideToUTF8(AppLogTimestamp()) + " " + line;
    if (payload.empty() || payload.back() != '\n') payload += "\n";
    os.write(payload.data(), static_cast<std::streamsize>(payload.size()));
}
} // namespace

void SetAppLogWorkspaceRoot(const std::wstring& root) {
    std::lock_guard<std::mutex> lock(g_appLogMutex);
    g_appLogWorkspaceRoot = root;
}

void ApplyRuntimeLogConfigFromWorkspace(const std::wstring& root, const WorkspaceConfig& cfg) {
    std::lock_guard<std::mutex> lock(g_appLogMutex);
    g_appLogWorkspaceRoot = root;
    g_runtimeAppLogConfig = cfg.debugLogs;
}

bool IsAppLogEnabled(AppLogKind kind) {
    std::lock_guard<std::mutex> lock(g_appLogMutex);
    return RuntimeAppLogEnabledLocked(kind);
}

std::filesystem::path AppLogPath(AppLogKind kind) {
    std::lock_guard<std::mutex> lock(g_appLogMutex);
    return AppLogPathLocked(kind);
}

void AppendAppLogLine(AppLogKind kind, const std::wstring& line) {
    std::lock_guard<std::mutex> lock(g_appLogMutex);
    AppendAppLogLineUtf8Locked(kind, WideToUTF8(line));
}

void AppendAppLogLineUtf8(AppLogKind kind, const std::string& line) {
    std::lock_guard<std::mutex> lock(g_appLogMutex);
    AppendAppLogLineUtf8Locked(kind, line);
}

void AppendCrashLogLine(const char* area, const char* detail) {
    std::string line = "crash";
    if (area && *area) {
        line += " [";
        line += area;
        line += "]";
    }
    if (detail && *detail) {
        line += ": ";
        line += detail;
    }
    AppendAppLogLineUtf8(AppLogKind::Crash, line);
}

void AppendCrashLogWide(const std::wstring& title, const std::wstring& detail) {
    std::wstring line = title.empty() ? L"crash" : title;
    if (!detail.empty()) line += L": " + detail;
    AppendAppLogLine(AppLogKind::Crash, line);
}

std::wstring TrimWhitespace(const std::wstring& s) {
    auto isSpace = [](wchar_t c) { return iswspace(c) != 0; };
    size_t start = 0;
    while (start < s.size() && isSpace(s[start])) ++start;
    size_t end = s.size();
    while (end > start && isSpace(s[end - 1])) --end;
    return s.substr(start, end - start);
}

bool IsEnglishUi() {
    std::wstring lang = g_config.language;
    std::transform(lang.begin(), lang.end(), lang.begin(), ::towlower);
    return !lang.empty() && lang.rfind(L"en", 0) == 0;
}

void UpdateLinkModeButtonState(bool active) {
    const wchar_t* label = IsEnglishUi() ? (active ? L"Linking" : L"Link")
                                          : (active ? L"リンク中" : L"リンク");
    if (g_hBtnShortcutPdfLink) {
        SetWindowTextW(g_hBtnShortcutPdfLink, label);
    }
}

void SetUIFont(HWND hWnd) {
    if (g_hUIFont) {
        SendMessageW(hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_hUIFont), TRUE);
    }
}

namespace {
int CALLBACK EnumFontFoundProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lParam) {
    auto* found = reinterpret_cast<bool*>(lParam);
    *found = true;
    return 0;
}

bool IsFontInstalled(const wchar_t* faceName) {
    if (!faceName || !*faceName) return false;
    HDC hdc = GetDC(nullptr);
    if (!hdc) return false;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, faceName, _TRUNCATE);
    bool found = false;
    EnumFontFamiliesExW(hdc, &lf, EnumFontFoundProc, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, hdc);
    return found;
}

std::wstring PickDefaultFontFaceName() {
    static std::wstring cached;
    if (!cached.empty()) return cached;
    for (const auto& entry : kDefaultFontList) {
        if (IsFontInstalled(entry.faceName)) {
            cached = entry.faceName;
            return cached;
        }
    }
    cached = L"Segoe UI";
    return cached;
}
} // namespace

std::wstring GetDefaultFontFaceName() {
    return PickDefaultFontFaceName();
}

bool IsJapaneseRenderCharForNoteFont(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') {
        return (g_config.noteFontDigitTarget == 1);
    }
    return (ch >= 0x3000 && ch <= 0x30FF) ||
           (ch >= 0x3400 && ch <= 0x4DBF) ||
           (ch >= 0x4E00 && ch <= 0x9FFF) ||
           (ch >= 0xF900 && ch <= 0xFAFF) ||
           (ch >= 0xFF00 && ch <= 0xFFEF);
}

std::wstring ResolveNoteRenderBaseFace() {
    if (g_config.noteFontCustomization == 0) {
        return L"Segoe UI";
    }
    std::wstring face = g_noteRenderFontName;
    if (face.empty()) face = g_noteFontName;
    if (face.empty()) face = GetDefaultFontFaceName();

    if (g_config.noteFontCustomization == 1) {
        if (face == L"Meiryo" || face == L"Meiryo UI" ||
            face == L"Yu Gothic" || face == L"Yu Gothic UI" ||
            face == L"MS Gothic" || face == L"MS PGothic" || face == L"MS Mincho") {
            return L"Arial";
        }
    }
    return face;
}

std::wstring ResolveNoteRenderJpFace() {
    if (g_config.noteFontCustomization == 0) {
        return L"Meiryo UI";
    }
    if (g_config.noteFontCustomization == 1) {
        std::wstring face = g_noteRenderFontName;
        if (face.empty()) face = g_noteFontName;
        if (face.empty()) face = GetDefaultFontFaceName();
        if (face == L"Arial" || face == L"Segoe UI" || face == L"Tahoma" ||
            face == L"Verdana" || face == L"Times New Roman" ||
            face == L"Consolas" || face == L"Courier New") {
            return L"Meiryo";
        }
        return face;
    }
    std::wstring face = g_noteRenderJpFontName;
    if (face.empty()) face = ResolveNoteRenderBaseFace();
    return face;
}

HFONT CreateUIFont() {
    LOGFONTW lf{};
    HDC screen = GetDC(nullptr);
    lf.lfHeight = -MulDiv(10, GetDeviceCaps(screen, LOGPIXELSY), 72);
    ReleaseDC(nullptr, screen);
    lf.lfWeight = FW_NORMAL;
    const auto face = GetDefaultFontFaceName();
    wcscpy_s(lf.lfFaceName, face.c_str());
    return CreateFontIndirectW(&lf);
}

HFONT CreateUIFontVariant(double pt) {
    LOGFONTW lf{};
    if (g_hUIFont && GetObjectW(g_hUIFont, sizeof(lf), &lf)) {
        // reuse UI font face/weight
    } else {
        lf.lfWeight = FW_NORMAL;
        const auto face = GetDefaultFontFaceName();
        wcscpy_s(lf.lfFaceName, face.c_str());
    }
    HDC screen = GetDC(nullptr);
    lf.lfHeight = -MulDiv(static_cast<int>(std::lround(pt)), GetDeviceCaps(screen, LOGPIXELSY), 72);
    ReleaseDC(nullptr, screen);
    return CreateFontIndirectW(&lf);
}

HFONT CreateFontFromFaceName(const std::wstring& faceName, double pt) {
    LOGFONTW lf{};
    lf.lfWeight = FW_NORMAL;
    const auto face = faceName.empty() ? GetDefaultFontFaceName() : faceName;
    wcscpy_s(lf.lfFaceName, face.c_str());
    HDC screen = GetDC(nullptr);
    lf.lfHeight = -MulDiv(static_cast<int>(std::lround(pt)), GetDeviceCaps(screen, LOGPIXELSY), 72);
    ReleaseDC(nullptr, screen);
    return CreateFontIndirectW(&lf);
}

void UpdateWindowTitle(HWND hWnd) {
    const auto& ui = GetUiText();
    std::wstring title = ui.appTitle;
    if (!HasOfficeConversionFeature()) {
        title += L" Lite";
    }
    const std::wstring& logicalPdfPath = CurrentLogicalPdfPath();

    auto appendField = [&title](bool useDoublePipe, std::wstring_view label, const std::wstring& value) {
        if (value.empty()) return false;
        title += useDoublePipe ? L" || " : L" | ";
        title += label;
        title += L":";
        title += value;
        return true;
    };

    appendField(true, L"WS", g_workspaceRoot);

    bool detailStarted = false;
    detailStarted |= appendField(!detailStarted, L"Lec", std::filesystem::path(g_currentLecturePath).filename().wstring());
    detailStarted |= appendField(!detailStarted, L"Ses", std::filesystem::path(g_currentSessionPath).filename().wstring());
    detailStarted |= appendField(!detailStarted, L"PDF", std::filesystem::path(logicalPdfPath).filename().wstring());
    detailStarted |= appendField(!detailStarted, L"Note", std::filesystem::path(g_currentNotePath).filename().wstring());

    if (g_noteDirty || g_annotsDirty || g_noteNeedsIntegrate || g_annotsNeedsIntegrate) title += L"*";
    SetWindowTextW(hWnd, title.c_str());
}

const std::wstring& CurrentLogicalPdfPath() {
    return g_pdf.path;
}

FPDF_DOCUMENT CurrentLogicalPdfDocument() {
    return g_pdf.doc;
}

const std::vector<Annotation>* CurrentLogicalPdfAnnotations() {
    return &g_annots;
}

bool IsPdfPreviewReadOnlyActive() {
    return false;
}

// ---------------------------------------------------------------------
// number / sort helpers (from original)
// ---------------------------------------------------------------------
int KanjiDigit(wchar_t ch) {
    switch (ch) {
    case L'零': case L'〇': return 0;
    case L'一': return 1;
    case L'二': return 2;
    case L'三': return 3;
    case L'四': return 4;
    case L'五': return 5;
    case L'六': return 6;
    case L'七': return 7;
    case L'八': return 8;
    case L'九': return 9;
    default: return -1;
    }
}

int FullwidthDigit(wchar_t ch) {
    if (ch >= 0xFF10 && ch <= 0xFF19) return static_cast<int>(ch - 0xFF10);
    return -1;
}

int RomanDigit(wchar_t ch) {
    wchar_t u = static_cast<wchar_t>(towupper(ch));
    switch (u) {
    case L'I': return 1;
    case L'V': return 5;
    case L'X': return 10;
    case L'L': return 50;
    case L'C': return 100;
    case L'D': return 500;
    case L'M': return 1000;
    default: return -1;
    }
}

std::optional<int> ParseRomanNumber(const std::wstring& s) {
    std::vector<int> vals;
    for (wchar_t ch : s) {
        int v = RomanDigit(ch);
        if (v >= 0) vals.push_back(v);
    }
    if (vals.empty()) return std::nullopt;
    int total = 0;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (i + 1 < vals.size() && vals[i] < vals[i + 1]) {
            total -= vals[i];
        } else {
            total += vals[i];
        }
    }
    return total;
}

std::optional<int> ParseKanjiNumber(const std::wstring& s) {
    int total = 0;
    int current = 0;
    bool found = false;
    for (wchar_t ch : s) {
        int d = KanjiDigit(ch);
        if (d >= 0) {
            current = current * 10 + d;
            found = true;
            continue;
        }
        if (ch == L'十') {
            current = (current == 0) ? 10 : current * 10;
            found = true;
        } else if (ch == L'百') {
            current = (current == 0) ? 100 : current * 100;
            found = true;
        } else if (ch == L'千') {
            current = (current == 0) ? 1000 : current * 1000;
            found = true;
        }
    }
    if (!found) return std::nullopt;
    total += current;
    return total;
}

std::optional<int> ExtractNumericKey(const std::wstring& name) {
    std::wstring digits;
    for (wchar_t ch : name) {
        int fw = FullwidthDigit(ch);
        if (fw >= 0) {
            digits.push_back(static_cast<wchar_t>(L'0' + fw));
        } else if (iswdigit(ch)) {
            digits.push_back(ch);
        }
    }
    if (!digits.empty()) {
        try { return std::stoi(digits); } catch (...) {}
    }
    auto roman = ParseRomanNumber(name);
    if (roman) return roman;
    return ParseKanjiNumber(name);
}

void SetListWide(HWND hList) {
    if (!hList) return;
    int count = static_cast<int>(SendMessageW(hList, LB_GETCOUNT, 0, 0));
    if (count <= 0) {
        RECT rc{};
        GetClientRect(hList, &rc);
        int clientW = std::max(0, static_cast<int>(rc.right - rc.left));
        SendMessageW(hList, LB_SETHORIZONTALEXTENT, clientW, 0);
        InvalidateRect(hList, nullptr, TRUE);
        return;
    }

    HDC hdc = GetDC(hList);
    if (!hdc) return;
    HFONT oldFont = nullptr;
    if (g_hUIFont) oldFont = static_cast<HFONT>(SelectObject(hdc, g_hUIFont));

    int maxPx = 0;
    constexpr size_t kBuf = 512;
    std::wstring text;
    text.reserve(kBuf);
    for (int i = 0; i < count; ++i) {
        int len = static_cast<int>(SendMessageW(hList, LB_GETTEXTLEN, i, 0));
        if (len <= 0) continue;
        text.resize(static_cast<size_t>(len) + 1);
        int got = static_cast<int>(SendMessageW(hList, LB_GETTEXT, i, reinterpret_cast<LPARAM>(text.data())));
        if (got <= 0) continue;
        SIZE sz{};
        if (GetTextExtentPoint32W(hdc, text.c_str(), got, &sz)) {
            maxPx = std::max(maxPx, static_cast<int>(sz.cx));
        }
    }
    if (oldFont) SelectObject(hdc, oldFont);
    ReleaseDC(hList, hdc);

    RECT rc{};
    GetClientRect(hList, &rc);
    int clientW = std::max(0, static_cast<int>(rc.right - rc.left));
    // add generous padding so文字がバーやスクロールバーに隠れない
    int extent = std::max(clientW, maxPx + 24);
    SendMessageW(hList, LB_SETHORIZONTALEXTENT, extent, 0);
    InvalidateRect(hList, nullptr, TRUE);
}

bool IsPdfFile(const std::filesystem::path& p) {
    auto ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L".pdf";
}

bool IsImageFile(const std::filesystem::path& p) {
    auto ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg";
}

bool IsPdfOrImageFile(const std::filesystem::path& p) {
    return IsPdfFile(p) || IsImageFile(p);
}

bool IsNoteFile(const std::filesystem::path& p) {
    auto ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L".txt" || ext == L".csv" || ext == L".md" || ext == L".markdown" ||
           ext == L".note" || ext == L".tex" || ext == L".icr" || ext == L".clro";
}

static bool ListSelectionNeedsReconcile(HWND hList) {
    if (!hList) return false;
    const int count = static_cast<int>(SendMessageW(hList, LB_GETCOUNT, 0, 0));
    if (count <= 0) return false;
    const int curSel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
    return curSel < 0 || curSel >= count;
}

static void RefreshLeftPaneOpenStateNow() {
    if (g_hLectureList) InvalidateRect(g_hLectureList, nullptr, FALSE);
    if (g_hSessionList) InvalidateRect(g_hSessionList, nullptr, FALSE);
    if (g_hPdfList) InvalidateRect(g_hPdfList, nullptr, FALSE);
    if (g_hNoteList) InvalidateRect(g_hNoteList, nullptr, FALSE);
}

static void SyncLeftPaneSelectionNow() {
    const ULONGLONG startTick = preview_trace::TickNow();
    const bool needLectureReconcile = ListSelectionNeedsReconcile(g_hLectureList);
    const bool needSessionReconcile = ListSelectionNeedsReconcile(g_hSessionList);
    const bool needPdfReconcile = ListSelectionNeedsReconcile(g_hPdfList);
    const bool needNoteReconcile = ListSelectionNeedsReconcile(g_hNoteList);
    preview_trace::Append(
        L"SyncLeftPaneSelection",
        L"begin lectureCount=" + std::to_wstring(g_lectures.size()) +
        L" sessionCount=" + std::to_wstring(g_sessions.size()) +
        L" pdfCount=" + std::to_wstring(g_pdfFiles.size()) +
        L" noteCount=" + std::to_wstring(g_noteFiles.size()) +
        L" needLectureReconcile=" + preview_trace::Bool(needLectureReconcile) +
        L" needSessionReconcile=" + preview_trace::Bool(needSessionReconcile) +
        L" needPdfReconcile=" + preview_trace::Bool(needPdfReconcile) +
        L" needNoteReconcile=" + preview_trace::Bool(needNoteReconcile));
    if (!needLectureReconcile &&
        !needSessionReconcile &&
        !needPdfReconcile &&
        !needNoteReconcile) {
        RefreshLeftPaneOpenStateNow();
        preview_trace::Append(
            L"SyncLeftPaneSelection",
            L"fast_path=refresh_only elapsed_ms=" + preview_trace::ElapsedMs(startTick));
        return;
    }
    int normalizeCallCount = 0;
    auto normalizePathKey = [&](const std::wstring& path) -> std::wstring {
        if (path.empty()) return L"";
        ++normalizeCallCount;
        std::filesystem::path p(path);
        std::error_code ec;
        auto canon = std::filesystem::weakly_canonical(p, ec);
        std::wstring key = ec ? p.wstring() : canon.wstring();
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);
        return key;
    };
    auto ensureSel = [](HWND hList, int idx) {
        if (!hList) return;
        int count = static_cast<int>(SendMessageW(hList, LB_GETCOUNT, 0, 0));
        int targetSel = (idx >= 0 && idx < count) ? idx : -1;
        int curSel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
        if (curSel != targetSel) {
            SendMessageW(hList, LB_SETCURSEL, static_cast<WPARAM>(targetSel), 0);
        }
        // Open-item highlighting depends on global current-path state, so repaint
        // even when the logical selection index has not changed.
        InvalidateRect(hList, nullptr, FALSE);
    };

    int lectureIdx = -1;
    const int lectureNormalizeStart = normalizeCallCount;
    if (!g_currentLecturePath.empty()) {
        std::wstring curKey = normalizePathKey(g_currentLecturePath);
        for (size_t i = 0; i < g_lectures.size(); ++i) {
            std::wstring entryKey = normalizePathKey(g_lectures[i]);
            if (!curKey.empty() && entryKey == curKey) {
                lectureIdx = static_cast<int>(i);
                break;
            }
        }
    }
    preview_trace::Append(
        L"SyncLeftPaneSelection",
        L"after_match_lecture idx=" + std::to_wstring(lectureIdx) +
        L" normalize_calls=" + std::to_wstring(normalizeCallCount - lectureNormalizeStart) +
        L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    ensureSel(g_hLectureList, lectureIdx);
    preview_trace::Append(
        L"SyncLeftPaneSelection",
        L"after_ensure_lecture idx=" + std::to_wstring(lectureIdx) +
        L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));

    int sessionIdx = -1;
    const int sessionNormalizeStart = normalizeCallCount;
    if (!g_currentSessionPath.empty()) {
        std::wstring curKey = normalizePathKey(g_currentSessionPath);
        for (size_t i = 0; i < g_sessions.size(); ++i) {
            const auto& s = g_sessions[i];
            std::wstring entryKey = normalizePathKey(s.path);
            if (!curKey.empty() && entryKey == curKey) {
                sessionIdx = static_cast<int>(i);
                break;
            }
        }
    }
    preview_trace::Append(
        L"SyncLeftPaneSelection",
        L"after_match_session idx=" + std::to_wstring(sessionIdx) +
        L" normalize_calls=" + std::to_wstring(normalizeCallCount - sessionNormalizeStart) +
        L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    ensureSel(g_hSessionList, sessionIdx);
    preview_trace::Append(
        L"SyncLeftPaneSelection",
        L"after_ensure_session idx=" + std::to_wstring(sessionIdx) +
        L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));

    int pdfIdx = -1;
    const std::wstring& logicalPdfPath = CurrentLogicalPdfPath();
    const int pdfNormalizeStart = normalizeCallCount;
    if (!logicalPdfPath.empty()) {
        std::wstring curKey = normalizePathKey(logicalPdfPath);
        for (size_t i = 0; i < g_pdfFiles.size(); ++i) {
            std::wstring entryKey = normalizePathKey(g_pdfFiles[i].path);
            if (!curKey.empty() && entryKey == curKey) {
                pdfIdx = static_cast<int>(i);
                break;
            }
        }
    }
    preview_trace::Append(
        L"SyncLeftPaneSelection",
        L"after_match_pdf idx=" + std::to_wstring(pdfIdx) +
        L" normalize_calls=" + std::to_wstring(normalizeCallCount - pdfNormalizeStart) +
        L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    ensureSel(g_hPdfList, pdfIdx);
    preview_trace::Append(
        L"SyncLeftPaneSelection",
        L"after_ensure_pdf idx=" + std::to_wstring(pdfIdx) +
        L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));

    int noteIdx = -1;
    const int noteNormalizeStart = normalizeCallCount;
    if (!g_currentNotePath.empty()) {
        std::wstring curKey = normalizePathKey(g_currentNotePath);
        for (size_t i = 0; i < g_noteFiles.size(); ++i) {
            std::wstring entryKey = normalizePathKey(g_noteFiles[i].path);
            if (!curKey.empty() && entryKey == curKey) {
                noteIdx = static_cast<int>(i);
                break;
            }
        }
    }
    preview_trace::Append(
        L"SyncLeftPaneSelection",
        L"after_match_note idx=" + std::to_wstring(noteIdx) +
        L" normalize_calls=" + std::to_wstring(normalizeCallCount - noteNormalizeStart) +
        L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    ensureSel(g_hNoteList, noteIdx);
    preview_trace::Append(
        L"SyncLeftPaneSelection",
        L"end noteIdx=" + std::to_wstring(noteIdx) +
        L" total_normalize_calls=" + std::to_wstring(normalizeCallCount) +
        L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));
}

void RefreshLeftPaneOpenState() {
    RefreshLeftPaneOpenStateNow();
}

void SyncLeftPaneSelection() {
    if (g_deferredLeftPaneSelectionDepth > 0) {
        g_deferredLeftPaneSelectionPending = true;
        preview_trace::Append(
            L"SyncLeftPaneSelection",
            L"deferred depth=" + std::to_wstring(g_deferredLeftPaneSelectionDepth));
        return;
    }
    SyncLeftPaneSelectionNow();
}

void BeginDeferredLeftPaneSelection() {
    ++g_deferredLeftPaneSelectionDepth;
}

void FlushDeferredLeftPaneSelection() {
    if (!g_deferredLeftPaneSelectionPending) return;
    g_deferredLeftPaneSelectionPending = false;
    preview_trace::Append(
        L"SyncLeftPaneSelection",
        L"flush depth=" + std::to_wstring(g_deferredLeftPaneSelectionDepth));
    SyncLeftPaneSelectionNow();
}

void EndDeferredLeftPaneSelection() {
    if (g_deferredLeftPaneSelectionDepth <= 0) return;
    --g_deferredLeftPaneSelectionDepth;
    if (g_deferredLeftPaneSelectionDepth != 0) return;
    FlushDeferredLeftPaneSelection();
}

WorkspaceConfig DefaultWorkspaceConfig() {
    WorkspaceConfig cfg;
    cfg.classesDir = kDefaultClassesDir;
    cfg.cacheDir = kDefaultCacheDir;
    cfg.showAnnots = true;
    cfg.pdfFlowMode = L"v_ttb";
    cfg.pdfBitmapBudgetMiB = kPdfBitmapBudgetMiBDefault;
    cfg.pdfSinglePageMode = false;
    cfg.mouseWheelInvertVertical = false;
    cfg.mouseWheelInvertHorizontal = false;
    if (auto sys = QuerySystemTouchpadInvertVertical()) {
        cfg.touchpadInvertVertical = *sys;
        cfg.touchpadInvertHorizontal = *sys;
    } else {
        cfg.touchpadInvertVertical = false;
        cfg.touchpadInvertHorizontal = false;
    }
    cfg.leftWidth = g_leftWidth;
    cfg.rightWidth = g_rightWidth;
    cfg.topHeight = g_topHeight;
    cfg.windowWidth = 0;
    cfg.windowHeight = 0;
    cfg.defaultWindowWidth = 0;
    cfg.defaultWindowHeight = 0;
    cfg.leftSplit1 = 0;
    cfg.leftSplit2 = 0;
    cfg.defaultLeftWidth = 0;
    cfg.defaultRightWidth = 0;
    cfg.defaultTopHeight = 0;
    cfg.defaultLeftSplit1 = 0;
    cfg.defaultLeftSplit2 = 0;
    cfg.leftPaneCollapsed = false;
    cfg.language = L"ja";
    cfg.bottomPanePin = L"note";
    cfg.bottomNoteMode = L"legacy";
    cfg.notePlacement = L"bottom";
    cfg.colorTone = L"default";
    cfg.toneVariant = L"pure";
    cfg.quickAnnotPopupPlacement = L"auto";
    cfg.ownerDrawUi = false;
    cfg.useNativeFileDialogs = false;
    cfg.developerMode = false;
    cfg.studentMode = true;
    cfg.showMathList = true;
    cfg.downKeyLastLineAction = L"lineend";
    cfg.leftRightLineMoveAction = L"allow";
    cfg.autoPairBrackets = true;
    cfg.fullWidthParenCaretInside = false;
    cfg.fullWidthParenCancelNextLeft = false;
    cfg.noteFontPt = (g_noteFontPt > 1.0) ? g_noteFontPt : 10.0;
    cfg.noteFontName = g_noteFontName.empty() ? GetDefaultFontFaceName() : g_noteFontName;
    cfg.noteRenderFontPt = cfg.noteFontPt;
    cfg.noteRenderFontName = g_noteRenderFontName.empty() ? cfg.noteFontName : g_noteRenderFontName;
    cfg.noteRenderJpFontName = g_noteRenderJpFontName.empty() ? cfg.noteRenderFontName : g_noteRenderJpFontName;
    cfg.noteSystem = L"legacy";
    cfg.lectureSortMode = L"recent";
    cfg.sessionSortMode = L"numeric_asc";
    cfg.sessionNumberingMode = L"count";
    cfg.sessionAutoOpenMode = L"off";
    cfg.sessionAutoOpenPairLinked = false;
    cfg.noteRenderEnabled = true;
    cfg.noteRawOnly = false;
    cfg.noteRenderMath = false;
    cfg.noteWrapEnabled = true;
    cfg.noteVimModeEnabled = true;
    cfg.noteVimCaretLineRawTextVisible = false;
    cfg.noteVimClickEntersInsertMode = true;
    cfg.noteMathMarginTopPercent = 75;
    cfg.noteMathSupSubGapSupPercent = 0;
    cfg.noteGridEnabled = false;
    cfg.noteGridPitch = 24;
    cfg.selectionStyle = L"windows";
    cfg.noteBgColor = RGB(255, 255, 255);
    cfg.noteFgColor = RGB(0, 0, 0);
    cfg.noteBgSource = L"explicit";
    cfg.noteBgThemeName.clear();
    cfg.noteShortcutBackColor = RGB(0xFF, 0xF7, 0xE8);
    cfg.noteShortcutTextColor = RGB(0x00, 0xAA, 0x7B);
    cfg.noteShortcutTextTagKey = L"char";
    cfg.noteShortcutHeadingArrowInvert = false;
    cfg.noteCustomTagKey = L"c";
    cfg.noteCustomTagBold = false;
    cfg.noteCustomTagItalic = false;
    cfg.noteCustomTagUnderline = true;
    cfg.noteCustomTagStrike = false;
    cfg.noteCustomTagBackColor = false;
    cfg.noteCustomTagTextColor = false;
    cfg.noteCustomTagPresetMasks.fill(0);
    cfg.noteFontCustomization = 0;
    cfg.noteFontDigitTarget = 0;
    cfg.autoSaveSeconds = kDefaultAutoSaveSeconds;
    cfg.autoIntegrateSeconds = kAutoIntegrateModeOffSwitchExit;
    cfg.autoIntegrateCustomMinutes = kAutoIntegrateCustomMinutesDefault;
    cfg.clroNamePattern = L"回数_授業名_ノート1(連番).clro";
    cfg.textFontName = g_textFontName.empty() ? L"Meiryo" : g_textFontName;
    cfg.textFontPt = (g_textFontPt > 1.0) ? g_textFontPt : 14.0;
    cfg.textFontUseA4Scale = g_textFontUseA4Scale;
    cfg.textFontActiveSizeSlot = std::clamp(g_textFontActiveSizeSlot, 0, 1);
    cfg.textFontPtSlotA = (g_textFontPtSlotA > 1.0) ? g_textFontPtSlotA : cfg.textFontPt;
    cfg.textFontPtSlotB = (g_textFontPtSlotB > 1.0) ? g_textFontPtSlotB : 20.0;
    cfg.textFontUseA4ScaleSlotA = g_textFontUseA4ScaleSlotA;
    cfg.textFontUseA4ScaleSlotB = g_textFontUseA4ScaleSlotB;
    cfg.textBoxReadableBackground = g_textBoxReadableBackground;
    cfg.textBoxReadableBackgroundInverted = g_textBoxReadableBackgroundInverted;
    cfg.lineToolsShareStyle = true;
    cfg.lineWidthPt = (g_lineWidthPt > 0.0) ? g_lineWidthPt : 2.5;
    cfg.arrowWidthPt = (g_arrowWidthPt > 0.0) ? g_arrowWidthPt : cfg.lineWidthPt;
    cfg.arrowHead = ArrowHeadToString(g_arrowHead);
    cfg.waveWidthPt = (g_waveWidthPt > 0.0) ? g_waveWidthPt : cfg.lineWidthPt;
    cfg.lineDashStyle = (g_lineDashStyle == L"dash") ? L"dash" : L"solid";
    cfg.freehandWidthPt = (g_freehandWidthPt > 0.0) ? g_freehandWidthPt : 2.5;
    cfg.markerFreeWidthPt = (g_markerFreeWidthPt > 0.0) ? g_markerFreeWidthPt : 8.0;
    cfg.markerTextWidthPt = (g_markerTextWidthPt > 0.0) ? g_markerTextWidthPt : 4.0;
    cfg.markerTextUnderline = g_markerTextUnderline;
    cfg.eraserWidthPt = (g_eraserWidthPt > 0.0) ? g_eraserWidthPt : 4.0;
    cfg.markerAlpha = (g_markerAlpha > 0.0) ? g_markerAlpha : kMarkerAlphaDefault;
    cfg.lineAlpha = std::clamp(g_lineAlpha, 0.05, 1.0);
    cfg.arrowAlpha = std::clamp(g_arrowAlpha, 0.05, 1.0);
    cfg.waveAlpha = std::clamp(g_waveAlpha, 0.05, 1.0);
    cfg.freehandAlpha = std::clamp(g_freehandAlpha, 0.05, 1.0);
    cfg.shapeAlpha = std::clamp(g_shapeAlpha, 0.0, 1.0);
    cfg.textColor = g_textColor;
    cfg.lineColor = g_lineColor;
    cfg.arrowColor = g_arrowColor;
    cfg.waveColor = g_waveColor;
    cfg.freehandColor = g_freehandColor;
    cfg.markerFreeColor = g_markerFreeColor;
    cfg.markerTextColor = g_markerTextColor;
    cfg.shapeColor = g_shapeColor;
    cfg.paletteCustomColor = g_paletteCustomColor;
    cfg.magnifierShape = MagnifierShapeToString(MagnifierShape::Circle);
    cfg.shapeDetail = UTF8ToWide(ShapeDetailKey(g_shapeDetail));
    cfg.shapeKind = ShapeKindToString(g_shapeKind);
    cfg.shapeDrawMode = ShapeDrawModeToString(g_shapeDrawMode);
    cfg.annotLastMarkerDetail = L"marker_text";
    cfg.annotLastPenDetail = L"freehand";
    cfg.annotLastShapePresentation = L"stroke";
    cfg.annotLastShapeGeometry = L"line";
    cfg.annotLastShapeDetail = L"line";
    cfg.freehandCorrection = L"off";
    cfg.freehandCorrectionStyle = L"auto";
    cfg.freehandCorrectionFill = L"off";
    cfg.scheduleDayMask = 0x1F;
    cfg.schedulePeriods = 6;
    cfg.scheduleCells.assign(static_cast<size_t>(7 * cfg.schedulePeriods), L"");
    cfg.scheduleStartTimes.assign(static_cast<size_t>(7 * 13), L"");
    cfg.shortcuts.clear();
    cfg.markFontPx = 0;
    cfg.headingFontPx = 0;
    cfg.markColor = -1;
    cfg.headingColor = -1;
    cfg.headingBold = true;
    cfg.headingUnderline = true;
    cfg.headingLeftBar = true;
    return cfg;
}

static std::string ReadTextFileUtf8(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    ifs.seekg(0, std::ios::end);
    std::streamsize size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    std::string buf(static_cast<size_t>(size), '\0');
    ifs.read(buf.data(), size);
    return buf;
}

static bool IsAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static std::string TrimAsciiWhitespace(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && IsAsciiSpace(s[start])) ++start;
    size_t end = s.size();
    while (end > start && IsAsciiSpace(s[end - 1])) --end;
    return s.substr(start, end - start);
}

static std::string StripUtf8Bom(const std::string& s) {
    if (s.size() >= 3) {
        unsigned char b0 = static_cast<unsigned char>(s[0]);
        unsigned char b1 = static_cast<unsigned char>(s[1]);
        unsigned char b2 = static_cast<unsigned char>(s[2]);
        if (b0 == 0xEF && b1 == 0xBB && b2 == 0xBF) {
            return s.substr(3);
        }
    }
    return s;
}

static std::optional<bool> QuerySystemTouchpadInvertVertical() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad",
                      0,
                      KEY_READ,
                      &hKey) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    LONG res = RegQueryValueExW(hKey, L"ScrollDirection", nullptr, &type,
                                reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(hKey);
    if (res != ERROR_SUCCESS || type != REG_DWORD) return std::nullopt;
    if (value == 0 || value == 1) {
        // 1 = down motion scrolls up (natural); 0 = down motion scrolls down.
        return value == 1;
    }
    return std::nullopt;
}

static bool IsSyntacticallyValidJsonLite(const std::string& rawJson);

static bool LooksLikeWorkspaceJson(const std::string& rawJson) {
    std::string json = TrimAsciiWhitespace(StripUtf8Bom(rawJson));
    if (json.size() < 2) return false;
    if (json.front() != '{' || json.back() != '}') return false;
    if (!IsSyntacticallyValidJsonLite(json)) return false;
    // NOTE: shortcuts are no longer persisted in workspace.json.
    static const std::regex keyRe("\"(classesDir|cacheDir|showAnnots|pdfFlowMode|pdfBitmapBudgetMiB|pdfSinglePageMode|mouseWheelInvertVertical|mouseWheelInvertHorizontal|touchpadInvertVertical|touchpadInvertHorizontal|leftWidth|rightWidth|topHeight|leftPaneCollapsed|language|bottomPanePin|bottomNoteMode|notePlacement|colorTone|toneVariant|quickAnnotPopupPlacement|noteFontPt|noteFontName|noteRenderFontPt|noteRenderFontName|noteRenderJpFontName|noteWrapEnabled|noteVimCaretLineRawTextVisible|noteVimClickEntersInsertMode|ownerDrawUi|useNativeFileDialogs|developerMode|studentMode|exportStandardTextAnnots|sessionSortMode|sessionNumberingMode|sessionAutoOpenMode|sessionAutoOpenPairLinked|fullWidthParenCaretInside|fullWidthParenCancelNextLeft)\"\\s*:");
    return std::regex_search(json, keyRe);
}

static std::filesystem::path MakeDamagedSettingsPath(const std::filesystem::path& dir,
                                                     const std::filesystem::path& original) {
    std::filesystem::path base = original.filename();
    std::wstring baseName = L"escape_" + base.wstring();
    std::filesystem::path candidate = dir / baseName;
    std::error_code ec;
    if (!std::filesystem::exists(candidate, ec)) return candidate;
    for (int i = 1; i <= 999; ++i) {
        std::wstring alt = L"escape_" + std::to_wstring(i) + L"_" + base.wstring();
        candidate = dir / alt;
        if (!std::filesystem::exists(candidate, ec)) return candidate;
    }
    std::wstring fallback = L"escape_" + std::to_wstring(static_cast<unsigned long long>(GetTickCount64())) +
                            L"_" + base.wstring();
    return dir / fallback;
}

static std::filesystem::path QuarantineCorruptWorkspaceJson(const std::filesystem::path& root,
                                                            const std::filesystem::path& path) {
    if (root.empty() || path.empty()) return {};
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return {};
    std::filesystem::path dir = root / L"__resource__" / L"__escape__";
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};
    std::filesystem::path dest = MakeDamagedSettingsPath(dir, path);
    std::filesystem::rename(path, dest, ec);
    if (ec) {
        ec.clear();
        std::filesystem::copy_file(path, dest, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::filesystem::remove(path, ec);
        }
        if (ec) return {};
    }
    return dest;
}

static std::filesystem::path QuarantineCorruptSetupJson(const std::filesystem::path& exeDir,
                                                         const std::filesystem::path& setup) {
    if (exeDir.empty() || setup.empty()) return {};
    std::error_code ec;
    if (!std::filesystem::exists(setup, ec) || ec) return {};
    std::filesystem::path dir = exeDir / L"__resource__" / L"__escape__";
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};
    std::filesystem::path dest = MakeDamagedSettingsPath(dir, setup);
    std::filesystem::rename(setup, dest, ec);
    if (ec) {
        ec.clear();
        std::filesystem::copy_file(setup, dest, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::filesystem::remove(setup, ec);
        }
        if (ec) return {};
    }
    return dest;
}


static std::wstring WorkspaceConfigRootKeyForCompare(const std::filesystem::path& root) {
    if (root.empty()) return L"";
    std::error_code ec;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(root, ec);
    if (ec || normalized.empty()) {
        ec.clear();
        normalized = std::filesystem::absolute(root, ec);
        if (ec || normalized.empty()) normalized = root;
    }
    std::wstring key = normalized.lexically_normal().wstring();
    std::replace(key.begin(), key.end(), L'/', L'\\');
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return key;
}

static void BlockWorkspaceConfigAutoPersistForRoot(const std::filesystem::path& root) {
    g_workspaceConfigAutoPersistBlocked = true;
    g_workspaceConfigAutoPersistBlockedRootKey = WorkspaceConfigRootKeyForCompare(root);
}

static bool IsWorkspaceConfigAutoPersistBlockedForRoot(const std::filesystem::path& root) {
    if (!g_workspaceConfigAutoPersistBlocked) return false;
    return WorkspaceConfigRootKeyForCompare(root) == g_workspaceConfigAutoPersistBlockedRootKey;
}

static bool IsWorkspaceConfigKnownTopLevelField(const std::string& key) {
    static const std::set<std::string> known{
        "classesDir", "cacheDir", "showAnnots", "pdfFlowMode", "pdfBitmapBudgetMiB",
        "pdfSinglePageMode", "mouseWheelInvertVertical",
        "mouseWheelInvertHorizontal", "touchpadInvertVertical", "touchpadInvertHorizontal",
        "ownerDrawUi", "useNativeFileDialogs", "developerMode", "studentMode",
        "exportStandardTextAnnots", "debugLogPreviewTrace", "debugLogSwitchTiming",
        "debugLogCrash", "debugLogStartupWatchdog", "debugLogOfficeConversion",
        "leftWidth", "rightWidth", "topHeight",
        "windowWidth", "windowHeight", "defaultWindowWidth", "defaultWindowHeight",
        "leftSplit1", "leftSplit2", "defaultLeftWidth", "defaultRightWidth",
        "defaultTopHeight", "defaultLeftSplit1", "defaultLeftSplit2", "leftPaneCollapsed",
        "language", "markFontPx", "headingFontPx", "markColor", "headingColor",
        "headingBold", "headingUnderline", "headingLeftBar", "bottomPanePin", "bottomNoteMode", "notePlacement",
        "colorTone", "toneVariant", "quickAnnotPopupPlacement",
        "lectureSortMode", "sessionSortMode", "sessionNumberingMode", "sessionAutoOpenMode", "sessionAutoOpenPairLinked", "selectionStyle", "pointerOffsetX", "pointerOffsetY",
        "showMathList", "downKeyLastLineAction", "downKeyLastLineInsertNewline",
        "leftRightLineMoveAction", "autoPairBrackets", "fullWidthParenCaretInside", "fullWidthParenCancelNextLeft",
        "noteFontName", "noteFontPt", "noteRenderFontName", "noteRenderJpFontName",
        "noteRenderFontPt", "noteSystem", "noteRenderEnabled", "noteRawOnly", "noteRenderMath",
        "noteWrapEnabled", "noteVimModeEnabled", "noteVimCaretLineRawTextVisible",
        "noteVimClickEntersInsertMode", "noteMathMarginTopPercent",
        "noteMathSupSubGapSupPercent", "noteGridEnabled", "noteGridPitch", "noteBgColor",
        "noteFgColor", "noteBgSource", "noteBgThemeName", "noteShortcutBackColor",
        "noteShortcutTextColor", "noteShortcutTextTagKey", "noteShortcutHeadingArrowInvert",
        "noteCustomTagKey", "noteCustomTagBold", "noteCustomTagItalic", "noteCustomTagUnderline",
        "noteCustomTagStrike", "noteCustomTagBackColor", "noteCustomTagTextColor",
        "noteFontCustomization", "noteFontDigitTarget", "autoSaveSeconds", "autoIntegrateSeconds",
        "autoIntegrateCustomMinutes", "clroNamePattern", "textFontName", "textFontPt",
        "textFontUseA4Scale", "textFontActiveSizeSlot", "textFontPtSlotA", "textFontPtSlotB",
        "textFontUseA4ScaleSlotA", "textFontUseA4ScaleSlotB", "textBoxReadableBackground", "textBoxReadableBackgroundInverted",
        "lineToolsShareStyle", "lineWidthPt", "arrowWidthPt", "arrowHead", "waveWidthPt", "lineDashStyle", "freehandWidthPt",
        "markerFreeWidthPt", "markerTextWidthPt", "markerTextUnderline", "eraserWidthPt",
        "markerAlpha", "lineAlpha", "arrowAlpha", "waveAlpha", "freehandAlpha", "shapeAlpha",
        "textColor", "lineColor", "arrowColor", "waveColor", "freehandColor", "markerFreeColor",
        "markerTextColor", "shapeColor", "paletteCustomColor", "magnifierShape", "shapeDetail", "shapeKind",
        "shapeDrawMode", "annotLastMarkerDetail", "annotLastPenDetail",
        "annotLastShapePresentation", "annotLastShapeGeometry", "annotLastShapeDetail",
        // Accepted only for one-time migration from the earlier numeric representation.
        "annotLastMarkerMode", "annotLastPenMode", "annotLastShapeMode",
        "freehandCorrection", "freehandCorrectionStyle", "freehandCorrectionFill",
        "scheduleDayMask", "schedulePeriods"
    };
    if (known.find(key) != known.end()) return true;
    constexpr std::string_view prefix = "noteCustomTag";
    constexpr std::string_view suffix = "Mask";
    if (key.size() > prefix.size() + suffix.size() &&
        key.compare(0, prefix.size(), prefix) == 0 &&
        key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
        const std::string indexText = key.substr(prefix.size(), key.size() - prefix.size() - suffix.size());
        if (!indexText.empty() && std::all_of(indexText.begin(), indexText.end(), [](unsigned char ch) {
                return std::isdigit(ch) != 0;
            })) {
            const int index = std::atoi(indexText.c_str());
            return index >= 1 && index <= kNoteCustomTagPresetCount;
        }
    }
    return false;
}

static bool ParseJsonStringToken(const std::string& json, size_t* pos, std::string* out) {
    if (!pos || !out || *pos >= json.size() || json[*pos] != '"') return false;
    out->clear();
    ++(*pos);
    while (*pos < json.size()) {
        char ch = json[*pos];
        ++(*pos);
        if (ch == '"') return true;
        if (ch != '\\') {
            out->push_back(ch);
            continue;
        }
        if (*pos >= json.size()) return false;
        char esc = json[*pos];
        ++(*pos);
        if (esc == 'u') {
            if (json.size() - *pos < 4) return false;
            *pos += 4;
            out->push_back('?');
        } else {
            out->push_back(esc);
        }
    }
    return false;
}

static void SkipJsonWhitespace(const std::string& json, size_t* pos) {
    while (pos && *pos < json.size() && std::isspace(static_cast<unsigned char>(json[*pos]))) ++(*pos);
}

static bool ParseJsonValueLite(const std::string& json, size_t* pos, int depth);

static bool ParseJsonLiteralLite(const std::string& json, size_t* pos, const char* literal) {
    if (!pos || !literal) return false;
    const size_t len = std::char_traits<char>::length(literal);
    if (json.size() - *pos < len) return false;
    if (json.compare(*pos, len, literal) != 0) return false;
    *pos += len;
    return true;
}

static bool ParseJsonNumberLite(const std::string& json, size_t* pos) {
    if (!pos || *pos >= json.size()) return false;
    size_t i = *pos;
    if (json[i] == '-') ++i;
    if (i >= json.size()) return false;
    if (json[i] == '0') {
        ++i;
    } else if (std::isdigit(static_cast<unsigned char>(json[i]))) {
        while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) ++i;
    } else {
        return false;
    }
    if (i < json.size() && json[i] == '.') {
        ++i;
        if (i >= json.size() || !std::isdigit(static_cast<unsigned char>(json[i]))) return false;
        while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) ++i;
    }
    if (i < json.size() && (json[i] == 'e' || json[i] == 'E')) {
        ++i;
        if (i < json.size() && (json[i] == '+' || json[i] == '-')) ++i;
        if (i >= json.size() || !std::isdigit(static_cast<unsigned char>(json[i]))) return false;
        while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) ++i;
    }
    *pos = i;
    return true;
}

static bool ParseJsonArrayLite(const std::string& json, size_t* pos, int depth) {
    if (!pos || *pos >= json.size() || json[*pos] != '[') return false;
    ++(*pos);
    SkipJsonWhitespace(json, pos);
    if (*pos < json.size() && json[*pos] == ']') {
        ++(*pos);
        return true;
    }
    while (*pos < json.size()) {
        if (!ParseJsonValueLite(json, pos, depth + 1)) return false;
        SkipJsonWhitespace(json, pos);
        if (*pos >= json.size()) return false;
        if (json[*pos] == ',') {
            ++(*pos);
            SkipJsonWhitespace(json, pos);
            continue;
        }
        if (json[*pos] == ']') {
            ++(*pos);
            return true;
        }
        return false;
    }
    return false;
}

static bool ParseJsonObjectLite(const std::string& json, size_t* pos, int depth) {
    if (!pos || *pos >= json.size() || json[*pos] != '{') return false;
    ++(*pos);
    SkipJsonWhitespace(json, pos);
    if (*pos < json.size() && json[*pos] == '}') {
        ++(*pos);
        return true;
    }
    while (*pos < json.size()) {
        std::string key;
        if (!ParseJsonStringToken(json, pos, &key)) return false;
        SkipJsonWhitespace(json, pos);
        if (*pos >= json.size() || json[*pos] != ':') return false;
        ++(*pos);
        if (!ParseJsonValueLite(json, pos, depth + 1)) return false;
        SkipJsonWhitespace(json, pos);
        if (*pos >= json.size()) return false;
        if (json[*pos] == ',') {
            ++(*pos);
            SkipJsonWhitespace(json, pos);
            continue;
        }
        if (json[*pos] == '}') {
            ++(*pos);
            return true;
        }
        return false;
    }
    return false;
}

static bool ParseJsonValueLite(const std::string& json, size_t* pos, int depth) {
    if (!pos || depth > 64) return false;
    SkipJsonWhitespace(json, pos);
    if (*pos >= json.size()) return false;
    const char ch = json[*pos];
    if (ch == '{') return ParseJsonObjectLite(json, pos, depth + 1);
    if (ch == '[') return ParseJsonArrayLite(json, pos, depth + 1);
    if (ch == '"') {
        std::string ignored;
        return ParseJsonStringToken(json, pos, &ignored);
    }
    if (ch == 't') return ParseJsonLiteralLite(json, pos, "true");
    if (ch == 'f') return ParseJsonLiteralLite(json, pos, "false");
    if (ch == 'n') return ParseJsonLiteralLite(json, pos, "null");
    if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) {
        return ParseJsonNumberLite(json, pos);
    }
    return false;
}

static bool IsSyntacticallyValidJsonLite(const std::string& rawJson) {
    const std::string json = TrimAsciiWhitespace(StripUtf8Bom(rawJson));
    size_t pos = 0;
    if (!ParseJsonValueLite(json, &pos, 0)) return false;
    SkipJsonWhitespace(json, &pos);
    return pos == json.size();
}

static std::set<std::string> CollectTopLevelJsonObjectKeys(const std::string& rawJson) {
    std::set<std::string> keys;
    const std::string json = TrimAsciiWhitespace(StripUtf8Bom(rawJson));
    int depth = 0;
    for (size_t i = 0; i < json.size();) {
        const char ch = json[i];
        if (ch == '"') {
            const size_t tokenStart = i;
            std::string token;
            if (!ParseJsonStringToken(json, &i, &token)) return {};
            if (depth == 1) {
                size_t j = i;
                while (j < json.size() && std::isspace(static_cast<unsigned char>(json[j]))) ++j;
                if (j < json.size() && json[j] == ':') keys.insert(token);
            }
            if (i == tokenStart) ++i;
            continue;
        }
        if (ch == '{' || ch == '[') {
            ++depth;
        } else if (ch == '}' || ch == ']') {
            if (depth > 0) --depth;
        }
        ++i;
    }
    return keys;
}

static bool WorkspaceJsonHasUnknownTopLevelFields(const std::string& json,
                                                  std::vector<std::string>* outUnknown = nullptr) {
    if (outUnknown) outUnknown->clear();
    bool found = false;
    for (const auto& key : CollectTopLevelJsonObjectKeys(json)) {
        if (IsWorkspaceConfigKnownTopLevelField(key)) continue;
        found = true;
        if (outUnknown) outUnknown->push_back(key);
    }
    return found;
}

static bool SetupJsonHasUnknownTopLevelFields(const std::string& json,
                                               std::vector<std::string>* outUnknown = nullptr) {
    if (outUnknown) outUnknown->clear();
    bool found = false;
    for (const auto& key : CollectTopLevelJsonObjectKeys(json)) {
        if (setup_json_policy::IsKnownTopLevelField(key)) continue;
        found = true;
        if (outUnknown) outUnknown->push_back(key);
    }
    return found;
}

static std::wstring SetupJsonAutoUpdateBlockedReason(
    setup_json_policy::AutoUpdateDecision decision,
    const std::wstring& readErr = L"",
    const std::vector<std::string>& unknownFields = {}) {
    switch (decision) {
    case setup_json_policy::AutoUpdateDecision::Allow:
        return L"";
    case setup_json_policy::AutoUpdateDecision::BlockReadFailure:
        return readErr.empty() ? L"setup.json を読み込めません。"
                               : L"setup.json を読み込めません: " + readErr;
    case setup_json_policy::AutoUpdateDecision::BlockInvalidJson:
        return L"setup.json がJSONとして壊れているため、自動更新しません。";
    case setup_json_policy::AutoUpdateDecision::BlockMissingWorkspaceRoot:
        return L"setup.json に workspaceRoot が無いため、自動更新しません。";
    case setup_json_policy::AutoUpdateDecision::BlockUnknownTopLevelField: {
        std::wstring reason = L"setup.json にこのバージョンでは解釈できない項目があるため、自動更新しません。";
        const size_t limit = std::min<size_t>(unknownFields.size(), 8);
        for (size_t i = 0; i < limit; ++i) {
            reason += L"\n- " + UTF8ToWide(unknownFields[i]);
        }
        if (unknownFields.size() > limit) reason += L"\n- ...";
        return reason;
    }
    }
    return L"setup.json を安全に自動更新できません。";
}

static setup_json_policy::AutoUpdateDecision ResolveSetupJsonAutoUpdateDecision(
    bool readOk,
    const std::string& json,
    const std::optional<std::string>& workspaceRoot,
    std::vector<std::string>* outUnknownFields = nullptr) {
    if (outUnknownFields) outUnknownFields->clear();
    if (!readOk) {
        return setup_json_policy::AutoUpdateDecision::BlockReadFailure;
    }
    const bool validJson = IsSyntacticallyValidJsonLite(json);
    std::vector<std::string> unknownFields;
    const bool hasUnknown = validJson && SetupJsonHasUnknownTopLevelFields(json, &unknownFields);
    if (outUnknownFields) *outUnknownFields = unknownFields;
    return setup_json_policy::ResolveAutoUpdateDecision(
        readOk, validJson, workspaceRoot.has_value(), hasUnknown);
}

static bool ReadExistingSetupJsonForAutoUpdate(const std::filesystem::path& setup,
                                               std::string* outJson,
                                               std::wstring* outBlockedReason) {
    if (outJson) outJson->clear();
    if (outBlockedReason) outBlockedReason->clear();
    std::wstring readErr;
    std::string json;
    const bool readOk = ReadTextFileUtf8Limited(setup, kMaxSetupJsonBytes, &json, &readErr);
    const auto workspaceRoot = readOk ? ParseJsonStringField(json, "workspaceRoot")
                                      : std::optional<std::string>{};
    std::vector<std::string> unknownFields;
    const auto decision = ResolveSetupJsonAutoUpdateDecision(
        readOk, json, workspaceRoot, &unknownFields);
    if (!setup_json_policy::AllowsAutoUpdate(decision)) {
        if (outBlockedReason) {
            *outBlockedReason = SetupJsonAutoUpdateBlockedReason(decision, readErr, unknownFields);
        }
        return false;
    }
    if (outJson) *outJson = std::move(json);
    return true;
}


static std::optional<std::string> ParseJsonStringField(const std::string& json, const std::string& key) {
    std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch m;
    if (std::regex_search(json, m, re)) {
        return m[1].str();
    }
    return std::nullopt;
}

static std::vector<std::string> ParseJsonStringArrayField(const std::string& json, const std::string& key) {
    std::vector<std::string> items;
    std::regex re("\"" + key + "\"\\s*:\\s*\\[([^\\]]*)\\]");
    std::smatch m;
    if (!std::regex_search(json, m, re)) return items;
    std::string body = m[1].str();
    std::regex itemRe("\"([^\"]*)\"");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), itemRe);
         it != std::sregex_iterator(); ++it) {
        items.push_back((*it)[1].str());
    }
    return items;
}

static const char* ToolModeKey(ToolMode mode) {
    switch (mode) {
    case ToolMode::Select:     return "select";
    case ToolMode::Pan:        return "pan";
    case ToolMode::Magnifier:  return "magnifier";
    case ToolMode::MarkerText: return "marker_text";
    case ToolMode::MarkerTextUnderline: return "marker_text_underline";
    case ToolMode::MarkerTextColor: return "marker_text_color";
    case ToolMode::MarkerFree: return "marker_free";
    case ToolMode::MarkerLine: return "marker_line";
    case ToolMode::MarkerArrow:return "marker_arrow";
    case ToolMode::MarkerWave: return "marker_wave";
    case ToolMode::TextBox:    return "text";
    case ToolMode::Line:       return "line";
    case ToolMode::Arrow:      return "arrow";
    case ToolMode::Wave:       return "wave";
    case ToolMode::Freehand:   return "freehand";
    case ToolMode::Shape:      return "shape";
    case ToolMode::Eraser:     return "eraser";
    default:                   return "";
    }
}

static bool ToolModeFromKey(std::string key, ToolMode& out) {
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (key == "select") { out = ToolMode::Select; return true; }
    if (key == "pan") { out = ToolMode::Pan; return true; }
    if (key == "magnifier") { out = ToolMode::Magnifier; return true; }
    if (key == "marker_text" || key == "markertext") { out = ToolMode::MarkerText; return true; }
    if (key == "marker_text_underline" || key == "markertextunderline" ||
        key == "marker_underline" || key == "markerunderline") { out = ToolMode::MarkerTextUnderline; return true; }
    if (key == "marker_text_color" || key == "markertextcolor" ||
        key == "marker_color_text" || key == "markercolortext" ||
        key == "text_color_marker" || key == "textcolormarker") { out = ToolMode::MarkerTextColor; return true; }
    if (key == "marker_free" || key == "markerfree") { out = ToolMode::MarkerFree; return true; }
    if (key == "marker_line" || key == "markerline") { out = ToolMode::MarkerLine; return true; }
    if (key == "marker_arrow" || key == "markerarrow") { out = ToolMode::MarkerArrow; return true; }
    if (key == "marker_wave" || key == "markerwave") { out = ToolMode::MarkerWave; return true; }
    if (key == "text" || key == "textbox") { out = ToolMode::TextBox; return true; }
    if (key == "line") { out = ToolMode::Line; return true; }
    if (key == "arrow") { out = ToolMode::Arrow; return true; }
    if (key == "wave") { out = ToolMode::Wave; return true; }
    if (key == "freehand") { out = ToolMode::Freehand; return true; }
    if (key == "shape") { out = ToolMode::Shape; return true; }
    if (key == "eraser") { out = ToolMode::Eraser; return true; }
    return false;
}

const char* AnnotToolModeKey(ToolMode mode) {
    return ToolModeKey(mode);
}

bool AnnotToolModeFromKey(const std::string& key, ToolMode& out) {
    return ToolModeFromKey(key, out);
}

static bool ParseShortcutKeyToken(const std::string& token, UINT& outVk) {
    outVk = 0;
    if (token.size() == 1) {
        unsigned char c = static_cast<unsigned char>(token[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            outVk = static_cast<UINT>(c);
            return true;
        }
    }
    if (token.size() >= 2 && token[0] == 'F') {
        bool digits = true;
        for (size_t i = 1; i < token.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(token[i]))) {
                digits = false;
                break;
            }
        }
        if (digits) {
            int n = std::atoi(token.c_str() + 1);
            if (n >= 1 && n <= 24) {
                outVk = static_cast<UINT>(VK_F1 + (n - 1));
                return true;
            }
        }
    }
    if (token == "SPACE") { outVk = VK_SPACE; return true; }
    if (token == "MINUS" || token == "-") { outVk = VK_OEM_MINUS; return true; }
    if (token == "EQUAL" || token == "=") { outVk = VK_OEM_PLUS; return true; }
    if (token == "COMMA" || token == ",") { outVk = VK_OEM_COMMA; return true; }
    if (token == "PERIOD" || token == ".") { outVk = VK_OEM_PERIOD; return true; }
    if (token == "SLASH" || token == "/") { outVk = VK_OEM_2; return true; }
    if (token == "BACKSLASH" || token == "\\") { outVk = VK_OEM_5; return true; }
    if (token == "SEMICOLON" || token == ";") { outVk = VK_OEM_1; return true; }
    if (token == "QUOTE" || token == "'") { outVk = VK_OEM_7; return true; }
    if (token == "BACKQUOTE" || token == "`") { outVk = VK_OEM_3; return true; }
    if (token == "LBRACKET" || token == "BRACKETLEFT" || token == "[") { outVk = VK_OEM_4; return true; }
    if (token == "RBRACKET" || token == "BRACKETRIGHT" || token == "]") { outVk = VK_OEM_6; return true; }
    if (token == "UP" || token == "ARROWUP") { outVk = VK_UP; return true; }
    if (token == "DOWN" || token == "ARROWDOWN") { outVk = VK_DOWN; return true; }
    if (token == "LEFT" || token == "ARROWLEFT") { outVk = VK_LEFT; return true; }
    if (token == "RIGHT" || token == "ARROWRIGHT") { outVk = VK_RIGHT; return true; }
    if ((token.size() == 7 && token.rfind("NUMPAD", 0) == 0) ||
        (token.size() == 6 && token.rfind("NPAD", 0) == 0)) {
        const char digit = token.back();
        if (digit >= '0' && digit <= '9') {
            outVk = static_cast<UINT>(VK_NUMPAD0 + (digit - '0'));
            return true;
        }
    }
    return false;
}

static bool ShortcutKeyAllowsNoModifier(UINT vk) {
    return vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9;
}

static bool ParseAnnotToolShortcutKey(const std::string& raw, AnnotToolShortcutBinding& out) {
    out = {};
    std::string compact;
    compact.reserve(raw.size());
    for (unsigned char c : raw) {
        if (!std::isspace(c)) compact.push_back(static_cast<char>(std::toupper(c)));
    }
    if (compact.empty()) return false;

    std::string keyToken;
    size_t start = 0;
    while (start <= compact.size()) {
        size_t end = compact.find('+', start);
        std::string token = compact.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (token.empty()) return false;
        if (token == "CTRL" || token == "CONTROL") {
            out.ctrl = true;
        } else if (token == "ALT" || token == "MENU") {
            out.alt = true;
        } else if (token == "SHIFT") {
            out.shift = true;
        } else {
            if (!keyToken.empty()) return false;
            keyToken = token;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (keyToken.empty()) return false;
    if (!ParseShortcutKeyToken(keyToken, out.vk) || out.vk == 0) return false;
    if (!out.ctrl && !out.alt && !ShortcutKeyAllowsNoModifier(out.vk)) return false;
    out.key = UTF8ToWide(raw);
    return true;
}

bool ParseAnnotToolShortcutKeyForUser(const std::wstring& text, AnnotToolShortcutBinding& out) {
    return ParseAnnotToolShortcutKey(WideToUTF8(text), out);
}

bool IsFixedAnnotToolNavigationShortcut(const AnnotToolShortcutBinding& binding) {
    if (!binding.ctrl || !binding.alt || binding.shift) return false;
    return binding.vk == VK_LEFT || binding.vk == VK_RIGHT ||
           binding.vk == VK_UP || binding.vk == VK_DOWN;
}

static std::vector<AnnotToolShortcutBinding> DefaultAnnotToolShortcuts() {
    struct DefaultBinding {
        const char* key;
        AnnotToolShortcutTargetKind kind;
        AnnotToolFamily family;
        ToolMode mode;
    };
    static const DefaultBinding defaults[] = {
        { "Ctrl+Alt+1", AnnotToolShortcutTargetKind::Category, AnnotToolFamily::Select, ToolMode::Select },
        { "Ctrl+Alt+2", AnnotToolShortcutTargetKind::Category, AnnotToolFamily::Pan, ToolMode::Pan },
        { "Ctrl+Alt+3", AnnotToolShortcutTargetKind::Category, AnnotToolFamily::Magnifier, ToolMode::Magnifier },
        { "Ctrl+Alt+4", AnnotToolShortcutTargetKind::Category, AnnotToolFamily::Text, ToolMode::TextBox },
        { "Ctrl+Alt+5", AnnotToolShortcutTargetKind::Category, AnnotToolFamily::Marker, ToolMode::MarkerText },
        { "Ctrl+Alt+6", AnnotToolShortcutTargetKind::Category, AnnotToolFamily::Pen, ToolMode::Freehand },
        { "Ctrl+Alt+7", AnnotToolShortcutTargetKind::Category, AnnotToolFamily::Shape, ToolMode::Shape },
        { "Ctrl+Alt+8", AnnotToolShortcutTargetKind::Category, AnnotToolFamily::Eraser, ToolMode::Eraser },
        { "Ctrl+Alt+9", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Marker, ToolMode::MarkerText },
        { "Ctrl+Alt+0", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Pen, ToolMode::Freehand },
        { "Numpad1", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Select, ToolMode::Select },
        { "Numpad2", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Pan, ToolMode::Pan },
        { "Numpad3", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Magnifier, ToolMode::Magnifier },
        { "Numpad4", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Text, ToolMode::TextBox },
        { "Numpad5", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Marker, ToolMode::MarkerText },
        { "Numpad6", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Marker, ToolMode::MarkerFree },
        { "Numpad7", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Pen, ToolMode::Freehand },
        { "Numpad8", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Shape, ToolMode::Line },
        { "Numpad9", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Shape, ToolMode::Shape },
        { "Numpad0", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Eraser, ToolMode::Eraser },
        { "Ctrl+Alt+U", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Marker, ToolMode::MarkerTextUnderline },
        { "Ctrl+Alt+C", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Marker, ToolMode::MarkerTextColor },
        { "Ctrl+Alt+A", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Shape, ToolMode::Arrow },
        { "Ctrl+Alt+W", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Shape, ToolMode::Wave },
    };

    std::vector<AnnotToolShortcutBinding> out;
    for (const auto& d : defaults) {
        AnnotToolShortcutBinding binding;
        if (!ParseAnnotToolShortcutKey(d.key, binding)) continue;
        binding.targetKind = d.kind;
        binding.family = d.family;
        binding.mode = d.mode;
        out.push_back(std::move(binding));
    }
    return out;
}

static std::filesystem::path UserToolShortcutsFilePath() {
    if (g_workspaceRoot.empty()) return {};
    return std::filesystem::path(g_workspaceRoot) / L"__resource__" / L"__settings__" / L"tool_shortcuts.json";
}

static std::string BuildUserToolShortcutsJson(const std::vector<AnnotToolShortcutBinding>& bindings) {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"version\": 1,\n";
    oss << "  \"shortcuts\": [\n";
    bool first = true;
    for (const auto& binding : bindings) {
        const char* target = binding.targetKind == AnnotToolShortcutTargetKind::Category
            ? AnnotToolFamilyKey(binding.family)
            : ToolModeKey(binding.mode);
        if (!target || !*target || binding.key.empty()) continue;
        if (!first) oss << ",\n";
        first = false;
        if (binding.targetKind == AnnotToolShortcutTargetKind::Category) {
            oss << "    { \"key\": \"" << WideToUTF8(binding.key) << "\", \"category\": \"" << target << "\" }";
        } else {
            oss << "    { \"key\": \"" << WideToUTF8(binding.key) << "\", \"tool\": \"" << target << "\" }";
        }
    }
    oss << "\n";
    oss << "  ]\n";
    oss << "}\n";
    return oss.str();
}

static void SaveDefaultUserToolShortcutsIfMissing(const std::vector<AnnotToolShortcutBinding>& defaults) {
    auto path = UserToolShortcutsFilePath();
    if (path.empty()) return;
    std::error_code ec;
    if (std::filesystem::exists(path, ec) || ec) return;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return;
    std::wstring err;
    AtomicWriteUtf8WithWorkspaceDirs(path, BuildUserToolShortcutsJson(defaults),
                                     std::filesystem::path(g_workspaceRoot), &err);
}

static bool LoadUserToolShortcuts(std::vector<AnnotToolShortcutBinding>& out) {
    out.clear();
    auto path = UserToolShortcutsFilePath();
    if (path.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return false;

    std::string json;
    std::wstring readErr;
    if (!ReadTextFileUtf8Limited(path, kMaxUserToolShortcutBytes, &json, &readErr) || json.empty()) {
        return false;
    }

    std::set<UINT> chordSeen;
    std::regex objRe("\\{([^\\}]*)\\}");
    std::regex keyRe("\"key\"\\s*:\\s*\"([^\"]+)\"");
    std::regex toolRe("\"tool\"\\s*:\\s*\"([^\"]+)\"");
    std::regex categoryRe("\"category\"\\s*:\\s*\"([^\"]+)\"");
    for (auto it = std::sregex_iterator(json.begin(), json.end(), objRe);
         it != std::sregex_iterator(); ++it) {
        std::string body = (*it)[1].str();
        std::smatch keyMatch;
        std::smatch toolMatch;
        std::smatch categoryMatch;
        const bool hasKey = std::regex_search(body, keyMatch, keyRe);
        const bool hasTool = std::regex_search(body, toolMatch, toolRe);
        const bool hasCategory = std::regex_search(body, categoryMatch, categoryRe);
        // A binding must name exactly one target. Prefer neither target to an
        // ambiguous tool/category combination so an existing user file is
        // never silently reinterpreted.
        if (!hasKey || hasTool == hasCategory) {
            continue;
        }
        AnnotToolShortcutBinding binding;
        if (!ParseAnnotToolShortcutKey(keyMatch[1].str(), binding)) continue;
        if (IsFixedAnnotToolNavigationShortcut(binding)) continue;
        if (!toolMatch.empty()) {
            ToolMode mode{};
            if (!ToolModeFromKey(toolMatch[1].str(), mode)) continue;
            binding.targetKind = AnnotToolShortcutTargetKind::Detail;
            binding.mode = mode;
            binding.family = AnnotToolFamilyForMode(mode);
        } else {
            AnnotToolFamily family{};
            if (!AnnotToolFamilyFromKey(categoryMatch[1].str(), family)) continue;
            binding.targetKind = AnnotToolShortcutTargetKind::Category;
            binding.family = family;
            binding.mode = ToolMode::Select;
        }
        UINT chordKey = binding.vk |
            (binding.ctrl ? 0x010000u : 0u) |
            (binding.alt ? 0x020000u : 0u) |
            (binding.shift ? 0x040000u : 0u);
        if (chordSeen.count(chordKey)) continue;
        chordSeen.insert(chordKey);
        out.push_back(std::move(binding));
    }
    return !out.empty();
}

const std::vector<AnnotToolShortcutBinding>& AnnotToolShortcutBindings() {
    return g_annotToolShortcuts;
}

bool ResolveAnnotToolShortcut(const MSG& msg, AnnotToolShortcutBinding* outBinding) {
    if (outBinding) *outBinding = AnnotToolShortcutBinding{};
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
    const UINT vk = static_cast<UINT>(msg.wParam);
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    for (const auto& binding : g_annotToolShortcuts) {
        if (binding.vk != vk) continue;
        if (binding.ctrl != ctrl || binding.alt != alt || binding.shift != shift) continue;
        if (outBinding) *outBinding = binding;
        return true;
    }
    return false;
}

void SyncUserToolShortcutsToRuntime() {
    std::vector<AnnotToolShortcutBinding> defaults = DefaultAnnotToolShortcuts();
    SaveDefaultUserToolShortcutsIfMissing(defaults);
    std::vector<AnnotToolShortcutBinding> loaded;
    if (LoadUserToolShortcuts(loaded)) {
        std::set<UINT> chordSeen;
        for (const auto& binding : loaded) {
            const UINT chordKey = binding.vk |
                (binding.ctrl ? 0x010000u : 0u) |
                (binding.alt ? 0x020000u : 0u) |
                (binding.shift ? 0x040000u : 0u);
            chordSeen.insert(chordKey);
        }
        for (const auto& binding : defaults) {
            if (binding.ctrl || binding.alt || binding.shift || !ShortcutKeyAllowsNoModifier(binding.vk)) {
                continue;
            }
            const UINT chordKey = binding.vk;
            if (chordSeen.count(chordKey)) continue;
            loaded.push_back(binding);
            chordSeen.insert(chordKey);
        }
        g_annotToolShortcuts = std::move(loaded);
    } else {
        g_annotToolShortcuts = std::move(defaults);
    }
}

static const char* AnnotToolUiStateKey(AnnotToolUiState s) {
    switch (s) {
    case AnnotToolUiState::Enabled:  return "enabled";
    case AnnotToolUiState::Disabled: return "disabled";
    case AnnotToolUiState::Hidden:   return "hidden";
    default:                         return "enabled";
    }
}

static bool AnnotToolUiStateFromKey(std::string key, AnnotToolUiState& out) {
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (key == "enabled" || key == "on" || key == "show") { out = AnnotToolUiState::Enabled; return true; }
    if (key == "disabled" || key == "off" || key == "gray" || key == "grey") { out = AnnotToolUiState::Disabled; return true; }
    if (key == "hidden" || key == "hide") { out = AnnotToolUiState::Hidden; return true; }
    return false;
}

void LoadAnnotToolUiConfigFromSetupJson(const std::string& setupJson) {
    std::vector<ToolMode> expandedOrder;
    auto modeOrderKeys = ParseJsonStringArrayField(setupJson, "annotToolModeOrder");
    for (const auto& k : modeOrderKeys) {
        ToolMode mode{};
        if (ToolModeFromKey(k, mode)) {
            expandedOrder.push_back(mode);
        }
    }

    std::array<AnnotToolUiState, kToolModeCount> expandedStates = DefaultAnnotToolModeUiStates();
    auto modeStateEntries = ParseJsonStringArrayField(setupJson, "annotToolModeState");
    for (const auto& entry : modeStateEntries) {
        size_t sep = entry.find(':');
        if (sep == std::string::npos) sep = entry.find('=');
        if (sep == std::string::npos) continue;
        std::string k = entry.substr(0, sep);
        std::string v = entry.substr(sep + 1);
        ToolMode mode{};
        AnnotToolUiState s{};
        if (!ToolModeFromKey(k, mode)) continue;
        if (!AnnotToolUiStateFromKey(v, s)) continue;
        int idx = static_cast<int>(mode);
        if (idx < 0 || idx >= kToolModeCount) continue;
        expandedStates[static_cast<size_t>(idx)] = s;
    }

    std::vector<AnnotToolUiState> expandedStateVec;
    expandedStateVec.reserve(kToolModeCount);
    for (int i = 0; i < kToolModeCount; ++i) {
        expandedStateVec.push_back(expandedStates[static_cast<size_t>(i)]);
    }

    ApplyAnnotToolModeUiConfig(expandedOrder, expandedStateVec, false);
}

constexpr int kScheduleMaxDays = 7;
constexpr int kScheduleMaxPeriods = 13;
constexpr size_t kScheduleStartTimesMax = static_cast<size_t>(kScheduleMaxDays * kScheduleMaxPeriods);

static void EnsureScheduleStartTimesSize(WorkspaceConfig& cfg) {
    if (cfg.scheduleStartTimes.size() < kScheduleStartTimesMax) {
        cfg.scheduleStartTimes.resize(kScheduleStartTimesMax);
    } else if (cfg.scheduleStartTimes.size() > kScheduleStartTimesMax) {
        cfg.scheduleStartTimes.resize(kScheduleStartTimesMax);
    }
}

static std::filesystem::path ScheduleSettingsPath(const std::filesystem::path& root) {
    return root / L"__resource__" / L"__settings__" / L"schedule.json";
}

static bool LoadScheduleStartTimes(const std::filesystem::path& root, WorkspaceConfig& cfg) {
    if (root.empty()) return false;
    std::filesystem::path path = ScheduleSettingsPath(root);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return false;
    std::string json = ReadTextFileUtf8(path);
    if (json.empty()) return false;
    bool loaded = false;
    auto times = ParseJsonStringArrayField(json, "scheduleStartTimes");
    if (!times.empty()) {
        cfg.scheduleStartTimes.clear();
        cfg.scheduleStartTimes.reserve(times.size());
        for (const auto& t : times) {
            cfg.scheduleStartTimes.push_back(UTF8ToWide(t));
        }
        loaded = true;
    }
    auto cells = ParseJsonStringArrayField(json, "scheduleCells");
    if (!cells.empty()) {
        cfg.scheduleCells.clear();
        cfg.scheduleCells.reserve(cells.size());
        for (const auto& cell : cells) {
            cfg.scheduleCells.push_back(UTF8ToWide(cell));
        }
        loaded = true;
    }
    size_t scheduleCount = static_cast<size_t>(
        std::max(1, std::clamp(cfg.schedulePeriods, 1, kScheduleMaxPeriods) * kScheduleMaxDays));
    if (cfg.scheduleCells.size() < scheduleCount) {
        cfg.scheduleCells.resize(scheduleCount);
    } else if (cfg.scheduleCells.size() > scheduleCount) {
        cfg.scheduleCells.resize(scheduleCount);
    }
    EnsureScheduleStartTimesSize(cfg);
    return loaded;
}

static void SaveScheduleStartTimes(const std::filesystem::path& root, const WorkspaceConfig& cfg) {
    if (root.empty()) return;
    std::filesystem::path path = ScheduleSettingsPath(root);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return;
    size_t scheduleCount = static_cast<size_t>(
        std::max(1, std::clamp(cfg.schedulePeriods, 1, kScheduleMaxPeriods) * kScheduleMaxDays));
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"scheduleStartTimes\": [\n";
    for (size_t i = 0; i < kScheduleStartTimesMax; ++i) {
        const std::wstring& t = (i < cfg.scheduleStartTimes.size()) ? cfg.scheduleStartTimes[i] : L"";
        oss << "    \"" << WideToUTF8(t) << "\"";
        if (i + 1 < kScheduleStartTimesMax) oss << ",";
        oss << "\n";
    }
    oss << "  ],\n";
    oss << "  \"scheduleCells\": [\n";
    for (size_t i = 0; i < scheduleCount; ++i) {
        const std::wstring& cell = (i < cfg.scheduleCells.size()) ? cfg.scheduleCells[i] : L"";
        oss << "    \"" << WideToUTF8(cell) << "\"";
        if (i + 1 < scheduleCount) oss << ",";
        oss << "\n";
    }
    oss << "  ]\n";
    oss << "}\n";
    std::string data = oss.str();
    std::wstring err;
    AtomicWriteUtf8WithWorkspaceDirs(path, data, root, &err);
}

static std::optional<int> ParseJsonIntField(const std::string& json, const std::string& key) {
    std::regex re("\"" + key + "\"\\s*:\\s*(-?\\d+)");
    std::smatch m;
    if (std::regex_search(json, m, re)) {
        try {
            return std::stoi(m[1].str());
        } catch (...) {}
    }
    return std::nullopt;
}

static std::optional<int> ParseJsonColor(const std::string& json, const std::string& key) {
    auto s = ParseJsonStringField(json, key);
    if (!s) return std::nullopt;
    std::string val = *s;
    if (!val.empty() && val[0] == '#') val = val.substr(1);
    if (val.size() != 6) return std::nullopt;
    try {
        return std::stoi(val, nullptr, 16);
    } catch (...) {
        return std::nullopt;
    }
}

static std::optional<double> ParseJsonDoubleField(const std::string& json, const std::string& key) {
    std::regex re("\"" + key + "\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
    std::smatch m;
    if (std::regex_search(json, m, re)) {
        try {
            return std::stod(m[1].str());
        } catch (...) {}
    }
    return std::nullopt;
}

static std::optional<bool> ParseJsonBoolField(const std::string& json, const std::string& key) {
    std::regex re("\"" + key + "\"\\s*:\\s*(true|false)");
    std::smatch m;
    if (std::regex_search(json, m, re)) {
        return m[1].str() == "true";
    }
    return std::nullopt;
}

static std::optional<COLORREF> ParseJsonColorField(const std::string& json, const std::string& key) {
    auto s = ParseJsonStringField(json, key);
    if (!s) return std::nullopt;
    std::string hex = *s;
    if (hex.size() == 7 && hex[0] == '#') hex = hex.substr(1);
    if (hex.size() != 6) return std::nullopt;
    unsigned int v = 0;
    std::stringstream ss;
    ss << std::hex << hex;
    ss >> v;
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

static std::optional<std::uint64_t> ParseJsonU64Field(const std::string& json, const std::string& key) {
    std::regex re("\"" + key + "\"\\s*:\\s*(\\d+)");
    std::smatch m;
    if (std::regex_search(json, m, re)) {
        try {
            return static_cast<std::uint64_t>(std::stoull(m[1].str()));
        } catch (...) {}
    }
    return std::nullopt;
}

static std::optional<std::int64_t> ParseJsonI64Field(const std::string& json, const std::string& key) {
    std::regex re("\"" + key + "\"\\s*:\\s*(-?\\d+)");
    std::smatch m;
    if (std::regex_search(json, m, re)) {
        try {
            return static_cast<std::int64_t>(std::stoll(m[1].str()));
        } catch (...) {}
    }
    return std::nullopt;
}

static std::vector<VerifiedThemeMeta> ParseVerifiedThemes(const std::string& json) {
    std::vector<VerifiedThemeMeta> out;
    std::regex objRe("\\{[^\\{\\}]*\"file\"\\s*:\\s*\"([^\"]+)\"[^\\{\\}]*\\}");
    for (auto it = std::sregex_iterator(json.begin(), json.end(), objRe);
         it != std::sregex_iterator(); ++it) {
        std::string block = it->str();
        auto file = ParseJsonStringField(block, "file");
        if (!file) continue;
        VerifiedThemeMeta v;
        v.file = UTF8ToWide(*file);
        if (!IsSafeThemeFileId(v.file)) continue;
        if (auto sz = ParseJsonU64Field(block, "size")) v.size = *sz;
        if (auto mt = ParseJsonI64Field(block, "mtime_ms")) v.mtimeMs = *mt;
        if (auto sha = ParseJsonStringField(block, "sha256")) v.sha256 = *sha;
        if (auto disp = ParseJsonStringField(block, "display")) v.displayName = UTF8ToWide(*disp);
        if (auto dispJp = ParseJsonStringField(block, "display_jp")) v.displayNameJp = UTF8ToWide(*dispJp);
        if (auto c = ParseJsonColorField(block, "accent")) v.accent = *c;
        if (auto c = ParseJsonColorField(block, "noteBg")) v.noteBg = *c;
        out.push_back(std::move(v));
    }
    return out;
}

static std::string ColorToHex(COLORREF c) {
    char buf[8]{};
    std::snprintf(buf, sizeof(buf), "%06X", (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c));
    return std::string("#") + buf;
}

static bool ParseThemeBlock(const std::string& json, ThemeColors& out) {
    auto name = ParseJsonStringField(json, "name");
    if (!name) return false;
    out.name = UTF8ToWide(*name);
    if (auto nameJp = ParseJsonStringField(json, "name_jp")) out.nameJp = UTF8ToWide(*nameJp);
    if (out.nameJp.empty()) out.nameJp = out.name;
    if (auto c = ParseJsonColorField(json, "windowBg")) out.windowBg = *c;
    if (auto c = ParseJsonColorField(json, "windowText")) out.windowText = *c;
    if (auto c = ParseJsonColorField(json, "panelBg")) out.panelBg = *c;
    if (auto c = ParseJsonColorField(json, "panelText")) out.panelText = *c;
    if (auto c = ParseJsonColorField(json, "menuBg")) out.menuBg = *c;
    if (auto c = ParseJsonColorField(json, "menuText")) out.menuText = *c;
    if (auto c = ParseJsonColorField(json, "menuSelBg")) out.menuSelBg = *c;
    if (auto c = ParseJsonColorField(json, "menuSelText")) out.menuSelText = *c;
    if (auto c = ParseJsonColorField(json, "toolbarBg")) out.toolbarBg = *c;
    if (auto c = ParseJsonColorField(json, "toolbarText")) out.toolbarText = *c;
    if (auto c = ParseJsonColorField(json, "buttonBg")) out.buttonBg = *c;
    if (auto c = ParseJsonColorField(json, "buttonText")) out.buttonText = *c;
    if (auto c = ParseJsonColorField(json, "buttonBorder")) out.buttonBorder = *c;
    if (auto c = ParseJsonColorField(json, "buttonHot")) out.buttonHot = *c;
    if (auto c = ParseJsonColorField(json, "buttonPressed")) out.buttonPressed = *c;
    if (auto c = ParseJsonColorField(json, "splitterBg")) out.splitterBg = *c;
    if (auto c = ParseJsonColorField(json, "splitterLine")) out.splitterLine = *c;
    if (auto c = ParseJsonColorField(json, "pdfBg")) out.pdfBg = *c;
    if (auto c = ParseJsonColorField(json, "pdfPageBg")) out.pdfPageBg = *c;
    if (auto c = ParseJsonColorField(json, "noteBg")) out.noteBg = *c;
    if (auto c = ParseJsonColorField(json, "noteText")) out.noteText = *c;
    if (auto c = ParseJsonColorField(json, "selectionBg")) out.selectionBg = *c;
    if (auto c = ParseJsonColorField(json, "selectionText")) out.selectionText = *c;
    if (auto c = ParseJsonColorField(json, "accent")) out.accent = *c;
    return true;
}

bool LoadThemeConfig(const std::wstring& root) {
    g_themeCatalog = DefaultThemeCatalog();
    // Restore a non-file "default" tone entry (used to disable owner-draw UI).
    {
        const ThemeColors sysDefault = MakeSystemDefaultTone();
        bool hasDefault = false;
        for (const auto& t : g_themeCatalog) {
            if (t.name == sysDefault.name) { hasDefault = true; break; }
        }
        if (!hasDefault) {
            g_themeCatalog.insert(g_themeCatalog.begin(), sysDefault);
        }
    }
    // Ensure "default" exists and sort by the requested display order.
    {
        const ThemeColors sysDefault = MakeSystemDefaultTone();
        bool hasDefault = false;
        for (const auto& t : g_themeCatalog) {
            if (t.name == sysDefault.name) { hasDefault = true; break; }
        }
        if (!hasDefault) {
            g_themeCatalog.insert(g_themeCatalog.begin(), sysDefault);
        }
    }
    SortThemeCatalogByHex(g_themeCatalog);
    g_themeName = g_themeCatalog.empty() ? L"" : g_themeCatalog.front().name;
    g_themeCurrentFile.clear();
    g_themeVerified.clear();
    if (root.empty()) {
        g_theme = g_themeCatalog.front();
        UpdateNoteBgColorFromConfig();
        UpdateThemeBrushes();
        return false;
    }

    EnsureThemeManualFile(root);

    std::filesystem::path desired = ThemeConfigPath(root);
    std::filesystem::path path = desired;
    std::string json;
    {
        std::wstring readErr;
        ReadTextFileUtf8Limited(path, kMaxThemeConfigBytes, &json, &readErr);
    }
    if (!json.empty()) {
        if (auto current = ParseJsonStringField(json, "current_file")) {
            g_themeCurrentFile = UTF8ToWide(*current);
        }
        g_themeVerified = ParseVerifiedThemes(json);
    }

    // Validate verified metadata cheaply (existence + size/mtime). Do not parse theme contents here.
    {
        std::vector<VerifiedThemeMeta> kept;
        kept.reserve(g_themeVerified.size());
        for (const auto& v : g_themeVerified) {
            if (v.file.empty()) continue;
            auto p = ThemeFilePathFromId(root, v.file);
            std::uint64_t sz = 0;
            std::int64_t mt = 0;
            if (!TryGetFileMeta(p, &sz, &mt)) continue;
            if (v.size != 0 && v.mtimeMs != 0) {
                if (v.size != sz || v.mtimeMs != mt) continue;
            }
            VerifiedThemeMeta vv = v;
            vv.size = sz;
            vv.mtimeMs = mt;
            kept.push_back(std::move(vv));
        }
        g_themeVerified = std::move(kept);
    }

    // Ensure theme directory exists.
    {
        auto themeRoot = ThemeRootPath(root);
        std::error_code ec2;
        std::filesystem::create_directories(themeRoot, ec2);
    }

    // Enumerate theme files. Built-in themes are embedded; do not auto-generate theme_*.json.
    std::vector<std::wstring> themeFiles = ListThemeFileIds(root);

    // Build catalog entries for theme files without parsing theme contents (use cached meta if verified).
    // Built-in themes always stay in the catalog (embedded).
    if (!themeFiles.empty()) {
        ThemeColors base = DefaultThemeCatalog().empty() ? ThemeColors{} : DefaultThemeCatalog().front();
        std::vector<ThemeColors> out = g_themeCatalog;
        out.reserve(out.size() + themeFiles.size());
        for (const auto& id : themeFiles) {
            if (FindThemeByName(out, id)) continue;
            ThemeColors t = base;
            t.name = id;     // id = file name (theme_XXXXXX.json...)
            t.nameJp.clear();
            std::wstring hex6;
            if (TryParseThemeIdHexFromFileId(id, &hex6) && !hex6.empty()) {
                t.nameJp = hex6; // placeholder until verified cache fills
                unsigned int v = 0;
                if (TryParseSixHexFromW(hex6, &v)) {
                    t.accent = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
                }
            }
            if (auto* v = FindVerifiedMetaByFile(id)) {
                // Use cached display names/colors for menus; the real theme is parsed on demand.
                if (!v->displayNameJp.empty()) t.nameJp = v->displayNameJp;
                else if (!v->displayName.empty()) t.nameJp = v->displayName;
                t.noteBg = v->noteBg;
                t.accent = v->accent;
            }
            out.push_back(std::move(t));
        }
        g_themeCatalog = std::move(out);
    }
    SortThemeCatalogByHex(g_themeCatalog);

    // Decide current theme id.
    if (g_themeCurrentFile.empty() || !FindThemeByName(g_themeCatalog, g_themeCurrentFile)) {
        const std::wstring defId = DefaultThemeFileId();
        if (!defId.empty() && FindThemeByName(g_themeCatalog, defId)) {
            g_themeCurrentFile = defId;
        } else if (!g_themeCatalog.empty()) {
            g_themeCurrentFile = g_themeCatalog.front().name;
        }
    }

    g_themeName = g_themeCurrentFile;
    g_themeLastDisplayEn.clear();
    g_themeLastDisplayJp.clear();
    bool applied = ApplyThemeByName(g_themeName, nullptr, /*persist=*/false);
    if (!applied) {
        // Prefer default theme on startup failure.
        const std::wstring defId = DefaultThemeFileId();
        if (!defId.empty() && FindThemeByName(g_themeCatalog, defId)) {
            g_themeLastDisplayEn.clear();
            g_themeLastDisplayJp.clear();
            applied = ApplyThemeByName(defId, nullptr, /*persist=*/false);
            if (applied) {
                g_themeCurrentFile = defId;
                g_themeName = defId;
            }
        }

        // Otherwise fall back to the first theme that can be loaded.
        if (!applied) {
            for (const auto& t : g_themeCatalog) {
                g_themeLastDisplayEn.clear();
                g_themeLastDisplayJp.clear();
                if (ApplyThemeByName(t.name, nullptr, /*persist=*/false)) {
                    g_themeCurrentFile = t.name;
                    g_themeName = t.name;
                    applied = true;
                    break;
                }
            }
        }
    }

    bool needWrite = (path != desired) || json.empty() || !applied;
    if (applied) {
        // Treat startup apply as a "reference": update verified cache lightly (no sha256).
        if (UpdateVerifiedMetaLightweight(root, g_themeCurrentFile, g_theme)) {
            needWrite = true;
        }
    } else {
        // If even the fallback failed, keep current_file empty to avoid looping on a bad id.
        g_themeCurrentFile.clear();
        g_themeName.clear();
        needWrite = true;
    }

    if (needWrite) {
        WriteThemeConfig(desired, root, g_themeCurrentFile, g_themeCatalog);
    }
    return true;
}

bool PersistThemeSelection(const std::wstring& name) {
    if (g_themeCatalog.empty()) return false;
    if (!FindThemeByName(g_themeCatalog, name)) return false;
    g_themeName = name;
    g_themeCurrentFile = name;
    if (!g_workspaceRoot.empty()) {
        WriteThemeConfig(ThemeConfigPath(g_workspaceRoot), g_workspaceRoot, g_themeCurrentFile, g_themeCatalog);
    }
    return true;
}

static double ThemeRelativeLuminance(COLORREF c) {
    auto channel = [](int value) {
        const double s = static_cast<double>(value) / 255.0;
        return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(GetRValue(c)) +
           0.7152 * channel(GetGValue(c)) +
           0.0722 * channel(GetBValue(c));
}

bool ReplaceAnnotToolShortcutBindings(const std::vector<AnnotToolShortcutBinding>& bindings) {
    std::set<UINT> seen;
    std::vector<AnnotToolShortcutBinding> normalized;
    normalized.reserve(bindings.size());
    for (const auto& source : bindings) {
        AnnotToolShortcutBinding binding = source;
        if (binding.key.empty() || binding.vk == 0) return false;
        if (IsFixedAnnotToolNavigationShortcut(binding)) return false;
        if (binding.targetKind == AnnotToolShortcutTargetKind::Category) {
            if (!AnnotToolFamilyKey(binding.family) || !*AnnotToolFamilyKey(binding.family)) return false;
        } else {
            const char* key = ToolModeKey(binding.mode);
            if (!key || !*key || static_cast<int>(binding.mode) < 0 ||
                static_cast<int>(binding.mode) >= kToolModeCount) return false;
            binding.family = AnnotToolFamilyForMode(binding.mode);
        }
        UINT chord = binding.vk |
            (binding.ctrl ? 0x010000u : 0u) |
            (binding.alt ? 0x020000u : 0u) |
            (binding.shift ? 0x040000u : 0u);
        if (!seen.insert(chord).second) return false;
        normalized.push_back(std::move(binding));
    }
    if (normalized.empty()) return false;
    auto path = UserToolShortcutsFilePath();
    if (path.empty()) return false;
    std::wstring err;
    if (!AtomicWriteUtf8WithWorkspaceDirs(path, BuildUserToolShortcutsJson(normalized),
                                          std::filesystem::path(g_workspaceRoot), &err)) {
        return false;
    }
    g_annotToolShortcuts = std::move(normalized);
    return true;
}

const char* AnnotToolFamilyKey(AnnotToolFamily family) {
    switch (family) {
    case AnnotToolFamily::Select: return "select";
    case AnnotToolFamily::Pan: return "pan";
    case AnnotToolFamily::Magnifier: return "magnifier";
    case AnnotToolFamily::Text: return "text";
    case AnnotToolFamily::Marker: return "marker";
    case AnnotToolFamily::Pen: return "pen";
    case AnnotToolFamily::Shape: return "shape";
    case AnnotToolFamily::Eraser: return "eraser";
    default: return "";
    }
}

bool AnnotToolFamilyFromKey(const std::string& raw, AnnotToolFamily& out) {
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (key == "select") { out = AnnotToolFamily::Select; return true; }
    if (key == "pan") { out = AnnotToolFamily::Pan; return true; }
    if (key == "magnifier") { out = AnnotToolFamily::Magnifier; return true; }
    if (key == "text" || key == "textbox") { out = AnnotToolFamily::Text; return true; }
    if (key == "marker") { out = AnnotToolFamily::Marker; return true; }
    if (key == "pen" || key == "freehand") { out = AnnotToolFamily::Pen; return true; }
    if (key == "shape") { out = AnnotToolFamily::Shape; return true; }
    if (key == "eraser") { out = AnnotToolFamily::Eraser; return true; }
    return false;
}

static double ThemeContrastRatio(COLORREF a, COLORREF b) {
    const double la = ThemeRelativeLuminance(a);
    const double lb = ThemeRelativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

static COLORREF ThemeVariantLerp(COLORREF a, COLORREF b, double t) {
    int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
    int r = static_cast<int>(std::lround(ar + (br - ar) * t));
    int g = static_cast<int>(std::lround(ag + (bg - ag) * t));
    int b2 = static_cast<int>(std::lround(ab + (bb - ab) * t));
    return RGB(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b2, 0, 255));
}

static COLORREF ThemeVariantTextForBg(COLORREF bg) {
    const COLORREF dark = RGB(20, 20, 20);
    const COLORREF light = RGB(242, 242, 242);
    return ThemeContrastRatio(bg, light) >= ThemeContrastRatio(bg, dark) ? light : dark;
}

static std::wstring NormalizeToneVariantLocal(const std::wstring& value) {
    std::wstring v = value;
    std::transform(v.begin(), v.end(), v.begin(), ::towlower);
    if (v == L"pure") return L"pure";
    if (v == L"guard" || v == L"tone_guard" || v == L"toneguard") return L"guard";
    if (v == L"emphasis" || v == L"accent" || v == L"tone_emphasis" || v == L"toneemphasis") return L"emphasis";
    if (v == L"white") return L"white";
    if (v == L"black") return L"black";
    return L"pure";
}

static ThemeColors ApplyToneVariantToTheme(const ThemeColors& base, const std::wstring& toneVariantRaw) {
    std::wstring toneVariant = NormalizeToneVariantLocal(toneVariantRaw);
    if (toneVariant == L"pure") return base;

    ThemeColors t = base;
    const COLORREF white = RGB(255, 255, 255);
    const COLORREF black = RGB(18, 18, 18);
    COLORREF accent = base.accent;

    if (toneVariant == L"guard") {
        // Keep the theme's surfaces and accent, but guard text readability.
        t.windowText = ThemeVariantTextForBg(t.windowBg);
        t.panelText = ThemeVariantTextForBg(t.panelBg);
        t.menuText = ThemeVariantTextForBg(t.menuBg);
        t.menuSelText = ThemeVariantTextForBg(t.menuSelBg);
        t.toolbarText = ThemeVariantTextForBg(t.toolbarBg);
        t.buttonText = ThemeVariantTextForBg(t.buttonBg);
        t.noteText = ThemeVariantTextForBg(t.noteBg);
        t.selectionText = ThemeVariantTextForBg(t.selectionBg);
        return t;
    } else if (toneVariant == L"emphasis") {
        // Reuse the theme accent across interaction surfaces without recoloring the whole UI.
        t.toolbarBg = ThemeVariantLerp(t.toolbarBg, accent, 0.06);
        t.menuSelBg = ThemeVariantLerp(t.menuBg, accent, 0.30);
        t.buttonHot = ThemeVariantLerp(t.buttonBg, accent, 0.18);
        t.buttonPressed = ThemeVariantLerp(t.buttonBg, accent, 0.28);
        t.selectionBg = ThemeVariantLerp(t.panelBg, accent, 0.28);
        t.buttonBorder = ThemeVariantLerp(t.buttonBorder, accent, 0.34);
        t.splitterLine = ThemeVariantLerp(t.splitterLine, accent, 0.34);

        t.toolbarText = ThemeVariantTextForBg(t.toolbarBg);
        t.menuSelText = ThemeVariantTextForBg(t.menuSelBg);
        t.selectionText = ThemeVariantTextForBg(t.selectionBg);
        return t;
    } else if (toneVariant == L"white") {
        // Pull most surfaces toward white, keep accent for highlights.
        t.windowBg = ThemeVariantLerp(base.windowBg, white, 0.90);
        t.panelBg  = ThemeVariantLerp(base.panelBg,  white, 0.92);
        t.menuBg   = ThemeVariantLerp(base.menuBg,   white, 0.92);
        t.toolbarBg = ThemeVariantLerp(base.toolbarBg, white, 0.90);
        t.buttonBg  = ThemeVariantLerp(base.buttonBg,  white, 0.92);

        COLORREF accentLite = ThemeVariantLerp(white, accent, 0.18);
        COLORREF accentLite2 = ThemeVariantLerp(white, accent, 0.24);
        t.menuSelBg = ThemeVariantLerp(t.menuBg, accentLite2, 0.70);
        t.buttonHot = ThemeVariantLerp(t.buttonBg, accentLite, 0.60);
        t.buttonPressed = ThemeVariantLerp(t.buttonBg, accentLite2, 0.70);
        t.selectionBg = ThemeVariantLerp(white, accent, 0.22);

        t.splitterBg = ThemeVariantLerp(base.splitterBg, white, 0.90);
        t.pdfBg = base.pdfBg;
        t.pdfPageBg = white;
        t.noteBg = ThemeVariantLerp(base.noteBg, white, 0.95);

        // Borders/lines
        t.buttonBorder = ThemeVariantLerp(RGB(170, 170, 170), accent, 0.06);
        t.splitterLine = ThemeVariantLerp(RGB(175, 175, 175), accent, 0.06);
    } else if (toneVariant == L"black") {
        // Dark surfaces; slightly brighten accent for readability.
        accent = ThemeVariantLerp(accent, white, 0.25);
        t.accent = accent;

        t.windowBg = ThemeVariantLerp(black, accent, 0.05);
        t.panelBg  = ThemeVariantLerp(RGB(26, 26, 26), accent, 0.06);
        t.menuBg   = ThemeVariantLerp(RGB(22, 22, 22), accent, 0.06);
        t.toolbarBg = ThemeVariantLerp(RGB(24, 24, 24), accent, 0.06);
        t.buttonBg  = ThemeVariantLerp(RGB(36, 36, 36), accent, 0.06);

        t.menuSelBg = ThemeVariantLerp(t.menuBg, accent, 0.28);
        t.buttonHot = ThemeVariantLerp(t.buttonBg, accent, 0.18);
        t.buttonPressed = ThemeVariantLerp(t.buttonBg, accent, 0.26);
        t.selectionBg = ThemeVariantLerp(t.panelBg, accent, 0.30);

        t.splitterBg = ThemeVariantLerp(RGB(28, 28, 28), accent, 0.05);
        t.pdfBg = base.pdfBg;
        // Keep page background from base (PDF pages are typically white).
        t.pdfPageBg = base.pdfPageBg;
        t.noteBg = ThemeVariantLerp(RGB(28, 28, 28), accent, 0.04);

        t.buttonBorder = ThemeVariantLerp(t.buttonBg, white, 0.20);
        t.splitterLine = ThemeVariantLerp(t.splitterBg, white, 0.18);
    }

    // Text colors (ensure readability on the transformed backgrounds).
    t.windowText = ThemeVariantTextForBg(t.windowBg);
    t.panelText = ThemeVariantTextForBg(t.panelBg);
    t.menuText = ThemeVariantTextForBg(t.menuBg);
    t.menuSelText = ThemeVariantTextForBg(t.menuSelBg);
    t.toolbarText = ThemeVariantTextForBg(t.toolbarBg);
    t.buttonText = ThemeVariantTextForBg(t.buttonBg);
    t.noteText = ThemeVariantTextForBg(t.noteBg);
    t.selectionText = ThemeVariantTextForBg(t.selectionBg);
    return t;
}

static bool ApplyThemeByNameImpl(const std::wstring& name, HWND owner, bool persist, bool applyUi) {
    // File-based theme: name is a file id like theme_00AA7B.json
    if (!g_workspaceRoot.empty()) {
        auto path = ThemeFilePathFromId(g_workspaceRoot, name);
        if (!path.empty() && TryParseThemeAccentFromFileName(path, nullptr)) {
            ThemeColors parsed = DefaultThemeCatalog().empty() ? ThemeColors{} : DefaultThemeCatalog().front();
            std::wstring reason;
            if (!ReadThemeFile(path, DefaultThemeCatalog(), parsed, &reason)) {
                static std::wstring s_warnWs;
                static std::set<std::wstring> s_warnedFiles;
                if (s_warnWs != g_workspaceRoot) {
                    s_warnWs = g_workspaceRoot;
                    s_warnedFiles.clear();
                }
                if (!s_warnedFiles.count(name)) {
                    s_warnedFiles.insert(name);
                    std::wstring msg = IsEnglishUi() ? L"Theme file is invalid and was ignored:\n" : L"テーマファイルが不正のため無視しました:\n";
                    msg += path.wstring();
                    if (!reason.empty()) msg += L"\n\n" + reason;
                    ShowAppCoreMessageDialog(nullptr, IsEnglishUi() ? L"Themes" : L"テーマ",
                                             msg, SoftNoticeKind::Warning);
                }

                RemoveVerifiedMetaByFile(name);
                if (!g_workspaceRoot.empty()) {
                    WriteThemeConfig(ThemeConfigPath(g_workspaceRoot), g_workspaceRoot,
                                     g_themeCurrentFile.empty() ? g_themeName : g_themeCurrentFile, g_themeCatalog);
                }
                return false;
            }

            const ThemeColors baseTheme = parsed;
            ThemeColors appliedTheme = ApplyToneVariantToTheme(baseTheme, g_config.toneVariant);

            const std::wstring displayEn = baseTheme.name;
            const std::wstring displayJp = baseTheme.nameJp;
            g_themeLastDisplayEn = displayEn;
            g_themeLastDisplayJp = displayJp;
            // Keep stable id as ThemeColors.name (file id), and keep a display label in nameJp.
            appliedTheme.name = name;
            appliedTheme.nameJp = displayJp.empty() ? displayEn : displayJp;
            g_theme = appliedTheme;
            g_themeName = name;
            g_themeCurrentFile = name;
            g_config.ownerDrawUi = (g_themeName != L"default");
            UpdateNoteBgColorFromConfig();
            UpdateThemeBrushes();
            if (applyUi) {
                ApplyThemeToUI(owner);
            }

            // Update verified meta only when the user explicitly applies/persists (or when we need to evict bad entries).
            if (persist) {
                VerifiedThemeMeta v;
                v.file = name;
                v.displayName = displayEn;
                v.displayNameJp = displayJp;
                v.accent = baseTheme.accent;
                v.noteBg = baseTheme.noteBg;
                std::uint64_t sz = 0;
                std::int64_t mt = 0;
                if (TryGetFileMeta(path, &sz, &mt)) {
                    v.size = sz;
                    v.mtimeMs = mt;
                }
                const VerifiedThemeMeta* existing = FindVerifiedMetaByFile(name);
                bool needSha = true;
                if (existing && !existing->sha256.empty() && v.size > 0 && v.mtimeMs > 0) {
                    if (existing->size == v.size && existing->mtimeMs == v.mtimeMs) {
                        v.sha256 = existing->sha256;
                        needSha = false;
                    }
                }
                if (needSha) {
                    // Compute sha256 (small files; called only when needed).
                    clrop::PdfId id = clrop::ComputePdfId(path.wstring());
                    if (!id.sha256.empty()) v.sha256 = id.sha256;
                }

                if (auto* existing = FindVerifiedMetaByFile(name)) {
                    *existing = v;
                } else {
                    g_themeVerified.push_back(std::move(v));
                }
                WriteThemeConfig(ThemeConfigPath(g_workspaceRoot), g_workspaceRoot, g_themeCurrentFile, g_themeCatalog);
            }
            return true;
        }
    }

    // Fallback: in-memory catalog (non-file themes).
    const ThemeColors* picked = FindThemeByName(g_themeCatalog, name);
    if (!picked) {
        if (g_themeCatalog.empty()) return false;
        picked = &g_themeCatalog.front();
    }
    {
        const ThemeColors baseTheme = *picked;
        ThemeColors appliedTheme = baseTheme;
        if (baseTheme.name != L"default") {
            appliedTheme = ApplyToneVariantToTheme(baseTheme, g_config.toneVariant);
        }
        g_theme = appliedTheme;
    }
    g_themeName = name;
    g_config.ownerDrawUi = (g_themeName != L"default");
    g_themeCurrentFile = name;
    UpdateNoteBgColorFromConfig();
    UpdateThemeBrushes();
    if (persist && !g_workspaceRoot.empty()) {
        WriteThemeConfig(ThemeConfigPath(g_workspaceRoot), g_workspaceRoot, g_themeCurrentFile, g_themeCatalog);
    }
    if (applyUi) {
        ApplyThemeToUI(owner);
    }
    return true;
}

bool ApplyThemeByName(const std::wstring& name, HWND owner, bool persist) {
    return ApplyThemeByNameImpl(name, owner, persist, true);
}

bool ApplyThemeByNameForSettingsBatch(const std::wstring& name, HWND owner, bool persist) {
    return ApplyThemeByNameImpl(name, owner, persist, false);
}

static std::wstring SanitizeThemeId(const std::wstring& raw) {
    std::wstring s = TrimWhitespace(raw);
    std::wstring out;
    out.reserve(s.size());
    bool lastWasUnderscore = false;
    for (wchar_t ch : s) {
        wchar_t c = ch;
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        bool ok = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_';
        if (!ok) {
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') {
                if (!out.empty() && !lastWasUnderscore) {
                    out.push_back(L'_');
                    lastWasUnderscore = true;
                }
            }
            continue;
        }
        out.push_back(c);
        lastWasUnderscore = (c == L'_');
        if (out.size() >= 48) break;
    }
    while (!out.empty() && out.back() == L'_') out.pop_back();
    if (out.empty()) out = L"theme";
    return out;
}

bool CreateThemeFromCurrent(HWND owner,
                            const std::wstring& displayName,
                            std::wstring* outThemeId,
                            std::wstring* err) {
    if (outThemeId) outThemeId->clear();
    if (err) err->clear();

    if (g_workspaceRoot.empty()) {
        if (err) *err = IsEnglishUi() ? L"No workspace is open." : L"ワークスペースが開かれていません。";
        return false;
    }
    std::wstring label = TrimWhitespace(displayName);
    if (label.empty()) {
        if (err) *err = IsEnglishUi() ? L"Theme name is empty." : L"テーマ名が空です。";
        return false;
    }

    std::wstring baseId = SanitizeThemeId(label);
    std::wstring cand = baseId;
    if (cand.size() > 44) cand.resize(44);

    int idx = 2;
    while (FindThemeByName(g_themeCatalog, cand)) {
        cand = baseId;
        if (cand.size() > 40) cand.resize(40);
        cand += L"_" + std::to_wstring(idx++);
        if (idx > 9999) {
            if (err) *err = IsEnglishUi() ? L"Failed to allocate a unique theme id." : L"テーマIDの採番に失敗しました。";
            return false;
        }
    }

    ThemeColors t = g_theme;
    t.name = cand;
    t.nameJp = label;

    std::wstring fileId;
    if (kThemeUseThemeFiles && !g_workspaceRoot.empty()) {
        if (!WriteThemeFile(g_workspaceRoot, t, &fileId) || fileId.empty()) {
            if (err) *err = IsEnglishUi() ? L"Failed to write theme file." : L"テーマファイルの保存に失敗しました。";
            return false;
        }
    } else {
        if (err) *err = IsEnglishUi() ? L"Theme files are disabled." : L"テーマファイル機能が無効です。";
        return false;
    }

    // Add a placeholder catalog entry (display name comes from the file content; id is the file name).
    {
        ThemeColors entry = DefaultThemeCatalog().empty() ? ThemeColors{} : DefaultThemeCatalog().front();
        entry.name = fileId;
        entry.nameJp = label;
        entry.accent = t.accent;
        entry.noteBg = t.noteBg;
        size_t idxExisting = static_cast<size_t>(-1);
        for (size_t i = 0; i < g_themeCatalog.size(); ++i) {
            if (g_themeCatalog[i].name == fileId) { idxExisting = i; break; }
        }
        if (idxExisting != static_cast<size_t>(-1)) g_themeCatalog[idxExisting] = entry;
        else g_themeCatalog.push_back(std::move(entry));
    }

    if (!ApplyThemeByName(fileId, owner, /*persist=*/true)) {
        if (err) *err = IsEnglishUi() ? L"Failed to apply theme." : L"テーマの適用に失敗しました。";
        return false;
    }
    if (outThemeId) *outThemeId = fileId;
    return true;
}

static void ApplyMenuThemeRecursive(HMENU menu) {
    if (!menu) return;
    MENUINFO mi{};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND;
    mi.hbrBack = g_hThemeMenuBrush ? g_hThemeMenuBrush : GetSysColorBrush(COLOR_MENU);
    SetMenuInfo(menu, &mi);

    int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, i, TRUE, &mii)) continue;
        if (mii.hSubMenu) ApplyMenuThemeRecursive(mii.hSubMenu);
    }
}

namespace {
using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);

static SetWindowThemeFn ResolveSetWindowTheme() {
    static HMODULE s_uxtheme = nullptr;
    static SetWindowThemeFn s_fn = nullptr;
    static bool s_tried = false;
    if (s_tried) return s_fn;
    s_tried = true;
    s_uxtheme = LoadLibraryW(L"uxtheme.dll");
    if (!s_uxtheme) return nullptr;
    s_fn = reinterpret_cast<SetWindowThemeFn>(GetProcAddress(s_uxtheme, "SetWindowTheme"));
    return s_fn;
}

static double ColorLuminance(COLORREF c) {
    return 0.299 * GetRValue(c) + 0.587 * GetGValue(c) + 0.114 * GetBValue(c);
}

static bool ThemeWantsDarkScrollbars() {
    // Use a conservative threshold; only flip to dark controls on clearly dark themes.
    return ColorLuminance(g_theme.panelBg) < 120.0;
}

static void ApplyThemedWindowChrome(HWND hWnd) {
    if (!hWnd) return;
    auto fn = ResolveSetWindowTheme();
    if (!fn) return;
    const bool dark = ThemeWantsDarkScrollbars();
    fn(hWnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

static bool ClassNameEquals(HWND hWnd, const wchar_t* target) {
    if (!hWnd || !target) return false;
    wchar_t name[64]{};
    if (!GetClassNameW(hWnd, name, static_cast<int>(std::size(name)))) return false;
    return _wcsicmp(name, target) == 0;
}

static constexpr const wchar_t* kThemeOriginalButtonTypeProp = L"PdfNoteThemeOriginalButtonType";

static bool ShouldThemeControlChrome(HWND hWnd) {
    if (!hWnd) return false;

    // Prevent uxtheme/comctl32 DrawTextW crash: ComboBox does not support DarkMode_Explorer theme.
    if (ClassNameEquals(hWnd, L"ComboBox")) return false;

    const LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    if ((style & (WS_VSCROLL | WS_HSCROLL)) != 0) return true;
    // Common controls whose scrollbars/chrome tend to clash with custom themes.
    if (ClassNameEquals(hWnd, L"Edit")) return true;
    if (ClassNameEquals(hWnd, L"ListBox")) return true;
    if (ClassNameEquals(hWnd, L"ComboLBox")) return true;
    if (ClassNameEquals(hWnd, L"SysListView32")) return true;
    if (ClassNameEquals(hWnd, L"SysTreeView32")) return true;
    return false;
}

static BOOL CALLBACK EnumChildApplyChrome(HWND hWnd, LPARAM) {
    if (ShouldThemeControlChrome(hWnd)) ApplyThemedWindowChrome(hWnd);
    return TRUE;
}

static void ApplyThemedChromeRecursive(HWND root) {
    if (!root) return;
    if (ShouldThemeControlChrome(root)) ApplyThemedWindowChrome(root);
    EnumChildWindows(root, EnumChildApplyChrome, 0);
}

static BOOL CALLBACK EnumChildOwnerDrawButtons(HWND hWnd, LPARAM) {
    if (!hWnd) return TRUE;
    if (!ClassNameEquals(hWnd, L"Button")) return TRUE;
    LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    const LONG_PTR type = (style & BS_TYPEMASK);
    if (type == BS_GROUPBOX) return TRUE;
    if ((style & BS_OWNERDRAW) != 0) return TRUE;
    SetPropW(hWnd, kThemeOriginalButtonTypeProp, reinterpret_cast<HANDLE>(type + 1));
    style &= ~BS_TYPEMASK;
    style |= BS_OWNERDRAW;
    SetWindowLongPtrW(hWnd, GWL_STYLE, style);
    SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(hWnd, nullptr, TRUE);
    return TRUE;
}

static void EnableOwnerDrawButtonsRecursive(HWND root) {
    if (!root) return;
    EnumChildWindows(root, EnumChildOwnerDrawButtons, 0);
}

static BOOL CALLBACK EnumChildRestoreOwnerDrawButtons(HWND hWnd, LPARAM) {
    if (!hWnd) return TRUE;
    if (!ClassNameEquals(hWnd, L"Button")) return TRUE;
    HANDLE prop = GetPropW(hWnd, kThemeOriginalButtonTypeProp);
    if (!prop) return TRUE;

    LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    style &= ~BS_TYPEMASK;
    style |= ((reinterpret_cast<LONG_PTR>(prop) - 1) & BS_TYPEMASK);
    SetWindowLongPtrW(hWnd, GWL_STYLE, style);
    RemovePropW(hWnd, kThemeOriginalButtonTypeProp);
    SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(hWnd, nullptr, TRUE);
    return TRUE;
}

static void RestoreOwnerDrawButtonsRecursive(HWND root) {
    if (!root) return;
    EnumChildWindows(root, EnumChildRestoreOwnerDrawButtons, 0);
}

static BOOL CALLBACK EnumThreadThemeChanged(HWND hWnd, LPARAM lParam) {
    HWND exclude = reinterpret_cast<HWND>(lParam);
    if (hWnd && hWnd != exclude) {
        PostMessageW(hWnd, WM_THEMECHANGED, 0, 0);
    }
    return TRUE;
}

static void BroadcastThemeChangedToThreadWindows(HWND exclude) {
    EnumThreadWindows(GetCurrentThreadId(), EnumThreadThemeChanged, reinterpret_cast<LPARAM>(exclude));
}
} // namespace

void ApplyThemeToUI(HWND owner) {
    if (g_hNoteEdit) {
        SendMessageW(g_hNoteEdit, EM_SETBKGNDCOLOR, 0, g_noteBgColor);
        InvalidateRect(g_hNoteEdit, nullptr, FALSE);
    }
    if (g_hPdfToolbar) InvalidateRect(g_hPdfToolbar, nullptr, TRUE);
    if (g_hBottomNote) InvalidateRect(g_hBottomNote, nullptr, FALSE);
    if (g_hBottomMath) InvalidateRect(g_hBottomMath, nullptr, FALSE);
    if (g_hAnnotList) InvalidateRect(g_hAnnotList, nullptr, TRUE);
    if (g_hPdfView) InvalidateRect(g_hPdfView, nullptr, TRUE);
    if (owner) {
        EnsureOwnerDrawUi(owner);
        if (g_config.ownerDrawUi) {
            HMENU menu = GetMenu(owner);
            if (menu) ApplyMenuThemeRecursive(menu);
            if (g_hMainMenu && g_hMainMenu != menu) ApplyMenuThemeRecursive(g_hMainMenu);
        }
        DrawMenuBar(owner);
        RedrawWindow(owner, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
        ApplyThemedChromeRecursive(owner);
        BroadcastThemeChangedToThreadWindows(owner);
    } else {
        // Best-effort: keep child control chrome consistent even before the main window exists.
        ApplyThemedWindowChrome(g_hNoteEdit);
        ApplyThemedWindowChrome(g_hLectureList);
        ApplyThemedWindowChrome(g_hSessionList);
        ApplyThemedWindowChrome(g_hPdfList);
        ApplyThemedWindowChrome(g_hNoteList);
        ApplyThemedWindowChrome(g_hAnnotList);
    }
}

LRESULT ThemeCtlColorPanel(HWND ctl, HDC hdc) {
    (void)ctl;
    if (!hdc) return 0;
    COLORREF text = g_theme.panelText;
    COLORREF back = g_theme.panelBg;
    HBRUSH br = g_hThemePanelBrush ? g_hThemePanelBrush : GetSysColorBrush(COLOR_WINDOW);
    SetTextColor(hdc, text);
    SetBkColor(hdc, back);
    return reinterpret_cast<LRESULT>(br);
}

void ApplyThemeToDialog(HWND hWnd) {
    if (!hWnd) return;
    if (g_config.ownerDrawUi) {
        EnableOwnerDrawButtonsRecursive(hWnd);
    } else {
        RestoreOwnerDrawButtonsRecursive(hWnd);
    }
    ApplyThemedChromeRecursive(hWnd);
    RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
}

namespace {
static COLORREF BlendColor(COLORREF a, COLORREF b, double t) {
    int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
    int r = static_cast<int>(std::lround(ar + (br - ar) * t));
    int g = static_cast<int>(std::lround(ag + (bg - ag) * t));
    int b2 = static_cast<int>(std::lround(ab + (bb - ab) * t));
    return RGB(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b2, 0, 255));
}

static COLORREF DisabledTextColor(COLORREF text, COLORREF back) {
    return BlendColor(text, back, 0.5);
}

static bool ButtonIsRadio(LONG_PTR style) {
    const LONG_PTR type = (style & BS_TYPEMASK);
    return type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
}

static bool ButtonIsCheckbox(LONG_PTR style) {
    const LONG_PTR type = (style & BS_TYPEMASK);
    return type == BS_CHECKBOX || type == BS_AUTOCHECKBOX;
}

static bool ButtonIsKnownCheckbox(HWND hWnd) {
    return hWnd == g_hAnnotShow ||
           hWnd == g_hChkTextReadableBackground ||
           hWnd == g_hChkShortcutHeading1 ||
           hWnd == g_hChkShortcutBack ||
           hWnd == g_hChkShortcutChar ||
           hWnd == g_hChkShortcutBold ||
           hWnd == g_hChkShortcutItalic ||
           hWnd == g_hChkShortcutStrike ||
           hWnd == g_hChkShortcutUnderline ||
           hWnd == g_hChkShortcutLinkDecor;
}

static bool ToolModeUsesPaletteColor(ToolMode mode) {
    switch (mode) {
    case ToolMode::TextBox:
    case ToolMode::MarkerLine:
    case ToolMode::MarkerArrow:
    case ToolMode::MarkerWave:
    case ToolMode::Line:
    case ToolMode::Arrow:
    case ToolMode::Wave:
    case ToolMode::Freehand:
    case ToolMode::MarkerFree:
    case ToolMode::MarkerText:
    case ToolMode::MarkerTextUnderline:
    case ToolMode::MarkerTextColor:
    case ToolMode::Shape:
        return true;
    default:
        return false;
    }
}

static bool ColorIsDark(COLORREF c) {
    return ColorLuminance(c) < 180.0;
}
} // namespace

COLORREF ToolColorForMode(ToolMode mode) {
    if (AnnotToolUsesGeometry(mode, AnnotToolGeometry::Line)) return g_lineColor;
    if (IsArrowAnnotToolMode(mode)) return g_lineToolsShareStyle ? g_lineColor : g_arrowColor;
    if (IsWaveAnnotToolMode(mode)) return g_lineToolsShareStyle ? g_lineColor : g_waveColor;
    if (AnnotToolUsesGeometry(mode, AnnotToolGeometry::Shape)) return g_shapeColor;
    switch (mode) {
    case ToolMode::TextBox:    return g_textColor;
    case ToolMode::MarkerFree:
        return g_markerFreeColor;
    case ToolMode::MarkerTextUnderline:
    case ToolMode::MarkerTextColor:
    case ToolMode::MarkerText: return g_markerTextColor;
    case ToolMode::Freehand:   return g_freehandColor;
    default:
        return g_activeColor;
    }
}

void StoreToolColorForMode(ToolMode mode, COLORREF color) {
    if (AnnotToolUsesGeometry(mode, AnnotToolGeometry::Line)) {
        g_lineColor = color;
        return;
    }
    if (IsArrowAnnotToolMode(mode)) {
        if (g_lineToolsShareStyle) g_lineColor = color;
        else g_arrowColor = color;
        return;
    }
    if (IsWaveAnnotToolMode(mode)) {
        if (g_lineToolsShareStyle) g_lineColor = color;
        else g_waveColor = color;
        return;
    }
    if (AnnotToolUsesGeometry(mode, AnnotToolGeometry::Shape)) {
        g_shapeColor = color;
        return;
    }
    switch (mode) {
    case ToolMode::TextBox:    g_textColor = color; break;
    case ToolMode::MarkerFree: g_markerFreeColor = color; break;
    case ToolMode::MarkerTextUnderline:
    case ToolMode::MarkerTextColor:
    case ToolMode::MarkerText: g_markerTextColor = color; break;
    case ToolMode::Freehand:   g_freehandColor = color; break;
    default: break;
    }
}

double ToolWidthPtForMode(ToolMode mode) {
    if (AnnotToolUsesGeometry(mode, AnnotToolGeometry::Line)) return g_lineWidthPt;
    if (IsArrowAnnotToolMode(mode)) return g_lineToolsShareStyle ? g_lineWidthPt : g_arrowWidthPt;
    if (IsWaveAnnotToolMode(mode)) return g_lineToolsShareStyle ? g_lineWidthPt : g_waveWidthPt;
    switch (mode) {
    case ToolMode::MarkerFree:
        return g_markerFreeWidthPt;
    case ToolMode::MarkerTextUnderline:
    case ToolMode::MarkerText:
        return g_markerTextWidthPt;
    case ToolMode::Freehand:
        return g_freehandWidthPt;
    case ToolMode::Eraser:
        return g_eraserWidthPt;
    default:
        return 0.0;
    }
}

void StoreToolWidthPtForMode(ToolMode mode, double pt) {
    if (AnnotToolUsesGeometry(mode, AnnotToolGeometry::Line)) {
        g_lineWidthPt = pt;
        return;
    }
    if (IsArrowAnnotToolMode(mode)) {
        if (g_lineToolsShareStyle) g_lineWidthPt = pt;
        else g_arrowWidthPt = pt;
        return;
    }
    if (IsWaveAnnotToolMode(mode)) {
        if (g_lineToolsShareStyle) g_lineWidthPt = pt;
        else g_waveWidthPt = pt;
        return;
    }
    switch (mode) {
    case ToolMode::MarkerFree:
        g_markerFreeWidthPt = pt;
        break;
    case ToolMode::MarkerTextUnderline:
    case ToolMode::MarkerText:
        g_markerTextWidthPt = pt;
        break;
    case ToolMode::Freehand:
        g_freehandWidthPt = pt;
        break;
    case ToolMode::Eraser:
        g_eraserWidthPt = std::clamp(pt, 1.0, 80.0);
        break;
    default:
        break;
    }
}

double ToolAlphaForMode(ToolMode mode) {
    if (AnnotToolUsesGeometry(mode, AnnotToolGeometry::Line)) {
        return std::clamp(g_lineAlpha, 0.05, 1.0);
    }
    if (IsArrowAnnotToolMode(mode)) {
        return g_lineToolsShareStyle
                   ? std::clamp(g_lineAlpha, 0.05, 1.0)
                   : std::clamp(g_arrowAlpha, 0.05, 1.0);
    }
    if (IsWaveAnnotToolMode(mode)) {
        return g_lineToolsShareStyle
                   ? std::clamp(g_lineAlpha, 0.05, 1.0)
                   : std::clamp(g_waveAlpha, 0.05, 1.0);
    }
    if (AnnotToolUsesGeometry(mode, AnnotToolGeometry::Shape)) {
        return std::clamp(g_shapeAlpha, 0.0, 1.0);
    }
    switch (mode) {
    case ToolMode::MarkerText:
    case ToolMode::MarkerTextUnderline:
    case ToolMode::MarkerFree:
        return std::clamp(g_markerAlpha, 0.05, 1.0);
    case ToolMode::Freehand:
        return std::clamp(g_freehandAlpha, 0.05, 1.0);
    default:
        return 1.0;
    }
}

void StoreToolAlphaForMode(ToolMode mode, double alpha) {
    if (AnnotToolUsesGeometry(mode, AnnotToolGeometry::Line)) {
        g_lineAlpha = std::clamp(alpha, 0.05, 1.0);
        return;
    }
    if (IsArrowAnnotToolMode(mode)) {
        alpha = std::clamp(alpha, 0.05, 1.0);
        if (g_lineToolsShareStyle) g_lineAlpha = alpha;
        else g_arrowAlpha = alpha;
        return;
    }
    if (IsWaveAnnotToolMode(mode)) {
        alpha = std::clamp(alpha, 0.05, 1.0);
        if (g_lineToolsShareStyle) g_lineAlpha = alpha;
        else g_waveAlpha = alpha;
        return;
    }
    if (AnnotToolUsesGeometry(mode, AnnotToolGeometry::Shape)) {
        g_shapeAlpha = std::clamp(alpha, 0.0, 1.0);
        return;
    }
    switch (mode) {
    case ToolMode::MarkerText:
    case ToolMode::MarkerTextUnderline:
    case ToolMode::MarkerFree:
        alpha = std::clamp(alpha, 0.05, 1.0);
        g_markerAlpha = alpha;
        break;
    case ToolMode::Freehand:
        g_freehandAlpha = std::clamp(alpha, 0.05, 1.0);
        break;
    default:
        break;
    }
}

static int ToolModeToCommand(ToolMode mode) {
    switch (mode) {
    case ToolMode::Select:     return ID_TOOL_MODE_SELECT;
    case ToolMode::Pan:        return ID_TOOL_MODE_PAN;
    case ToolMode::Magnifier:  return ID_TOOL_MODE_MAGNIFIER;
    case ToolMode::MarkerText: return ID_TOOL_MODE_MARKER_TEXT;
    case ToolMode::MarkerTextUnderline: return ID_TOOL_MODE_MARKER_UNDERLINE;
    case ToolMode::MarkerTextColor: return ID_TOOL_MODE_MARKER_TEXT_COLOR;
    case ToolMode::MarkerFree: return ID_TOOL_MODE_MARKER_FREE;
    case ToolMode::MarkerLine: return ID_TOOL_MODE_MARKER_LINE;
    case ToolMode::MarkerArrow:return ID_TOOL_MODE_MARKER_ARROW;
    case ToolMode::MarkerWave: return ID_TOOL_MODE_MARKER_WAVE;
    case ToolMode::TextBox:    return ID_TOOL_MODE_TEXT;
    case ToolMode::Line:       return ID_TOOL_MODE_LINE;
    case ToolMode::Arrow:      return ID_TOOL_MODE_ARROW;
    case ToolMode::Wave:       return ID_TOOL_MODE_WAVE;
    case ToolMode::Freehand:   return ID_TOOL_MODE_FREEHAND;
    case ToolMode::Shape:      return ID_TOOL_MODE_SHAPE;
    case ToolMode::Eraser:     return ID_TOOL_MODE_ERASER;
    default:                   return 0;
    }
}

int AnnotToolFamilyCommand(AnnotToolFamily family) {
    switch (family) {
    case AnnotToolFamily::Select:    return ID_TOOL_MODE_SELECT;
    case AnnotToolFamily::Pan:       return ID_TOOL_MODE_PAN;
    case AnnotToolFamily::Magnifier: return ID_TOOL_MODE_MAGNIFIER;
    case AnnotToolFamily::Text:      return ID_TOOL_MODE_TEXT;
    case AnnotToolFamily::Marker:    return ID_TOOL_MODE_MARKER_TEXT;
    case AnnotToolFamily::Pen:       return ID_TOOL_MODE_FREEHAND;
    case AnnotToolFamily::Shape:     return ID_TOOL_MODE_SHAPE;
    case AnnotToolFamily::Eraser:    return ID_TOOL_MODE_ERASER;
    default:                         return 0;
    }
}

std::wstring AnnotToolFamilyLabel(AnnotToolFamily family) {
    const UiText& ui = GetUiText();
    switch (family) {
    case AnnotToolFamily::Select:    return ui.btnModeSelect;
    case AnnotToolFamily::Pan:       return ui.btnModePan;
    case AnnotToolFamily::Magnifier: return ui.btnModeMagnifier;
    case AnnotToolFamily::Text:      return ui.btnModeText;
    case AnnotToolFamily::Marker:    return ui.btnModeMarker;
    case AnnotToolFamily::Pen:       return IsEnglishUi() ? L"Pen" : L"ペン";
    case AnnotToolFamily::Shape:     return ui.btnModeShape;
    case AnnotToolFamily::Eraser:    return ui.btnModeEraser;
    default:                         return IsEnglishUi() ? L"Tool" : L"ツール";
    }
}

AnnotToolUiState AnnotToolFamilyUiStateFor(AnnotToolFamily family) {
    bool anyEnabled = false;
    bool anyDisabled = false;
    for (ToolMode mode : AnnotToolModeUiOrder()) {
        if (AnnotToolFamilyForMode(mode) != family) continue;
        AnnotToolUiState state = AnnotToolModeUiStateFor(mode);
        if (state == AnnotToolUiState::Enabled) anyEnabled = true;
        else if (state == AnnotToolUiState::Disabled) anyDisabled = true;
    }
    if (anyEnabled) return AnnotToolUiState::Enabled;
    if (anyDisabled) return AnnotToolUiState::Disabled;
    return AnnotToolUiState::Hidden;
}

static POINT g_prevMouseMoveScreenPt{};
static DWORD g_prevMouseMoveTick = 0;
static HWND g_prevMouseMoveHwnd = nullptr;
static POINT g_recentMouseMoveScreenPt{};
static DWORD g_recentMouseMoveTick = 0;
static HWND g_recentMouseMoveHwnd = nullptr;
static HWND g_quickAnnotPrimedHwnd = nullptr;
static DWORD g_quickAnnotPrimedTick = 0;
static HWND g_quickAnnotMenuActiveHwnd = nullptr;
static HWND g_suppressedQuickAnnotContextHwnd = nullptr;
static DWORD g_suppressedQuickAnnotContextTick = 0;
static constexpr wchar_t kQuickAnnotPopupClassName[] = L"ClroQuickAnnotPopup";

struct QuickAnnotPopupItem {
    int command = 0;
    RECT rc{};
    bool enabled = true;
    bool active = false;
    std::wstring label;
};

struct QuickAnnotPopupState {
    HWND popupHwnd = nullptr;
    HWND hostHwnd = nullptr;
    std::vector<QuickAnnotPopupItem> items;
    int hotIndex = -1;
};

static QuickAnnotPopupState g_quickAnnotPopup;
static bool g_quickAnnotPopupClosing = false;

static std::wstring NormalizeQuickAnnotPopupPlacement(const std::wstring& value) {
    std::wstring v = value;
    std::transform(v.begin(), v.end(), v.begin(), ::towlower);
    if (v == L"boundary") return L"boundary";
    if (v == L"up") return L"up";
    if (v == L"down") return L"down";
    return L"auto";
}

static LRESULT CALLBACK QuickAnnotPopupProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static bool EnsureQuickAnnotPopupClass() {
    static bool s_registered = false;
    if (s_registered) return true;
    WNDCLASSW wc{};
    wc.lpfnWndProc = QuickAnnotPopupProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kQuickAnnotPopupClassName;
    wc.hbrBackground = nullptr;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    s_registered = true;
    return true;
}

static int HitTestQuickAnnotPopupItem(const POINT& clientPt) {
    for (size_t i = 0; i < g_quickAnnotPopup.items.size(); ++i) {
        const auto& item = g_quickAnnotPopup.items[i];
        if (PtInRect(&item.rc, clientPt)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static void ResetQuickAnnotPopupOnly() {
    g_quickAnnotPopup.popupHwnd = nullptr;
    g_quickAnnotPopup.hostHwnd = nullptr;
    g_quickAnnotPopup.items.clear();
    g_quickAnnotPopup.hotIndex = -1;
}

static void CloseQuickAnnotPopup(bool commitSelection) {
    HWND popup = g_quickAnnotPopup.popupHwnd;
    if (!popup) {
        ResetQuickAnnotPopupOnly();
        return;
    }

    int selected = (commitSelection && g_quickAnnotPopup.hotIndex >= 0 &&
                    g_quickAnnotPopup.hotIndex < static_cast<int>(g_quickAnnotPopup.items.size()))
                       ? g_quickAnnotPopup.hotIndex
                       : -1;
    HWND host = g_quickAnnotPopup.hostHwnd;
    int chosenCommand = 0;
    bool chosenEnabled = false;
    if (selected >= 0) {
        const auto& item = g_quickAnnotPopup.items[static_cast<size_t>(selected)];
        chosenCommand = item.command;
        chosenEnabled = item.enabled;
    }

    g_quickAnnotPopupClosing = true;
    if (g_quickAnnotMenuActiveHwnd == host) {
        g_quickAnnotMenuActiveHwnd = nullptr;
    }
    ResetQuickAnnotPopupOnly();
    if (GetCapture() == popup) {
        ReleaseCapture();
    }
    DestroyWindow(popup);
    g_quickAnnotPopupClosing = false;

    if (selected >= 0 && chosenEnabled) {
        if (chosenCommand != 0) {
            HWND target = g_hMainWnd ? g_hMainWnd : (host ? GetParent(host) : nullptr);
            if (target) {
                SendMessageW(target, WM_COMMAND, MAKEWPARAM(chosenCommand, kAnnotContextMenuCommandCode), reinterpret_cast<LPARAM>(host));
            }
        }
    }
}

static SIZE MeasureQuickAnnotPopup(HWND host, std::vector<QuickAnnotPopupItem>& items) {
    const int itemH = 24;
    const int padX = 10;
    const int padY = 6;
    const int minW = 120;
    SIZE out{ minW, padY * 2 };

    HDC hdc = GetDC(host ? host : g_hMainWnd);
    HFONT font = g_hUIFont ? g_hUIFont : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HFONT old = hdc && font ? static_cast<HFONT>(SelectObject(hdc, font)) : nullptr;
    int maxTextW = 0;
    for (auto& item : items) {
        SIZE textSize{};
        if (hdc && !item.label.empty()) {
            GetTextExtentPoint32W(hdc, item.label.c_str(), static_cast<int>(item.label.size()), &textSize);
        }
        maxTextW = std::max(maxTextW, static_cast<int>(textSize.cx));
    }
    if (hdc && old) SelectObject(hdc, old);
    if (hdc) ReleaseDC(host ? host : g_hMainWnd, hdc);

    out.cx = std::max(minW, maxTextW + padX * 2);
    out.cy = std::max(0, static_cast<int>(items.size()) * itemH + padY * 2);

    int y = padY;
    for (auto& item : items) {
        item.rc = RECT{ 0, y, out.cx, y + itemH };
        y += itemH;
    }
    return out;
}

static std::wstring QuickAnnotFormatPt(double pt) {
    wchar_t buf[32]{};
    swprintf_s(buf, L"%.1f", pt);
    std::wstring s = buf;
    if (s.size() >= 2 && s.substr(s.size() - 2) == L".0") s.resize(s.size() - 2);
    return s;
}

static std::wstring QuickAnnotPtLabel(const wchar_t* label, double pt) {
    return std::wstring(label) + L" (" + QuickAnnotFormatPt(pt) + L"pt)";
}

static std::wstring QuickAnnotAlphaLabel(const wchar_t* label, double alpha) {
    int pct = static_cast<int>(std::llround(std::clamp(alpha, 0.0, 1.0) * 100.0));
    return std::wstring(label) + L" (" + std::to_wstring(pct) + L"%)";
}

static void AddQuickAnnotContextOptions(std::vector<QuickAnnotPopupItem>& items) {
    auto addItem = [&](int command, const std::wstring& label, bool active) {
        QuickAnnotPopupItem item{};
        item.command = command;
        item.enabled = true;
        item.active = active;
        item.label = label;
        items.push_back(std::move(item));
    };

    if (ToolbarHasFontOptions(g_toolMode)) {
        static constexpr int kFontPt10[] = { 100, 120, 140, 160, 180, 200, 240, 280, 320 };
        const int currentPt10 = static_cast<int>(std::llround(std::clamp(g_textFontPt, 6.0, 96.0) * 10.0));
        for (int i = 0; i < static_cast<int>(std::size(kFontPt10)); ++i) {
            const double pt = static_cast<double>(kFontPt10[i]) / 10.0;
            addItem(ID_ANNOT_CONTEXT_FONT_SIZE_BASE + i,
                    std::wstring(IsEnglishUi() ? L"Size " : L"文字サイズ ") + QuickAnnotFormatPt(pt) + L"pt",
                    std::abs(currentPt10 - kFontPt10[i]) <= 1);
        }
    }

    if (ToolbarHasWidthOptions(g_toolMode)) {
        int widthPt10[3]{ 15, 25, 40 };
        if (g_toolMode == ToolMode::MarkerFree ||
            g_toolMode == ToolMode::MarkerLine ||
            g_toolMode == ToolMode::MarkerArrow ||
            g_toolMode == ToolMode::MarkerWave) {
            widthPt10[0] = 60;
            widthPt10[1] = 80;
            widthPt10[2] = 100;
        } else if (g_toolMode == ToolMode::MarkerText ||
                   g_toolMode == ToolMode::MarkerTextUnderline) {
            widthPt10[0] = 20;
            widthPt10[1] = 40;
            widthPt10[2] = 60;
        } else if (g_toolMode == ToolMode::Eraser) {
            widthPt10[0] = 20;
            widthPt10[1] = 40;
            widthPt10[2] = 80;
        }
        const wchar_t* labelsJa[] = { L"太さ 細", L"太さ 中", L"太さ 太" };
        const wchar_t* labelsEn[] = { L"Width Thin", L"Width Medium", L"Width Thick" };
        const int currentPt10 = static_cast<int>(std::llround(ToolWidthPtForMode(g_toolMode) * 10.0));
        for (int i = 0; i < 3; ++i) {
            const double pt = static_cast<double>(widthPt10[i]) / 10.0;
            addItem(ID_ANNOT_CONTEXT_WIDTH_BASE + i,
                    QuickAnnotPtLabel(IsEnglishUi() ? labelsEn[i] : labelsJa[i], pt),
                    std::abs(currentPt10 - widthPt10[i]) <= 1);
        }
    }

    if (ToolbarHasMarkerAlphaOptions(g_toolMode)) {
        const wchar_t* labelsJa[] = { L"濃さ 薄", L"濃さ 標準", L"濃さ 濃", L"濃さ 最大" };
        const wchar_t* labelsEn[] = { L"Opacity Light", L"Opacity Standard", L"Opacity Dark", L"Opacity Maximum" };
        const double current = ToolAlphaForMode(g_toolMode);
        const int count = ToolAlphaOptionCountForMode(g_toolMode);
        for (int i = 0; i < count; ++i) {
            const double alpha = ToolAlphaOptionValueForMode(g_toolMode, i);
            addItem(ID_ANNOT_CONTEXT_ALPHA_BASE + i,
                    QuickAnnotAlphaLabel(IsEnglishUi() ? labelsEn[i] : labelsJa[i], alpha),
                    std::abs(current - alpha) < 0.01);
        }
    }
}

static POINT ResolveQuickAnnotPopupOrigin(HWND host, const POINT& anchorClient, const SIZE& popupSize) {
    POINT anchorScreen = anchorClient;
    ClientToScreen(host, &anchorScreen);

    RECT clientRc{};
    GetClientRect(host, &clientRc);
    POINT topLeft{ clientRc.left, clientRc.top };
    POINT bottomRight{ clientRc.right, clientRc.bottom };
    ClientToScreen(host, &topLeft);
    ClientToScreen(host, &bottomRight);
    RECT hostScreen{ topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };

    const int margin = 6;
    const int popupW = static_cast<int>(popupSize.cx);
    const int popupH = static_cast<int>(popupSize.cy);
    const int baseSideGap = std::clamp(popupW / 6, 14, 26);
    const int baseVerticalGap = std::clamp(popupH / 10, 10, 18);
    int x = anchorScreen.x + baseSideGap;
    int y = anchorScreen.y + baseVerticalGap;

    std::wstring placement = NormalizeQuickAnnotPopupPlacement(g_config.quickAnnotPopupPlacement);
    int dx = 0;
    int dy = 0;
    int moveMag = 0;
    int extraLead = 0;
    if (g_recentMouseMoveHwnd == host && g_prevMouseMoveHwnd == host) {
        dx = g_recentMouseMoveScreenPt.x - g_prevMouseMoveScreenPt.x;
        dy = g_recentMouseMoveScreenPt.y - g_prevMouseMoveScreenPt.y;
        moveMag = std::max(std::abs(dx), std::abs(dy)) + std::min(std::abs(dx), std::abs(dy)) / 2;
        if (g_recentMouseMoveTick != 0 && g_prevMouseMoveTick != 0 && g_recentMouseMoveTick >= g_prevMouseMoveTick) {
            DWORD dt = std::max<DWORD>(1, g_recentMouseMoveTick - g_prevMouseMoveTick);
            int speedLead = std::clamp((moveMag * 12) / static_cast<int>(dt) - 1, 0, 9);
            int distanceLead = std::clamp(moveMag / 5, 0, 6);
            extraLead = std::min(10, speedLead + distanceLead / 2);
        }
    }
    const int horizontalLead = std::clamp(extraLead + moveMag / 10, 0, std::max(4, popupW / 8));
    const int verticalLead = std::clamp(extraLead / 2 + moveMag / 14, 0, std::max(3, popupH / 10));
    const int forwardLead = std::clamp(extraLead + moveMag / 8, 0, std::max(6, std::min(popupW, popupH) / 6));
    const int centeredX = anchorScreen.x - popupW / 4;

    auto placeAbove = [&]() {
        int xBias = (dx > 0) ? horizontalLead : ((dx < 0) ? -horizontalLead : 0);
        x = centeredX + xBias;
        y = anchorScreen.y - popupH - baseVerticalGap - ((dy < 0) ? extraLead : verticalLead);
    };
    auto placeBelow = [&]() {
        int xBias = (dx > 0) ? horizontalLead : ((dx < 0) ? -horizontalLead : 0);
        x = centeredX + xBias;
        y = anchorScreen.y + baseVerticalGap + ((dy > 0) ? extraLead : verticalLead);
    };

    if (placement == L"up") {
        placeAbove();
    } else if (placement == L"down") {
        placeBelow();
    } else if (placement == L"boundary") {
        if (anchorClient.y >= (clientRc.bottom - clientRc.top) / 2) placeAbove();
        else placeBelow();
    } else {
        const int absDx = std::abs(dx);
        const int absDy = std::abs(dy);
        if (absDx == 0 && absDy == 0) {
            placeBelow();
        } else {
            const bool horizontalDominant = (absDx > absDy * 11 / 10);
            const bool verticalDominant = (absDy > absDx * 11 / 10);
            if (horizontalDominant) {
                int sideGap = baseSideGap + forwardLead;
                x = anchorScreen.x + ((dx >= 0) ? sideGap : (-popupW - sideGap));
                int yBias = 0;
                if (dy > 0) yBias = verticalLead;
                else if (dy < 0) yBias = -verticalLead;
                y = anchorScreen.y - popupH / 2 + yBias;
            } else if (verticalDominant) {
                if (dy < 0) placeAbove();
                else placeBelow();
            } else {
                // Diagonal motion: keep the popup in the actual travel direction and
                // bias the perpendicular axis just enough to keep the path natural.
                int sideGap = baseSideGap + forwardLead / 2;
                int verticalGap = baseVerticalGap + forwardLead / 2;
                x = anchorScreen.x + ((dx >= 0) ? sideGap : (-popupW - sideGap));
                y = anchorScreen.y + ((dy >= 0) ? verticalGap : (-popupH - verticalGap));
                x += (dx >= 0) ? horizontalLead / 2 : -horizontalLead / 2;
            }
        }
    }

    if (y < hostScreen.top + margin) {
        int downY = anchorScreen.y + margin;
        if (downY + popupSize.cy <= hostScreen.bottom - margin) {
            y = downY;
        }
    } else if (y + popupSize.cy > hostScreen.bottom - margin) {
        int upY = anchorScreen.y - popupSize.cy - margin;
        if (upY >= hostScreen.top + margin) {
            y = upY;
        }
    }

    int minX = static_cast<int>(hostScreen.left) + margin;
    int maxX = std::max(minX, static_cast<int>(hostScreen.right) - static_cast<int>(popupSize.cx) - margin);
    int minY = static_cast<int>(hostScreen.top) + margin;
    int maxY = std::max(minY, static_cast<int>(hostScreen.bottom) - static_cast<int>(popupSize.cy) - margin);
    x = std::clamp(x, minX, maxX);
    y = std::clamp(y, minY, maxY);
    return POINT{ x, y };
}

void RecordRecentMouseMove(HWND host, const POINT* clientPt) {
    if (!host || !clientPt) return;
    POINT pt = *clientPt;
    ClientToScreen(host, &pt);
    g_prevMouseMoveScreenPt = g_recentMouseMoveScreenPt;
    g_prevMouseMoveTick = g_recentMouseMoveTick;
    g_prevMouseMoveHwnd = g_recentMouseMoveHwnd;
    g_recentMouseMoveScreenPt = pt;
    g_recentMouseMoveTick = GetTickCount();
    g_recentMouseMoveHwnd = host;
}

static bool ShouldUseQuickAnnotPressDrag(HWND host, const POINT* clientPt, bool allowWithoutCtrl) {
    if (!host || !clientPt) return false;
    const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (allowWithoutCtrl) {
        g_quickAnnotPrimedHwnd = host;
        g_quickAnnotPrimedTick = GetTickCount();
        return true;
    }
    if (!allowWithoutCtrl && !ctrlDown) {
        g_quickAnnotPrimedHwnd = nullptr;
        g_quickAnnotPrimedTick = 0;
        return false;
    }
    DWORD now = GetTickCount();
    if (g_recentMouseMoveHwnd == host && g_recentMouseMoveTick != 0) {
        DWORD dtRecent = now - g_recentMouseMoveTick;
        if (dtRecent <= 220) {
            POINT clickScreen = *clientPt;
            ClientToScreen(host, &clickScreen);
            bool movedEnough = false;
            if (g_prevMouseMoveHwnd == host && g_prevMouseMoveTick != 0) {
                int recentDx = g_recentMouseMoveScreenPt.x - g_prevMouseMoveScreenPt.x;
                int recentDy = g_recentMouseMoveScreenPt.y - g_prevMouseMoveScreenPt.y;
                int recentDist2 = recentDx * recentDx + recentDy * recentDy;
                int clickDx = clickScreen.x - g_recentMouseMoveScreenPt.x;
                int clickDy = clickScreen.y - g_recentMouseMoveScreenPt.y;
                int clickDist2 = clickDx * clickDx + clickDy * clickDy;
                movedEnough = (std::max(recentDist2, clickDist2) >= (1 * 1));
            } else {
                // First move after focus/idle: if a move event was just observed, prefer the
                // press-drag path rather than forcing a second move sample first.
                movedEnough = true;
            }
            if (movedEnough) {
                g_quickAnnotPrimedHwnd = host;
                g_quickAnnotPrimedTick = now;
                return true;
            }
        }
    }
    if (g_quickAnnotPrimedHwnd == host && g_quickAnnotPrimedTick != 0 &&
        (now - g_quickAnnotPrimedTick) <= 1500) {
        g_quickAnnotPrimedTick = now;
        return true;
    }
    return false;
}

static void SuppressNextQuickAnnotContextMenu(HWND host) {
    g_suppressedQuickAnnotContextHwnd = host;
    g_suppressedQuickAnnotContextTick = GetTickCount();
}

static bool ConsumeSuppressedQuickAnnotContextMenu(HWND host) {
    if (!host || host != g_suppressedQuickAnnotContextHwnd) return false;
    DWORD now = GetTickCount();
    bool hit = (g_suppressedQuickAnnotContextTick != 0 &&
                (now - g_suppressedQuickAnnotContextTick) <= 300);
    g_suppressedQuickAnnotContextHwnd = nullptr;
    g_suppressedQuickAnnotContextTick = 0;
    return hit;
}

static void ResetQuickAnnotMenuSession(HWND host, bool clearSuppressed) {
    if (!host) return;
    if (g_quickAnnotPopup.popupHwnd && g_quickAnnotPopup.hostHwnd == host) {
        CloseQuickAnnotPopup(false);
    }
    if (g_quickAnnotMenuActiveHwnd == host) {
        g_quickAnnotMenuActiveHwnd = nullptr;
    }
    if (g_quickAnnotPrimedHwnd == host) {
        g_quickAnnotPrimedHwnd = nullptr;
        g_quickAnnotPrimedTick = 0;
    }
    if (clearSuppressed && g_suppressedQuickAnnotContextHwnd == host) {
        g_suppressedQuickAnnotContextHwnd = nullptr;
        g_suppressedQuickAnnotContextTick = 0;
    }
}

static bool ShowQuickAnnotToolMenu(HWND host, const POINT* clientPt, bool includeOptions) {
    if (!host) return false;
    if (g_quickAnnotMenuActiveHwnd) {
        ResetQuickAnnotMenuSession(g_quickAnnotMenuActiveHwnd, false);
    }
    ResetQuickAnnotMenuSession(host, false);
    if (!EnsureQuickAnnotPopupClass()) return false;
    std::vector<QuickAnnotPopupItem> items;
    for (AnnotToolFamily family : AnnotToolFamilyUiOrder()) {
        AnnotToolUiState state = AnnotToolFamilyUiStateFor(family);
        if (state == AnnotToolUiState::Hidden) continue;
        int command = AnnotToolFamilyCommand(family);
        if (command == 0) continue;
        QuickAnnotPopupItem item{};
        item.command = command;
        item.enabled = (state != AnnotToolUiState::Disabled);
        item.active = (AnnotToolFamilyForMode(g_toolMode) == family);
        item.label = AnnotToolFamilyLabel(family);
        items.push_back(std::move(item));
    }
    if (includeOptions) {
        AddQuickAnnotContextOptions(items);
    }
    if (items.empty()) return false;

    POINT anchorClient{};
    if (clientPt) {
        anchorClient = *clientPt;
    }
    SIZE popupSize = MeasureQuickAnnotPopup(host, items);
    POINT origin = ResolveQuickAnnotPopupOrigin(host, anchorClient, popupSize);

    HWND popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                 kQuickAnnotPopupClassName, L"",
                                 WS_POPUP | WS_BORDER,
                                 origin.x, origin.y, popupSize.cx, popupSize.cy,
                                 host, nullptr, g_hInst, nullptr);
    if (!popup) return false;

    g_quickAnnotMenuActiveHwnd = host;
    g_quickAnnotPopup.popupHwnd = popup;
    g_quickAnnotPopup.hostHwnd = host;
    g_quickAnnotPopup.items = std::move(items);
    g_quickAnnotPopup.hotIndex = -1;

    ShowWindow(popup, SW_SHOWNOACTIVATE);
    UpdateWindow(popup);
    SetCapture(popup);

    POINT screenPt{};
    GetCursorPos(&screenPt);
    ScreenToClient(popup, &screenPt);
    int hit = HitTestQuickAnnotPopupItem(screenPt);
    if (hit != g_quickAnnotPopup.hotIndex) {
        g_quickAnnotPopup.hotIndex = hit;
        InvalidateRect(popup, nullptr, FALSE);
    }
    return true;
}

static LRESULT CALLBACK QuickAnnotPopupProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int hit = HitTestQuickAnnotPopupItem(pt);
        if (hit != g_quickAnnotPopup.hotIndex) {
            g_quickAnnotPopup.hotIndex = hit;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        CloseQuickAnnotPopup(true);
        return 0;
    case WM_CAPTURECHANGED:
        if (!g_quickAnnotPopupClosing && g_quickAnnotPopup.popupHwnd == hWnd) {
            CloseQuickAnnotPopup(false);
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            CloseQuickAnnotPopup(false);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH bg = g_hThemeToolbarBrush ? g_hThemeToolbarBrush : CreateSolidBrush(g_theme.toolbarBg);
        FillRect(hdc, &rc, bg);
        if (!g_hThemeToolbarBrush && bg) DeleteObject(bg);

        HFONT font = g_hUIFont ? g_hUIFont : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HFONT old = font ? static_cast<HFONT>(SelectObject(hdc, font)) : nullptr;
        SetBkMode(hdc, TRANSPARENT);

        for (size_t i = 0; i < g_quickAnnotPopup.items.size(); ++i) {
            const auto& item = g_quickAnnotPopup.items[i];
            RECT itemRc = item.rc;
            bool hot = (static_cast<int>(i) == g_quickAnnotPopup.hotIndex);
            bool active = item.active;

            COLORREF fill = g_theme.toolbarBg;
            if (hot) fill = BlendColor(g_theme.accent, g_theme.toolbarBg, 0.22);
            else if (active) fill = BlendColor(g_theme.accent, g_theme.toolbarBg, 0.12);
            HBRUSH itemBrush = CreateSolidBrush(fill);
            FillRect(hdc, &itemRc, itemBrush);
            DeleteObject(itemBrush);

            COLORREF textColor = item.enabled ? g_theme.toolbarText : BlendColor(g_theme.toolbarText, g_theme.toolbarBg, 0.55);
            SetTextColor(hdc, textColor);

            RECT textRc = itemRc;
            textRc.left += 10;
            textRc.right -= 8;
            DrawTextW(hdc, item.label.c_str(), -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        if (old) SelectObject(hdc, old);
        EndPaint(hWnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool HandleQuickAnnotRightButtonDown(HWND host, const POINT* clientPt, bool allowWithoutCtrl) {
    if (!host || !clientPt) return false;
    ResetQuickAnnotMenuSession(host, true);
    if (!allowWithoutCtrl && (GetKeyState(VK_CONTROL) & 0x8000) == 0) {
        return false;
    }
    bool pressDrag = ShouldUseQuickAnnotPressDrag(host, clientPt, allowWithoutCtrl);
    if (!pressDrag) return false;
    SuppressNextQuickAnnotContextMenu(host);
    return ShowQuickAnnotToolMenu(host, clientPt, false);
}

bool HandleQuickAnnotContextMenu(HWND host, const POINT* screenPt, bool allowWithoutCtrl) {
    if (!host) return false;
    if (ConsumeSuppressedQuickAnnotContextMenu(host)) {
        return true;
    }
    if (!allowWithoutCtrl && (GetKeyState(VK_CONTROL) & 0x8000) == 0) {
        ResetQuickAnnotMenuSession(host, false);
        return false;
    }
    ResetQuickAnnotMenuSession(host, false);

    POINT pt{};
    if (screenPt) {
        pt = *screenPt;
        if (pt.x == -1 && pt.y == -1) {
            GetCursorPos(&pt);
        }
    } else {
        GetCursorPos(&pt);
    }
    POINT clientPt = pt;
    ScreenToClient(host, &clientPt);
    return ShowQuickAnnotToolMenu(host, &clientPt, false);
}

HWND ToolModeButtonHwnd(ToolMode mode) {
    switch (mode) {
    case ToolMode::Select:     return g_hBtnModeSelect;
    case ToolMode::Pan:        return g_hBtnModePan;
    case ToolMode::Magnifier:  return g_hBtnModeMagnifier;
    case ToolMode::MarkerText: return g_hBtnModeMarker;
    case ToolMode::MarkerTextUnderline:
    case ToolMode::MarkerTextColor: return nullptr;
    case ToolMode::MarkerFree: return g_hBtnModeMarkerFree;
    case ToolMode::MarkerLine: return g_hBtnModeMarkerLine;
    case ToolMode::MarkerArrow:return g_hBtnModeMarkerArrow;
    case ToolMode::MarkerWave: return g_hBtnModeMarkerWave;
    case ToolMode::TextBox:    return g_hBtnModeText;
    case ToolMode::Line:       return g_hBtnModeLine;
    case ToolMode::Arrow:      return g_hBtnModeArrow;
    case ToolMode::Wave:       return g_hBtnModeWave;
    case ToolMode::Freehand:   return g_hBtnModeFreehand;
    case ToolMode::Shape:      return g_hBtnModeShape;
    case ToolMode::Eraser:     return g_hBtnModeEraser;
    default:                   return nullptr;
    }
}

const std::vector<AnnotToolFamily>& AnnotToolFamilyUiOrder() {
    return DefaultAnnotToolFamilyUiOrder();
}

AnnotToolFamily AnnotToolFamilyForMode(ToolMode mode) {
    switch (mode) {
    case ToolMode::Select:
        return AnnotToolFamily::Select;
    case ToolMode::Pan:
        return AnnotToolFamily::Pan;
    case ToolMode::Magnifier:
        return AnnotToolFamily::Magnifier;
    case ToolMode::TextBox:
        return AnnotToolFamily::Text;
    case ToolMode::MarkerText:
    case ToolMode::MarkerTextUnderline:
    case ToolMode::MarkerTextColor:
    case ToolMode::MarkerFree:
        return AnnotToolFamily::Marker;
    case ToolMode::Freehand:
        return AnnotToolFamily::Pen;
    case ToolMode::MarkerLine:
    case ToolMode::MarkerArrow:
    case ToolMode::Line:
    case ToolMode::MarkerWave:
    case ToolMode::Arrow:
    case ToolMode::Wave:
    case ToolMode::Shape:
        return AnnotToolFamily::Shape;
    case ToolMode::Eraser:
    default:
        return AnnotToolFamily::Eraser;
    }
}

HWND AnnotToolFamilyButtonHwnd(AnnotToolFamily family) {
    switch (family) {
    case AnnotToolFamily::Select:    return g_hBtnModeSelect;
    case AnnotToolFamily::Pan:       return g_hBtnModePan;
    case AnnotToolFamily::Magnifier: return g_hBtnModeMagnifier;
    case AnnotToolFamily::Text:      return g_hBtnModeText;
    case AnnotToolFamily::Marker:    return g_hBtnModeMarker;
    case AnnotToolFamily::Pen:       return g_hBtnModeFreehand;
    case AnnotToolFamily::Shape:     return g_hBtnModeShape;
    case AnnotToolFamily::Eraser:    return g_hBtnModeEraser;
    default:                         return nullptr;
    }
}

const std::vector<ToolMode>& AnnotToolModeUiOrder() {
    return g_annotToolModeUiOrder;
}

AnnotToolUiState AnnotToolModeUiStateFor(ToolMode mode) {
    int idx = static_cast<int>(mode);
    if (idx < 0 || idx >= kToolModeCount) return AnnotToolUiState::Enabled;
    return g_annotToolModeUiStates[static_cast<size_t>(idx)];
}

std::vector<AnnotToolUiState> GetAnnotToolModeUiStates() {
    std::vector<AnnotToolUiState> out;
    out.reserve(kToolModeCount);
    for (int i = 0; i < kToolModeCount; ++i) {
        out.push_back(g_annotToolModeUiStates[static_cast<size_t>(i)]);
    }
    return out;
}

static std::vector<ToolMode> NormalizeAnnotToolModeUiOrder(const std::vector<ToolMode>& order) {
    std::array<bool, kToolModeCount> seen{};
    seen.fill(false);
    std::vector<ToolMode> out;
    out.reserve(kToolModeCount);
    for (ToolMode mode : order) {
        int idx = static_cast<int>(mode);
        if (idx < 0 || idx >= kToolModeCount) continue;
        if (seen[static_cast<size_t>(idx)]) continue;
        seen[static_cast<size_t>(idx)] = true;
        out.push_back(mode);
    }
    for (ToolMode mode : DefaultAnnotToolModeUiOrder()) {
        int idx = static_cast<int>(mode);
        if (idx < 0 || idx >= kToolModeCount) continue;
        if (seen[static_cast<size_t>(idx)]) continue;
        seen[static_cast<size_t>(idx)] = true;
        out.push_back(mode);
    }
    if (out.empty()) return DefaultAnnotToolModeUiOrder();
    return out;
}

void ApplyAnnotToolModeUiConfig(const std::vector<ToolMode>& order,
                                const std::vector<AnnotToolUiState>& states,
                                bool persistSetupJson) {
    g_annotToolModeUiOrder = NormalizeAnnotToolModeUiOrder(order);
    if (states.size() == static_cast<size_t>(kToolModeCount)) {
        for (int i = 0; i < kToolModeCount; ++i) {
            g_annotToolModeUiStates[static_cast<size_t>(i)] = states[static_cast<size_t>(i)];
        }
    } else {
        g_annotToolModeUiStates = DefaultAnnotToolModeUiStates();
    }
    bool anyEnabled = false;
    for (int i = 0; i < kToolModeCount; ++i) {
        if (g_annotToolModeUiStates[static_cast<size_t>(i)] == AnnotToolUiState::Enabled) {
            anyEnabled = true;
            break;
        }
    }
    if (!anyEnabled) {
        g_annotToolModeUiStates[static_cast<size_t>(static_cast<int>(ToolMode::Select))] =
            AnnotToolUiState::Enabled;
    }
    if (persistSetupJson) {
        PersistAnnotToolUiConfigToSetupJson();
    }
}

bool DrawThemeButton(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_BUTTON || !dis->hwndItem) return false;
    HWND hWnd = dis->hwndItem;
    RECT targetRc = dis->rcItem;
    const int width = targetRc.right - targetRc.left;
    const int height = targetRc.bottom - targetRc.top;
    HDC paintDc = dis->hDC;
    RECT rc = targetRc;
    HDC memDc = nullptr;
    HBITMAP memBmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
    if (width > 0 && height > 0) {
        memDc = CreateCompatibleDC(dis->hDC);
        if (memDc) {
            memBmp = CreateCompatibleBitmap(dis->hDC, width, height);
            if (memBmp) {
                oldBmp = SelectObject(memDc, memBmp);
                paintDc = memDc;
                rc = RECT{ 0, 0, width, height };
            } else {
                DeleteDC(memDc);
                memDc = nullptr;
            }
        }
    }

    LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    bool isRadio = ButtonIsRadio(style);
    bool isCheckbox = ButtonIsCheckbox(style);
    if (!isCheckbox && ButtonIsKnownCheckbox(hWnd)) {
        isCheckbox = true;
    }
    bool isPushlike = (style & BS_PUSHLIKE) != 0;
    int checkState = static_cast<int>(SendMessageW(hWnd, BM_GETCHECK, 0, 0));
    bool checked = (checkState == BST_CHECKED || checkState == BST_INDETERMINATE);
    bool isToggle = isRadio || isCheckbox || checked;
    int id = GetDlgCtrlID(hWnd);
    bool isModeToggle = (id == ID_TOOL_MODE_SELECT ||
                         id == ID_TOOL_MODE_PAN ||
                         id == ID_TOOL_MODE_MAGNIFIER ||
                         id == ID_TOOL_MODE_MARKER_TEXT ||
                         id == ID_TOOL_MODE_MARKER_TEXT_COLOR ||
                         id == ID_TOOL_MODE_MARKER_FREE ||
                         id == ID_TOOL_MODE_MARKER_LINE ||
                         id == ID_TOOL_MODE_MARKER_ARROW ||
                         id == ID_TOOL_MODE_MARKER_WAVE ||
                         id == ID_TOOL_MODE_TEXT ||
                         id == ID_TOOL_MODE_LINE ||
                         id == ID_TOOL_MODE_ARROW ||
                         id == ID_TOOL_MODE_WAVE ||
                         id == ID_TOOL_MODE_FREEHAND ||
                         id == ID_TOOL_MODE_SHAPE ||
                         id == ID_TOOL_MODE_ERASER);
    bool isActiveMode = false;
    if (isModeToggle) {
        if (id == ID_TOOL_MODE_MARKER_TEXT) {
            isActiveMode = IsMarkerGroupMode(g_toolMode);
        } else if (id == ID_TOOL_MODE_FREEHAND) {
            isActiveMode = IsPenGroupMode(g_toolMode);
        } else if (id == ID_TOOL_MODE_SHAPE) {
            isActiveMode = IsShapeGroupMode(g_toolMode);
        } else {
            isActiveMode = (id == ToolModeToCommand(g_toolMode));
        }
    }

    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    COLORREF bg = g_theme.buttonBg;
    COLORREF text = g_theme.buttonText;
    if (pressed) {
        bg = g_theme.buttonPressed;
    } else if (isActiveMode) {
        if (ToolModeUsesPaletteColor(g_toolMode)) {
            COLORREF sel = ToolColorForMode(g_toolMode);
            bg = BlendColor(sel, g_theme.buttonBg, 0.45);
            text = ColorIsDark(bg) ? RGB(255, 255, 255) : RGB(0, 0, 0);
        } else {
            bg = BlendColor(g_theme.buttonHot, g_theme.buttonBg, 0.35);
            text = g_theme.buttonText;
        }
    } else if (!isModeToggle && checked) {
        bg = g_theme.selectionBg;
        text = g_theme.selectionText;
    } else if (id == ID_TOOL_COLOR_CUSTOM && g_activeColor == g_paletteCustomColor) {
        bg = BlendColor(g_activeColor, g_theme.buttonBg, 0.45);
        text = ColorIsDark(bg) ? RGB(255, 255, 255) : RGB(0, 0, 0);
    }
    if (disabled) {
        text = DisabledTextColor(text, bg);
    }

    HBRUSH br = CreateSolidBrush(bg);
    FillRect(paintDc, &rc, br);
    DeleteObject(br);

    HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.buttonBorder);
    HGDIOBJ oldPen = SelectObject(paintDc, borderPen);
    HGDIOBJ oldBrush = SelectObject(paintDc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(paintDc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(paintDc, oldBrush);
    SelectObject(paintDc, oldPen);
    DeleteObject(borderPen);

    std::wstring label;
    wchar_t buf[256]{};
    int len = GetWindowTextW(hWnd, buf, static_cast<int>(std::size(buf) - 1));
    if (len > 0) {
        buf[std::min<int>(len, static_cast<int>(std::size(buf) - 1))] = L'\0';
        label.assign(buf);
    }

    SetBkMode(paintDc, TRANSPARENT);
    SetTextColor(paintDc, text);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hWnd, WM_GETFONT, 0, 0));
    HFONT oldFont = font ? static_cast<HFONT>(SelectObject(paintDc, font)) : nullptr;

    RECT textRc = rc;
    const bool showIndicator = isToggle && !isModeToggle && !isPushlike;
    if (showIndicator) {
        int boxSize = 12;
        int boxLeft = rc.left + 6;
        int boxTop = rc.top + (rc.bottom - rc.top - boxSize) / 2;
        RECT boxRc{ boxLeft, boxTop, boxLeft + boxSize, boxTop + boxSize };
        HBRUSH boxBr = CreateSolidBrush(g_theme.panelBg);
        HBRUSH boxFillOld = static_cast<HBRUSH>(SelectObject(paintDc, boxBr));
        if (isRadio) {
            HPEN pen = CreatePen(PS_SOLID, 1, g_theme.buttonBorder);
            HGDIOBJ oldPen = SelectObject(paintDc, pen);
            Ellipse(paintDc, boxRc.left, boxRc.top, boxRc.right, boxRc.bottom);
            SelectObject(paintDc, oldPen);
            DeleteObject(pen);
        } else {
            FillRect(paintDc, &boxRc, boxBr);
            HBRUSH boxFrame = CreateSolidBrush(g_theme.buttonBorder);
            FrameRect(paintDc, &boxRc, boxFrame);
            DeleteObject(boxFrame);
        }
        SelectObject(paintDc, boxFillOld);
        DeleteObject(boxBr);
        if (checked) {
            if (isRadio) {
                HBRUSH dot = CreateSolidBrush(g_theme.buttonText);
                RECT dotRc{ boxLeft + 3, boxTop + 3, boxLeft + boxSize - 3, boxTop + boxSize - 3 };
                HBRUSH oldDot = static_cast<HBRUSH>(SelectObject(paintDc, dot));
                Ellipse(paintDc, dotRc.left, dotRc.top, dotRc.right, dotRc.bottom);
                SelectObject(paintDc, oldDot);
                DeleteObject(dot);
            } else {
                HPEN mark = CreatePen(PS_SOLID, 2, g_theme.buttonText);
                HGDIOBJ old = SelectObject(paintDc, mark);
                MoveToEx(paintDc, boxLeft + 2, boxTop + boxSize / 2, nullptr);
                LineTo(paintDc, boxLeft + boxSize / 2 - 1, boxTop + boxSize - 3);
                LineTo(paintDc, boxLeft + boxSize - 2, boxTop + 3);
                SelectObject(paintDc, old);
                DeleteObject(mark);
            }
        }
        textRc.left = boxLeft + boxSize + 8;
        DrawTextW(paintDc, label.c_str(), -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        DrawTextW(paintDc, label.c_str(), -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (dis->itemState & ODS_FOCUS) {
        RECT focusRc = rc;
        InflateRect(&focusRc, -3, -3);
        DrawFocusRect(paintDc, &focusRc);
    }
    if (memDc && memBmp) {
        BitBlt(dis->hDC, targetRc.left, targetRc.top, width, height, memDc, 0, 0, SRCCOPY);
    }
    if (oldFont) {
        SelectObject(paintDc, oldFont);
    }
    if (memDc) {
        if (oldBmp) SelectObject(memDc, oldBmp);
        if (memBmp) DeleteObject(memBmp);
        DeleteDC(memDc);
    }
    return true;
}

static std::wstring NormalizeLectureSortMode(const std::wstring& mode) {
    std::wstring m = mode;
    std::transform(m.begin(), m.end(), m.begin(), ::towlower);
    if (m == L"name" || m == L"schedule" || m == L"recent") return m;
    return L"recent";
}

static std::wstring NormalizeSessionSortMode(const std::wstring& mode) {
    std::wstring m = mode;
    std::transform(m.begin(), m.end(), m.begin(), ::towlower);
    if (m == L"numeric_desc" || m == L"desc" || m == L"number_desc") return L"numeric_desc";
    if (m == L"name" || m == L"lexical") return L"name";
    return L"numeric_asc";
}

static std::wstring NormalizeSessionNumberingMode(const std::wstring& mode) {
    std::wstring m = mode;
    std::transform(m.begin(), m.end(), m.begin(), ::towlower);
    if (m == L"max_number" || m == L"max" || m == L"maximum") return L"max_number";
    return L"count";
}

static std::wstring NormalizeSessionAutoOpenMode(const std::wstring& mode) {
    std::wstring m = mode;
    std::transform(m.begin(), m.end(), m.begin(), ::towlower);
    if (m == L"off" || m == L"edit" || m == L"view") return m;
    return L"off";
}

static std::wstring NormalizeLineDashStyle(const std::wstring& mode) {
    std::wstring m = mode;
    std::transform(m.begin(), m.end(), m.begin(), ::towlower);
    if (m == L"dash" || m == L"dashed" || m == L"broken") return L"dash";
    return L"solid";
}

static std::wstring NormalizeSelectionStyle(const std::wstring& style) {
    std::wstring s = style;
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    if (s == L"theme" || s == L"themed" || s == L"tone") return L"theme";
    if (s == L"legacy" || s == L"classic") return L"windows";
    return L"windows";
}

static std::wstring NormalizeShortcutTextTagKey(const std::wstring& key) {
    std::wstring v = key;
    std::transform(v.begin(), v.end(), v.begin(), ::towlower);
    if (v == L"c") return L"c";
    return L"char";
}

static std::wstring NormalizeNoteCustomTagKey(const std::wstring& key) {
    std::wstring v = key;
    std::transform(v.begin(), v.end(), v.begin(), ::towlower);
    if (v.empty()) return L"c";
    for (wchar_t ch : v) {
        if (!(iswalnum(ch) || ch == L'_' || ch == L'-')) return L"c";
    }
    return v;
}

static std::wstring NormalizeNoteBgSource(const std::wstring& source) {
    std::wstring s = source;
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    if (s == L"theme" || s == L"theme_current" || s == L"theme-follow") return L"theme_current";
    if (s == L"theme_named" || s == L"theme_name" || s == L"theme_fixed") return L"theme_named";
    if (s == L"explicit" || s == L"color") return L"explicit";
    return L"explicit";
}

static void UpdateNoteBgColorFromConfig() {
    g_config.noteBgSource = NormalizeNoteBgSource(g_config.noteBgSource);
    if (g_config.noteBgSource == L"theme_current") {
        g_noteBgColor = g_theme.noteBg;
        return;
    }
    if (g_config.noteBgSource == L"theme_named") {
        if (!g_config.noteBgThemeName.empty()) {
            if (const ThemeColors* t = FindThemeByName(g_themeCatalog, g_config.noteBgThemeName)) {
                g_noteBgColor = t->noteBg;
                return;
            }
        }
        g_noteBgColor = g_theme.noteBg;
        return;
    }
    g_noteBgColor = g_config.noteBgColor;
}

static std::wstring NormalizePdfFlowMode(const std::wstring& mode) {
    std::wstring m = mode;
    std::transform(m.begin(), m.end(), m.begin(), ::towlower);
    if (m == L"v_ttb" || m == L"v_btu" || m == L"h_ltr" || m == L"h_rtl") return m;
    return L"v_ttb";
}

static std::wstring NormalizeToneVariant(const std::wstring& value) {
    std::wstring v = value;
    std::transform(v.begin(), v.end(), v.begin(), ::towlower);
    if (v == L"pure") return L"pure";
    if (v == L"guard" || v == L"tone_guard" || v == L"toneguard") return L"guard";
    if (v == L"emphasis" || v == L"accent" || v == L"tone_emphasis" || v == L"toneemphasis") return L"emphasis";
    if (v == L"white") return L"white";
    if (v == L"black") return L"black";
    return L"pure";
}

static void ApplyJsonToWorkspaceConfig(const std::string& json, WorkspaceConfig& cfg) {
    if (auto s = ParseJsonStringField(json, "classesDir")) {
        cfg.classesDir = UTF8ToWide(*s);
    }
    if (auto s = ParseJsonStringField(json, "cacheDir")) {
        cfg.cacheDir = UTF8ToWide(*s);
    }
    if (auto b = ParseJsonBoolField(json, "showAnnots")) {
        cfg.showAnnots = *b;
    }
    if (auto s = ParseJsonStringField(json, "pdfFlowMode")) {
        cfg.pdfFlowMode = NormalizePdfFlowMode(UTF8ToWide(*s));
    }
    if (auto v = ParseJsonIntField(json, "pdfBitmapBudgetMiB")) {
        cfg.pdfBitmapBudgetMiB = std::clamp(*v, kPdfBitmapBudgetMiBMin, kPdfBitmapBudgetMiBMax);
    }
    if (auto b = ParseJsonBoolField(json, "pdfSinglePageMode")) {
        cfg.pdfSinglePageMode = *b;
    }
    if (auto b = ParseJsonBoolField(json, "mouseWheelInvertVertical")) {
        cfg.mouseWheelInvertVertical = *b;
    }
    if (auto b = ParseJsonBoolField(json, "mouseWheelInvertHorizontal")) {
        cfg.mouseWheelInvertHorizontal = *b;
    }
    if (auto b = ParseJsonBoolField(json, "touchpadInvertVertical")) {
        cfg.touchpadInvertVertical = *b;
    }
    if (auto b = ParseJsonBoolField(json, "touchpadInvertHorizontal")) {
        cfg.touchpadInvertHorizontal = *b;
    }
    if (auto b = ParseJsonBoolField(json, "ownerDrawUi")) {
        cfg.ownerDrawUi = *b;
    }
    if (auto b = ParseJsonBoolField(json, "useNativeFileDialogs")) {
        cfg.useNativeFileDialogs = *b;
    }
    if (auto b = ParseJsonBoolField(json, "developerMode")) {
        cfg.developerMode = *b;
    }
    if (auto b = ParseJsonBoolField(json, "studentMode")) {
        cfg.studentMode = *b;
    }
    if (auto b = ParseJsonBoolField(json, "exportStandardTextAnnots")) {
        cfg.exportStandardTextAnnots = *b;
    }
    if (auto b = ParseJsonBoolField(json, "debugLogPreviewTrace")) {
        cfg.debugLogs.previewTrace = *b;
    }
    if (auto b = ParseJsonBoolField(json, "debugLogSwitchTiming")) {
        cfg.debugLogs.switchTiming = *b;
    }
    if (auto b = ParseJsonBoolField(json, "debugLogCrash")) {
        cfg.debugLogs.crash = *b;
    }
    if (auto b = ParseJsonBoolField(json, "debugLogStartupWatchdog")) {
        cfg.debugLogs.startupWatchdog = *b;
    }
    if (auto b = ParseJsonBoolField(json, "debugLogOfficeConversion")) {
        cfg.debugLogs.officeConversion = *b;
    }
    if (auto v = ParseJsonIntField(json, "leftWidth"))  cfg.leftWidth  = *v;
    if (auto v = ParseJsonIntField(json, "rightWidth")) cfg.rightWidth = *v;
    if (auto v = ParseJsonIntField(json, "topHeight"))  cfg.topHeight  = *v;
    if (auto v = ParseJsonIntField(json, "windowWidth")) {
        if (*v > 0) cfg.windowWidth = *v;
    }
    if (auto v = ParseJsonIntField(json, "windowHeight")) {
        if (*v > 0) cfg.windowHeight = *v;
    }
    if (auto v = ParseJsonIntField(json, "defaultWindowWidth")) {
        if (*v > 0) cfg.defaultWindowWidth = *v;
    }
    if (auto v = ParseJsonIntField(json, "defaultWindowHeight")) {
        if (*v > 0) cfg.defaultWindowHeight = *v;
    }
    if (auto v = ParseJsonIntField(json, "leftSplit1")) cfg.leftSplit1 = *v;
    if (auto v = ParseJsonIntField(json, "leftSplit2")) cfg.leftSplit2 = *v;
    if (auto v = ParseJsonIntField(json, "defaultLeftWidth"))  cfg.defaultLeftWidth  = *v;
    if (auto v = ParseJsonIntField(json, "defaultRightWidth")) cfg.defaultRightWidth = *v;
    if (auto v = ParseJsonIntField(json, "defaultTopHeight"))  cfg.defaultTopHeight  = *v;
    if (auto v = ParseJsonIntField(json, "defaultLeftSplit1")) cfg.defaultLeftSplit1 = *v;
    if (auto v = ParseJsonIntField(json, "defaultLeftSplit2")) cfg.defaultLeftSplit2 = *v;
    if (auto b = ParseJsonBoolField(json, "leftPaneCollapsed")) cfg.leftPaneCollapsed = *b;
    if (auto s = ParseJsonStringField(json, "language")) {
        cfg.language = UTF8ToWide(*s);
    }
    if (auto v = ParseJsonIntField(json, "markFontPx")) cfg.markFontPx = *v;
    if (auto v = ParseJsonIntField(json, "headingFontPx")) cfg.headingFontPx = *v;
    if (auto c = ParseJsonColor(json, "markColor")) cfg.markColor = *c;
    if (auto c = ParseJsonColor(json, "headingColor")) cfg.headingColor = *c;
    if (auto b = ParseJsonBoolField(json, "headingBold")) cfg.headingBold = *b;
    if (auto b = ParseJsonBoolField(json, "headingUnderline")) cfg.headingUnderline = *b;
    if (auto b = ParseJsonBoolField(json, "headingLeftBar")) cfg.headingLeftBar = *b;
    if (auto s = ParseJsonStringField(json, "bottomPanePin")) {
        cfg.bottomPanePin = UTF8ToWide(*s);
    }
    if (auto s = ParseJsonStringField(json, "bottomNoteMode")) {
        cfg.bottomNoteMode = UTF8ToWide(*s);
    }
    if (auto s = ParseJsonStringField(json, "notePlacement")) {
        cfg.notePlacement = NotePlacementToString(ParseNotePlacement(UTF8ToWide(*s)));
    }
    if (auto s = ParseJsonStringField(json, "colorTone")) {
        cfg.colorTone = UTF8ToWide(*s);
    }
    if (auto s = ParseJsonStringField(json, "toneVariant")) {
        cfg.toneVariant = NormalizeToneVariant(UTF8ToWide(*s));
    }
    if (auto s = ParseJsonStringField(json, "quickAnnotPopupPlacement")) {
        cfg.quickAnnotPopupPlacement = UTF8ToWide(*s);
    }
    if (auto s = ParseJsonStringField(json, "lectureSortMode")) {
        cfg.lectureSortMode = NormalizeLectureSortMode(UTF8ToWide(*s));
    }
    if (auto s = ParseJsonStringField(json, "sessionSortMode")) {
        cfg.sessionSortMode = NormalizeSessionSortMode(UTF8ToWide(*s));
    }
    if (auto s = ParseJsonStringField(json, "sessionNumberingMode")) {
        cfg.sessionNumberingMode = NormalizeSessionNumberingMode(UTF8ToWide(*s));
    }
    if (auto s = ParseJsonStringField(json, "sessionAutoOpenMode")) {
        cfg.sessionAutoOpenMode = NormalizeSessionAutoOpenMode(UTF8ToWide(*s));
    }
    if (auto b = ParseJsonBoolField(json, "sessionAutoOpenPairLinked")) {
        cfg.sessionAutoOpenPairLinked = *b;
    }
    if (auto s = ParseJsonStringField(json, "selectionStyle")) {
        cfg.selectionStyle = NormalizeSelectionStyle(UTF8ToWide(*s));
    }
    if (auto v = ParseJsonIntField(json, "pointerOffsetX")) {
        cfg.pointerOffsetX = std::clamp(*v, -20, 20);
    }
    if (auto v = ParseJsonIntField(json, "pointerOffsetY")) {
        cfg.pointerOffsetY = std::clamp(*v, -20, 20);
    }
    if (auto b = ParseJsonBoolField(json, "showMathList")) {
        cfg.showMathList = *b;
    }
    bool hasDownKeyAction = false;
    if (auto s = ParseJsonStringField(json, "downKeyLastLineAction")) {
        cfg.downKeyLastLineAction = DownKeyLastLineActionToString(
            ParseDownKeyLastLineAction(UTF8ToWide(*s)));
        hasDownKeyAction = true;
    }
    if (!hasDownKeyAction) {
        if (auto b = ParseJsonBoolField(json, "downKeyLastLineInsertNewline")) {
            cfg.downKeyLastLineAction = *b ? L"newline" : L"lineend";
        }
    }
    if (auto s = ParseJsonStringField(json, "leftRightLineMoveAction")) {
        cfg.leftRightLineMoveAction = LeftRightLineMoveActionToString(
            ParseLeftRightLineMoveAction(UTF8ToWide(*s)));
    }
    if (auto b = ParseJsonBoolField(json, "autoPairBrackets")) {
        cfg.autoPairBrackets = *b;
    }
    if (auto b = ParseJsonBoolField(json, "fullWidthParenCaretInside")) {
        cfg.fullWidthParenCaretInside = *b;
    }
    if (auto b = ParseJsonBoolField(json, "fullWidthParenCancelNextLeft")) {
        cfg.fullWidthParenCancelNextLeft = *b;
    }
    if (auto v = ParseJsonDoubleField(json, "noteFontPt")) {
        cfg.noteFontPt = std::clamp(*v, 6.0, 32.0);
    }
    if (auto s = ParseJsonStringField(json, "noteFontName")) {
        cfg.noteFontName = UTF8ToWide(*s);
    }
    if (auto v = ParseJsonDoubleField(json, "noteRenderFontPt")) {
        cfg.noteRenderFontPt = std::clamp(*v, 6.0, 32.0);
    }
    if (auto s = ParseJsonStringField(json, "noteRenderFontName")) {
        cfg.noteRenderFontName = UTF8ToWide(*s);
    }
    if (auto s = ParseJsonStringField(json, "noteRenderJpFontName")) {
        cfg.noteRenderJpFontName = UTF8ToWide(*s);
    }
    if (auto s = ParseJsonStringField(json, "noteSystem")) {
        cfg.noteSystem = UTF8ToWide(*s);
    }
    if (auto b = ParseJsonBoolField(json, "noteRenderEnabled")) {
        cfg.noteRenderEnabled = *b;
    }
    if (auto b = ParseJsonBoolField(json, "noteRawOnly")) {
        cfg.noteRawOnly = *b;
    }
    if (auto b = ParseJsonBoolField(json, "noteRenderMath")) {
        cfg.noteRenderMath = *b;
    }
    if (auto b = ParseJsonBoolField(json, "noteWrapEnabled")) {
        cfg.noteWrapEnabled = *b;
    }
    if (auto b = ParseJsonBoolField(json, "noteVimModeEnabled")) {
        cfg.noteVimModeEnabled = *b;
    }
    if (auto b = ParseJsonBoolField(json, "noteVimCaretLineRawTextVisible")) {
        cfg.noteVimCaretLineRawTextVisible = *b;
    }
    if (auto b = ParseJsonBoolField(json, "noteVimClickEntersInsertMode")) {
        cfg.noteVimClickEntersInsertMode = *b;
    }
    if (auto v = ParseJsonIntField(json, "noteMathMarginTopPercent")) {
        cfg.noteMathMarginTopPercent = std::clamp(*v, 5, 95);
    }
    if (auto v = ParseJsonIntField(json, "noteMathSupSubGapSupPercent")) {
        cfg.noteMathSupSubGapSupPercent = (*v <= 0) ? 0 : std::clamp(*v, 5, 95);
    }
    if (auto b = ParseJsonBoolField(json, "noteGridEnabled")) {
        cfg.noteGridEnabled = *b;
    }
    if (auto v = ParseJsonIntField(json, "noteGridPitch")) {
        cfg.noteGridPitch = std::clamp(*v, 4, 256);
    }
    if (auto c = ParseJsonColorField(json, "noteBgColor")) cfg.noteBgColor = *c;
    if (auto c = ParseJsonColorField(json, "noteFgColor")) cfg.noteFgColor = *c;
    if (auto s = ParseJsonStringField(json, "noteBgSource")) {
        cfg.noteBgSource = NormalizeNoteBgSource(UTF8ToWide(*s));
    }
    if (auto s = ParseJsonStringField(json, "noteBgThemeName")) {
        cfg.noteBgThemeName = UTF8ToWide(*s);
    }
    if (auto c = ParseJsonColorField(json, "noteShortcutBackColor")) cfg.noteShortcutBackColor = *c;
    if (auto c = ParseJsonColorField(json, "noteShortcutTextColor")) cfg.noteShortcutTextColor = *c;
    if (auto s = ParseJsonStringField(json, "noteShortcutTextTagKey")) {
        cfg.noteShortcutTextTagKey = NormalizeShortcutTextTagKey(UTF8ToWide(*s));
    }
    if (auto b = ParseJsonBoolField(json, "noteShortcutHeadingArrowInvert")) {
        cfg.noteShortcutHeadingArrowInvert = *b;
    }
    if (auto s = ParseJsonStringField(json, "noteCustomTagKey")) {
        cfg.noteCustomTagKey = NormalizeNoteCustomTagKey(UTF8ToWide(*s));
    }
    if (auto b = ParseJsonBoolField(json, "noteCustomTagBold")) {
        cfg.noteCustomTagBold = *b;
    }
    if (auto b = ParseJsonBoolField(json, "noteCustomTagItalic")) {
        cfg.noteCustomTagItalic = *b;
    }
    if (auto b = ParseJsonBoolField(json, "noteCustomTagUnderline")) {
        cfg.noteCustomTagUnderline = *b;
    }
    if (auto b = ParseJsonBoolField(json, "noteCustomTagStrike")) {
        cfg.noteCustomTagStrike = *b;
    }
    if (auto b = ParseJsonBoolField(json, "noteCustomTagBackColor")) {
        cfg.noteCustomTagBackColor = *b;
    }
    if (auto b = ParseJsonBoolField(json, "noteCustomTagTextColor")) {
        cfg.noteCustomTagTextColor = *b;
    }
    for (int i = 0; i < kNoteCustomTagPresetCount; ++i) {
        std::string key = "noteCustomTag" + std::to_string(i + 1) + "Mask";
        if (auto v = ParseJsonIntField(json, key.c_str())) {
            cfg.noteCustomTagPresetMasks[static_cast<size_t>(i)] =
                (*v & kNoteCustomTagStyleAllMask);
        }
    }
    if (auto v = ParseJsonIntField(json, "noteFontCustomization")) {
        cfg.noteFontCustomization = std::clamp(*v, 0, 2);
    }
    if (auto v = ParseJsonIntField(json, "noteFontDigitTarget")) {
        cfg.noteFontDigitTarget = std::clamp(*v, 0, 1);
    }
    if (auto v = ParseJsonIntField(json, "autoSaveSeconds")) {
        cfg.autoSaveSeconds = NormalizeAutoStageSaveSeconds(*v);
    }
    if (auto v = ParseJsonIntField(json, "autoIntegrateSeconds")) {
        cfg.autoIntegrateSeconds = *v;
    }
    if (auto v = ParseJsonIntField(json, "autoIntegrateCustomMinutes")) {
        cfg.autoIntegrateCustomMinutes = std::clamp(*v, kAutoIntegrateCustomMinutesMin, kAutoIntegrateCustomMinutesMax);
    }
    if (auto s = ParseJsonStringField(json, "clroNamePattern")) {
        cfg.clroNamePattern = UTF8ToWide(*s);
    }
    if (auto s = ParseJsonStringField(json, "textFontName")) {
        cfg.textFontName = UTF8ToWide(*s);
    }
    if (auto v = ParseJsonDoubleField(json, "textFontPt")) {
        cfg.textFontPt = std::clamp(*v, 6.0, 96.0);
        cfg.textFontPtSlotA = cfg.textFontPt;
    }
    if (auto b = ParseJsonBoolField(json, "textFontUseA4Scale")) {
        cfg.textFontUseA4Scale = *b;
        cfg.textFontUseA4ScaleSlotA = cfg.textFontUseA4Scale;
    }
    if (auto v = ParseJsonIntField(json, "textFontActiveSizeSlot")) {
        cfg.textFontActiveSizeSlot = std::clamp(*v, 0, 1);
    }
    if (auto v = ParseJsonDoubleField(json, "textFontPtSlotA")) {
        cfg.textFontPtSlotA = std::clamp(*v, 6.0, 96.0);
    }
    if (auto v = ParseJsonDoubleField(json, "textFontPtSlotB")) {
        cfg.textFontPtSlotB = std::clamp(*v, 6.0, 96.0);
    }
    if (auto b = ParseJsonBoolField(json, "textFontUseA4ScaleSlotA")) {
        cfg.textFontUseA4ScaleSlotA = *b;
    }
    if (auto b = ParseJsonBoolField(json, "textFontUseA4ScaleSlotB")) {
        cfg.textFontUseA4ScaleSlotB = *b;
    }
    if (auto b = ParseJsonBoolField(json, "textBoxReadableBackground")) {
        cfg.textBoxReadableBackground = *b;
    }
    if (auto b = ParseJsonBoolField(json, "textBoxReadableBackgroundInverted")) {
        cfg.textBoxReadableBackgroundInverted = *b;
    }
    if (cfg.textFontActiveSizeSlot == 1) {
        cfg.textFontPt = cfg.textFontPtSlotB;
        cfg.textFontUseA4Scale = cfg.textFontUseA4ScaleSlotB;
    } else {
        cfg.textFontPt = cfg.textFontPtSlotA;
        cfg.textFontUseA4Scale = cfg.textFontUseA4ScaleSlotA;
    }
    if (auto b = ParseJsonBoolField(json, "lineToolsShareStyle")) {
        cfg.lineToolsShareStyle = *b;
    }
    if (auto v = ParseJsonDoubleField(json, "lineWidthPt")) {
        cfg.lineWidthPt = std::clamp(*v, 0.5, 24.0);
    }
    if (auto v = ParseJsonDoubleField(json, "arrowWidthPt")) {
        cfg.arrowWidthPt = std::clamp(*v, 0.5, 24.0);
    }
    if (auto s = ParseJsonStringField(json, "arrowHead")) {
        cfg.arrowHead = ArrowHeadToString(ParseArrowHead(UTF8ToWide(*s)));
    }
    if (auto v = ParseJsonDoubleField(json, "waveWidthPt")) {
        cfg.waveWidthPt = std::clamp(*v, 0.5, 24.0);
    }
    if (auto s = ParseJsonStringField(json, "lineDashStyle")) {
        cfg.lineDashStyle = NormalizeLineDashStyle(UTF8ToWide(*s));
    }
    if (auto v = ParseJsonDoubleField(json, "freehandWidthPt")) {
        cfg.freehandWidthPt = std::clamp(*v, 0.5, 24.0);
    }
    if (auto v = ParseJsonDoubleField(json, "markerFreeWidthPt")) {
        cfg.markerFreeWidthPt = std::clamp(*v, 1.0, 80.0);
    }
    if (auto v = ParseJsonDoubleField(json, "markerTextWidthPt")) {
        cfg.markerTextWidthPt = std::clamp(*v, 1.0, 80.0);
    }
    if (auto b = ParseJsonBoolField(json, "markerTextUnderline")) {
        cfg.markerTextUnderline = *b;
    }
    if (auto v = ParseJsonDoubleField(json, "eraserWidthPt")) {
        cfg.eraserWidthPt = std::clamp(*v, 1.0, 80.0);
    }
    if (auto v = ParseJsonDoubleField(json, "markerAlpha")) {
        cfg.markerAlpha = std::clamp(*v, 0.05, 1.0);
    }
    if (auto v = ParseJsonDoubleField(json, "lineAlpha")) {
        cfg.lineAlpha = std::clamp(*v, 0.05, 1.0);
    }
    if (auto v = ParseJsonDoubleField(json, "arrowAlpha")) {
        cfg.arrowAlpha = std::clamp(*v, 0.05, 1.0);
    }
    if (auto v = ParseJsonDoubleField(json, "waveAlpha")) {
        cfg.waveAlpha = std::clamp(*v, 0.05, 1.0);
    }
    if (auto v = ParseJsonDoubleField(json, "freehandAlpha")) {
        cfg.freehandAlpha = std::clamp(*v, 0.05, 1.0);
    }
    if (auto v = ParseJsonDoubleField(json, "shapeAlpha")) {
        cfg.shapeAlpha = std::clamp(*v, 0.0, 1.0);
    }
    if (auto c = ParseJsonColorField(json, "textColor")) cfg.textColor = *c;
    if (auto c = ParseJsonColorField(json, "lineColor")) cfg.lineColor = *c;
    if (auto c = ParseJsonColorField(json, "arrowColor")) {
        cfg.arrowColor = *c;
    }
    if (auto c = ParseJsonColorField(json, "waveColor")) {
        cfg.waveColor = *c;
    }
    if (auto c = ParseJsonColorField(json, "freehandColor")) cfg.freehandColor = *c;
    if (auto c = ParseJsonColorField(json, "markerFreeColor")) cfg.markerFreeColor = *c;
    if (auto c = ParseJsonColorField(json, "markerTextColor")) cfg.markerTextColor = *c;
    if (auto c = ParseJsonColorField(json, "shapeColor")) cfg.shapeColor = *c;
    if (auto c = ParseJsonColorField(json, "paletteCustomColor")) cfg.paletteCustomColor = *c;
    if (auto s = ParseJsonStringField(json, "magnifierShape")) {
        cfg.magnifierShape = MagnifierShapeToString(ParseMagnifierShape(UTF8ToWide(*s)));
    }
    const bool hasShapeDetail = ParseJsonStringField(json, "shapeDetail").has_value();
    if (auto s = ParseJsonStringField(json, "shapeDetail")) {
        ShapeDetail parsed{};
        if (ShapeDetailFromKey(*s, parsed)) {
            cfg.shapeDetail = UTF8ToWide(ShapeDetailKey(parsed));
        }
    }
    if (auto s = ParseJsonStringField(json, "shapeKind")) {
        cfg.shapeKind = ShapeKindToString(ParseShapeKind(UTF8ToWide(*s)));
    }
    if (auto s = ParseJsonStringField(json, "shapeDrawMode")) {
        cfg.shapeDrawMode = ShapeDrawModeToString(ParseShapeDrawMode(UTF8ToWide(*s)));
    }
    if (auto s = ParseJsonStringField(json, "annotLastMarkerDetail")) cfg.annotLastMarkerDetail = UTF8ToWide(*s);
    if (auto s = ParseJsonStringField(json, "annotLastPenDetail")) cfg.annotLastPenDetail = UTF8ToWide(*s);
    if (auto s = ParseJsonStringField(json, "annotLastShapePresentation")) cfg.annotLastShapePresentation = UTF8ToWide(*s);
    if (auto s = ParseJsonStringField(json, "annotLastShapeGeometry")) cfg.annotLastShapeGeometry = UTF8ToWide(*s);
    if (auto s = ParseJsonStringField(json, "annotLastShapeDetail")) cfg.annotLastShapeDetail = UTF8ToWide(*s);
    const auto legacyDetailKey = [](const std::optional<int>& value, const wchar_t* fallback) {
        if (!value) return std::wstring(fallback);
        switch (*value) {
        case static_cast<int>(ToolMode::MarkerText): return std::wstring(L"marker_text");
        case static_cast<int>(ToolMode::MarkerTextUnderline): return std::wstring(L"marker_text_underline");
        case static_cast<int>(ToolMode::MarkerTextColor): return std::wstring(L"marker_text_color");
        case static_cast<int>(ToolMode::MarkerFree): return std::wstring(L"marker_free");
        case static_cast<int>(ToolMode::MarkerLine): return std::wstring(L"marker_line");
        case static_cast<int>(ToolMode::MarkerArrow): return std::wstring(L"marker_arrow");
        case static_cast<int>(ToolMode::MarkerWave): return std::wstring(L"marker_wave");
        case static_cast<int>(ToolMode::Freehand): return std::wstring(L"freehand");
        case static_cast<int>(ToolMode::Line): return std::wstring(L"line");
        case static_cast<int>(ToolMode::Arrow): return std::wstring(L"arrow");
        case static_cast<int>(ToolMode::Wave): return std::wstring(L"wave");
        case static_cast<int>(ToolMode::Shape): return std::wstring(L"shape");
        default: return std::wstring(fallback);
        }
    };
    if (!ParseJsonStringField(json, "annotLastMarkerDetail")) {
        cfg.annotLastMarkerDetail = legacyDetailKey(ParseJsonIntField(json, "annotLastMarkerMode"), L"marker_text");
    }
    if (!ParseJsonStringField(json, "annotLastPenDetail")) {
        cfg.annotLastPenDetail = legacyDetailKey(ParseJsonIntField(json, "annotLastPenMode"), L"freehand");
    }
    if (!ParseJsonStringField(json, "annotLastShapeDetail")) {
        cfg.annotLastShapeDetail = legacyDetailKey(ParseJsonIntField(json, "annotLastShapeMode"), L"line");
    }
    const bool hasShapePresentation = ParseJsonStringField(json, "annotLastShapePresentation").has_value();
    const bool hasShapeGeometry = ParseJsonStringField(json, "annotLastShapeGeometry").has_value();
    if (!hasShapePresentation || !hasShapeGeometry) {
        ToolMode legacyShapeMode = ToolMode::Line;
        if (!AnnotToolModeFromKey(WideToUTF8(cfg.annotLastShapeDetail), legacyShapeMode) ||
            AnnotToolFamilyForMode(legacyShapeMode) != AnnotToolFamily::Shape) {
            legacyShapeMode = ToolMode::Line;
        }
        const ShapeToolSelection selection = ShapeToolSelectionForMode(
            legacyShapeMode, ParseShapeDrawMode(cfg.shapeDrawMode));
        cfg.annotLastShapePresentation = UTF8ToWide(ShapeToolPresentationKey(selection.presentation));
        cfg.annotLastShapeGeometry = UTF8ToWide(ShapeToolGeometryKey(selection.geometry));
    }
    if (!hasShapeDetail) {
        ToolMode legacyShapeMode = ToolMode::Line;
        if (!AnnotToolModeFromKey(WideToUTF8(cfg.annotLastShapeDetail), legacyShapeMode) ||
            AnnotToolFamilyForMode(legacyShapeMode) != AnnotToolFamily::Shape) {
            legacyShapeMode = ToolMode::Line;
        }
        cfg.shapeDetail = UTF8ToWide(ShapeDetailKey(
            ShapeDetailForLegacyState(legacyShapeMode, ParseShapeKind(cfg.shapeKind))));
    }
    if (auto s = ParseJsonStringField(json, "freehandCorrection")) {
        cfg.freehandCorrection = NormalizeFreehandCorrection(UTF8ToWide(*s));
    }
    if (auto s = ParseJsonStringField(json, "freehandCorrectionStyle")) {
        cfg.freehandCorrectionStyle = NormalizeFreehandCorrectionStyle(UTF8ToWide(*s));
    }
    if (auto s = ParseJsonStringField(json, "freehandCorrectionFill")) {
        cfg.freehandCorrectionFill = NormalizeFreehandCorrectionFill(UTF8ToWide(*s));
    }
    auto scheduleMask = ParseJsonIntField(json, "scheduleDayMask");
    if (scheduleMask) {
        cfg.scheduleDayMask = *scheduleMask;
    }
    cfg.scheduleDayMask &= 0x7F;
    if (cfg.scheduleDayMask == 0) cfg.scheduleDayMask = 0x1F;
    if (auto v = ParseJsonIntField(json, "schedulePeriods")) {
        cfg.schedulePeriods = std::clamp(*v, 1, 13);
    }
    // NOTE: shortcuts are no longer loaded from workspace.json.

}

const std::vector<AnnotToolDetailDescriptor>& AnnotToolDetailDescriptors() {
    static const std::vector<AnnotToolDetailDescriptor> descriptors = [] {
        std::vector<AnnotToolDetailDescriptor> out;
        out.reserve(kToolModeCount);
        for (ToolMode mode : DefaultAnnotToolModeUiOrder()) {
            AnnotToolOptionGroup optionGroup = AnnotToolOptionGroup::None;
            AnnotToolGeometry geometry = AnnotToolGeometry::None;
            AnnotToolPresentation presentation = AnnotToolPresentation::None;
            switch (mode) {
            case ToolMode::Magnifier: optionGroup = AnnotToolOptionGroup::Magnifier; break;
            case ToolMode::TextBox: optionGroup = AnnotToolOptionGroup::Text; break;
            case ToolMode::MarkerText:
            case ToolMode::MarkerTextUnderline:
            case ToolMode::MarkerTextColor: optionGroup = AnnotToolOptionGroup::MarkerText; break;
            case ToolMode::MarkerFree: optionGroup = AnnotToolOptionGroup::MarkerFree; break;
            case ToolMode::MarkerLine:
                optionGroup = AnnotToolOptionGroup::Line;
                geometry = AnnotToolGeometry::Line;
                presentation = AnnotToolPresentation::Emphasis;
                break;
            case ToolMode::MarkerArrow:
                optionGroup = AnnotToolOptionGroup::Arrow;
                geometry = AnnotToolGeometry::Arrow;
                presentation = AnnotToolPresentation::Emphasis;
                break;
            case ToolMode::MarkerWave:
                optionGroup = AnnotToolOptionGroup::Wave;
                geometry = AnnotToolGeometry::Wave;
                presentation = AnnotToolPresentation::Emphasis;
                break;
            case ToolMode::Line:
                optionGroup = AnnotToolOptionGroup::Line;
                geometry = AnnotToolGeometry::Line;
                presentation = AnnotToolPresentation::Stroke;
                break;
            case ToolMode::Arrow:
                optionGroup = AnnotToolOptionGroup::Arrow;
                geometry = AnnotToolGeometry::Arrow;
                presentation = AnnotToolPresentation::Stroke;
                break;
            case ToolMode::Wave:
                optionGroup = AnnotToolOptionGroup::Wave;
                geometry = AnnotToolGeometry::Wave;
                presentation = AnnotToolPresentation::Stroke;
                break;
            case ToolMode::Freehand: optionGroup = AnnotToolOptionGroup::Pen; break;
            case ToolMode::Shape:
                optionGroup = AnnotToolOptionGroup::Shape;
                geometry = AnnotToolGeometry::Shape;
                break;
            default: break;
            }
            out.push_back({mode, AnnotToolFamilyForMode(mode), ToolModeKey(mode), optionGroup, geometry, presentation});
        }
        return out;
    }();
    return descriptors;
}

const AnnotToolDetailDescriptor* FindAnnotToolDetailDescriptor(ToolMode mode) {
    const auto& descriptors = AnnotToolDetailDescriptors();
    auto it = std::find_if(descriptors.begin(), descriptors.end(), [mode](const AnnotToolDetailDescriptor& descriptor) {
        return descriptor.mode == mode;
    });
    return it == descriptors.end() ? nullptr : &*it;
}

AnnotToolGeometry AnnotToolGeometryForMode(ToolMode mode) {
    const auto* descriptor = FindAnnotToolDetailDescriptor(mode);
    return descriptor ? descriptor->geometry : AnnotToolGeometry::None;
}

AnnotToolPresentation AnnotToolPresentationForMode(ToolMode mode, ShapeDrawMode shapeDrawMode) {
    if (mode == ToolMode::Shape) {
        return shapeDrawMode == ShapeDrawMode::Fill
            ? AnnotToolPresentation::Emphasis
            : AnnotToolPresentation::Stroke;
    }
    const auto* descriptor = FindAnnotToolDetailDescriptor(mode);
    return descriptor ? descriptor->presentation : AnnotToolPresentation::None;
}

ShapeToolSelection ShapeToolSelectionForMode(ToolMode mode, ShapeDrawMode shapeDrawMode) {
    return { AnnotToolPresentationForMode(mode, shapeDrawMode), AnnotToolGeometryForMode(mode) };
}

std::optional<ToolMode> ToolModeForShapeToolSelection(ShapeToolSelection selection) {
    if (selection.presentation != AnnotToolPresentation::Stroke &&
        selection.presentation != AnnotToolPresentation::Emphasis) {
        return std::nullopt;
    }
    switch (selection.geometry) {
    case AnnotToolGeometry::Line:
        return selection.presentation == AnnotToolPresentation::Emphasis ? ToolMode::MarkerLine : ToolMode::Line;
    case AnnotToolGeometry::Wave:
        return selection.presentation == AnnotToolPresentation::Emphasis ? ToolMode::MarkerWave : ToolMode::Wave;
    case AnnotToolGeometry::Arrow:
        return selection.presentation == AnnotToolPresentation::Emphasis ? ToolMode::MarkerArrow : ToolMode::Arrow;
    case AnnotToolGeometry::Shape:
        return ToolMode::Shape;
    default:
        return std::nullopt;
    }
}

const char* ShapeToolPresentationKey(AnnotToolPresentation presentation) {
    switch (presentation) {
    case AnnotToolPresentation::Stroke: return "stroke";
    case AnnotToolPresentation::Emphasis: return "emphasis";
    default: return "stroke";
    }
}

bool ShapeToolPresentationFromKey(const std::string& raw, AnnotToolPresentation& out) {
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (key == "stroke") { out = AnnotToolPresentation::Stroke; return true; }
    if (key == "emphasis") { out = AnnotToolPresentation::Emphasis; return true; }
    return false;
}

const char* ShapeToolGeometryKey(AnnotToolGeometry geometry) {
    switch (geometry) {
    case AnnotToolGeometry::Line: return "line";
    case AnnotToolGeometry::Wave: return "wave";
    case AnnotToolGeometry::Arrow: return "arrow";
    case AnnotToolGeometry::Shape: return "shape";
    default: return "line";
    }
}

bool ShapeToolGeometryFromKey(const std::string& raw, AnnotToolGeometry& out) {
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (key == "line") { out = AnnotToolGeometry::Line; return true; }
    if (key == "wave") { out = AnnotToolGeometry::Wave; return true; }
    if (key == "arrow") { out = AnnotToolGeometry::Arrow; return true; }
    if (key == "shape") { out = AnnotToolGeometry::Shape; return true; }
    return false;
}

std::vector<AnnotToolGeometry> OrderedShapeToolGeometries() {
    std::vector<AnnotToolGeometry> out;
    for (ToolMode mode : AnnotToolModeUiOrder()) {
        const AnnotToolGeometry geometry = AnnotToolGeometryForMode(mode);
        if (geometry == AnnotToolGeometry::None) continue;
        if (std::find(out.begin(), out.end(), geometry) == out.end()) out.push_back(geometry);
    }
    return out;
}

const char* ShapeDetailKey(ShapeDetail detail) {
    switch (detail) {
    case ShapeDetail::Line: return "line";
    case ShapeDetail::Arrow: return "arrow";
    case ShapeDetail::Wave: return "wave";
    case ShapeDetail::Rectangle: return "rectangle";
    case ShapeDetail::Ellipse: return "ellipse";
    case ShapeDetail::Triangle: return "triangle";
    case ShapeDetail::Diamond: return "diamond";
    default: return "line";
    }
}

bool ShapeDetailFromKey(const std::string& raw, ShapeDetail& out) {
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (key == "line" || key == "marker_line") { out = ShapeDetail::Line; return true; }
    if (key == "arrow" || key == "marker_arrow") { out = ShapeDetail::Arrow; return true; }
    if (key == "wave" || key == "marker_wave") { out = ShapeDetail::Wave; return true; }
    if (key == "shape" || key == "rect" || key == "rectangle" || key == "square") {
        out = ShapeDetail::Rectangle;
        return true;
    }
    if (key == "ellipse" || key == "circle" || key == "rotated_ellipse") {
        out = ShapeDetail::Ellipse;
        return true;
    }
    if (key == "triangle" || key == "equilateral_triangle") {
        out = ShapeDetail::Triangle;
        return true;
    }
    if (key == "diamond") { out = ShapeDetail::Diamond; return true; }
    return false;
}

std::vector<ShapeDetail> OrderedShapeDetails() {
    return {
        ShapeDetail::Line,
        ShapeDetail::Arrow,
        ShapeDetail::Wave,
        ShapeDetail::Rectangle,
        ShapeDetail::Ellipse,
        ShapeDetail::Triangle,
        ShapeDetail::Diamond,
    };
}

bool ShapeDetailIsLinear(ShapeDetail detail) {
    return detail == ShapeDetail::Line ||
           detail == ShapeDetail::Arrow ||
           detail == ShapeDetail::Wave;
}

bool ShapeDetailIsClosed(ShapeDetail detail) {
    return !ShapeDetailIsLinear(detail);
}

ToolMode ToolModeForShapeDetail(ShapeDetail detail) {
    switch (detail) {
    case ShapeDetail::Line: return ToolMode::Line;
    case ShapeDetail::Arrow: return ToolMode::Arrow;
    case ShapeDetail::Wave: return ToolMode::Wave;
    case ShapeDetail::Rectangle:
    case ShapeDetail::Ellipse:
    case ShapeDetail::Triangle:
    case ShapeDetail::Diamond:
        return ToolMode::Shape;
    default:
        return ToolMode::Line;
    }
}

ShapeKind ShapeKindForShapeDetail(ShapeDetail detail) {
    switch (detail) {
    case ShapeDetail::Ellipse: return ShapeKind::Ellipse;
    case ShapeDetail::Triangle: return ShapeKind::Triangle;
    case ShapeDetail::Diamond: return ShapeKind::Diamond;
    case ShapeDetail::Rectangle:
    default:
        return ShapeKind::Rectangle;
    }
}

ShapeDetail ShapeDetailForLegacyState(ToolMode mode, ShapeKind kind) {
    switch (mode) {
    case ToolMode::MarkerLine:
    case ToolMode::Line:
        return ShapeDetail::Line;
    case ToolMode::MarkerArrow:
    case ToolMode::Arrow:
        return ShapeDetail::Arrow;
    case ToolMode::MarkerWave:
    case ToolMode::Wave:
        return ShapeDetail::Wave;
    case ToolMode::Shape:
        switch (kind) {
        case ShapeKind::Ellipse:
        case ShapeKind::Circle:
        case ShapeKind::RotatedEllipse:
            return ShapeDetail::Ellipse;
        case ShapeKind::Triangle:
        case ShapeKind::EquilateralTriangle:
            return ShapeDetail::Triangle;
        case ShapeKind::Diamond:
            return ShapeDetail::Diamond;
        case ShapeKind::Rectangle:
        case ShapeKind::Square:
        default:
            return ShapeDetail::Rectangle;
        }
    default:
        return ShapeDetail::Line;
    }
}

void SyncLegacyShapeStateFromDetail() {
    if (ShapeDetailIsClosed(g_shapeDetail)) {
        g_shapeKind = ShapeKindForShapeDetail(g_shapeDetail);
    }
    g_shapeGroupMode = ToolModeForShapeDetail(g_shapeDetail);
    g_shapeToolSelection = ShapeToolSelectionForMode(g_shapeGroupMode, g_shapeDrawMode);
}

bool AnnotToolUsesGeometry(ToolMode mode, AnnotToolGeometry geometry) {
    return geometry != AnnotToolGeometry::None && AnnotToolGeometryForMode(mode) == geometry;
}

bool IsLineLikeAnnotToolMode(ToolMode mode) {
    const AnnotToolGeometry geometry = AnnotToolGeometryForMode(mode);
    return geometry == AnnotToolGeometry::Line ||
           geometry == AnnotToolGeometry::Wave ||
           geometry == AnnotToolGeometry::Arrow;
}

bool IsArrowAnnotToolMode(ToolMode mode) {
    return AnnotToolUsesGeometry(mode, AnnotToolGeometry::Arrow);
}

bool IsWaveAnnotToolMode(ToolMode mode) {
    return AnnotToolUsesGeometry(mode, AnnotToolGeometry::Wave);
}

bool IsEmphasisAnnotToolMode(ToolMode mode, ShapeDrawMode shapeDrawMode) {
    return AnnotToolPresentationForMode(mode, shapeDrawMode) == AnnotToolPresentation::Emphasis;
}

static void NormalizeCacheDir(const std::filesystem::path& root, WorkspaceConfig& cfg) {
    if (root.empty()) return;

    const std::wstring originalCacheDir = cfg.cacheDir;
    const cache_dir_policy::CacheDirDecision decision =
        cache_dir_policy::ResolveCacheDirDecision(cfg.cacheDir);

    if (decision == cache_dir_policy::CacheDirDecision::UnsafeCustom) {
        // P0 data-loss guard: never treat arbitrary folders such as "bin" as app-managed cache.
        // Keep their contents untouched and fall back to the reserved resource tmp directory.
        cfg.cacheDir = kDefaultCacheDir;
        std::error_code ec;
        std::filesystem::path cfgFile = root / L"workspace.json";
        if (!IsWorkspaceConfigAutoPersistBlockedForRoot(root) &&
            originalCacheDir != cfg.cacheDir &&
            std::filesystem::exists(cfgFile, ec) && !ec) {
            SaveWorkspaceConfigToFile(cfgFile, cfg);
        }
        return;
    }

    cfg.cacheDir = kDefaultCacheDir; // canonical form (forward slashes)

    std::error_code ec;
    std::filesystem::path cfgFile = root / L"workspace.json";
    if (!IsWorkspaceConfigAutoPersistBlockedForRoot(root) &&
        originalCacheDir != cfg.cacheDir &&
        std::filesystem::exists(cfgFile, ec) && !ec) {
        SaveWorkspaceConfigToFile(cfgFile, cfg);
    }
}

WorkspaceConfig LoadWorkspaceConfig(const std::wstring& root) {
    WorkspaceConfig cfg = DefaultWorkspaceConfig();
    std::filesystem::path p = std::filesystem::path(root) / L"workspace.json";
        std::error_code ec;
    if (!std::filesystem::exists(p, ec) || ec) {
        ec.clear();
        auto classesPath = WorkspaceClassesPath(root, cfg);
        if (!std::filesystem::exists(classesPath, ec) || ec) {
            ec.clear();
            for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
                bool isReparse = false;
                if (TryIsReparsePointNoFollow(e.path(), isReparse) && isReparse) continue;
                std::error_code stEc;
                if (e.is_directory(stEc) && !stEc) {
                    cfg.classesDir = L".";
                    break;
                }
            }
        }
        LoadScheduleStartTimes(std::filesystem::path(root), cfg);
        EnsureScheduleStartTimesSize(cfg);
        NormalizeCacheDir(std::filesystem::path(root), cfg);
        return cfg;
    }

    std::wstring readErr;
    std::string json;
    bool readOk = ReadTextFileUtf8Limited(p, kMaxWorkspaceJsonBytes, &json, &readErr);
    if (!readOk || !LooksLikeWorkspaceJson(json)) {
        const std::filesystem::path rootPath(root);
        std::filesystem::path quarantined = QuarantineCorruptWorkspaceJson(rootPath, p);
        BlockWorkspaceConfigAutoPersistForRoot(rootPath);
        LoadScheduleStartTimes(rootPath, cfg);
        EnsureScheduleStartTimesSize(cfg);
        std::wstring msg = L"workspace.json を読み込めないため、既存設定を初期化せず、この起動では設定ファイルの自動保存を停止しました。\n";
        if (!readOk && !readErr.empty()) {
            msg += L"\n読み込みエラー: " + readErr + L"\n";
        }
        if (!quarantined.empty()) {
            msg += L"\n読めない設定ファイルを退避しました:\n" + quarantined.wstring();
        } else {
            msg += L"\n読めない設定ファイルの退避に失敗しました。元の workspace.json は上書きしません。";
        }
        msg += L"\n\n退避ファイルまたは元ファイルを確認し、手動で復旧するまで workspace.json は作り直しません。";
        ShowAppCoreMessageDialog(nullptr, L"設定", msg, SoftNoticeKind::Warning);
        return cfg;
    }
    const std::filesystem::path rootPath(root);
    std::vector<std::string> unknownFields;
    if (WorkspaceJsonHasUnknownTopLevelFields(json, &unknownFields)) {
        BlockWorkspaceConfigAutoPersistForRoot(rootPath);
        std::wstring msg = L"workspace.json にこのバージョンでは解釈できない項目があるため、この起動では設定ファイルの自動保存を停止しました。\n";
        msg += L"既存設定を既定値で上書きしないための保護です。必要なら workspace.json を手動で確認してください。\n\n未対応項目:";
        const size_t limit = std::min<size_t>(unknownFields.size(), 8);
        for (size_t i = 0; i < limit; ++i) {
            msg += L"\n- " + UTF8ToWide(unknownFields[i]);
        }
        if (unknownFields.size() > limit) {
            msg += L"\n- ...";
        }
        ShowAppCoreMessageDialog(nullptr, L"設定", msg, SoftNoticeKind::Warning);
    }
    ApplyJsonToWorkspaceConfig(json, cfg);
    LoadScheduleStartTimes(rootPath, cfg);
    EnsureScheduleStartTimesSize(cfg);
    NormalizeCacheDir(rootPath, cfg);
    return cfg;
}

std::optional<WorkspaceConfig> LoadWorkspaceConfigFromFile(const std::filesystem::path& path, std::wstring* outErr) {
    if (outErr) outErr->clear();
    if (path.empty()) {
        if (outErr) *outErr = L"invalid path";
        return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        if (outErr) *outErr = L"file does not exist";
        return std::nullopt;
    }
    std::wstring readErr;
    std::string json;
    if (!ReadTextFileUtf8Limited(path, kMaxPresetJsonBytes, &json, &readErr)) {
        if (outErr) *outErr = L"読み込みに失敗しました。(" + readErr + L")";
        return std::nullopt;
    }
    if (!LooksLikeWorkspaceJson(json)) {
        if (outErr) *outErr = L"workspace.json として認識できない内容です。";
        return std::nullopt;
    }
    WorkspaceConfig cfg = DefaultWorkspaceConfig();
    ApplyJsonToWorkspaceConfig(json, cfg);
    return cfg;
}

bool SaveWorkspaceConfigToFile(const std::filesystem::path& path, const WorkspaceConfig& cfg) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;

    std::ostringstream ofs;
    if (!ofs) return false;
    ofs << "{\n";
    ofs << "  \"classesDir\": \"" << WideToUTF8(cfg.classesDir) << "\",\n";
    ofs << "  \"cacheDir\": \""   << WideToUTF8(cfg.cacheDir)   << "\",\n";
    ofs << "  \"showAnnots\": "   << (cfg.showAnnots ? "true" : "false") << ",\n";
    ofs << "  \"pdfFlowMode\": \"" << WideToUTF8(NormalizePdfFlowMode(cfg.pdfFlowMode)) << "\",\n";
    ofs << "  \"pdfBitmapBudgetMiB\": " << std::clamp(cfg.pdfBitmapBudgetMiB, kPdfBitmapBudgetMiBMin, kPdfBitmapBudgetMiBMax) << ",\n";
    ofs << "  \"pdfSinglePageMode\": " << (cfg.pdfSinglePageMode ? "true" : "false") << ",\n";
    ofs << "  \"mouseWheelInvertVertical\": " << (cfg.mouseWheelInvertVertical ? "true" : "false") << ",\n";
    ofs << "  \"mouseWheelInvertHorizontal\": " << (cfg.mouseWheelInvertHorizontal ? "true" : "false") << ",\n";
    ofs << "  \"touchpadInvertVertical\": " << (cfg.touchpadInvertVertical ? "true" : "false") << ",\n";
    ofs << "  \"touchpadInvertHorizontal\": " << (cfg.touchpadInvertHorizontal ? "true" : "false") << ",\n";
    ofs << "  \"ownerDrawUi\": "  << (cfg.ownerDrawUi ? "true" : "false") << ",\n";
    ofs << "  \"useNativeFileDialogs\": " << (cfg.useNativeFileDialogs ? "true" : "false") << ",\n";
    ofs << "  \"developerMode\": " << (cfg.developerMode ? "true" : "false") << ",\n";
    ofs << "  \"studentMode\": " << (cfg.studentMode ? "true" : "false") << ",\n";
    ofs << "  \"exportStandardTextAnnots\": " << (cfg.exportStandardTextAnnots ? "true" : "false") << ",\n";
    ofs << "  \"debugLogPreviewTrace\": " << (cfg.debugLogs.previewTrace ? "true" : "false") << ",\n";
    ofs << "  \"debugLogSwitchTiming\": " << (cfg.debugLogs.switchTiming ? "true" : "false") << ",\n";
    ofs << "  \"debugLogCrash\": " << (cfg.debugLogs.crash ? "true" : "false") << ",\n";
    ofs << "  \"debugLogStartupWatchdog\": " << (cfg.debugLogs.startupWatchdog ? "true" : "false") << ",\n";
    ofs << "  \"debugLogOfficeConversion\": " << (cfg.debugLogs.officeConversion ? "true" : "false") << ",\n";
    ofs << "  \"leftWidth\": "    << cfg.leftWidth  << ",\n";
    ofs << "  \"rightWidth\": "   << cfg.rightWidth << ",\n";
    ofs << "  \"topHeight\": "    << cfg.topHeight  << ",\n";
    if (cfg.windowWidth > 0)  ofs << "  \"windowWidth\": "  << cfg.windowWidth  << ",\n";
    if (cfg.windowHeight > 0) ofs << "  \"windowHeight\": " << cfg.windowHeight << ",\n";
    if (cfg.defaultWindowWidth > 0) {
        ofs << "  \"defaultWindowWidth\": " << cfg.defaultWindowWidth << ",\n";
    }
    if (cfg.defaultWindowHeight > 0) {
        ofs << "  \"defaultWindowHeight\": " << cfg.defaultWindowHeight << ",\n";
    }
    if (cfg.leftSplit1 > 0) ofs << "  \"leftSplit1\": " << cfg.leftSplit1 << ",\n";
    if (cfg.leftSplit2 > 0) ofs << "  \"leftSplit2\": " << cfg.leftSplit2 << ",\n";
    if (cfg.defaultLeftWidth > 0)  ofs << "  \"defaultLeftWidth\": "  << cfg.defaultLeftWidth << ",\n";
    if (cfg.defaultRightWidth > 0) ofs << "  \"defaultRightWidth\": " << cfg.defaultRightWidth << ",\n";
    if (cfg.defaultTopHeight > 0)  ofs << "  \"defaultTopHeight\": "  << cfg.defaultTopHeight << ",\n";
    if (cfg.defaultLeftSplit1 > 0) ofs << "  \"defaultLeftSplit1\": " << cfg.defaultLeftSplit1 << ",\n";
    if (cfg.defaultLeftSplit2 > 0) ofs << "  \"defaultLeftSplit2\": " << cfg.defaultLeftSplit2 << ",\n";
    ofs << "  \"leftPaneCollapsed\": " << (cfg.leftPaneCollapsed ? "true" : "false") << ",\n";
    ofs << "  \"language\": \""   << WideToUTF8(cfg.language.empty() ? L"ja" : cfg.language) << "\",\n";
    if (cfg.markFontPx > 0) ofs << "  \"markFontPx\": " << cfg.markFontPx << ",\n";
    if (cfg.headingFontPx > 0) ofs << "  \"headingFontPx\": " << cfg.headingFontPx << ",\n";
    if (cfg.markColor >= 0) {
        char buf[8]{};
        std::snprintf(buf, sizeof(buf), "%06X", cfg.markColor & 0xFFFFFF);
        ofs << "  \"markColor\": \"#" << buf << "\",\n";
    }
    if (cfg.headingColor >= 0) {
        char buf[8]{};
        std::snprintf(buf, sizeof(buf), "%06X", cfg.headingColor & 0xFFFFFF);
        ofs << "  \"headingColor\": \"#" << buf << "\",\n";
    }
    ofs << "  \"headingBold\": " << (cfg.headingBold ? "true" : "false") << ",\n";
    ofs << "  \"headingUnderline\": " << (cfg.headingUnderline ? "true" : "false") << ",\n";
    ofs << "  \"headingLeftBar\": " << (cfg.headingLeftBar ? "true" : "false") << ",\n";
    ofs << "  \"bottomPanePin\": \"" << WideToUTF8(cfg.bottomPanePin.empty() ? L"note" : cfg.bottomPanePin) << "\",\n";
    ofs << "  \"bottomNoteMode\": \"" << WideToUTF8(cfg.bottomNoteMode.empty() ? L"legacy" : cfg.bottomNoteMode) << "\",\n";
    ofs << "  \"notePlacement\": \"" << WideToUTF8(NotePlacementToString(ParseNotePlacement(cfg.notePlacement))) << "\",\n";
    ofs << "  \"colorTone\": \"" << WideToUTF8(cfg.colorTone.empty() ? L"default" : cfg.colorTone) << "\",\n";
    ofs << "  \"toneVariant\": \"" << WideToUTF8(NormalizeToneVariant(cfg.toneVariant)) << "\",\n";
    ofs << "  \"quickAnnotPopupPlacement\": \"" << WideToUTF8(cfg.quickAnnotPopupPlacement.empty() ? L"auto" : cfg.quickAnnotPopupPlacement) << "\",\n";
    ofs << "  \"lectureSortMode\": \"" << WideToUTF8(NormalizeLectureSortMode(cfg.lectureSortMode)) << "\",\n";
    ofs << "  \"sessionSortMode\": \"" << WideToUTF8(NormalizeSessionSortMode(cfg.sessionSortMode)) << "\",\n";
    ofs << "  \"sessionNumberingMode\": \"" << WideToUTF8(NormalizeSessionNumberingMode(cfg.sessionNumberingMode)) << "\",\n";
    ofs << "  \"sessionAutoOpenMode\": \"" << WideToUTF8(NormalizeSessionAutoOpenMode(cfg.sessionAutoOpenMode)) << "\",\n";
    ofs << "  \"sessionAutoOpenPairLinked\": " << (cfg.sessionAutoOpenPairLinked ? "true" : "false") << ",\n";
    ofs << "  \"selectionStyle\": \"" << WideToUTF8(NormalizeSelectionStyle(cfg.selectionStyle)) << "\",\n";
    ofs << "  \"pointerOffsetX\": " << std::clamp(cfg.pointerOffsetX, -20, 20) << ",\n";
    ofs << "  \"pointerOffsetY\": " << std::clamp(cfg.pointerOffsetY, -20, 20) << ",\n";
    ofs << "  \"showMathList\": " << (cfg.showMathList ? "true" : "false") << ",\n";
    ofs << "  \"downKeyLastLineAction\": \"" << WideToUTF8(DownKeyLastLineActionToString(
        ParseDownKeyLastLineAction(cfg.downKeyLastLineAction))) << "\",\n";
    ofs << "  \"leftRightLineMoveAction\": \"" << WideToUTF8(LeftRightLineMoveActionToString(
        ParseLeftRightLineMoveAction(cfg.leftRightLineMoveAction))) << "\",\n";
    ofs << "  \"autoPairBrackets\": " << (cfg.autoPairBrackets ? "true" : "false") << ",\n";
    ofs << "  \"fullWidthParenCaretInside\": " << (cfg.fullWidthParenCaretInside ? "true" : "false") << ",\n";
    ofs << "  \"fullWidthParenCancelNextLeft\": " << (cfg.fullWidthParenCancelNextLeft ? "true" : "false") << ",\n";
    ofs << "  \"noteFontName\": \"" << WideToUTF8(cfg.noteFontName.empty() ? GetDefaultFontFaceName() : cfg.noteFontName) << "\",\n";
    ofs << "  \"noteFontPt\": " << cfg.noteFontPt << ",\n";
    ofs << "  \"noteRenderFontName\": \"" << WideToUTF8(cfg.noteRenderFontName.empty() ? cfg.noteFontName : cfg.noteRenderFontName) << "\",\n";
    ofs << "  \"noteRenderJpFontName\": \"" << WideToUTF8(cfg.noteRenderJpFontName.empty() ? cfg.noteRenderFontName : cfg.noteRenderJpFontName) << "\",\n";
    ofs << "  \"noteRenderFontPt\": " << cfg.noteRenderFontPt << ",\n";
    ofs << "  \"noteSystem\": \"" << WideToUTF8(cfg.noteSystem.empty() ? L"legacy" : cfg.noteSystem) << "\",\n";
    ofs << "  \"noteRenderEnabled\": " << (cfg.noteRenderEnabled ? "true" : "false") << ",\n";
    ofs << "  \"noteRawOnly\": " << (cfg.noteRawOnly ? "true" : "false") << ",\n";
    ofs << "  \"noteRenderMath\": " << (cfg.noteRenderMath ? "true" : "false") << ",\n";
    ofs << "  \"noteWrapEnabled\": " << (cfg.noteWrapEnabled ? "true" : "false") << ",\n";
    ofs << "  \"noteVimModeEnabled\": " << (cfg.noteVimModeEnabled ? "true" : "false") << ",\n";
    ofs << "  \"noteVimCaretLineRawTextVisible\": " << (cfg.noteVimCaretLineRawTextVisible ? "true" : "false") << ",\n";
    ofs << "  \"noteVimClickEntersInsertMode\": " << (cfg.noteVimClickEntersInsertMode ? "true" : "false") << ",\n";
    ofs << "  \"noteMathMarginTopPercent\": " << std::clamp(cfg.noteMathMarginTopPercent, 5, 95) << ",\n";
    ofs << "  \"noteMathSupSubGapSupPercent\": "
        << ((cfg.noteMathSupSubGapSupPercent <= 0) ? 0 : std::clamp(cfg.noteMathSupSubGapSupPercent, 5, 95)) << ",\n";
    ofs << "  \"noteGridEnabled\": " << (cfg.noteGridEnabled ? "true" : "false") << ",\n";
    ofs << "  \"noteGridPitch\": " << cfg.noteGridPitch << ",\n";
    ofs << "  \"noteBgColor\": \"" << ColorToHex(cfg.noteBgColor) << "\",\n";
    ofs << "  \"noteFgColor\": \"" << ColorToHex(cfg.noteFgColor) << "\",\n";
    ofs << "  \"noteBgSource\": \"" << WideToUTF8(NormalizeNoteBgSource(cfg.noteBgSource)) << "\",\n";
    ofs << "  \"noteBgThemeName\": \"" << WideToUTF8(cfg.noteBgThemeName) << "\",\n";
    ofs << "  \"noteShortcutBackColor\": \"" << ColorToHex(cfg.noteShortcutBackColor) << "\",\n";
    ofs << "  \"noteShortcutTextColor\": \"" << ColorToHex(cfg.noteShortcutTextColor) << "\",\n";
    ofs << "  \"noteShortcutTextTagKey\": \"" << WideToUTF8(NormalizeShortcutTextTagKey(cfg.noteShortcutTextTagKey)) << "\",\n";
    ofs << "  \"noteShortcutHeadingArrowInvert\": " << (cfg.noteShortcutHeadingArrowInvert ? "true" : "false") << ",\n";
    ofs << "  \"noteCustomTagKey\": \"" << WideToUTF8(NormalizeNoteCustomTagKey(cfg.noteCustomTagKey)) << "\",\n";
    ofs << "  \"noteCustomTagBold\": " << (cfg.noteCustomTagBold ? "true" : "false") << ",\n";
    ofs << "  \"noteCustomTagItalic\": " << (cfg.noteCustomTagItalic ? "true" : "false") << ",\n";
    ofs << "  \"noteCustomTagUnderline\": " << (cfg.noteCustomTagUnderline ? "true" : "false") << ",\n";
    ofs << "  \"noteCustomTagStrike\": " << (cfg.noteCustomTagStrike ? "true" : "false") << ",\n";
    ofs << "  \"noteCustomTagBackColor\": " << (cfg.noteCustomTagBackColor ? "true" : "false") << ",\n";
    ofs << "  \"noteCustomTagTextColor\": " << (cfg.noteCustomTagTextColor ? "true" : "false") << ",\n";
    for (int i = 0; i < kNoteCustomTagPresetCount; ++i) {
        int mask = cfg.noteCustomTagPresetMasks[static_cast<size_t>(i)] & kNoteCustomTagStyleAllMask;
        ofs << "  \"noteCustomTag" << (i + 1) << "Mask\": " << mask << ",\n";
    }
    ofs << "  \"noteFontCustomization\": " << cfg.noteFontCustomization << ",\n";
    ofs << "  \"noteFontDigitTarget\": " << cfg.noteFontDigitTarget << ",\n";
    ofs << "  \"autoSaveSeconds\": " << cfg.autoSaveSeconds << ",\n";
    ofs << "  \"autoIntegrateSeconds\": " << cfg.autoIntegrateSeconds << ",\n";
    ofs << "  \"autoIntegrateCustomMinutes\": " << cfg.autoIntegrateCustomMinutes << ",\n";
    if (!cfg.clroNamePattern.empty()) {
        ofs << "  \"clroNamePattern\": \"" << WideToUTF8(cfg.clroNamePattern) << "\",\n";
    }
    ofs << "  \"textFontName\": \"" << WideToUTF8(cfg.textFontName.empty() ? GetDefaultFontFaceName() : cfg.textFontName) << "\",\n";
    ofs << "  \"textFontPt\": " << cfg.textFontPt << ",\n";
    ofs << "  \"textFontUseA4Scale\": " << (cfg.textFontUseA4Scale ? "true" : "false") << ",\n";
    ofs << "  \"textFontActiveSizeSlot\": " << std::clamp(cfg.textFontActiveSizeSlot, 0, 1) << ",\n";
    ofs << "  \"textFontPtSlotA\": " << std::clamp(cfg.textFontPtSlotA, 6.0, 96.0) << ",\n";
    ofs << "  \"textFontPtSlotB\": " << std::clamp(cfg.textFontPtSlotB, 6.0, 96.0) << ",\n";
    ofs << "  \"textFontUseA4ScaleSlotA\": " << (cfg.textFontUseA4ScaleSlotA ? "true" : "false") << ",\n";
    ofs << "  \"textFontUseA4ScaleSlotB\": " << (cfg.textFontUseA4ScaleSlotB ? "true" : "false") << ",\n";
    ofs << "  \"textBoxReadableBackground\": " << (cfg.textBoxReadableBackground ? "true" : "false") << ",\n";
    ofs << "  \"textBoxReadableBackgroundInverted\": " << (cfg.textBoxReadableBackgroundInverted ? "true" : "false") << ",\n";
    ofs << "  \"lineToolsShareStyle\": " << (cfg.lineToolsShareStyle ? "true" : "false") << ",\n";
    ofs << "  \"lineWidthPt\": " << cfg.lineWidthPt << ",\n";
    ofs << "  \"arrowWidthPt\": " << cfg.arrowWidthPt << ",\n";
    ofs << "  \"arrowHead\": \"" << WideToUTF8(ArrowHeadToString(ParseArrowHead(cfg.arrowHead))) << "\",\n";
    ofs << "  \"waveWidthPt\": " << cfg.waveWidthPt << ",\n";
    ofs << "  \"lineDashStyle\": \"" << WideToUTF8(NormalizeLineDashStyle(cfg.lineDashStyle)) << "\",\n";
    ofs << "  \"freehandWidthPt\": " << cfg.freehandWidthPt << ",\n";
    ofs << "  \"markerFreeWidthPt\": " << cfg.markerFreeWidthPt << ",\n";
    ofs << "  \"markerTextWidthPt\": " << cfg.markerTextWidthPt << ",\n";
    ofs << "  \"markerTextUnderline\": " << (cfg.markerTextUnderline ? "true" : "false") << ",\n";
    ofs << "  \"eraserWidthPt\": " << cfg.eraserWidthPt << ",\n";
    ofs << "  \"markerAlpha\": " << cfg.markerAlpha << ",\n";
    ofs << "  \"lineAlpha\": " << std::clamp(cfg.lineAlpha, 0.05, 1.0) << ",\n";
    ofs << "  \"arrowAlpha\": " << std::clamp(cfg.arrowAlpha, 0.05, 1.0) << ",\n";
    ofs << "  \"waveAlpha\": " << std::clamp(cfg.waveAlpha, 0.05, 1.0) << ",\n";
    ofs << "  \"freehandAlpha\": " << std::clamp(cfg.freehandAlpha, 0.05, 1.0) << ",\n";
    ofs << "  \"shapeAlpha\": " << std::clamp(cfg.shapeAlpha, 0.0, 1.0) << ",\n";
    ofs << "  \"textColor\": \"" << ColorToHex(cfg.textColor) << "\",\n";
    ofs << "  \"lineColor\": \"" << ColorToHex(cfg.lineColor) << "\",\n";
    ofs << "  \"arrowColor\": \"" << ColorToHex(cfg.arrowColor) << "\",\n";
    ofs << "  \"waveColor\": \"" << ColorToHex(cfg.waveColor) << "\",\n";
    ofs << "  \"freehandColor\": \"" << ColorToHex(cfg.freehandColor) << "\",\n";
    ofs << "  \"markerFreeColor\": \"" << ColorToHex(cfg.markerFreeColor) << "\",\n";
    ofs << "  \"markerTextColor\": \"" << ColorToHex(cfg.markerTextColor) << "\",\n";
    ofs << "  \"shapeColor\": \"" << ColorToHex(cfg.shapeColor) << "\",\n";
    ofs << "  \"paletteCustomColor\": \"" << ColorToHex(cfg.paletteCustomColor) << "\",\n";
    ofs << "  \"magnifierShape\": \"" << WideToUTF8(MagnifierShapeToString(ParseMagnifierShape(cfg.magnifierShape))) << "\",\n";
    ofs << "  \"shapeDetail\": \"" << WideToUTF8(cfg.shapeDetail) << "\",\n";
    ofs << "  \"shapeKind\": \"" << WideToUTF8(ShapeKindToString(ParseShapeKind(cfg.shapeKind))) << "\",\n";
    ofs << "  \"shapeDrawMode\": \"" << WideToUTF8(ShapeDrawModeToString(ParseShapeDrawMode(cfg.shapeDrawMode))) << "\",\n";
    ofs << "  \"annotLastMarkerDetail\": \"" << WideToUTF8(cfg.annotLastMarkerDetail) << "\",\n";
    ofs << "  \"annotLastPenDetail\": \"" << WideToUTF8(cfg.annotLastPenDetail) << "\",\n";
    ofs << "  \"annotLastShapePresentation\": \"" << WideToUTF8(cfg.annotLastShapePresentation) << "\",\n";
    ofs << "  \"annotLastShapeGeometry\": \"" << WideToUTF8(cfg.annotLastShapeGeometry) << "\",\n";
    ofs << "  \"annotLastShapeDetail\": \"" << WideToUTF8(cfg.annotLastShapeDetail) << "\",\n";
    ofs << "  \"freehandCorrection\": \"" << WideToUTF8(NormalizeFreehandCorrection(cfg.freehandCorrection)) << "\",\n";
    ofs << "  \"freehandCorrectionStyle\": \"" << WideToUTF8(NormalizeFreehandCorrectionStyle(cfg.freehandCorrectionStyle)) << "\",\n";
    ofs << "  \"freehandCorrectionFill\": \"" << WideToUTF8(NormalizeFreehandCorrectionFill(cfg.freehandCorrectionFill)) << "\",\n";
    int scheduleDayMask = cfg.scheduleDayMask & 0x7F;
    if (scheduleDayMask == 0) scheduleDayMask = 0x1F;
    int schedulePeriods = std::clamp(cfg.schedulePeriods, 1, 13);
    ofs << "  \"scheduleDayMask\": " << scheduleDayMask << ",\n";
    ofs << "  \"schedulePeriods\": " << schedulePeriods << "\n";
    // NOTE: shortcuts are no longer persisted in workspace.json.
    ofs << "}\n";
    std::string data = ofs.str();
    std::wstring err;
    return AtomicWriteUtf8WithWorkspaceDirs(path, data, std::filesystem::path(g_workspaceRoot), &err);
}

void SaveWorkspaceConfig(const std::wstring& root, const WorkspaceConfig& cfg) {
    const std::filesystem::path rootPath(root);
    if (IsWorkspaceConfigAutoPersistBlockedForRoot(rootPath)) return;
    SaveWorkspaceConfigToFile(rootPath / L"workspace.json", cfg);
}

void PersistConfig() {
    if (!g_workspaceRoot.empty()) {
        g_config.leftWidth  = g_leftWidth;
        g_config.rightWidth = g_rightWidth;
        g_config.topHeight  = g_topHeight;
        g_config.leftSplit1 = g_leftSplit1;
        g_config.leftSplit2 = g_leftSplit2;
        g_config.leftPaneCollapsed = g_leftPaneCollapsed;
        g_config.showAnnots = g_showAnnots;
        g_config.bottomPanePin = BottomPanePinToString(g_bottomPanePin);
        g_config.bottomNoteMode = BottomNoteModeToString(g_bottomNoteMode);
        g_config.notePlacement = NotePlacementToString(g_notePlacement);
        g_config.textFontName = g_textFontName;
        if (std::clamp(g_textFontActiveSizeSlot, 0, 1) == 1) {
            g_textFontPtSlotB = g_textFontPt;
            g_textFontUseA4ScaleSlotB = g_textFontUseA4Scale;
        } else {
            g_textFontPtSlotA = g_textFontPt;
            g_textFontUseA4ScaleSlotA = g_textFontUseA4Scale;
        }
        g_config.textFontPt = g_textFontPt;
        g_config.textFontUseA4Scale = g_textFontUseA4Scale;
        g_config.textFontActiveSizeSlot = std::clamp(g_textFontActiveSizeSlot, 0, 1);
        g_config.textFontPtSlotA = g_textFontPtSlotA;
        g_config.textFontPtSlotB = g_textFontPtSlotB;
        g_config.textFontUseA4ScaleSlotA = g_textFontUseA4ScaleSlotA;
        g_config.textFontUseA4ScaleSlotB = g_textFontUseA4ScaleSlotB;
        g_config.textBoxReadableBackground = g_textBoxReadableBackground;
        g_config.textBoxReadableBackgroundInverted = g_textBoxReadableBackgroundInverted;
        g_config.noteFontName = g_noteFontName;
        g_config.noteFontPt = g_noteFontPt;
        g_config.noteRenderFontName = g_noteRenderFontName;
        g_config.noteRenderJpFontName = g_noteRenderJpFontName;
        g_config.noteRenderFontPt = g_noteRenderFontPt;
        g_config.noteSystem = NoteSystemToString(g_noteSystem);
        g_config.noteRenderEnabled = g_noteRenderEnabled;
        g_config.noteRawOnly = g_noteRawOnly;
        g_config.noteRenderMath = g_noteRenderMath;
        g_config.noteWrapEnabled = g_noteWrapEnabled;
        g_config.noteVimModeEnabled = g_noteVimModeEnabled;
        g_config.noteVimCaretLineRawTextVisible = g_noteVimCaretLineRawTextVisible;
        g_config.noteVimClickEntersInsertMode = g_noteVimClickEntersInsertMode;
        g_config.noteGridEnabled = g_noteGridEnabled;
        g_config.noteGridPitch = g_noteGridPitch;
        if (NormalizeNoteBgSource(g_config.noteBgSource) == L"explicit") {
            g_config.noteBgColor = g_noteBgColor;
        }
        g_config.noteFgColor = g_noteFgColor;
        g_config.noteShortcutBackColor = g_noteShortcutBackColor;
        g_config.noteShortcutTextColor = g_noteShortcutTextColor;
        g_config.lineToolsShareStyle = g_lineToolsShareStyle;
        g_config.lineWidthPt = g_lineWidthPt;
        g_config.arrowWidthPt = g_arrowWidthPt;
        g_config.arrowHead = ArrowHeadToString(g_arrowHead);
        g_config.waveWidthPt = g_waveWidthPt;
        g_config.lineDashStyle = NormalizeLineDashStyle(g_lineDashStyle);
        g_config.freehandWidthPt = g_freehandWidthPt;
        g_config.markerFreeWidthPt = g_markerFreeWidthPt;
        g_config.markerTextWidthPt = g_markerTextWidthPt;
        g_config.markerTextUnderline = g_markerTextUnderline;
        g_config.eraserWidthPt = g_eraserWidthPt;
        g_config.markerAlpha = g_markerAlpha;
        g_config.lineAlpha = g_lineAlpha;
        g_config.arrowAlpha = g_arrowAlpha;
        g_config.waveAlpha = g_waveAlpha;
        g_config.freehandAlpha = g_freehandAlpha;
        g_config.shapeAlpha = g_shapeAlpha;
        g_config.textColor = g_textColor;
        g_config.lineColor = g_lineColor;
        g_config.arrowColor = g_arrowColor;
        g_config.waveColor = g_waveColor;
        g_config.freehandColor = g_freehandColor;
        g_config.markerFreeColor = g_markerFreeColor;
        g_config.markerTextColor = g_markerTextColor;
        g_config.shapeColor = g_shapeColor;
        g_config.paletteCustomColor = g_paletteCustomColor;
        g_config.magnifierShape = MagnifierShapeToString(g_magnifierShape);
        SyncLegacyShapeStateFromDetail();
        g_config.shapeDetail = UTF8ToWide(ShapeDetailKey(g_shapeDetail));
        g_config.shapeKind = ShapeKindToString(g_shapeKind);
        g_config.shapeDrawMode = ShapeDrawModeToString(g_shapeDrawMode);
        g_config.annotLastMarkerDetail = UTF8ToWide(AnnotToolModeKey(g_markerGroupMode));
        g_config.annotLastPenDetail = UTF8ToWide(AnnotToolModeKey(g_penGroupMode));
        g_config.annotLastShapePresentation = UTF8ToWide(ShapeToolPresentationKey(g_shapeToolSelection.presentation));
        g_config.annotLastShapeGeometry = UTF8ToWide(ShapeToolGeometryKey(g_shapeToolSelection.geometry));
        g_config.annotLastShapeDetail = UTF8ToWide(AnnotToolModeKey(g_shapeGroupMode));
        g_config.showMathList = g_showMathList;
        EnsureScheduleStartTimesSize(g_config);
        static std::wstring s_lastPersistConfigErrorPath;
        static DWORD s_lastPersistConfigErrorTick = 0;
        const std::filesystem::path configPath = std::filesystem::path(g_workspaceRoot) / L"workspace.json";
        const bool workspaceConfigPersistBlocked =
            IsWorkspaceConfigAutoPersistBlockedForRoot(std::filesystem::path(g_workspaceRoot));
        if (!workspaceConfigPersistBlocked && !SaveWorkspaceConfigToFile(configPath, g_config)) {
            DWORD now = GetTickCount();
            const std::wstring key = configPath.wstring();
            if (key != s_lastPersistConfigErrorPath || (now - s_lastPersistConfigErrorTick) >= 10000) {
                s_lastPersistConfigErrorPath = key;
                s_lastPersistConfigErrorTick = now;
                std::wstring msg = IsEnglishUi()
                    ? L"Failed to save workspace settings.\n\nPath:\n" + key
                    : L"ワークスペース設定を保存できませんでした。\n\n保存先:\n" + key;
                ShowAppCoreSoftNotice(g_hMainWnd, msg, SoftNoticeKind::Warning);
            }
        }
        SaveScheduleStartTimes(std::filesystem::path(g_workspaceRoot), g_config);
    }
}

std::filesystem::path WorkspaceClassesPath(const std::wstring& root, const WorkspaceConfig& cfg) {
    std::filesystem::path base(root);
    if (cfg.classesDir.empty() || cfg.classesDir == L"." ||
        base.filename().wstring() == cfg.classesDir) {
        return base;
    }
    return base / cfg.classesDir;
}

std::filesystem::path WorkspaceCachePath(const std::wstring& root, const WorkspaceConfig& cfg) {
    return std::filesystem::path(root) / cache_dir_policy::EffectiveCacheDir(cfg.cacheDir);
}

const UiText& GetUiText() {
    std::wstring lang = g_config.language;
    std::transform(lang.begin(), lang.end(), lang.begin(), ::towlower);
    if (!lang.empty() && lang.rfind(L"en", 0) == 0) {
        if (!g_config.studentMode) {
            static UiText genericEn;
            genericEn = g_uiEn;
            genericEn.menuImportDirAsLecture = L"Import Directory as Parent Item...";
            genericEn.menuImportDirAsSession = L"Import Directory as Child Item...";
            genericEn.menuOrganizeSessionFiles = L"Organize PDF/notes into folders...";
            genericEn.menuNewLecture = L"New Parent Item...";
            genericEn.btnNewLecture = L"New Parent Item";
            genericEn.btnNewSession = L"New Child Item";
            genericEn.menuOpenLectureDir = L"Open Parent Item Folder";
            genericEn.errNewClroNoSession = L"No child item is open. Select a parent/child item first.";
            genericEn.dlgNewLectureTitle = L"Create Parent Item";
            genericEn.dlgNewLectureLabel = L"Parent item name";
            genericEn.errLectureCreate = L"Failed to create parent item directory.";
            genericEn.dlgNewSessionTitle = L"Create Child Item";
            genericEn.dlgNewSessionLabel = L"Child item name";
            genericEn.errSessionCreate = L"Failed to create child item directory.";
            genericEn.errSessionExists = L"Child item already exists.";
            genericEn.errSessionNoLecture = L"Select a parent item first.";
            genericEn.menuLectureSchedule = L"Parent Item Schedule";
            genericEn.menuAddTempExternalLecture = L"Add Temporary External Parent Item...";
            genericEn.menuRemoveTempExternalLecture = L"Remove Temporary External Parent Item";
            genericEn.menuResetLectureLastOpen = L"Reset Parent Item Last-Open Times";
            genericEn.menuResetSessionLastOpen = L"Reset Child Item Last-Open History";
            genericEn.menuRestoreLectureLastOpen = L"Restore Parent Item Last-Open Times";
            genericEn.menuRestoreSessionLastOpen = L"Restore Child Item Last-Open History";
            genericEn.menuDeleteLectureLastOpenBackup = L"Delete Parent Item Last-Open Backup";
            genericEn.menuDeleteSessionLastOpenBackup = L"Delete Child Item Last-Open History Backup";
            return genericEn;
        }
        return g_uiEn;
    }
    if (!g_config.studentMode) {
        static UiText genericJa;
        genericJa = g_uiJa;
        genericJa.menuImportDirAsLecture = L"フォルダを上位項目として取り込む...";
        genericJa.menuImportDirAsSession = L"フォルダを下位項目として取り込む...";
        genericJa.menuNewLecture = L"上位項目を作成...";
        genericJa.btnNewLecture = L"上位項目を作成";
        genericJa.btnNewSession = L"下位項目作成";
        genericJa.menuOpenLectureDir = L"上位項目フォルダをエクスプローラーで開く";
        genericJa.errNewClroNoSession = L"下位項目が開かれていません。上位項目→下位項目を選択してください。";
        genericJa.dlgNewLectureTitle = L"上位項目を作成";
        genericJa.dlgNewLectureLabel = L"上位項目名";
        genericJa.errLectureCreate = L"上位項目フォルダを作成できませんでした。";
        genericJa.dlgNewSessionTitle = L"下位項目ファイルを作成";
        genericJa.dlgNewSessionLabel = L"下位項目名";
        genericJa.errSessionCreate = L"下位項目フォルダを作成できませんでした。";
        genericJa.errSessionExists = L"同名の下位項目が存在します。";
        genericJa.errSessionNoLecture = L"上位項目を選択してください。";
        genericJa.menuLectureSchedule = L"上位項目スケジュール";
        genericJa.menuAddTempExternalLecture = L"一時外部上位項目パス追加";
        genericJa.menuRemoveTempExternalLecture = L"一時外部上位項目パス削除";
        genericJa.menuResetLectureLastOpen = L"上位項目の最終オープン時刻をリセット";
        genericJa.menuResetSessionLastOpen = L"下位項目最終オープン履歴をリセット";
        genericJa.menuRestoreLectureLastOpen = L"上位項目の最終オープン時刻を復元";
        genericJa.menuRestoreSessionLastOpen = L"下位項目最終オープン履歴を復元";
        genericJa.menuDeleteLectureLastOpenBackup = L"上位項目最終オープン時刻バックアップを削除";
        genericJa.menuDeleteSessionLastOpenBackup = L"下位項目最終オープン履歴バックアップを削除";
        return genericJa;
    }
    return g_uiJa;
}

std::wstring BuildAboutDialogText() {
    const bool englishUi = IsEnglishUi();
    const BuildInfoManifestData buildInfo = LoadBuildInfoManifestData();
    std::wstring text = GetUiText().aboutText;

    ReplaceAllInPlace(&text, L"{APP_VERSION}",
                      buildInfo.version.empty() ? L"0.0.0" : buildInfo.version);
    ReplaceAllInPlace(&text, L"{BUILD_TIMESTAMP}",
                      buildInfo.buildTimestamp.empty()
                          ? (englishUi ? L"(unavailable)" : L"(未取得)")
                          : buildInfo.buildTimestamp);
    ReplaceAllInPlace(&text, L"{BUILD_ARTIFACTS}",
                      BuildArtifactDigestText(buildInfo, englishUi));
    return text;
}

static std::optional<std::filesystem::path> FindWorkspaceRootInBase(
    const std::filesystem::path& baseDir,
    const std::filesystem::path& targetPath) {
    std::error_code ec;
    if (baseDir.empty() || !std::filesystem::exists(baseDir, ec) ||
        !std::filesystem::is_directory(baseDir, ec)) {
        return std::nullopt;
    }
    std::filesystem::path targetName = targetPath.filename();
    if (targetName.empty()) return std::nullopt;

    std::filesystem::path direct = baseDir / targetName;
    if (std::filesystem::exists(direct, ec) && std::filesystem::is_directory(direct, ec)) {
        return direct;
    }

    constexpr int kMaxDepth = 6;
    std::filesystem::recursive_directory_iterator it(
        baseDir, std::filesystem::directory_options::skip_permission_denied, ec);
    for (; it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(it->path(), isReparse) && isReparse) {
            it.disable_recursion_pending();
            continue;
        }
        if (it.depth() >= kMaxDepth) {
            it.disable_recursion_pending();
        }
        if (!it->is_directory(ec)) continue;
        if (it->path().filename() == targetName) {
            return it->path();
        }
    }
    return std::nullopt;
}

static std::optional<std::filesystem::path> FindWorkspaceRootFallback(
    const std::filesystem::path& exeDir,
    const std::filesystem::path& targetPath) {
    std::filesystem::path parent = targetPath.parent_path();
    if (auto found = FindWorkspaceRootInBase(parent, targetPath)) return found;
    if (parent != exeDir) {
        if (auto found = FindWorkspaceRootInBase(exeDir, targetPath)) return found;
    }
    return std::nullopt;
}

static std::filesystem::path DefaultWorkspaceRootPath(const std::filesystem::path& exeDir) {
    return exeDir / L"workspace";
}

static std::filesystem::path ResolveSetupJsonPath(const std::filesystem::path& exeDir) {
    return exeDir / L"pdf_workspace_setup.json";
}

static std::string WorkspaceRootPathForSetupJson(const std::filesystem::path& exeDir,
                                                 const std::filesystem::path& workspaceRoot) {
    std::error_code ec;
    std::filesystem::path rootCanon = std::filesystem::weakly_canonical(workspaceRoot, ec);
    if (ec || rootCanon.empty()) rootCanon = workspaceRoot;
    ec.clear();
    std::filesystem::path exeCanon = std::filesystem::weakly_canonical(exeDir, ec);
    if (ec || exeCanon.empty()) exeCanon = exeDir;
    ec.clear();
    std::filesystem::path rel = std::filesystem::relative(rootCanon, exeCanon, ec);
    if (!ec && !rel.empty() && !rel.is_absolute()) {
        auto it = rel.begin();
        if (it == rel.end() || *it != L"..") {
            return WideToUTF8(rel.wstring());
        }
    }
    return WideToUTF8(rootCanon.wstring());
}

static std::string WorkspaceRootModeForSetupJson(const std::filesystem::path& exeDir,
                                                 const std::filesystem::path& workspaceRoot) {
    std::error_code ec;
    std::filesystem::path rootCanon = std::filesystem::weakly_canonical(workspaceRoot, ec);
    if (ec || rootCanon.empty()) rootCanon = workspaceRoot;
    ec.clear();
    std::filesystem::path exeCanon = std::filesystem::weakly_canonical(exeDir, ec);
    if (ec || exeCanon.empty()) exeCanon = exeDir;
    ec.clear();
    std::filesystem::path rel = std::filesystem::relative(rootCanon, exeCanon, ec);
    if (!ec && !rel.empty() && !rel.is_absolute()) {
        auto it = rel.begin();
        if (it == rel.end() || *it != L"..") {
            return "relative";
        }
    }
    return "absolute";
}

static bool ValidateSetupJsonForWrite(const std::string& json) {
    if (!IsSyntacticallyValidJsonLite(json)) return false;
    const auto workspaceRoot = ParseJsonStringField(json, "workspaceRoot");
    std::vector<std::string> unknownFields;
    const auto decision = ResolveSetupJsonAutoUpdateDecision(
        true, json, workspaceRoot, &unknownFields);
    return setup_json_policy::AllowsAutoUpdate(decision);
}

static bool AtomicWriteSetupJsonIfValid(const std::filesystem::path& setup,
                                        const std::string& json,
                                        std::wstring* outErr = nullptr) {
    if (outErr) outErr->clear();
    if (!ValidateSetupJsonForWrite(json)) {
        if (outErr) *outErr = L"setup.json として安全に保存できない内容です。";
        return false;
    }
    std::wstring err;
    if (!atomic_write::AtomicWriteUtf8(setup, json, /*preferredTempDir=*/setup.parent_path(), &err)) {
        if (outErr) *outErr = err;
        return false;
    }
    std::wstring readErr;
    std::string reread;
    if (!ReadTextFileUtf8Limited(setup, kMaxSetupJsonBytes, &reread, &readErr) || reread != json ||
        !ValidateSetupJsonForWrite(reread)) {
        if (outErr) {
            *outErr = readErr.empty() ? L"setup.json の保存後検証に失敗しました。"
                                      : L"setup.json の保存後検証に失敗しました: " + readErr;
        }
        return false;
    }
    return true;
}


static bool WriteSetupJsonFile(const std::filesystem::path& setup,
                              const std::filesystem::path& exeDir,
                              const std::filesystem::path& workspaceRoot) {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"workspaceRootMode\": \"" << WorkspaceRootModeForSetupJson(exeDir, workspaceRoot) << "\",\n";
    oss << "  \"workspaceRoot\": \"" << WorkspaceRootPathForSetupJson(exeDir, workspaceRoot) << "\",\n";
    oss << "  \"tempExternalLectureDirs\": [],\n";
    oss << "  \"annotToolModeOrder\": [";
    {
        bool first = true;
        for (ToolMode mode : DefaultAnnotToolModeUiOrder()) {
            const char* k = ToolModeKey(mode);
            if (!k || !*k) continue;
            if (!first) oss << ", ";
            first = false;
            oss << "\"" << k << "\"";
        }
    }
    oss << "],\n";
    oss << "  \"annotToolModeState\": [";
    {
        bool first = true;
        auto states = DefaultAnnotToolModeUiStates();
        for (ToolMode mode : DefaultAnnotToolModeUiOrder()) {
            const char* k = ToolModeKey(mode);
            if (!k || !*k) continue;
            if (!first) oss << ", ";
            first = false;
            int idx = static_cast<int>(mode);
            AnnotToolUiState s = (idx >= 0 && idx < kToolModeCount)
                ? states[static_cast<size_t>(idx)]
                : AnnotToolUiState::Enabled;
            oss << "\"" << k << ":" << AnnotToolUiStateKey(s) << "\"";
        }
    }
    oss << "]\n";
    oss << "}\n";
    std::string json = oss.str();
    SaveOperationGuard guard;
    std::wstring err;
    return AtomicWriteSetupJsonIfValid(setup, json, &err);
}

static std::wstring ToExtendedWin32PathIfAbsolute(const std::filesystem::path& p) {
    std::wstring s = p.wstring();
    if (s.empty()) return s;
    if (s.rfind(L"\\\\?\\", 0) == 0) return s;
    if (s.size() >= 2 && s[1] == L':') {
        return L"\\\\?\\" + s;
    }
    if (s.rfind(L"\\\\", 0) == 0) {
        // UNC: \\server\share -> \\?\UNC\server\share
        return L"\\\\?\\UNC\\" + s.substr(2);
    }
    return s;
}

static bool TryWriteTempDeleteOnCloseFileInDir(const std::filesystem::path& dir, std::wstring* outErr) {
    if (outErr) outErr->clear();
    if (dir.empty()) {
        if (outErr) *outErr = L"invalid directory path";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec || !std::filesystem::is_directory(dir, ec) || ec) {
        if (outErr) *outErr = L"directory not found";
        return false;
    }

    DWORD pid = GetCurrentProcessId();
    for (int i = 0; i < 16; ++i) {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        uint64_t ts = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | static_cast<uint64_t>(ft.dwLowDateTime);
        std::wstring name = L".__perm_test__" + std::to_wstring(pid) + L"_" + std::to_wstring(ts) +
                            L"_" + std::to_wstring(i) + L".tmp";
        std::filesystem::path testPath = dir / name;
        std::wstring openPath = ToExtendedWin32PathIfAbsolute(testPath);
        HANDLE h = CreateFileW(openPath.c_str(),
                               GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr,
                               CREATE_NEW,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                               nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return true;
        }
        DWORD e = GetLastError();
        if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) continue;
        if (outErr) *outErr = atomic_write::Win32ErrorMessage(e);
        return false;
    }
    if (outErr) *outErr = L"failed to create temp file (name collision)";
    return false;
}

static bool VerifySetupJsonStillExistsReadableImpl(const std::filesystem::path& setupPath, std::wstring* outErr) {
    if (outErr) outErr->clear();
    if (setupPath.empty()) {
        if (outErr) *outErr = L"invalid path";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(setupPath, ec) || ec || !std::filesystem::is_regular_file(setupPath, ec) || ec) {
        if (outErr) *outErr = L"file not found";
        return false;
    }
    std::wstring openPath = ToExtendedWin32PathIfAbsolute(setupPath);
    HANDLE h = CreateFileW(openPath.c_str(),
                           GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (outErr) *outErr = atomic_write::Win32ErrorMessage(GetLastError());
        return false;
    }
    CloseHandle(h);
    return true;
}

static bool g_hasPendingSetupJsonExistenceCheck = false;
static std::wstring g_pendingSetupJsonExistenceCheckPath;

std::optional<std::wstring> ConsumePendingSetupJsonExistenceCheckPath() {
    if (!g_hasPendingSetupJsonExistenceCheck || g_pendingSetupJsonExistenceCheckPath.empty()) return std::nullopt;
    g_hasPendingSetupJsonExistenceCheck = false;
    std::wstring out = g_pendingSetupJsonExistenceCheckPath;
    g_pendingSetupJsonExistenceCheckPath.clear();
    return out;
}

bool VerifySetupJsonStillExistsReadable(const std::filesystem::path& setupPath, std::wstring* outErr) {
    return VerifySetupJsonStillExistsReadableImpl(setupPath, outErr);
}

static bool EnsureDefaultSetupJson(const std::filesystem::path& exeDir,
                                   const std::filesystem::path& setup,
                                   std::wstring* outError) {
    if (std::filesystem::exists(setup)) return true;
    {
        std::wstring perr;
        if (!TryWriteTempDeleteOnCloseFileInDir(exeDir, &perr)) {
            if (outError) {
                *outError = L"setup.json を作成できません（exeファイルのあるフォルダに書き込みできません）。\n";
                *outError += L"この起動では setup.json の自動作成は行いません。\n";
                *outError += L"設定の一部が永続化されない可能性があります。\n\n";
                *outError += L"exeDir:\n" + exeDir.wstring();
                *outError += L"\n\nアクセス権・セキュリティ制御を確認してください。";
                *outError += L"\nまれにウイルス対策アルゴリズムが自動作成されたファイルをブロックすることがあります。";
                if (!perr.empty()) *outError += L"\n\n" + perr;
            }
            return false;
        }
    }
    std::filesystem::path root = DefaultWorkspaceRootPath(exeDir);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        std::filesystem::create_directories(root, ec);
    }
    if (ec || !std::filesystem::exists(root, ec)) {
        if (outError) {
            *outError = g_config.studentMode ? L"授業フォルダを作成できませんでした。\n"
                                             : L"上位項目フォルダを作成できませんでした。\n";
            *outError += L"この起動では setup.json の自動作成は行いません。\n";
            *outError += L"設定の一部が永続化されない可能性があります。\n\n";
            *outError += root.wstring();
            *outError += L"\n\nアクセス権・セキュリティ制御を確認してください。";
        }
        return false;
    }
    if (!WriteSetupJsonFile(setup, exeDir, root)) {
        if (outError) {
            *outError = L"setup.json の作成に失敗しました。\n";
            *outError += L"この起動では setup.json の自動作成は行いません。\n";
            *outError += L"設定の一部が永続化されない可能性があります。\n\n";
            *outError += setup.wstring();
            *outError += L"\n\nアクセス権・セキュリティ制御を確認してください。";
        }
        return false;
    }
    // Best-effort: remember for delayed verification on the main window.
    g_hasPendingSetupJsonExistenceCheck = true;
    g_pendingSetupJsonExistenceCheckPath = setup.wstring();
    std::filesystem::path wsjson = root / L"workspace.json";
    if (!std::filesystem::exists(wsjson, ec)) {
        WorkspaceConfig cfg = DefaultWorkspaceConfig();
        cfg.classesDir = root.filename().wstring();
        SaveWorkspaceConfig(root.wstring(), cfg);
    }
    return true;
}

static bool ReplaceOrInsertJsonStringField(std::string& json,
                                          const std::string& key,
                                          const std::string& value);
static bool ReplaceOrInsertJsonStringFieldAfter(std::string& json,
                                                const std::string& key,
                                                const std::string& value,
                                                const std::string& anchorKey);
static bool ReplaceOrInsertJsonArrayFieldAfter(std::string& json,
                                               const std::string& key,
                                               const std::string& newArrayJson,
                                               const std::string& anchorKey);

bool ConsumePendingStartupNotice(std::wstring* outText, SoftNoticeKind* outKind) {
    if (g_pendingStartupNoticeText.empty()) return false;
    if (outText) *outText = g_pendingStartupNoticeText;
    if (outKind) *outKind = g_pendingStartupNoticeKind;
    g_pendingStartupNoticeText.clear();
    g_pendingStartupNoticeKind = SoftNoticeKind::Info;
    return true;
}

std::optional<std::wstring> LoadSetupWorkspaceRoot() {
    wchar_t exePath[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        QueuePendingStartupNotice(L"実行ファイルのパス取得に失敗しました。", SoftNoticeKind::Error);
        return std::nullopt;
    }
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path setup = ResolveSetupJsonPath(exeDir);
    const std::filesystem::path defaultRoot = DefaultWorkspaceRootPath(exeDir);
    std::wstring setupErr;
    if (!EnsureDefaultSetupJson(exeDir, setup, &setupErr)) {
        if (setupErr.empty()) {
            setupErr = L"setup.json の作成に失敗しました。";
        }
        QueuePendingStartupNotice(setupErr, SoftNoticeKind::Error);
        return std::nullopt;
    }
    std::wstring readErr;
    std::string json;
    if (!ReadTextFileUtf8Limited(setup, kMaxSetupJsonBytes, &json, &readErr)) {
        std::wstring message =
            L"設定JSONファイルの読み込みに失敗しました。\n\npath:\n" + setup.wstring();
        if (!readErr.empty()) {
            message += L"\n\n理由:\n" + readErr;
        }
        message += L"\n\n既定ワークスペースで起動を継続します:\n" + defaultRoot.wstring();
        const std::filesystem::path quarantined = QuarantineCorruptSetupJson(exeDir, setup);
        if (!quarantined.empty()) {
            message += L"\n\n読めない setup.json を退避しました:\n" + quarantined.wstring();
        } else {
            message += L"\n\n読めない setup.json の退避に失敗しました。元ファイルは上書きしません。";
        }
        QueuePendingStartupNotice(message, SoftNoticeKind::Warning);
        return defaultRoot.wstring();
    }
    const bool setupJsonSyntaxOk = IsSyntacticallyValidJsonLite(json);
    if (!setupJsonSyntaxOk) {
        std::wstring message =
            L"設定JSONファイルが壊れているため、自動修復や既定値上書きは行いません。\n\npath:\n" +
            setup.wstring() +
            L"\n\n既定ワークスペースで起動を継続します:\n" + defaultRoot.wstring();
        const std::filesystem::path quarantined = QuarantineCorruptSetupJson(exeDir, setup);
        if (!quarantined.empty()) {
            message += L"\n\n読めない setup.json を退避しました:\n" + quarantined.wstring();
        } else {
            message += L"\n\n読めない setup.json の退避に失敗しました。元ファイルは上書きしません。";
        }
        QueuePendingStartupNotice(message, SoftNoticeKind::Warning);
        return defaultRoot.wstring();
    }

    std::vector<std::string> setupUnknownFields;
    const bool setupHasUnknownFields = SetupJsonHasUnknownTopLevelFields(json, &setupUnknownFields);
    bool setupAutoUpdateAllowed = !setupHasUnknownFields;
    if (setupHasUnknownFields) {
        std::wstring message = SetupJsonAutoUpdateBlockedReason(
            setup_json_policy::AutoUpdateDecision::BlockUnknownTopLevelField,
            L"",
            setupUnknownFields);
        message += L"\n\n既存 setup.json を既定値で上書きしないための保護です。";
        QueuePendingStartupNotice(message, SoftNoticeKind::Warning);
    }

    if (!json.empty()) {
        LoadAnnotToolUiConfigFromSetupJson(json);
    }
    auto ws = ParseJsonStringField(json, "workspaceRoot");
    if (!ws) {
        std::wstring message =
            L"設定JSONファイルの workspaceRoot を読み取れませんでした。\n"
            L"既存 setup.json は上書きしません。\n\npath:\n" +
            setup.wstring() +
            L"\n\n既定ワークスペースで起動を継続します:\n" + defaultRoot.wstring();
        const std::filesystem::path quarantined = QuarantineCorruptSetupJson(exeDir, setup);
        if (!quarantined.empty()) {
            message += L"\n\nworkspaceRoot を読めない setup.json を退避しました:\n" + quarantined.wstring();
        } else {
            message += L"\n\nworkspaceRoot を読めない setup.json の退避に失敗しました。元ファイルは上書きしません。";
        }
        QueuePendingStartupNotice(message, SoftNoticeKind::Warning);
        return defaultRoot.wstring();
    }
    std::filesystem::path root = UTF8ToWide(*ws);

    if (root.is_relative()) {
        root = exeDir / root;
    }
    // ディレクトリが存在しなくても作成しない（ここでcreate_directoriesしない）
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(root, ec);
    if (!ec && !canonical.empty()) {
        root = canonical;
    }

    std::error_code existsEc;
    bool rootExists = std::filesystem::exists(root, existsEc);
    if (existsEc) rootExists = false;
    if (!rootExists) {
        if (auto fallback = FindWorkspaceRootFallback(exeDir, root)) {
            std::wstring message = g_config.studentMode
                ? L"指定された授業フォルダが見つからなかったため、候補のワークスペースに切り替えました。\n\n"
                : L"指定された上位項目フォルダが見つからなかったため、候補のワークスペースに切り替えました。\n\n";
            message += L"設定: " + root.wstring() + L"\n";
            message += L"使用: " + fallback->wstring();
            QueuePendingStartupNotice(message, SoftNoticeKind::Warning);
            root = *fallback;
        } else {
            std::wstring message = g_config.studentMode ? L"指定された授業フォルダが見つかりません。\n\n"
                                                        : L"指定された上位項目フォルダが見つかりません。\n\n";
            message += root.wstring();
            QueuePendingStartupNotice(message, SoftNoticeKind::Warning);
        }
    }

    // setup.jsonのworkspaceRootを相対/絶対で自動更新
    std::string desiredPath = WorkspaceRootPathForSetupJson(exeDir, root);
    std::string desiredMode = WorkspaceRootModeForSetupJson(exeDir, root);
    std::string newJson = json;
    bool changed = false;
    if (*ws != desiredPath) {
        size_t pos = newJson.find(*ws);
        if (pos != std::string::npos) {
            newJson.replace(pos, ws->size(), desiredPath);
            changed = true;
        }
    }
    changed |= ReplaceOrInsertJsonStringFieldAfter(newJson, "workspaceRootMode", desiredMode, "workspaceRoot");
    if (changed && setupAutoUpdateAllowed) {
        SaveOperationGuard guard;
        std::wstring werr;
        if (!newJson.empty()) {
            AtomicWriteSetupJsonIfValid(setup, newJson, &werr);
        }
    }

    // workspace.jsonのclassdirチェック・更新
    std::filesystem::path wsjson = root / L"workspace.json";
    if (std::filesystem::exists(wsjson)) {
        std::wstring workspaceReadErr;
        std::string wsjson_str;
        if (!ReadTextFileUtf8Limited(wsjson, kMaxWorkspaceJsonBytes, &wsjson_str, &workspaceReadErr)) {
            std::wstring message =
                L"ワークスペース設定JSONファイルの読み込みに失敗しました。\n\npath:\n" +
                wsjson.wstring();
            if (!workspaceReadErr.empty()) {
                message += L"\n\n理由:\n" + workspaceReadErr;
            }
            QueuePendingStartupNotice(message, SoftNoticeKind::Warning);
            return root.wstring();
        }
        auto classdir = ParseJsonStringField(wsjson_str, "classesDir");
        std::wstring setupClassdir = root.filename();
        if (classdir && UTF8ToWide(*classdir) != setupClassdir) {
            // workspace.jsonのclassesDirをsetupのディレクトリ名に更新
            size_t pos = wsjson_str.find(*classdir);
            if (pos != std::string::npos) {
                std::string newWsjson = wsjson_str;
                std::string newClassdir = WideToUTF8(setupClassdir);
                newWsjson.replace(pos, classdir->size(), newClassdir);
                std::wstring werr;
                AtomicWriteUtf8WithWorkspaceDirs(wsjson, newWsjson, root, &werr);
                ShowAppCoreSoftNotice(nullptr,
                                      L"workspace.json の classesDir を setup.json の内容で更新しました。",
                                      SoftNoticeKind::Info);
            }
        }
    }

    return root.wstring();
}

// ワークスペースを開く時にsetup.jsonを書き換える関数
void UpdateSetupJsonWorkspaceRoot(const std::wstring& newRoot) {
    if (newRoot.empty()) return;
    // No-network requirement: never persist UNC/device-prefix workspace roots.
    if (newRoot.rfind(L"\\\\", 0) == 0) return;
    bool isReparse = false;
    if (TryIsReparsePointNoFollow(std::filesystem::path(newRoot), isReparse) && isReparse) return;
    wchar_t exePath[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return;
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path setup = ResolveSetupJsonPath(exeDir);
    if (!std::filesystem::exists(setup)) {
        WriteSetupJsonFile(setup, exeDir, std::filesystem::path(newRoot));
        return;
    }
    std::string json;
    std::wstring blockedReason;
    if (!ReadExistingSetupJsonForAutoUpdate(setup, &json, &blockedReason)) {
        if (!blockedReason.empty()) {
            blockedReason += L"\n\n既存 setup.json を保護するため、ワークスペースパスの自動更新を中止しました。";
            ShowAppCoreSoftNotice(nullptr, blockedReason, SoftNoticeKind::Warning);
        }
        return;
    }
    auto ws = ParseJsonStringField(json, "workspaceRoot");
    std::string desiredPath = WorkspaceRootPathForSetupJson(exeDir, std::filesystem::path(newRoot));
    std::string desiredMode = WorkspaceRootModeForSetupJson(exeDir, std::filesystem::path(newRoot));
    if (!ws || *ws != desiredPath) {
        // jsonのworkspaceRootだけ書き換え
        std::string newJson = json;
        if (ws) {
            size_t pos = newJson.find(*ws);
            if (pos != std::string::npos) {
                newJson.replace(pos, ws->size(), desiredPath);
            }
        } else {
            // workspaceRootがなければ追加（単純な実装）
            size_t insertPos = newJson.find_last_of('}');
            if (insertPos != std::string::npos) {
                newJson.insert(insertPos, ",\n  \"workspaceRoot\": \"" + desiredPath + "\"");
            }
        }
        ReplaceOrInsertJsonStringFieldAfter(newJson, "workspaceRootMode", desiredMode, "workspaceRoot");
        if (!newJson.empty()) {
            SaveOperationGuard guard;
            std::wstring werr;
            AtomicWriteSetupJsonIfValid(setup, newJson, &werr);
        }
        return;
    }
    std::string newJson = json;
    if (ReplaceOrInsertJsonStringFieldAfter(newJson, "workspaceRootMode", desiredMode, "workspaceRoot")) {
        if (!newJson.empty()) {
            SaveOperationGuard guard;
            std::wstring werr;
            AtomicWriteSetupJsonIfValid(setup, newJson, &werr);
        }
    }
}

std::vector<std::wstring> LoadSetupTempExternalLectureDirs() {
    std::vector<std::wstring> result;
    wchar_t exePath[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return result;
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path setup = ResolveSetupJsonPath(exeDir);
    if (!std::filesystem::exists(setup)) return result;
    std::string json = ReadTextFileUtf8(setup);
    if (json.empty()) return result;
    auto entries = ParseJsonStringArrayField(json, "tempExternalLectureDirs");
    result.reserve(entries.size());
    for (const auto& item : entries) {
        if (item.empty()) continue;
        std::filesystem::path p = UTF8ToWide(item);
        if (p.is_relative()) {
            p = exeDir / p;
        }
        std::wstring w = p.wstring();
        if (w.rfind(L"\\\\", 0) == 0) continue; // block UNC/device paths
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(p, isReparse) && isReparse) continue;
        result.push_back(std::move(w));
    }
    return result;
}

static std::string BuildSetupPathArrayJson(const std::filesystem::path& exeDir,
                                           const std::vector<std::wstring>& dirs) {
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    for (const auto& w : dirs) {
        if (w.empty()) continue;
        if (w.rfind(L"\\\\", 0) == 0) continue; // block UNC/device paths
        std::filesystem::path p = w;
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(p, isReparse) && isReparse) continue;
        std::string s = WorkspaceRootPathForSetupJson(exeDir, p);
        if (!first) oss << ", ";
        first = false;
        oss << "\"" << s << "\"";
    }
    oss << "]";
    return oss.str();
}

bool PersistSetupTempExternalLectureDirs(const std::vector<std::wstring>& dirs) {
    wchar_t exePath[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path setup = ResolveSetupJsonPath(exeDir);
    std::string json;
    std::error_code existsEc;
    const bool setupExists = std::filesystem::exists(setup, existsEc) && !existsEc;
    if (setupExists) {
        std::wstring blockedReason;
        if (!ReadExistingSetupJsonForAutoUpdate(setup, &json, &blockedReason)) {
            if (!blockedReason.empty()) {
                blockedReason += L"\n\n既存 setup.json を保護するため、一時外部講義フォルダ一覧の保存を中止しました。";
                ShowAppCoreSoftNotice(nullptr, blockedReason, SoftNoticeKind::Warning);
            }
            return false;
        }
    } else {
        std::filesystem::path root = g_workspaceRoot.empty()
            ? DefaultWorkspaceRootPath(exeDir)
            : std::filesystem::path(g_workspaceRoot);
        if (!WriteSetupJsonFile(setup, exeDir, root)) return false;
        json = ReadTextFileUtf8(setup);
        if (json.empty()) return false;
    }

    std::string arrayJson = BuildSetupPathArrayJson(exeDir, dirs);
    bool changed = ReplaceOrInsertJsonArrayFieldAfter(
        json, "tempExternalLectureDirs", arrayJson, "workspaceRoot");
    if (!changed) return true;
    SaveOperationGuard guard;
    std::wstring werr;
    return AtomicWriteSetupJsonIfValid(setup, json, &werr);
}

static std::string BuildAnnotToolModeOrderArrayJson() {
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    for (ToolMode mode : g_annotToolModeUiOrder) {
        const char* k = ToolModeKey(mode);
        if (!k || !*k) continue;
        if (!first) oss << ", ";
        first = false;
        oss << "\"" << k << "\"";
    }
    oss << "]";
    return oss.str();
}

static std::string BuildAnnotToolModeStateArrayJson() {
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    for (ToolMode mode : DefaultAnnotToolModeUiOrder()) {
        const char* k = ToolModeKey(mode);
        if (!k || !*k) continue;
        if (!first) oss << ", ";
        first = false;
        int idx = static_cast<int>(mode);
        AnnotToolUiState s = (idx >= 0 && idx < kToolModeCount)
            ? g_annotToolModeUiStates[static_cast<size_t>(idx)]
            : AnnotToolUiState::Enabled;
        oss << "\"" << k << ":" << AnnotToolUiStateKey(s) << "\"";
    }
    oss << "]";
    return oss.str();
}

static bool ReplaceOrInsertJsonArrayField(std::string& json,
                                         const std::string& key,
                                         const std::string& newArrayJson) {
    std::regex re("\"" + key + "\"\\s*:\\s*\\[[^\\]]*\\]");
    std::smatch m;
    std::string replacement = "\"" + key + "\": " + newArrayJson;
    if (std::regex_search(json, m, re)) {
        std::string before = json;
        json = std::regex_replace(json, re, replacement, std::regex_constants::format_first_only);
        return json != before;
    }
    size_t brace = json.find_last_of('}');
    if (brace == std::string::npos) return false;
    size_t pos = brace;
    while (pos > 0 && std::isspace(static_cast<unsigned char>(json[pos - 1]))) pos--;
    bool needComma = true;
    if (pos <= 1) needComma = false;
    else {
        char last = json[pos - 1];
        if (last == '{' || last == ',') needComma = false;
    }
    std::string field = "  \"" + key + "\": " + newArrayJson;
    std::string insert = std::string(needComma ? ",\n" : "\n") + field + "\n";
    json.insert(brace, insert);
    return true;
}

static bool InsertJsonFieldAfterAnchor(std::string& json,
                                       const std::string& anchorKey,
                                       const std::string& fieldText) {
    std::regex anchorRe("\"" + anchorKey + "\"\\s*:\\s*(\"[^\"]*\"|\\[[^\\]]*\\])");
    std::smatch m;
    if (!std::regex_search(json, m, anchorRe)) return false;
    size_t anchorEnd = m.position() + m.length();
    size_t pos = anchorEnd;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        pos++;
    }
    bool hasComma = (pos < json.size() && json[pos] == ',');
    if (hasComma) {
        size_t insertPos = pos + 1;
        std::string insert = "\n" + fieldText + ",";
        json.insert(insertPos, insert);
        return true;
    }
    std::string insert = ",\n" + fieldText;
    json.insert(anchorEnd, insert);
    return true;
}

static bool ReplaceOrInsertJsonArrayFieldAfter(std::string& json,
                                               const std::string& key,
                                               const std::string& newArrayJson,
                                               const std::string& anchorKey) {
    std::regex re("\"" + key + "\"\\s*:\\s*\\[[^\\]]*\\]");
    std::smatch m;
    std::string replacement = "\"" + key + "\": " + newArrayJson;
    if (std::regex_search(json, m, re)) {
        std::string before = json;
        json = std::regex_replace(json, re, replacement, std::regex_constants::format_first_only);
        return json != before;
    }
    std::string field = "  \"" + key + "\": " + newArrayJson;
    if (InsertJsonFieldAfterAnchor(json, anchorKey, field)) return true;
    return ReplaceOrInsertJsonArrayField(json, key, newArrayJson);
}

static bool ReplaceOrInsertJsonStringFieldAfter(std::string& json,
                                                const std::string& key,
                                                const std::string& value,
                                                const std::string& anchorKey) {
    std::regex re("\"" + key + "\"\\s*:\\s*\"[^\"]*\"");
    std::smatch m;
    std::string replacement = "\"" + key + "\": \"" + value + "\"";
    if (std::regex_search(json, m, re)) {
        std::string before = json;
        json = std::regex_replace(json, re, replacement, std::regex_constants::format_first_only);
        return json != before;
    }
    std::string field = "  \"" + key + "\": \"" + value + "\"";
    if (InsertJsonFieldAfterAnchor(json, anchorKey, field)) return true;
    return ReplaceOrInsertJsonStringField(json, key, value);
}

static bool ReplaceOrInsertJsonStringField(std::string& json,
                                          const std::string& key,
                                          const std::string& value) {
    std::regex re("\"" + key + "\"\\s*:\\s*\"[^\"]*\"");
    std::smatch m;
    std::string replacement = "\"" + key + "\": \"" + value + "\"";
    if (std::regex_search(json, m, re)) {
        std::string before = json;
        json = std::regex_replace(json, re, replacement, std::regex_constants::format_first_only);
        return json != before;
    }
    size_t brace = json.find_last_of('}');
    if (brace == std::string::npos) return false;
    size_t pos = brace;
    while (pos > 0 && std::isspace(static_cast<unsigned char>(json[pos - 1]))) pos--;
    bool needComma = true;
    if (pos <= 1) needComma = false;
    else {
        char last = json[pos - 1];
        if (last == '{' || last == ',') needComma = false;
    }
    std::string field = "  \"" + key + "\": \"" + value + "\"";
    std::string insert = std::string(needComma ? ",\n" : "\n") + field + "\n";
    json.insert(brace, insert);
    return true;
}

static bool RemoveJsonScalarOrArrayField(std::string& json, const std::string& key) {
    std::regex lineRe("\\s*\"" + key + "\"\\s*:\\s*(\"[^\"]*\"|\\[[^\\]]*\\])\\s*,?\\s*\\n");
    std::string before = json;
    json = std::regex_replace(json, lineRe, "", std::regex_constants::format_first_only);
    if (json != before) return true;

    std::regex inlineRe(",?\\s*\"" + key + "\"\\s*:\\s*(\"[^\"]*\"|\\[[^\\]]*\\])\\s*,?");
    before = json;
    json = std::regex_replace(json, inlineRe, "", std::regex_constants::format_first_only);
    return json != before;
}

bool PersistAnnotToolUiConfigToSetupJson() {
    wchar_t exePath[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path setup = ResolveSetupJsonPath(exeDir);

    std::string json;
    std::error_code existsEc;
    const bool setupExists = std::filesystem::exists(setup, existsEc) && !existsEc;
    if (setupExists) {
        std::wstring blockedReason;
        if (!ReadExistingSetupJsonForAutoUpdate(setup, &json, &blockedReason)) {
            if (!blockedReason.empty()) {
                blockedReason += L"\n\n既存 setup.json を保護するため、注釈ツールUI設定の保存を中止しました。";
                ShowAppCoreSoftNotice(nullptr, blockedReason, SoftNoticeKind::Warning);
            }
            return false;
        }
    } else {
        std::filesystem::path root = g_workspaceRoot.empty()
            ? DefaultWorkspaceRootPath(exeDir)
            : std::filesystem::path(g_workspaceRoot);
        if (!WriteSetupJsonFile(setup, exeDir, root)) return false;
        json = ReadTextFileUtf8(setup);
        if (json.empty()) return false;
    }

    bool changed = false;
    changed |= RemoveJsonScalarOrArrayField(json, "annotToolUiStructure");
    changed |= RemoveJsonScalarOrArrayField(json, "annotToolOrder");
    changed |= RemoveJsonScalarOrArrayField(json, "annotToolState");
    changed |= ReplaceOrInsertJsonArrayField(json, "annotToolModeOrder", BuildAnnotToolModeOrderArrayJson());
    changed |= ReplaceOrInsertJsonArrayField(json, "annotToolModeState", BuildAnnotToolModeStateArrayJson());
    std::filesystem::path root = g_workspaceRoot.empty()
        ? DefaultWorkspaceRootPath(exeDir)
        : std::filesystem::path(g_workspaceRoot);
    changed |= ReplaceOrInsertJsonStringFieldAfter(
        json, "workspaceRootMode", WorkspaceRootModeForSetupJson(exeDir, root), "workspaceRoot");
    if (!changed) return true;

    SaveOperationGuard guard;
    std::wstring werr;
    return AtomicWriteSetupJsonIfValid(setup, json, &werr);
}

// workspace.jsonのclassDirをチェックし、ずれていればユーザーに選択を促す
void CheckAndPromptClassdirMismatch(HWND hWnd, const std::wstring& workspaceRoot) {
    std::filesystem::path wsjson = std::filesystem::path(workspaceRoot) / L"workspace.json";
    if (!std::filesystem::exists(wsjson)) return;
    
    std::wstring readErr;
    std::string wsjson_str;
    if (!ReadTextFileUtf8Limited(wsjson, kMaxWorkspaceJsonBytes, &wsjson_str, &readErr)) {
        return;
    }
    auto classdir = ParseJsonStringField(wsjson_str, "classesDir");
    if (!classdir) return;
    
    // setup側のディレクトリ名を取得
    std::wstring setupClassdir = std::filesystem::path(workspaceRoot).filename().wstring();
    std::wstring currentClassdir = UTF8ToWide(*classdir);
    
    // ずれている場合のみ処理
    if (currentClassdir == setupClassdir) return;
    
    // ずれている場合、ユーザーに選択を促す
    std::wstring message = L"workspace.jsonのclassesDirが異なります。\n\n";
    message += L"setup.json側: " + setupClassdir + L"\n";
    message += L"workspace.json側: " + currentClassdir + L"\n\n";
    message += L"workspace.jsonを更新して合わせますか？\n";
    message += L"[はい] 更新して合わせる\n[いいえ] そのままにする";
    
    if (ConfirmAppCoreYesNo(hWnd, L"classDirの確認", message, SoftNoticeKind::Warning,
                            SilentDialogResult::No, SilentDialogResult::No)) {
        // workspace.jsonのclassesDirをsetup側のディレクトリ名に更新
        std::string newWsjson = wsjson_str;
        size_t pos = newWsjson.find(*classdir);
        if (pos != std::string::npos) {
            std::string newClassdir = WideToUTF8(setupClassdir);
            newWsjson.replace(pos, classdir->size(), newClassdir);
            std::wstring werr;
            if (AtomicWriteUtf8WithWorkspaceDirs(wsjson, newWsjson, std::filesystem::path(workspaceRoot), &werr)) {
                ShowAppCoreSoftNotice(hWnd, L"workspace.json の classesDir を更新しました。",
                                      SoftNoticeKind::Info);
            }
        }
    }
}


namespace {
static constexpr wchar_t kSoftNoticeClass[] = L"PdfWorkspaceSoftNotice";
static constexpr UINT_PTR kSoftNoticeTimerId = 0x5E10;
static constexpr int kSoftNoticeMarginPx = 16;
static constexpr int kSoftNoticePaddingX = 12;
static constexpr int kSoftNoticePaddingY = 8;
static constexpr int kSoftNoticeMaxWidthPx = 420;
static constexpr int kSoftNoticeAnchorGapPx = 18;
static constexpr ULONGLONG kSoftNoticeRepeatSuppressMs = 4000;

static HWND g_hSoftNotice = nullptr;
static std::wstring g_softNoticeText;
static SoftNoticeKind g_softNoticeKind = SoftNoticeKind::Info;

struct UiMessageRepeatState {
    std::wstring title;
    std::wstring message;
    SoftNoticeKind kind = SoftNoticeKind::Info;
    ULONGLONG lastShownTick = 0;
};

static UiMessageRepeatState g_softNoticeRepeatState;

// Only track the immediately previous notice to cheaply suppress retry storms.
static bool ShouldSuppressRepeatedUiMessage(UiMessageRepeatState& state,
                                            const wchar_t* title,
                                            const std::wstring& message,
                                            SoftNoticeKind kind,
                                            ULONGLONG suppressMs) {
    const wchar_t* normalizedTitle = title ? title : L"";
    const ULONGLONG now = GetTickCount64();
    if (state.kind == kind &&
        state.title == normalizedTitle &&
        state.message == message &&
        state.lastShownTick != 0 &&
        now - state.lastShownTick < suppressMs) {
        return true;
    }
    state.title = normalizedTitle;
    state.message = message;
    state.kind = kind;
    state.lastShownTick = now;
    return false;
}

static int DpiScaleY(HWND hWnd, int pxAt96Dpi) {
    HDC dc = GetDC(hWnd);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(hWnd, dc);
    if (dpi <= 0) dpi = 96;
    return MulDiv(pxAt96Dpi, dpi, 96);
}

static void DrawSoftNoticeBackground(HDC hdc, const RECT& rc, COLORREF bg, COLORREF border) {
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

static UINT SoftNoticeDurationMsForKind(SoftNoticeKind kind) {
    switch (kind) {
    case SoftNoticeKind::Warning:
        return 2600;
    case SoftNoticeKind::Error:
        return 3400;
    case SoftNoticeKind::Info:
    default:
        return 1800;
    }
}

static void GetSoftNoticeColors(SoftNoticeKind kind, COLORREF& outBg, COLORREF& outBorder, COLORREF& outText) {
    COLORREF accent = g_theme.accent;
    switch (kind) {
    case SoftNoticeKind::Warning:
        accent = BlendColor(g_theme.accent, RGB(214, 144, 24), 0.75);
        break;
    case SoftNoticeKind::Error:
        accent = BlendColor(g_theme.accent, RGB(196, 64, 64), 0.85);
        break;
    case SoftNoticeKind::Info:
    default:
        break;
    }
    outBg = BlendColor(g_theme.panelBg, accent, (kind == SoftNoticeKind::Info) ? 0.08 : 0.14);
    outBorder = BlendColor(g_theme.buttonBorder, accent, (kind == SoftNoticeKind::Info) ? 0.20 : 0.42);
    outText = g_theme.panelText;
}

static LRESULT CALLBACK SoftNoticeProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT; // do not steal clicks
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wParam == kSoftNoticeTimerId) {
            KillTimer(hWnd, kSoftNoticeTimerId);
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc{};
        GetClientRect(hWnd, &rc);

        COLORREF bg = g_theme.panelBg;
        COLORREF border = g_theme.buttonBorder;
        COLORREF text = g_theme.panelText;
        GetSoftNoticeColors(g_softNoticeKind, bg, border, text);

        DrawSoftNoticeBackground(hdc, rc, bg, border);

        RECT trc = rc;
        int padX = DpiScaleY(hWnd, kSoftNoticePaddingX);
        int padY = DpiScaleY(hWnd, kSoftNoticePaddingY);
        trc.left += padX;
        trc.right -= padX;
        trc.top += padY;
        trc.bottom -= padY;

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, text);
        HFONT old = nullptr;
        if (g_hUIFont) old = static_cast<HFONT>(SelectObject(hdc, g_hUIFont));
        DrawTextW(hdc, g_softNoticeText.c_str(), -1, &trc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
        if (old) SelectObject(hdc, old);

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (g_hSoftNotice == hWnd) g_hSoftNotice = nullptr;
        break;
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void EnsureSoftNoticeWindow(HWND ownerForZOrder) {
    if (g_hSoftNotice && IsWindow(g_hSoftNotice)) return;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SoftNoticeProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kSoftNoticeClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        registered = true;
    }

    g_hSoftNotice = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kSoftNoticeClass,
        L"",
        WS_POPUP,
        0, 0, 10, 10,
        ownerForZOrder,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
}

static RECT SoftNoticeFallbackWorkRect() {
    return RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
}

static RECT SoftNoticeWorkRectForRect(const RECT& rc) {
    HMONITOR monitor = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (monitor && GetMonitorInfoW(monitor, &mi)) {
        return mi.rcWork;
    }
    return SoftNoticeFallbackWorkRect();
}

static RECT SoftNoticeWorkRectForPoint(const POINT& pt) {
    HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (monitor && GetMonitorInfoW(monitor, &mi)) {
        return mi.rcWork;
    }
    return SoftNoticeFallbackWorkRect();
}

static bool TryGetSoftNoticeCursorPoint(HWND anchor, POINT* outPt) {
    if (!outPt) return false;
    POINT pt{};
    if (!GetCursorPos(&pt)) return false;
    if (anchor && IsWindow(anchor)) {
        RECT anchorRc{};
        if (GetWindowRect(anchor, &anchorRc) && !PtInRect(&anchorRc, pt)) {
            return false;
        }
    }
    *outPt = pt;
    return true;
}

static bool TryGetSoftNoticeFocusRect(HWND anchor, RECT* outRc) {
    if (!outRc) return false;

    HWND focus = nullptr;
    DWORD threadId = 0;
    if (anchor && IsWindow(anchor)) {
        threadId = GetWindowThreadProcessId(anchor, nullptr);
    }
    if (threadId != 0) {
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        if (GetGUIThreadInfo(threadId, &info)) {
            focus = info.hwndFocus;
        }
    }
    if (!focus) {
        focus = GetFocus();
    }
    if (!focus || !IsWindow(focus) || !IsWindowVisible(focus)) return false;
    if (anchor && focus != anchor && !IsChild(anchor, focus)) return false;
    if (!GetWindowRect(focus, outRc)) return false;
    return (outRc->right > outRc->left && outRc->bottom > outRc->top);
}

static void ClampSoftNoticePosition(const RECT& workRc, int margin, int w, int h, int* x, int* y) {
    if (!x || !y) return;
    const int minX = static_cast<int>(workRc.left) + margin;
    const int minY = static_cast<int>(workRc.top) + margin;
    const int maxX = std::max(minX, static_cast<int>(workRc.right) - w - margin);
    const int maxY = std::max(minY, static_cast<int>(workRc.bottom) - h - margin);
    *x = std::clamp(*x, minX, maxX);
    *y = std::clamp(*y, minY, maxY);
}

static void PlaceSoftNoticeNearPoint(const POINT& pt, const RECT& workRc,
                                     int gap, int margin, int w, int h, int* outX, int* outY) {
    if (!outX || !outY) return;
    int x = pt.x + gap;
    int y = pt.y + gap;
    if (x + w + margin > static_cast<int>(workRc.right)) {
        x = pt.x - w - gap;
    }
    if (y + h + margin > static_cast<int>(workRc.bottom)) {
        y = pt.y - h - gap;
    }
    ClampSoftNoticePosition(workRc, margin, w, h, &x, &y);
    *outX = x;
    *outY = y;
}

static void PlaceSoftNoticeNearRect(const RECT& targetRc, const RECT& workRc,
                                    int gap, int margin, int w, int h, int* outX, int* outY) {
    if (!outX || !outY) return;
    int x = static_cast<int>(targetRc.left) + gap;
    int y = static_cast<int>(targetRc.bottom) + gap;
    if (x + w + margin > static_cast<int>(workRc.right)) {
        x = static_cast<int>(targetRc.right) - w - gap;
    }
    if (y + h + margin > static_cast<int>(workRc.bottom)) {
        y = static_cast<int>(targetRc.top) - h - gap;
    }
    ClampSoftNoticePosition(workRc, margin, w, h, &x, &y);
    *outX = x;
    *outY = y;
}

static void PlaceSoftNoticeBottomRight(const RECT& ownerRc, const RECT& workRc,
                                       int margin, int w, int h, int* outX, int* outY) {
    if (!outX || !outY) return;
    int x = static_cast<int>(ownerRc.right) - w - margin;
    int y = static_cast<int>(ownerRc.bottom) - h - margin;
    ClampSoftNoticePosition(workRc, margin, w, h, &x, &y);
    *outX = x;
    *outY = y;
}
} // namespace

void ShowSoftNotice(HWND owner, const std::wstring& text, SoftNoticeKind kind) {
    std::wstring msg = text;
    msg.erase(std::remove(msg.begin(), msg.end(), L'\r'), msg.end());
    while (!msg.empty() && (msg.back() == L'\n' || iswspace(msg.back()))) msg.pop_back();
    if (msg.empty()) return;
    if (ShouldSuppressRepeatedUiMessage(g_softNoticeRepeatState, nullptr, msg, kind,
                                        kSoftNoticeRepeatSuppressMs)) {
        return;
    }

    HWND anchor = owner ? owner : GetForegroundWindow();
    if (!anchor) anchor = GetActiveWindow();
    if (anchor) {
        HWND root = GetAncestor(anchor, GA_ROOT);
        if (root) anchor = root;
    }
    EnsureSoftNoticeWindow(anchor);
    if (!g_hSoftNotice) return;

    g_softNoticeText = msg;
    g_softNoticeKind = kind;

    RECT ownerRc{};
    if (!anchor || !GetWindowRect(anchor, &ownerRc)) {
        ownerRc = SoftNoticeFallbackWorkRect();
    }

    HDC hdc = GetDC(g_hSoftNotice);
    HFONT oldFont = nullptr;
    if (hdc && g_hUIFont) oldFont = static_cast<HFONT>(SelectObject(hdc, g_hUIFont));

    int padX = DpiScaleY(g_hSoftNotice, kSoftNoticePaddingX);
    int padY = DpiScaleY(g_hSoftNotice, kSoftNoticePaddingY);
    int margin = DpiScaleY(g_hSoftNotice, kSoftNoticeMarginPx);
    int gap = DpiScaleY(g_hSoftNotice, kSoftNoticeAnchorGapPx);
    int maxW = DpiScaleY(g_hSoftNotice, kSoftNoticeMaxWidthPx);

    RECT calc{ 0, 0, maxW - padX * 2, 0 };
    DrawTextW(hdc, g_softNoticeText.c_str(), -1, &calc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);

    if (hdc && oldFont) SelectObject(hdc, oldFont);
    if (hdc) ReleaseDC(g_hSoftNotice, hdc);

    int w = (calc.right - calc.left) + padX * 2;
    int h = (calc.bottom - calc.top) + padY * 2;
    w = std::max(w, DpiScaleY(g_hSoftNotice, 140));
    h = std::max(h, DpiScaleY(g_hSoftNotice, 32));

    int x = 0;
    int y = 0;
    POINT cursorPt{};
    RECT focusRc{};
    if (TryGetSoftNoticeCursorPoint(anchor, &cursorPt)) {
        RECT workRc = SoftNoticeWorkRectForPoint(cursorPt);
        PlaceSoftNoticeNearPoint(cursorPt, workRc, gap, margin, w, h, &x, &y);
    } else if (TryGetSoftNoticeFocusRect(anchor, &focusRc)) {
        RECT workRc = SoftNoticeWorkRectForRect(focusRc);
        PlaceSoftNoticeNearRect(focusRc, workRc, gap, margin, w, h, &x, &y);
    } else {
        RECT workRc = SoftNoticeWorkRectForRect(ownerRc);
        PlaceSoftNoticeBottomRight(ownerRc, workRc, margin, w, h, &x, &y);
    }

    KillTimer(g_hSoftNotice, kSoftNoticeTimerId);
    SetWindowPos(g_hSoftNotice, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    InvalidateRect(g_hSoftNotice, nullptr, TRUE);
    UpdateWindow(g_hSoftNotice);
    SetTimer(g_hSoftNotice, kSoftNoticeTimerId, SoftNoticeDurationMsForKind(kind), nullptr);
}
// --- Global Beep Filter ---
static HHOOK g_hBeepFilterHook = nullptr;

static LRESULT CALLBACK BeepFilterHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && wParam == PM_REMOVE) {
        MSG* msg = reinterpret_cast<MSG*>(lParam);
        if (msg->message == WM_CHAR && (msg->wParam == L'\r' || msg->wParam == 27)) {
            wchar_t cls[64]{};
            if (GetClassNameW(msg->hwnd, cls, 63)) {
                if (_wcsicmp(cls, L"Edit") == 0) {
                    LONG_PTR style = GetWindowLongPtrW(msg->hwnd, GWL_STYLE);
                    if ((style & ES_MULTILINE) == 0) {
                        msg->message = WM_NULL;
                    } else if (msg->wParam == 27) {
                        msg->message = WM_NULL;
                    }
                }
            }
        }
    }
    return CallNextHookEx(g_hBeepFilterHook, code, wParam, lParam);
}

void InstallGlobalBeepFilter() {
    if (!g_hBeepFilterHook) {
        g_hBeepFilterHook = SetWindowsHookExW(WH_GETMESSAGE, BeepFilterHookProc, nullptr, GetCurrentThreadId());
    }
}
