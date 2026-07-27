// file: search.cpp
#include "search/search.h"
#include "ui/noop_nav_guard.h"

#include "resources/app_resource.h"
#include "core/app_core.h"
#include "core/fault_injection.h"
#include "clrop/bridge.h"
#include "file_output/file_output.h"
#include "file_output/note_snapshot.h"
#include "bridge/view_bridge.h"
#include "note/note_semantic_index.h"
#include "note/note_identity_store.h"
#include "note/note_workspace_service.h"
#include "note_view/note_view.h"
#include "pdf_view/pdf_view.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <optional>
#include <unordered_map>

namespace {

enum class SearchRange { CurrentLecture = 0, CurrentSession, WholeWorkspace, WorkspaceAndTempExternal };
enum SearchTargetMask : unsigned {
    TargetNote = 1u << 0,
    TargetPdf = 1u << 1,
    TargetAnnot = 1u << 2,
};
constexpr int kSearchRangeCount = 4;
constexpr int kSearchTargetCount = 3;
constexpr SearchRange kInvalidSearchRange = static_cast<SearchRange>(-1);
// Search window child control IDs are scoped to SearchWndProc. The same numeric
// values may exist in other child windows, but must stay unique within this proc.
constexpr int IDC_SEARCH_QUERY = 4101;
constexpr int IDC_SEARCH_RUN = 4102;
constexpr int IDC_SEARCH_RESULTS = 4103;
constexpr int IDC_SEARCH_STATUS = 4104;
constexpr int IDC_SEARCH_RANGE_BASE = 4110;
constexpr int IDC_SEARCH_TARGET_BASE = 4130;
constexpr int IDC_SEARCH_OPTION_BASE = 4140;
constexpr UINT kSearchTimerId = 4120;
constexpr UINT kSearchTimerIntervalMs = 30;
constexpr int kSearchEnumStepsPerTick = 120;
constexpr int kSearchNoteFilesPerTick = 1;
constexpr int kSearchPdfPagesPerTick = 1;
constexpr int IDC_SEARCH_OPTION_NORMALIZE = IDC_SEARCH_OPTION_BASE + 0;
constexpr int IDC_SEARCH_OPTION_IGNORE_CASE = IDC_SEARCH_OPTION_BASE + 1;
constexpr int IDC_SEARCH_OPTION_TRANSLUCENT = IDC_SEARCH_OPTION_BASE + 2;
constexpr UINT kSearchResultHideCommand = 42001;
constexpr UINT kSearchResultOpenReadonlyCommand = 42002;
// SearchWnd-local WM_APP message. Keep value in the central registry comment
// in core/app_core.h to make cross-file WM_APP audits simple.
constexpr UINT kMsgOpenSearchResult = WM_APP + 420;
static constexpr wchar_t kSearchWndClass[] = L"SearchWnd";
static HWND g_hSearchWnd = nullptr;

static HICON LoadSearchWindowIcon(int width, int height) {
    HICON icon = static_cast<HICON>(LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                               width, height, LR_DEFAULTCOLOR | LR_SHARED));
    if (icon) return icon;
    return LoadIcon(nullptr, IDI_APPLICATION);
}

struct SearchUiStrings {
    std::wstring title;
    std::wstring queryLabel;
    std::wstring queryHint;
    std::wstring queryCue;
    std::wstring runLabel;
    std::wstring cancelLabel;
    std::wstring rangeLabel;
    std::wstring targetLabel;
    std::wstring optionsLabel;
    std::wstring normalizeWidthKanaLabel;
    std::wstring ignoreCaseLabel;
    std::wstring translucentLabel;
    std::wstring resultLabel;
    std::wstring resultHint;
    std::wstring hideResultLabel;
    std::wstring openReadonlyLabel;
    std::wstring statusLabel;
    std::wstring summaryLabel;
    std::wstring statusReady;
    std::wstring statusSearching;
    std::wstring statusDone;
    std::wstring statusCanceled;
    std::wstring statusHits;
    std::wstring statusFiles;
    std::wstring statusPages;
    std::vector<std::wstring> ranges;
    std::vector<std::wstring> targets;
    std::wstring msgNeedQuery;
    std::wstring msgNeedRange;
    std::wstring msgNeedTarget;
    std::wstring msgNoNote;
    std::wstring msgNoPdf;
    std::wstring msgNoSession;
    std::wstring msgNoLecture;
    std::wstring msgNoSearchableFiles;
    std::wstring msgNoResults;
    std::wstring tagNote;
    std::wstring tagPdf;
    std::wstring tagAnnot;
    std::wstring tagPath;
    std::wstring metaLine;
    std::wstring metaPage;
    std::wstring metaHits;
    std::wstring metaCurrent;
};

static SearchUiStrings GetSearchUiStrings() {
    if (IsEnglishUi()) {
        SearchUiStrings s{
            L"Search",
            L"Query",
            L"Spaces=AND, OR/|=OR, quotes=phrase. Width variants and separators (space, ,、・) are ignored.",
            L"term OR term, term | term, \"exact phrase\", A,B",
            L"Search",
            L"Cancel",
            L"Range (choose one)",
            L"Target (choose any)",
            L"Options",
            L"Normalize width/kana and separators",
            L"Ignore case",
            L"Translucent window",
            L"Results",
            L"Enter/double-click=open, right-click=more, Delete=hide",
            L"Hide from results",
            L"Open in read-only viewer",
            L"Status",
            L"Summary",
            L"Ready.",
            L"Searching",
            L"Done.",
            L"Canceled.",
            L"Hits: ",
            L"Files: ",
            L"Pages: ",
            { L"Open lecture", L"Open session", L"Whole workspace", L"Whole workspace + temporary paths" },
            { L"Note", L"PDF", L"Annotations" },
            L"Enter a search term.",
            L"Select a range.",
            L"Select at least one target.",
            L"No note is open.",
            L"No PDF is open.",
            L"No session is open.",
            L"No lecture is open.",
            L"No searchable files were found.",
            L"No matches.",
            L"Note",
            L"PDF",
            L"Annot",
            L"Path",
            L"Line",
            L"Page",
            L"hits",
            L"Current"
        };
        if (!g_config.studentMode) {
            s.ranges = { L"Open parent item", L"Open child item", L"Whole workspace", L"Whole workspace + temporary paths" };
            s.msgNoSession = L"No child item is open.";
            s.msgNoLecture = L"No parent item is open.";
        }
        return s;
    }
    SearchUiStrings s{
        L"検索",
        L"検索語",
        L"空白=AND、OR/|=OR、\"...\"=フレーズ。半角/全角と区切り（空白,、・）は同一視",
        L"語 OR 語、語 | 語、\"フレーズ\"、A,B",
        L"検索",
        L"中止",
        L"検索範囲（1つ選択）",
        L"検索対象（複数選択可）",
        L"オプション",
        L"全角/半角・カナ・区切りを同一視",
        L"大文字/小文字を同一視",
        L"検索ウィンドウを半透明",
        L"結果",
        L"Enter/ダブルクリック=開く、右クリック=操作、Delete=下げる",
        L"検索結果から下げる",
        L"読み取り専用ビューアで開く",
        L"状態",
        L"内訳",
        L"待機中",
        L"検索中",
        L"完了",
        L"中断",
        L"一致: ",
        L"ファイル: ",
        L"ページ: ",
        { L"開いている授業", L"開いている回次", L"ワークスペース全体", L"ワークスペース+一時パス全体" },
        { L"ノート", L"PDF", L"注釈" },
        L"検索語を入力してください。",
        L"検索範囲を選択してください。",
        L"検索対象を1つ以上選択してください。",
        L"ノートが開かれていません。",
        L"PDFが開かれていません。",
        L"セッションが開かれていません。",
        L"授業が開かれていません。",
        L"検索できるファイルが見つかりません。",
        L"該当なし",
        L"ノート",
        L"PDF",
        L"注釈",
        L"パス",
        L"行",
        L"ページ",
        L"件",
        L"現在"
    };
    if (!g_config.studentMode) {
        s.ranges = { L"開いている上位項目", L"開いている下位項目", L"ワークスペース全体", L"ワークスペース+一時パス全体" };
        s.msgNoSession = L"下位項目が開かれていません。";
        s.msgNoLecture = L"上位項目が開かれていません。";
    }
    return s;
}

enum class SearchResultKind { Info, NoteLine, PdfPage, Annot, Path };

using SearchQueryGroups = note::NoteQueryGroups;

struct SearchResultItem {
    SearchResultKind kind = SearchResultKind::Info;
    std::wstring display;
    std::wstring title;
    std::wstring meta;
    std::wstring preview;
    std::filesystem::path path;
    note::SnapshotIdentity noteSnapshotIdentity{};
    int lineNumber = -1;
    int pageIndex = -1;
    size_t textStart = std::wstring::npos;
    size_t textEnd = std::wstring::npos;
    int hitCount = 0;
    double focusX = 0.0;
    double focusY = 0.0;
    bool hasFocusPoint = false;
};

struct PdfScanState {
    std::filesystem::path path;
    FPDF_DOCUMENT doc = nullptr;
    int pageIndex = 0;
    int pageCount = 0;
    std::wstring display;
    bool ownsDoc = false;
    bool includeAnnots = false;
};

struct StagedSearchFile {
    std::filesystem::path targetPath;
    std::filesystem::path stagePath;
    uint64_t persistenceRevision = 0;
};

struct SearchJob {
    bool active = false;
    bool canceled = false;
    SearchRange range = SearchRange::CurrentLecture;
    unsigned targetMask = TargetNote | TargetPdf | TargetAnnot;
    std::wstring query;
    SearchQueryGroups queryTermsLower;
    note::SemanticSearchOptions options;
    size_t matchCount = 0;
    size_t noteMatches = 0;
    size_t pdfMatches = 0;
    size_t annotMatches = 0;
    size_t pathMatches = 0;
    size_t processedFiles = 0;
    size_t processedPages = 0;

    std::vector<SearchResultItem> results;

    bool enumerationDone = true;
    bool includePaths = false;
    bool includeNotes = false;
    bool includePdfs = false;
    bool includeAnnots = false;
    std::vector<std::filesystem::path> roots;
    size_t rootIndex = 0;
    std::filesystem::recursive_directory_iterator iter;
    bool iterValid = false;

    std::vector<std::filesystem::path> noteFiles;
    size_t noteIndex = 0;
    std::vector<StagedSearchFile> stagedNoteFiles;
    size_t stagedNoteIndex = 0;
    std::vector<std::filesystem::path> pdfFiles;
    size_t pdfIndex = 0;
    std::vector<StagedSearchFile> stagedAnnotFiles;
    size_t stagedAnnotIndex = 0;
    std::optional<PdfScanState> pdf;
};

struct SearchCtx {
    HWND labelQuery = nullptr;
    HWND edit = nullptr;
    HWND btnRun = nullptr;
    HWND labelHint = nullptr;
    HWND labelStatus = nullptr;
    HWND labelSummary = nullptr;
    HWND labelRange = nullptr;
    std::vector<HWND> rangeRadios;
    HWND labelTarget = nullptr;
    std::vector<HWND> targetChecks;
    HWND labelOptions = nullptr;
    HWND optNormalizeWidthKana = nullptr;
    HWND optIgnoreCase = nullptr;
    HWND optTranslucent = nullptr;
    HWND labelResults = nullptr;
    HWND results = nullptr;
    HWND owner = nullptr;
    int statusPulse = 0;
    int maxResultWidth = 0;
    double hWheelRemainderPx = 0.0;
    int vWheelRemainder1000 = 0;
    std::wstring idleStatusText;
    SearchJob job;
};

static std::wstring ToLowerCopy(std::wstring s);

static void RunSearch(HWND hWnd, SearchCtx* ctx);
static void OpenSearchResult(HWND hWnd, SearchCtx* ctx, int listIndex);
static void CancelSearchJob(HWND hWnd, SearchCtx* ctx, bool silent);
static bool HideSearchResultAt(HWND hWnd, SearchCtx* ctx, int listIndex);
static void UpdateStatusLabel(SearchCtx* ctx, const SearchUiStrings& ui);
static void AppendInfo(SearchCtx* ctx, const std::wstring& text);
static bool CloseSearchWindowForCtrlF(HWND hWnd);

static bool IsCtrlFKeyDown(WPARAM wParam) {
    return wParam == static_cast<WPARAM>(L'F') &&
           (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
           (GetKeyState(VK_MENU) & 0x8000) == 0;
}

static bool CloseSearchWindowForCtrlF(HWND hWnd) {
    HWND root = hWnd ? GetAncestor(hWnd, GA_ROOT) : nullptr;
    if (!root) root = hWnd;
    if (!root) return false;
    DestroyWindow(root);
    return true;
}

static void QueueOpenSearchResult(HWND hWnd, int listIndex) {
    if (!hWnd || listIndex < 0) return;
    PostMessageW(hWnd, kMsgOpenSearchResult, static_cast<WPARAM>(listIndex), 0);
}

static void SetIdleStatus(SearchCtx* ctx, const std::wstring& text) {
    if (!ctx) return;
    ctx->idleStatusText = text;
}

static void ClearOpenedSearchResultMarkers() {
    ClearNoteSearchResultMarker();
    ClearPdfSearchResultMarker();
}

static COLORREF BlendColor(COLORREF a, COLORREF b, double t) {
    int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
    int r = static_cast<int>(std::lround(ar + (br - ar) * t));
    int g = static_cast<int>(std::lround(ag + (bg - ag) * t));
    int b2 = static_cast<int>(std::lround(ab + (bb - ab) * t));
    return RGB(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b2, 0, 255));
}

static double ColorLuminance(COLORREF c) {
    return 0.2126 * GetRValue(c) + 0.7152 * GetGValue(c) + 0.0722 * GetBValue(c);
}

static bool IsSearchToggleButtonId(int id) {
    if (id >= IDC_SEARCH_RANGE_BASE && id < IDC_SEARCH_RANGE_BASE + kSearchRangeCount) return true;
    return id >= IDC_SEARCH_TARGET_BASE && id < IDC_SEARCH_TARGET_BASE + kSearchTargetCount;
}

static bool IsSearchRangeButtonId(int id) {
    return id >= IDC_SEARCH_RANGE_BASE && id < IDC_SEARCH_RANGE_BASE + kSearchRangeCount;
}

static void EnableSearchOwnerDrawButton(HWND hWnd) {
    if (!hWnd) return;
    LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    if ((style & BS_OWNERDRAW) != 0) return;
    style |= BS_OWNERDRAW;
    SetWindowLongPtrW(hWnd, GWL_STYLE, style);
    InvalidateRect(hWnd, nullptr, TRUE);
}

static bool DrawSearchToggleButton(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_BUTTON || !dis->hwndItem) return false;
    const int id = GetDlgCtrlID(dis->hwndItem);
    if (!IsSearchToggleButtonId(id)) return false;

    const bool isRadio = IsSearchRangeButtonId(id);
    const bool checked = (SendMessageW(dis->hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED);
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool focused = (dis->itemState & ODS_FOCUS) != 0;

    const RECT rc = dis->rcItem;
    COLORREF rowBg = g_theme.panelBg;
    COLORREF rowBorder = BlendColor(g_theme.buttonBorder, g_theme.panelBg, 0.55);
    COLORREF text = g_theme.windowText;
    COLORREF indicatorFill = BlendColor(g_theme.buttonBg, g_theme.panelBg, 0.35);
    COLORREF indicatorBorder = BlendColor(g_theme.buttonBorder, g_theme.panelBg, 0.22);
    COLORREF indicatorMark = BlendColor(g_theme.accent, RGB(255, 255, 255), 0.18);
    if (checked) {
        rowBg = BlendColor(g_theme.selectionBg, g_theme.panelBg, 0.20);
        rowBorder = BlendColor(g_theme.accent, g_theme.buttonBorder, 0.55);
        text = (ColorLuminance(rowBg) < 150.0) ? RGB(255, 255, 255) : g_theme.windowText;
        indicatorFill = BlendColor(g_theme.accent, g_theme.selectionBg, 0.38);
        indicatorBorder = BlendColor(g_theme.accent, g_theme.buttonBorder, 0.72);
        indicatorMark = (ColorLuminance(indicatorFill) < 150.0) ? RGB(255, 255, 255) : g_theme.selectionText;
    } else if (pressed) {
        rowBg = BlendColor(g_theme.buttonPressed, g_theme.panelBg, 0.22);
        rowBorder = BlendColor(g_theme.buttonBorder, g_theme.buttonPressed, 0.45);
    }
    if (disabled) {
        text = BlendColor(text, rowBg, 0.48);
        rowBorder = BlendColor(rowBorder, rowBg, 0.45);
        indicatorFill = BlendColor(indicatorFill, rowBg, 0.45);
        indicatorBorder = BlendColor(indicatorBorder, rowBg, 0.45);
        indicatorMark = BlendColor(indicatorMark, rowBg, 0.42);
    }

    HBRUSH baseBrush = CreateSolidBrush(g_theme.panelBg);
    FillRect(dis->hDC, &rc, baseBrush);
    DeleteObject(baseBrush);

    RECT rowRc = rc;
    InflateRect(&rowRc, -2, -3);
    HBRUSH rowBrush = CreateSolidBrush(rowBg);
    HPEN rowPen = CreatePen(PS_SOLID, 1, rowBorder);
    HGDIOBJ oldBrush = SelectObject(dis->hDC, rowBrush);
    HGDIOBJ oldPen = SelectObject(dis->hDC, rowPen);
    RoundRect(dis->hDC, rowRc.left, rowRc.top, rowRc.right, rowRc.bottom, 7, 7);
    SelectObject(dis->hDC, oldPen);
    SelectObject(dis->hDC, oldBrush);
    DeleteObject(rowPen);
    DeleteObject(rowBrush);

    wchar_t label[128]{};
    GetWindowTextW(dis->hwndItem, label, static_cast<int>(sizeof(label) / sizeof(label[0])));

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, text);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(dis->hDC, font) : nullptr;

    const int indicatorSize = 16;
    const int indicatorLeft = rowRc.left + 12;
    const int buttonHeight = static_cast<int>(rowRc.bottom - rowRc.top);
    const int indicatorTop = static_cast<int>(rowRc.top) + std::max(0, (buttonHeight - indicatorSize) / 2);
    RECT indicatorRc{ indicatorLeft, indicatorTop, indicatorLeft + indicatorSize, indicatorTop + indicatorSize };

    if (isRadio) {
        HBRUSH indicatorBrush = CreateSolidBrush(indicatorFill);
        HPEN indicatorPen = CreatePen(PS_SOLID, 1, indicatorBorder);
        HGDIOBJ oldIndicatorBrush = SelectObject(dis->hDC, indicatorBrush);
        HGDIOBJ oldIndicatorPen = SelectObject(dis->hDC, indicatorPen);
        Ellipse(dis->hDC, indicatorRc.left, indicatorRc.top, indicatorRc.right, indicatorRc.bottom);
        SelectObject(dis->hDC, oldIndicatorPen);
        SelectObject(dis->hDC, oldIndicatorBrush);
        DeleteObject(indicatorPen);
        DeleteObject(indicatorBrush);
        if (checked) {
            RECT dotRc{ indicatorRc.left + 4, indicatorRc.top + 4, indicatorRc.right - 4, indicatorRc.bottom - 4 };
            HBRUSH dotBrush = CreateSolidBrush(indicatorMark);
            HGDIOBJ oldDotBrush = SelectObject(dis->hDC, dotBrush);
            HGDIOBJ oldDotPen = SelectObject(dis->hDC, GetStockObject(NULL_PEN));
            Ellipse(dis->hDC, dotRc.left, dotRc.top, dotRc.right, dotRc.bottom);
            SelectObject(dis->hDC, oldDotPen);
            SelectObject(dis->hDC, oldDotBrush);
            DeleteObject(dotBrush);
        }
    } else {
        HBRUSH indicatorBrush = CreateSolidBrush(indicatorFill);
        FillRect(dis->hDC, &indicatorRc, indicatorBrush);
        DeleteObject(indicatorBrush);
        HPEN framePen = CreatePen(PS_SOLID, 1, indicatorBorder);
        HGDIOBJ oldFramePen = SelectObject(dis->hDC, framePen);
        HGDIOBJ oldFrameBrush = SelectObject(dis->hDC, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dis->hDC, indicatorRc.left, indicatorRc.top, indicatorRc.right, indicatorRc.bottom);
        SelectObject(dis->hDC, oldFrameBrush);
        SelectObject(dis->hDC, oldFramePen);
        DeleteObject(framePen);
        if (checked) {
            HPEN checkPen = CreatePen(PS_SOLID, 2, indicatorMark);
            HGDIOBJ oldCheckPen = SelectObject(dis->hDC, checkPen);
            MoveToEx(dis->hDC, indicatorRc.left + 3, indicatorRc.top + indicatorSize / 2, nullptr);
            LineTo(dis->hDC, indicatorRc.left + indicatorSize / 2 - 1, indicatorRc.bottom - 4);
            LineTo(dis->hDC, indicatorRc.right - 3, indicatorRc.top + 4);
            SelectObject(dis->hDC, oldCheckPen);
            DeleteObject(checkPen);
        }
    }

    RECT textRc = rowRc;
    textRc.left = indicatorRc.right + 10;
    textRc.right -= 10;
    DrawTextW(dis->hDC, label, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (focused) {
        RECT focusRc = rowRc;
        InflateRect(&focusRc, -3, -3);
        DrawFocusRect(dis->hDC, &focusRc);
    }
    if (oldFont) SelectObject(dis->hDC, oldFont);
    return true;
}

static int CalcListItemHeightFromFont(HWND hList, int lineCount = 1, int extraPadPx = 6) {
    if (!hList) return 0;
    HDC hdc = GetDC(hList);
    if (!hdc) return 0;
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hList, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = nullptr;
    if (font) oldFont = SelectObject(hdc, font);
    TEXTMETRICW tm{};
    int height = 0;
    if (GetTextMetricsW(hdc, &tm)) {
        int lines = std::max(1, lineCount);
        height = std::max(22, static_cast<int>(tm.tmHeight) * lines + extraPadPx);
    }
    if (oldFont) SelectObject(hdc, oldFont);
    ReleaseDC(hList, hdc);
    return height;
}

using TextRange = note::NoteTextRange;
using SearchMatchInfo = note::NoteQueryMatch;

static int ClampHScrollPos(HWND hWnd, int pos) {
    if (!hWnd) return 0;
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE;
    if (!GetScrollInfo(hWnd, SB_HORZ, &si)) return std::max(0, pos);
    int minPos = static_cast<int>(si.nMin);
    int maxPos = static_cast<int>(si.nMax);
    int page = static_cast<int>(si.nPage);
    int maxEffective = maxPos - std::max(0, page - 1);
    if (maxEffective < minPos) maxEffective = minPos;
    return std::clamp(pos, minPos, maxEffective);
}

static int GetHScrollPosPx(HWND hWnd) {
    if (!hWnd) return 0;
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    if (GetScrollInfo(hWnd, SB_HORZ, &si)) return static_cast<int>(si.nPos);
    return static_cast<int>(GetScrollPos(hWnd, SB_HORZ));
}

static void SetHScrollPosPx(HWND hWnd, int pos) {
    if (!hWnd) return;
    pos = ClampHScrollPos(hWnd, pos);
    SendMessageW(hWnd, WM_HSCROLL, MAKEWPARAM(SB_THUMBPOSITION, pos), 0);
}

static int WheelStepPxForList(HWND hWnd, int aveCharWidth) {
    UINT chars = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &chars, 0);
    if (chars == WHEEL_PAGESCROLL) {
        RECT rc{};
        GetClientRect(hWnd, &rc);
        int w = static_cast<int>(rc.right - rc.left);
        return std::max(24, w - 16);
    }
    int c = static_cast<int>(std::clamp(chars, 1u, 120u));
    return std::max(8, c * std::max(1, aveCharWidth));
}

static int ListBoxVisibleCount(HWND list) {
    if (!list) return 1;
    RECT rc{};
    GetClientRect(list, &rc);
    int h = std::max(1, static_cast<int>(rc.bottom - rc.top));
    int itemH = static_cast<int>(SendMessageW(list, LB_GETITEMHEIGHT, 0, 0));
    if (itemH <= 0) itemH = 16;
    return std::max(1, h / itemH);
}

static void ScrollListBoxByWheel(HWND list, int delta, int* remainder1000) {
    if (!list || !IsWindow(list) || !remainder1000) return;

    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);

    if (lines == WHEEL_PAGESCROLL) {
        // Page-scroll uses full notches; no fractional smoothness available.
        *remainder1000 += static_cast<int>((static_cast<long long>(-delta) * 1000) / WHEEL_DELTA);
        int notches = *remainder1000 / 1000;
        if (notches == 0) return;
        *remainder1000 -= notches * 1000;

        int step = std::max(1, ListBoxVisibleCount(list) - 1);
        int top = static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0));
        int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
        if (top < 0 || count <= 0) return;

        int move = std::abs(notches) * step;
        int newTop = top + ((notches >= 0) ? move : -move);
        newTop = std::clamp(newTop, 0, std::max(0, count - 1));
        if (newTop != top) {
            SendMessageW(list, LB_SETTOPINDEX, static_cast<WPARAM>(newTop), 0);
        }
        return;
    }

    // Smooth(ish) scrolling: accumulate fractional lines from high-resolution wheel/touchpad.
    int step = std::max(1, static_cast<int>(lines));
    *remainder1000 += static_cast<int>((static_cast<long long>(-delta) * step * 1000) / WHEEL_DELTA);
    int moveLines = *remainder1000 / 1000;
    if (moveLines == 0) return;
    *remainder1000 -= moveLines * 1000;

    int top = static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0));
    int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
    if (top < 0 || count <= 0) return;

    int newTop = top + moveLines;
    newTop = std::clamp(newTop, 0, std::max(0, count - 1));
    if (newTop != top) {
        SendMessageW(list, LB_SETTOPINDEX, static_cast<WPARAM>(newTop), 0);
    }
}

static bool CanOpenSearchResultInReadonlyViewer(const SearchResultItem& item) {
    if (item.path.empty() || !IsPdfFile(item.path)) return false;
    return item.kind == SearchResultKind::PdfPage ||
           item.kind == SearchResultKind::Annot ||
           item.kind == SearchResultKind::Path;
}

static bool OpenSearchResultInReadonlyViewer(HWND owner, const SearchResultItem& item) {
    if (!CanOpenSearchResultInReadonlyViewer(item)) return false;
    const bool hasPdfY = (item.kind == SearchResultKind::Annot || item.kind == SearchResultKind::PdfPage) &&
                         item.hasFocusPoint &&
                         item.pageIndex >= 0;
    return LaunchReadOnlyViewerForPdfAt(owner,
                                        item.path.wstring(),
                                        item.pageIndex,
                                        item.focusY,
                                        hasPdfY);
}

static LRESULT CALLBACK SearchResultsProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR idSubclass, DWORD_PTR refData) {
    (void)idSubclass;
    auto* ctx = reinterpret_cast<SearchCtx*>(refData);
    static constexpr UINT_PTR kResultsRedrawTimerId = 0x5A23;
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        if (!hdc) break;
        RECT rc{};
        GetClientRect(hWnd, &rc);
        HBRUSH br = CreateSolidBrush(g_theme.panelBg);
        FillRect(hdc, &rc, br);
        DeleteObject(br);
        return 1;
    }
    case WM_MOUSEHWHEEL:
    case WM_MOUSEWHEEL: {
        const bool shiftWheel = (msg == WM_MOUSEWHEEL) && ((GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0);
        const bool horizWheel = (msg == WM_MOUSEHWHEEL) || shiftWheel;
        if (!horizWheel) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (ctx) {
                ScrollListBoxByWheel(hWnd, delta, &ctx->vWheelRemainder1000);
                return 0;
            }
            return DefSubclassProc(hWnd, msg, wParam, lParam);
        }

        // Wheel-based horizontal scroll tends to feel better without delay, and usually doesn't flicker.
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (shiftWheel) {
                // Make Shift+wheel feel like horizontal wheel: wheel forward -> scroll left.
                delta = -delta;
            }

            HDC hdc = GetDC(hWnd);
            int aveChar = 8;
            if (hdc) {
                HFONT font = reinterpret_cast<HFONT>(SendMessageW(hWnd, WM_GETFONT, 0, 0));
                HGDIOBJ oldFont = nullptr;
                if (font) oldFont = SelectObject(hdc, font);
                TEXTMETRICW tm{};
                if (GetTextMetricsW(hdc, &tm)) {
                    aveChar = std::max(1, static_cast<int>(tm.tmAveCharWidth));
                }
                if (oldFont) SelectObject(hdc, oldFont);
                ReleaseDC(hWnd, hdc);
            }
            int stepPx = WheelStepPxForList(hWnd, aveChar);
            const double movePx = static_cast<double>(delta) * static_cast<double>(stepPx) / static_cast<double>(WHEEL_DELTA);
            double acc = movePx + (ctx ? ctx->hWheelRemainderPx : 0.0);
            int addPx = static_cast<int>(std::lround(acc));
            if (addPx != 0) {
                acc -= static_cast<double>(addPx);
            }
            if (ctx) ctx->hWheelRemainderPx = acc;

            // Clamp per-event move to avoid sudden jumps on some devices/drivers.
            addPx = std::clamp(addPx, -stepPx * 6, stepPx * 6);
            if (addPx != 0) {
                int cur = GetHScrollPosPx(hWnd);
                SetHScrollPosPx(hWnd, cur + addPx);
            }
            KillTimer(hWnd, kResultsRedrawTimerId);
            RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
            return 0;
        }
    }
    case WM_HSCROLL:
    case WM_KEYDOWN: {
        const bool maybeHScroll = (msg == WM_HSCROLL) ||
                                  (msg == WM_KEYDOWN && (wParam == VK_LEFT || wParam == VK_RIGHT ||
                                                        wParam == VK_HOME || wParam == VK_END));
        if (msg == WM_KEYDOWN && IsCtrlFKeyDown(wParam)) {
            if (CloseSearchWindowForCtrlF(hWnd)) return 0;
        }
        if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
            if (ctx) {
                int sel = static_cast<int>(SendMessageW(hWnd, LB_GETCURSEL, 0, 0));
                if (sel >= 0) {
                    QueueOpenSearchResult(GetParent(hWnd), sel);
                    return 0;
                }
            }
            return DefSubclassProc(hWnd, msg, wParam, lParam);
        }
        if (msg == WM_KEYDOWN && wParam == VK_DELETE) {
            if (ctx) {
                int sel = static_cast<int>(SendMessageW(hWnd, LB_GETCURSEL, 0, 0));
                if (HideSearchResultAt(GetParent(hWnd), ctx, sel)) return 0;
            }
            return DefSubclassProc(hWnd, msg, wParam, lParam);
        }
        if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
            HWND parent = GetParent(hWnd);
            if (parent && ctx) {
                if (ctx->job.active) {
                    CancelSearchJob(parent, ctx, false);
                } else {
                    DestroyWindow(parent);
                }
                return 0;
            }
        }
        if (!maybeHScroll) return DefSubclassProc(hWnd, msg, wParam, lParam);

        // Scrollbar drag / keyboard repeats can generate many events; coalesce redraws to reduce flicker.
        UINT delayMs = 15;
        if (msg == WM_HSCROLL) {
            int code = LOWORD(wParam);
            if (code == SB_THUMBTRACK) delayMs = 30;
            if (code == SB_ENDSCROLL) delayMs = 0;
        }
        KillTimer(hWnd, kResultsRedrawTimerId);
        if (delayMs == 0) {
            RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
        } else {
            SetTimer(hWnd, kResultsRedrawTimerId, delayMs, nullptr);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return DefSubclassProc(hWnd, msg, wParam, lParam);
    }
    case WM_CONTEXTMENU: {
        if (!ctx) return DefSubclassProc(hWnd, msg, wParam, lParam);
        POINT screenPt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        int index = -1;
        if (screenPt.x == -1 && screenPt.y == -1) {
            index = static_cast<int>(SendMessageW(hWnd, LB_GETCURSEL, 0, 0));
            if (index < 0) return 0;
            RECT itemRect{};
            if (SendMessageW(hWnd, LB_GETITEMRECT, static_cast<WPARAM>(index),
                             reinterpret_cast<LPARAM>(&itemRect)) != LB_ERR) {
                screenPt = {itemRect.left + 18, (itemRect.top + itemRect.bottom) / 2};
                ClientToScreen(hWnd, &screenPt);
            } else {
                GetCursorPos(&screenPt);
            }
        } else {
            POINT clientPt = screenPt;
            ScreenToClient(hWnd, &clientPt);
            DWORD hit = static_cast<DWORD>(
                SendMessageW(hWnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(clientPt.x, clientPt.y)));
            if (HIWORD(hit) != 0) return 0;
            index = static_cast<int>(LOWORD(hit));
            SendMessageW(hWnd, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
        }
        if (index < 0 || index >= static_cast<int>(ctx->job.results.size()) ||
            ctx->job.results[static_cast<size_t>(index)].kind == SearchResultKind::Info) {
            return 0;
        }

        HMENU menu = CreatePopupMenu();
        if (!menu) return 0;
        const auto ui = GetSearchUiStrings();
        const auto& item = ctx->job.results[static_cast<size_t>(index)];
        const UINT readonlyFlags = CanOpenSearchResultInReadonlyViewer(item) ? MF_STRING : (MF_STRING | MF_GRAYED);
        AppendMenuW(menu, readonlyFlags, kSearchResultOpenReadonlyCommand, ui.openReadonlyLabel.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kSearchResultHideCommand, ui.hideResultLabel.c_str());
        SetForegroundWindow(GetParent(hWnd));
        const UINT cmd = TrackPopupMenu(menu,
                                        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                        screenPt.x,
                                        screenPt.y,
                                        0,
                                        GetParent(hWnd),
                                        nullptr);
        DestroyMenu(menu);
        if (cmd == kSearchResultOpenReadonlyCommand) {
            OpenSearchResultInReadonlyViewer(GetParent(hWnd), item);
            return 0;
        }
        if (cmd == kSearchResultHideCommand) {
            HideSearchResultAt(GetParent(hWnd), ctx, index);
            return 0;
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == kResultsRedrawTimerId) {
            KillTimer(hWnd, kResultsRedrawTimerId);
            RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        KillTimer(hWnd, kResultsRedrawTimerId);
        break;
    default:
        break;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

static std::wstring NormalizeSearchTerm(std::wstring term, note::SemanticSearchOptions options) {
    return note::NormalizeSemanticSearchTerm(term, options);
}

static void PushUniqueSearchTerm(std::vector<std::wstring>& out, const std::wstring& raw, note::SemanticSearchOptions options) {
    std::wstring term = NormalizeSearchTerm(raw, options);
    if (term.empty()) return;
    if (std::find(out.begin(), out.end(), term) == out.end()) {
        out.push_back(std::move(term));
    }
}

static bool IsOrToken(const std::wstring& token) {
    std::wstring lower = ToLowerCopy(TrimWhitespace(token));
    return lower == L"or" || lower == L"|";
}

static std::vector<std::wstring> ParseSearchTokens(const std::wstring& query) {
    std::vector<std::wstring> tokens;
    std::wstring token;
    bool inQuote = false;
    for (wchar_t ch : query) {
        if (ch == L'"') {
            if (inQuote) {
                if (!TrimWhitespace(token).empty()) tokens.push_back(token);
                inQuote = false;
            } else {
                if (!TrimWhitespace(token).empty()) tokens.push_back(token);
                inQuote = true;
            }
            token.clear();
            continue;
        }
        if (!inQuote && ch == L'|') {
            if (!TrimWhitespace(token).empty()) tokens.push_back(token);
            tokens.push_back(L"|");
            token.clear();
            continue;
        }
        if (!inQuote && iswspace(static_cast<wint_t>(ch))) {
            if (!TrimWhitespace(token).empty()) tokens.push_back(token);
            token.clear();
            continue;
        }
        token.push_back(ch);
    }
    if (!TrimWhitespace(token).empty()) tokens.push_back(token);
    return tokens;
}

static SearchQueryGroups ParseSearchTermsLower(const std::wstring& query, note::SemanticSearchOptions options) {
    SearchQueryGroups groups;
    bool pendingOr = false;
    for (const auto& token : ParseSearchTokens(query)) {
        if (IsOrToken(token)) {
            pendingOr = !groups.empty();
            continue;
        }
        std::wstring term = NormalizeSearchTerm(token, options);
        if (term.empty()) continue;
        if (pendingOr && !groups.empty()) {
            PushUniqueSearchTerm(groups.back(), term, options);
        } else {
            std::vector<std::wstring> group;
            PushUniqueSearchTerm(group, term, options);
            if (!group.empty()) groups.push_back(std::move(group));
        }
        pendingOr = false;
    }
    return groups;
}

static SearchMatchInfo MatchText(const std::wstring& text,
                                 const SearchQueryGroups& queryTermsLower,
                                 note::SemanticSearchOptions options) {
    return note::MatchNoteText(text, queryTermsLower, options);
}

static std::vector<TextRange> FindHighlightRanges(const std::wstring& text,
                                                  const SearchQueryGroups& queryTermsLower,
                                                  note::SemanticSearchOptions options) {
    return MatchText(text, queryTermsLower, options).ranges;
}

static std::wstring JoinMetaParts(const std::vector<std::wstring>& parts) {
    std::wstring out;
    for (const auto& part : parts) {
        if (part.empty()) continue;
        if (!out.empty()) out += L"  |  ";
        out += part;
    }
    return out;
}

static std::wstring FormatResultHitText(const SearchUiStrings& ui, int hits) {
    if (hits <= 0) return L"";
    if (IsEnglishUi()) {
        return std::to_wstring(hits) + L" " + ui.metaHits;
    }
    return std::to_wstring(hits) + ui.metaHits;
}

static std::wstring GetSearchResultBadge(const SearchUiStrings& ui, SearchResultKind kind) {
    switch (kind) {
    case SearchResultKind::NoteLine:
        return ui.tagNote;
    case SearchResultKind::PdfPage:
        return ui.tagPdf;
    case SearchResultKind::Annot:
        return ui.tagAnnot;
    case SearchResultKind::Path:
        return ui.tagPath;
    default:
        break;
    }
    return L"";
}

static COLORREF GetSearchResultAccent(SearchResultKind kind) {
    switch (kind) {
    case SearchResultKind::NoteLine:
        return g_theme.accent;
    case SearchResultKind::PdfPage:
        return BlendColor(g_theme.accent, g_theme.selectionBg, 0.40);
    case SearchResultKind::Annot:
        return BlendColor(g_theme.accent, g_theme.buttonHot, 0.35);
    case SearchResultKind::Path:
        return BlendColor(g_theme.accent, g_theme.buttonBorder, 0.45);
    default:
        break;
    }
    return g_theme.buttonBorder;
}

static int MeasureTextWidth(HDC hdc, const std::wstring& text) {
    if (!hdc || text.empty()) return 0;
    SIZE sz{};
    if (!GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &sz)) return 0;
    return static_cast<int>(sz.cx);
}

static int MeasureSearchResultWidth(HDC hdc, const SearchResultItem& item, const SearchUiStrings& ui) {
    if (!hdc) return 0;
    if (item.kind == SearchResultKind::Info) {
        return MeasureTextWidth(hdc, item.display);
    }
    std::wstring badge = GetSearchResultBadge(ui, item.kind);
    int badgeW = badge.empty() ? 0 : MeasureTextWidth(hdc, badge) + 16;
    int titleW = MeasureTextWidth(hdc, item.title);
    int metaW = MeasureTextWidth(hdc, item.meta);
    int previewW = MeasureTextWidth(hdc, item.preview);
    return std::max(badgeW + 8 + titleW, std::max(metaW, previewW));
}

static void DrawHighlightedTextLine(HDC hdc,
                                    int x,
                                    int y,
                                    const RECT& itemRect,
                                    const std::wstring& text,
                                    const SearchQueryGroups& queryTermsLower,
                                    note::SemanticSearchOptions options,
                                    COLORREF fg,
                                    COLORREF matchBg) {
    if (!hdc || text.empty()) return;
    const auto ranges = FindHighlightRanges(text, queryTermsLower, options);
    const int n = static_cast<int>(std::min(text.size(), static_cast<size_t>(INT_MAX)));
    if (n > 0 && !ranges.empty()) {
        std::vector<int> dx(static_cast<size_t>(n));
        SIZE sz{};
        if (GetTextExtentExPointW(hdc, text.c_str(), n, 0, nullptr, dx.data(), &sz)) {
            for (const auto& r : ranges) {
                if (r.len == 0 || r.start >= static_cast<size_t>(n)) continue;
                size_t end = std::min(static_cast<size_t>(n), r.start + r.len);
                if (end <= r.start) continue;
                int leftOff = (r.start == 0) ? 0 : dx[r.start - 1];
                int rightOff = dx[end - 1];
                RECT rc = itemRect;
                rc.left = x + leftOff - 1;
                rc.right = x + rightOff + 1;
                rc.top = y - 1;
                rc.bottom = y + static_cast<int>(sz.cy) + 1;
                HBRUSH br = CreateSolidBrush(matchBg);
                FillRect(hdc, &rc, br);
                DeleteObject(br);
            }
        }
    }
    SetTextColor(hdc, fg);
    TextOutW(hdc, x, y, text.c_str(), n);
}

static void DrawSearchResultsItem(const SearchCtx* ctx, const DRAWITEMSTRUCT* dis) {
    if (!ctx || !ctx->results || !dis) return;
    if (dis->itemID == static_cast<UINT>(-1)) return;
    if (dis->hwndItem != ctx->results) return;

    size_t idx = static_cast<size_t>(dis->itemID);
    if (idx >= ctx->job.results.size()) return;

    const auto& item = ctx->job.results[idx];
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    const bool focused = (dis->itemState & ODS_FOCUS) != 0;

    const COLORREF bg = selected ? g_theme.selectionBg : g_theme.panelBg;
    const COLORREF fg = selected ? g_theme.selectionText : g_theme.panelText;

    HBRUSH bgBrush = CreateSolidBrush(bg);
    FillRect(dis->hDC, &dis->rcItem, bgBrush);
    DeleteObject(bgBrush);

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(ctx->results, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = nullptr;
    if (font) oldFont = SelectObject(dis->hDC, font);

    const int saved = SaveDC(dis->hDC);
    IntersectClipRect(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
    SetBkMode(dis->hDC, TRANSPARENT);

    TEXTMETRICW tm{};
    GetTextMetricsW(dis->hDC, &tm);
    const int padX = 10;
    int scrollX = 0;
    {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_POS;
        if (GetScrollInfo(ctx->results, SB_HORZ, &si)) {
            scrollX = static_cast<int>(si.nPos);
        } else {
            scrollX = static_cast<int>(GetScrollPos(ctx->results, SB_HORZ));
        }
    }
    const int xBase = dis->rcItem.left + padX - scrollX;
    const COLORREF matchBg = BlendColor(bg, g_theme.accent, selected ? 0.18 : 0.12);
    const COLORREF metaFg = selected ? fg : BlendColor(fg, bg, 0.28);
    const COLORREF previewFg = selected ? fg : BlendColor(fg, bg, 0.14);

    if (item.kind == SearchResultKind::Info) {
        int itemH = static_cast<int>(dis->rcItem.bottom - dis->rcItem.top);
        int y = dis->rcItem.top + std::max(0, (itemH - static_cast<int>(tm.tmHeight)) / 2);
        if (!item.display.empty()) {
            DrawHighlightedTextLine(dis->hDC, xBase, y, dis->rcItem, item.display,
                                    ctx->job.queryTermsLower, ctx->job.options, fg, matchBg);
        }
    } else {
        RECT sep = dis->rcItem;
        sep.top = std::max(sep.top, sep.bottom - 1);
        HBRUSH sepBrush = CreateSolidBrush(selected ? bg : BlendColor(g_theme.buttonBorder, bg, 0.70));
        FillRect(dis->hDC, &sep, sepBrush);
        DeleteObject(sepBrush);

        std::wstring badge = GetSearchResultBadge(GetSearchUiStrings(), item.kind);
        int badgeW = badge.empty() ? 0 : MeasureTextWidth(dis->hDC, badge) + 16;
        int badgeH = std::max(18, static_cast<int>(tm.tmHeight) + 4);
        int badgeY = dis->rcItem.top + 6;

        if (badgeW > 0) {
            RECT badgeRc{ xBase, badgeY, xBase + badgeW, badgeY + badgeH };
            COLORREF accent = GetSearchResultAccent(item.kind);
            HBRUSH badgeBr = CreateSolidBrush(selected ? BlendColor(accent, bg, 0.28)
                                                       : BlendColor(accent, bg, 0.12));
            HPEN badgePen = CreatePen(PS_SOLID, 1, BlendColor(accent, bg, selected ? 0.10 : 0.35));
            HGDIOBJ oldPen = SelectObject(dis->hDC, badgePen);
            HGDIOBJ oldBrush2 = SelectObject(dis->hDC, badgeBr);
            RoundRect(dis->hDC, badgeRc.left, badgeRc.top, badgeRc.right, badgeRc.bottom, 8, 8);
            SelectObject(dis->hDC, oldBrush2);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(badgePen);
            DeleteObject(badgeBr);

            SetTextColor(dis->hDC, selected ? fg : BlendColor(accent, g_theme.panelText, 0.20));
            RECT badgeTextRc = badgeRc;
            DrawTextW(dis->hDC, badge.c_str(), -1, &badgeTextRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        int titleX = xBase + (badgeW > 0 ? badgeW + 8 : 0);
        int titleY = dis->rcItem.top + 6;
        int metaY = titleY + static_cast<int>(tm.tmHeight) + 4;
        int previewY = metaY + static_cast<int>(tm.tmHeight) + 4;

        DrawHighlightedTextLine(dis->hDC, titleX, titleY, dis->rcItem, item.title,
                                ctx->job.queryTermsLower, ctx->job.options, fg, matchBg);
        if (!item.meta.empty()) {
            SetTextColor(dis->hDC, metaFg);
            TextOutW(dis->hDC, titleX, metaY, item.meta.c_str(), static_cast<int>(item.meta.size()));
        }
        if (!item.preview.empty()) {
            DrawHighlightedTextLine(dis->hDC, xBase, previewY, dis->rcItem, item.preview,
                                    ctx->job.queryTermsLower, ctx->job.options, previewFg, matchBg);
        }
    }

    if (focused) {
        DrawFocusRect(dis->hDC, &dis->rcItem);
    }
    RestoreDC(dis->hDC, saved);

    if (oldFont) SelectObject(dis->hDC, oldFont);
}

static LRESULT CALLBACK SearchEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR idSubclass, DWORD_PTR refData) {
    auto* ctx = reinterpret_cast<SearchCtx*>(refData);
    if (msg == WM_KEYDOWN && IsCtrlFKeyDown(wParam)) {
        if (CloseSearchWindowForCtrlF(hWnd)) return 0;
    }
    if (msg == WM_KEYDOWN) {
        MSG edgeNavMsg{};
        edgeNavMsg.hwnd = hWnd;
        edgeNavMsg.message = msg;
        edgeNavMsg.wParam = wParam;
        edgeNavMsg.lParam = lParam;
        if (ui::ConsumeNoOpEdgeNavKeyForMultilineEdit(edgeNavMsg)) return 0;
    }
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        HWND parent = GetParent(hWnd);
        if (parent && ctx) {
            RunSearch(parent, ctx);
        }
        return 0;
    }
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        HWND parent = GetParent(hWnd);
        if (parent && ctx) {
            if (ctx->job.active) {
                CancelSearchJob(parent, ctx, false);
            } else {
                DestroyWindow(parent);
            }
        }
        return 0;
    }
    if (msg == WM_CHAR && (wParam == L'\r' || wParam == 27)) {
        return 0;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

static std::wstring GetWindowTextValue(HWND hWnd) {
    if (!hWnd) return L"";
    int len = GetWindowTextLengthW(hWnd);
    if (len <= 0) return L"";
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    int got = GetWindowTextW(hWnd, text.data(), len + 1);
    if (got <= 0) return L"";
    text.resize(static_cast<size_t>(got));
    return text;
}

static std::wstring ToLowerCopy(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

static std::wstring CleanSnippet(const std::wstring& s, size_t maxLen = 110) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t ch : s) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
            out.push_back(L' ');
        } else {
            out.push_back(ch);
        }
    }
    out = TrimWhitespace(out);
    if (out.size() > maxLen) {
        out = out.substr(0, maxLen);
        out += L"...";
    }
    return out;
}

static std::wstring SnipAround(const std::wstring& text, size_t pos, size_t len, size_t maxLen = 110) {
    if (text.empty()) return L"";
    size_t context = maxLen / 2;
    size_t start = (pos > context) ? pos - context : 0;
    size_t end = std::min(text.size(), pos + len + context);
    std::wstring snippet = text.substr(start, end - start);
    snippet = CleanSnippet(snippet, maxLen);
    if (start > 0) snippet = L"..." + snippet;
    if (end < text.size()) snippet += L"...";
    return snippet;
}

static std::wstring DecodeUtf8BytesWide(std::string data) {
    if (data.size() >= 3 &&
        static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data.erase(0, 3);
    }
    return UTF8ToWide(data);
}

static std::wstring FormatPathForDisplay(const std::filesystem::path& path) {
    std::error_code ec;
    if (!g_workspaceRoot.empty()) {
        auto rel = std::filesystem::relative(path, g_workspaceRoot, ec);
        if (!ec && !rel.empty()) return rel.wstring();
    }
    return path.wstring();
}

static bool IsCurrentOpenNotePath(const std::filesystem::path& path) {
    if (g_currentNotePath.empty() || path.empty()) return false;
    const std::wstring current = ToLowerCopy(std::filesystem::path(g_currentNotePath).lexically_normal().wstring());
    const std::wstring candidate = ToLowerCopy(path.lexically_normal().wstring());
    return !current.empty() && current == candidate;
}

static note_snapshot::CurrentEditTextSnapshot CurrentEditSnapshotForNotePath(const std::filesystem::path& path) {
    note_snapshot::CurrentEditTextSnapshot snapshot;
    if (!g_hNoteEdit || !IsCurrentOpenNotePath(path)) return snapshot;
    snapshot.available = true;
    snapshot.targetPath = path.wstring();
    snapshot.bytes = WideToUTF8(GetWindowTextValue(g_hNoteEdit));
    snapshot.identity = CaptureCurrentNoteSnapshotIdentity();
    return snapshot;
}

static bool CurrentOpenNoteMatchesSnapshot(
    const note::SnapshotIdentity& expectedIdentity) {
    if (!expectedIdentity.valid()) return true;
    if (!g_hNoteEdit) return false;
    note::SnapshotIdentity currentIdentity = CaptureCurrentNoteSnapshotIdentity();
    if (!currentIdentity.note_id.valid()) return false;
    const std::string bytes = WideToUTF8(GetWindowTextValue(g_hNoteEdit));
    currentIdentity = note::BuildSnapshotIdentity(
        currentIdentity.note_id,
        currentIdentity.content_revision,
        currentIdentity.persistence_revision,
        bytes);
    return note::SameSnapshotContent(expectedIdentity, currentIdentity);
}

static void SearchNoteText(const std::wstring& text,
                           const SearchQueryGroups& queryTermsLower,
                           note::SemanticSearchOptions options,
                           const SearchUiStrings& ui,
                           const std::wstring& fallbackTitle,
                           const std::filesystem::path& path,
                           const std::wstring& extraMeta,
                           const note::SnapshotIdentity& snapshotIdentity,
                           std::vector<SearchResultItem>& results) {
    if (text.empty() || queryTermsLower.empty()) return;
    std::wstring title = !path.empty() ? FormatPathForDisplay(path) : fallbackTitle;
    if (title.empty()) {
        title = ui.tagNote + L" (" + ui.metaCurrent + L")";
    }
    note::NoteMetadata metadata;
    metadata.file_name = path.filename().wstring();
    metadata.title = note::DeriveTitleFromFileName(metadata.file_name);
    std::wstring extension = ToLowerCopy(path.extension().wstring());
    const note::NoteContentKind contentKind = extension == L".txt"
        ? note::NoteContentKind::PlainText
        : note::NoteContentKind::Markdown;
    const auto index = note::RuntimeNoteWorkspaceService().ResolveIndex(
        snapshotIdentity, path.wstring(), std::move(metadata), text, contentKind);
    if (!index) return;
    for (const note::WorkspaceNoteLineMatch& lineMatch :
         note::SearchWorkspaceNoteIndex(*index, queryTermsLower, options)) {
        const SearchMatchInfo& match = lineMatch.match;
        SearchResultItem item;
        item.kind = SearchResultKind::NoteLine;
        item.path = path;
        item.noteSnapshotIdentity = index->snapshot_identity;
        item.lineNumber = lineMatch.line_number;
        if (match.firstPos != std::wstring::npos && match.firstLen > 0) {
            item.textStart = match.firstPos;
            item.textEnd = item.textStart + match.firstLen;
        }
        item.hitCount = match.totalHits;
        item.title = title;
        item.meta = JoinMetaParts({
            ui.tagNote,
            extraMeta,
            ui.metaLine + L" " + std::to_wstring(lineMatch.line_number),
            FormatResultHitText(ui, match.totalHits)
        });
        item.preview = CleanSnippet(lineMatch.text);
        item.display = item.title + L" " + item.meta + L" " + item.preview;
        results.push_back(std::move(item));
    }
}

static void SearchNoteFile(const std::filesystem::path& path,
                           const SearchQueryGroups& queryTermsLower,
                           note::SemanticSearchOptions options,
                           const SearchUiStrings& ui,
                           std::vector<SearchResultItem>& results) {
    note_snapshot::CurrentEditTextSnapshot currentEdit = CurrentEditSnapshotForNotePath(path);
    note_snapshot::LatestTextSnapshot snapshot =
        note_snapshot::LoadLatestTextSnapshot(path.wstring(), currentEdit);
    if (!snapshot.ok) return;
    std::wstring text = DecodeUtf8BytesWide(std::move(snapshot.bytes));
    if (text.empty()) return;
    const std::wstring meta = note_snapshot::LatestTextSnapshotMetaLabel(snapshot);
    const std::filesystem::path targetPath(snapshot.targetPath);
    SearchNoteText(text, queryTermsLower, options, ui, FormatPathForDisplay(targetPath),
                   targetPath, meta, snapshot.identity, results);
}

static void SearchStagedNoteFile(const StagedSearchFile& staged,
                                 const SearchQueryGroups& queryTermsLower,
                                 note::SemanticSearchOptions options,
                                 const SearchUiStrings& ui,
                                 std::vector<SearchResultItem>& results) {
    if (staged.targetPath.empty() || staged.stagePath.empty()) return;
    note_snapshot::CurrentEditTextSnapshot currentEdit = CurrentEditSnapshotForNotePath(staged.targetPath);
    if (currentEdit.available) {
        note_snapshot::LatestTextSnapshot snapshot =
            note_snapshot::LoadLatestTextSnapshot(staged.targetPath.wstring(), currentEdit);
        if (!snapshot.ok) return;
        std::wstring text = DecodeUtf8BytesWide(std::move(snapshot.bytes));
        if (text.empty()) return;
        const std::wstring meta = note_snapshot::LatestTextSnapshotMetaLabel(snapshot);
        const std::filesystem::path targetPath(snapshot.targetPath);
        SearchNoteText(text, queryTermsLower, options, ui, FormatPathForDisplay(targetPath),
                        targetPath, meta, snapshot.identity, results);
        return;
    }
    std::string bytes;
    std::wstring err;
    if (!note_snapshot::LoadStagedTextBytes(staged.targetPath.wstring(),
                                            staged.stagePath,
                                            &bytes,
                                            &err)) {
        return;
    }
    const note::NoteIdentity noteIdentity =
        note::ResolveRuntimeNoteIdentityPath(staged.targetPath.wstring());
    const note::SnapshotIdentity snapshotIdentity = note::BuildSnapshotIdentity(
        noteIdentity.note_id, 0, staged.persistenceRevision, bytes);
    std::wstring text = DecodeUtf8BytesWide(std::move(bytes));
    if (text.empty()) return;
    SearchNoteText(text, queryTermsLower, options, ui, FormatPathForDisplay(staged.targetPath),
                    staged.targetPath,
                    note_snapshot::TextSnapshotSourceLabel(note_snapshot::TextSnapshotSource::Stage),
                    snapshotIdentity,
                    results);
}

static bool ExtractPdfPageText(FPDF_DOCUMENT doc, int pageIndex, std::wstring& out) {
    out.clear();
    if (!doc) return false;
    std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return false;
    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage) {
        FPDF_ClosePage(page);
        return false;
    }
    int count = FPDFText_CountChars(textPage);
    if (count > 0) {
        std::wstring text(count + 1, L'\0');
        int got = FPDFText_GetText(textPage, 0, count,
                                   reinterpret_cast<unsigned short*>(text.data()));
        if (got > 0) {
            text.resize(got);
            out = std::move(text);
        }
    }
    FPDFText_ClosePage(textPage);
    FPDF_ClosePage(page);
    return true;
}

static bool GetPdfPageCharCenter(FPDF_DOCUMENT doc,
                                 int pageIndex,
                                 size_t charIndex,
                                 double& outX,
                                 double& outY) {
    if (!doc || pageIndex < 0 || charIndex > static_cast<size_t>(INT_MAX)) return false;
    std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return false;
    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage) {
        FPDF_ClosePage(page);
        return false;
    }
    double left = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double top = 0.0;
    const bool ok = FPDFText_GetCharBox(textPage,
                                        static_cast<int>(charIndex),
                                        &left,
                                        &right,
                                        &bottom,
                                        &top);
    FPDFText_ClosePage(textPage);
    FPDF_ClosePage(page);
    if (!ok) return false;
    outX = (left + right) * 0.5;
    outY = (bottom + top) * 0.5;
    return std::isfinite(outX) && std::isfinite(outY);
}

static void SearchPdfPage(FPDF_DOCUMENT doc,
                          int pageIndex,
                          const std::wstring& displayName,
                          const std::filesystem::path& path,
                          const SearchQueryGroups& queryTermsLower,
                          note::SemanticSearchOptions options,
                          const SearchUiStrings& ui,
                          std::vector<SearchResultItem>& results) {
    if (!doc || queryTermsLower.empty() || pageIndex < 0) return;
    std::wstring text;
    if (!ExtractPdfPageText(doc, pageIndex, text)) return;
    if (text.empty()) return;
    SearchMatchInfo match = MatchText(text, queryTermsLower, options);
    if (!match.matched) return;
    std::wstring title = !displayName.empty() ? displayName : ui.tagPdf;
    std::wstring snippet = SnipAround(text, match.firstPos, std::max<size_t>(match.firstLen, 1));
    SearchResultItem item;
    item.kind = SearchResultKind::PdfPage;
    item.path = path;
    item.pageIndex = pageIndex;
    if (match.firstPos != std::wstring::npos && match.firstLen > 0) {
        item.textStart = match.firstPos;
        item.textEnd = match.firstPos + match.firstLen;
        if (GetPdfPageCharCenter(doc, pageIndex, match.firstPos, item.focusX, item.focusY)) {
            item.hasFocusPoint = true;
        }
    }
    item.hitCount = match.totalHits;
    item.title = title;
    item.meta = JoinMetaParts({
        ui.tagPdf,
        ui.metaPage + L" " + std::to_wstring(pageIndex + 1),
        FormatResultHitText(ui, match.totalHits)
    });
    item.preview = snippet;
    item.display = item.title + L" " + item.meta + L" " + item.preview;
    results.push_back(std::move(item));
}

static void SearchAnnotations(const std::vector<Annotation>& annots,
                              const SearchQueryGroups& queryTermsLower,
                              note::SemanticSearchOptions options,
                              const SearchUiStrings& ui,
                              const std::filesystem::path& path,
                              const std::wstring& extraMeta,
                              std::vector<SearchResultItem>& results) {
    if (queryTermsLower.empty()) return;
    std::wstring title = !path.empty() ? FormatPathForDisplay(path) : ui.tagAnnot;
    for (const auto& a : annots) {
        if (a.text.empty()) continue;
        SearchMatchInfo match = MatchText(a.text, queryTermsLower, options);
        if (!match.matched) continue;
        SearchResultItem item;
        item.kind = SearchResultKind::Annot;
        item.path = path;
        item.pageIndex = a.pageIndex;
        item.hitCount = match.totalHits;
        item.focusX = a.x1;
        item.focusY = a.y1;
        item.hasFocusPoint = true;
        item.title = title;
        item.meta = JoinMetaParts({
            ui.tagAnnot,
            extraMeta,
            a.pageIndex >= 0 ? (ui.metaPage + L" " + std::to_wstring(a.pageIndex + 1)) : L"",
            FormatResultHitText(ui, match.totalHits)
        });
        item.preview = CleanSnippet(a.text);
        item.display = item.title + L" " + item.meta + L" " + item.preview;
        results.push_back(std::move(item));
    }
}

static void SearchAnnotations(const std::vector<Annotation>& annots,
                              const SearchQueryGroups& queryTermsLower,
                              note::SemanticSearchOptions options,
                              const SearchUiStrings& ui,
                              const std::filesystem::path& path,
                              std::vector<SearchResultItem>& results) {
    SearchAnnotations(annots, queryTermsLower, options, ui, path, L"", results);
}

static bool SearchStagedAnnotationsForPdf(const std::filesystem::path& pdfPath,
                                          const std::filesystem::path& stagePath,
                                          const SearchQueryGroups& queryTermsLower,
                                          note::SemanticSearchOptions options,
                                          const SearchUiStrings& ui,
                                          std::vector<SearchResultItem>& results) {
    if (pdfPath.empty()) return false;
    std::vector<Annotation> annots;
    std::wstring err;
    if (!file_output::LoadResolvedStagedAnnotations(pdfPath.wstring(), stagePath, &annots, &err)) {
        return false;
    }
    SearchAnnotations(annots,
                      queryTermsLower,
                      options,
                      ui,
                      pdfPath,
                      note_snapshot::TextSnapshotSourceLabel(note_snapshot::TextSnapshotSource::Stage),
                      results);
    return true;
}

static void SearchAnnotationsForPdf(const std::filesystem::path& pdfPath,
                                    const SearchQueryGroups& queryTermsLower,
                                    note::SemanticSearchOptions options,
                                    const SearchUiStrings& ui,
                                    std::vector<SearchResultItem>& results) {
    const auto stagePath = file_output::FindLatestStagedClropPathForPdf(pdfPath.wstring());
    if (!stagePath.empty() && SearchStagedAnnotationsForPdf(pdfPath, stagePath, queryTermsLower, options, ui, results)) {
        return;
    }
    std::wstring clropPath = clrop_bridge::ClropPathForPdf(pdfPath.wstring());
    std::error_code ec;
    if (!std::filesystem::exists(clropPath, ec) || ec) return;
    std::vector<Annotation> annots;
    bool mismatch = false;
    std::wstring err;
    if (!clrop_bridge::LoadAnnotations(clropPath, pdfPath.wstring(), annots, mismatch, nullptr, err,
                                       clrop_bridge::LoadAnnotationsValidation::None)) {
        return;
    }
    SearchAnnotations(annots, queryTermsLower, options, ui, pdfPath, results);
}

static std::wstring NormalizePathKey(const std::filesystem::path& path) {
    std::wstring s = path.wstring();
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    std::replace(s.begin(), s.end(), L'/', L'\\');
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

static bool IsSearchPathUnderRoot(const std::filesystem::path& path,
                                  const std::filesystem::path& root) {
    if (path.empty() || root.empty()) return false;
    std::error_code ec;
    auto rel = std::filesystem::relative(path, root, ec);
    if (ec || rel.empty()) return false;
    auto it = rel.begin();
    if (it == rel.end()) return false;
    std::wstring first = it->wstring();
    return first != L".." && first.rfind(L"..", 0) != 0;
}

static bool IsSearchPathUnderAnyRoot(const std::filesystem::path& path,
                                     const std::vector<std::filesystem::path>& roots) {
    for (const auto& root : roots) {
        if (IsSearchPathUnderRoot(path, root)) return true;
    }
    return false;
}

static bool HasStagedNoteForTarget(const SearchJob& job, const std::filesystem::path& targetPath) {
    const std::wstring key = NormalizePathKey(targetPath);
    for (const auto& staged : job.stagedNoteFiles) {
        if (NormalizePathKey(staged.targetPath) == key) return true;
    }
    return false;
}

static bool ExtractLectureSessionFromPath(const std::filesystem::path& targetPath,
                                          std::filesystem::path& lecturePath,
                                          std::filesystem::path& sessionPath) {
    lecturePath.clear();
    sessionPath.clear();
    if (g_workspaceRoot.empty()) return false;

    std::error_code ec;
    std::filesystem::path classesPath = WorkspaceClassesPath(g_workspaceRoot, g_config);
    if (!std::filesystem::exists(classesPath, ec)) classesPath = g_workspaceRoot;
    if (classesPath.empty()) return false;

    std::filesystem::path dir = targetPath;
    const bool targetIsDir = std::filesystem::is_directory(dir, ec);
    if (!targetIsDir) dir = dir.parent_path();
    if (dir.empty()) return false;

    auto rel = std::filesystem::relative(dir, classesPath, ec);
    if (ec || rel.empty()) return false;
    auto it = rel.begin();
    if (it == rel.end()) return false;
    std::wstring first = it->wstring();
    if (first == L"." || first == L".." || first.rfind(L"..", 0) == 0) return false;

    lecturePath = classesPath / *it;
    ++it;
    if (it == rel.end()) {
        if (!targetIsDir) sessionPath = lecturePath;
        return true;
    }

    std::wstring second = it->wstring();
    std::wstring key = ToLowerCopy(TrimWhitespace(second));
    if (key == L"pdf" || key == L"note") {
        sessionPath = lecturePath;
        return true;
    }
    sessionPath = lecturePath / *it;
    return true;
}

static bool ExtractLectureSessionFromVisibleLecturePath(const std::filesystem::path& targetPath,
                                                        std::filesystem::path& lecturePath,
                                                        std::filesystem::path& sessionPath) {
    lecturePath.clear();
    sessionPath.clear();

    std::error_code ec;
    std::filesystem::path dir = targetPath;
    const bool targetIsDir = std::filesystem::is_directory(dir, ec);
    if (!targetIsDir) dir = dir.parent_path();
    if (dir.empty()) return false;

    for (const auto& lecture : g_lectures) {
        if (lecture.empty()) continue;
        std::filesystem::path lectureRoot(lecture);
        ec.clear();
        auto rel = std::filesystem::relative(dir, lectureRoot, ec);
        if (ec || rel.empty()) continue;
        auto it = rel.begin();
        if (it == rel.end()) continue;

        std::wstring first = it->wstring();
        if (first == L"..") continue;
        if (first.rfind(L"..", 0) == 0) continue;

        lecturePath = lectureRoot;
        if (first == L".") {
            if (!targetIsDir) sessionPath = lectureRoot;
            return true;
        }

        std::wstring key = ToLowerCopy(TrimWhitespace(first));
        if (key == L"pdf" || key == L"note") {
            sessionPath = lectureRoot;
            return true;
        }
        sessionPath = lectureRoot / *it;
        return true;
    }

    return false;
}

static void JumpToNoteLine(int line) {
    if (!g_hNoteEdit || line < 1) return;
    int lineIndex = std::max(0, line - 1);
    LRESULT start = SendMessageW(g_hNoteEdit, EM_LINEINDEX, static_cast<WPARAM>(lineIndex), 0);
    if (start < 0) return;
    LRESULT end = SendMessageW(g_hNoteEdit, EM_LINEINDEX, static_cast<WPARAM>(lineIndex + 1), 0);
    if (end < 0) end = start;
    SendMessageW(g_hNoteEdit, EM_SETSEL, static_cast<WPARAM>(start), static_cast<LPARAM>(end));
    SendMessageW(g_hNoteEdit, EM_SCROLLCARET, 0, 0);
}

static std::wstring BuildSummaryText(const SearchJob& job, const SearchUiStrings& ui) {
    std::wstring hitsLabel = TrimWhitespace(ui.statusHits);
    while (!hitsLabel.empty() && (hitsLabel.back() == L':' || hitsLabel.back() == L' ')) {
        hitsLabel.pop_back();
    }
    std::wstring text = ui.summaryLabel + L": ";
    text += hitsLabel + L" " + std::to_wstring(job.matchCount);
    if (job.includeNotes || job.noteMatches > 0) {
        text += L"  ";
        text += ui.tagNote + L" " + std::to_wstring(job.noteMatches);
    }
    if (job.includePdfs || job.pdfMatches > 0) {
        text += L"  ";
        text += ui.tagPdf + L" " + std::to_wstring(job.pdfMatches);
    }
    if (job.includeAnnots || job.annotMatches > 0) {
        text += L"  ";
        text += ui.tagAnnot + L" " + std::to_wstring(job.annotMatches);
    }
    if (job.includePaths || job.pathMatches > 0) {
        text += L"  ";
        text += ui.tagPath + L" " + std::to_wstring(job.pathMatches);
    }
    return text;
}

static void UpdateSummaryLabel(SearchCtx* ctx, const SearchUiStrings& ui) {
    if (!ctx || !ctx->labelSummary) return;
    std::wstring text = BuildSummaryText(ctx->job, ui);
    SetWindowTextW(ctx->labelSummary, text.c_str());
}

static void UpdateRunButtonLabel(SearchCtx* ctx, const SearchUiStrings& ui) {
    if (!ctx || !ctx->btnRun) return;
    SetWindowTextW(ctx->btnRun, ctx->job.active ? ui.cancelLabel.c_str() : ui.runLabel.c_str());
    EnableWindow(ctx->btnRun, TRUE);
}

static void AppendResults(SearchCtx* ctx, const std::vector<SearchResultItem>& items) {
    if (!ctx || !ctx->results || items.empty()) return;
    SendMessageW(ctx->results, WM_SETREDRAW, FALSE, 0);
    HDC dc = GetDC(ctx->results);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(ctx->results, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = nullptr;
    if (dc && font) oldFont = SelectObject(dc, font);
    const auto ui = GetSearchUiStrings();

    for (const auto& item : items) {
        ctx->job.results.push_back(item);
        SendMessageW(ctx->results, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.display.c_str()));
        if (item.kind != SearchResultKind::Info) {
            ctx->job.matchCount++;
            switch (item.kind) {
            case SearchResultKind::NoteLine:
                ctx->job.noteMatches++;
                break;
            case SearchResultKind::PdfPage:
                ctx->job.pdfMatches++;
                break;
            case SearchResultKind::Annot:
                ctx->job.annotMatches++;
                break;
            case SearchResultKind::Path:
                ctx->job.pathMatches++;
                break;
            default:
                break;
            }
        }
        if (dc) {
            ctx->maxResultWidth = std::max(ctx->maxResultWidth, MeasureSearchResultWidth(dc, item, ui));
        }
    }

    if (dc) {
        if (oldFont) SelectObject(dc, oldFont);
        ReleaseDC(ctx->results, dc);
    }
    SendMessageW(ctx->results, LB_SETHORIZONTALEXTENT,
                 static_cast<WPARAM>(ctx->maxResultWidth + 28), 0);
    SendMessageW(ctx->results, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(ctx->results, nullptr, TRUE);
    UpdateSummaryLabel(ctx, ui);
}

static bool HideSearchResultAt(HWND hWnd, SearchCtx* ctx, int listIndex) {
    if (!ctx || !ctx->results || listIndex < 0) return false;
    const size_t idx = static_cast<size_t>(listIndex);
    if (idx >= ctx->job.results.size()) return false;
    if (ctx->job.results[idx].kind == SearchResultKind::Info) return false;

    ctx->job.results.erase(ctx->job.results.begin() + static_cast<std::ptrdiff_t>(idx));
    SendMessageW(ctx->results, LB_DELETESTRING, static_cast<WPARAM>(listIndex), 0);

    const int count = static_cast<int>(SendMessageW(ctx->results, LB_GETCOUNT, 0, 0));
    if (count > 0) {
        int nextSel = std::min(listIndex, count - 1);
        SendMessageW(ctx->results, LB_SETCURSEL, static_cast<WPARAM>(nextSel), 0);
    }
    InvalidateRect(ctx->results, nullptr, TRUE);
    const auto ui = GetSearchUiStrings();
    UpdateSummaryLabel(ctx, ui);
    UpdateStatusLabel(ctx, ui);
    (void)hWnd;
    return true;
}

static void SetSearchControlsEnabled(SearchCtx* ctx, bool enabled) {
    if (!ctx) return;
    if (ctx->edit) EnableWindow(ctx->edit, enabled);
    for (HWND radio : ctx->rangeRadios) {
        if (radio) EnableWindow(radio, enabled);
    }
    for (HWND btn : ctx->targetChecks) {
        if (btn) EnableWindow(btn, enabled);
    }
    UpdateRunButtonLabel(ctx, GetSearchUiStrings());
}

static void UpdateStatusLabel(SearchCtx* ctx, const SearchUiStrings& ui) {
    if (!ctx || !ctx->labelStatus) return;
    if (!ctx->job.active) {
        const std::wstring& idle = ctx->idleStatusText.empty() ? ui.statusReady : ctx->idleStatusText;
        std::wstring text = ui.statusLabel + L": " + idle;
        SetWindowTextW(ctx->labelStatus, text.c_str());
        return;
    }
    std::wstring dots(static_cast<size_t>(ctx->statusPulse % 4), L'.');
    std::wstring text = ui.statusLabel + L": " + ui.statusSearching + dots;
    if (ctx->job.matchCount > 0) {
        text += L"  " + ui.statusHits + std::to_wstring(ctx->job.matchCount);
    }
    if (ctx->job.processedFiles > 0) {
        text += L"  " + ui.statusFiles + std::to_wstring(ctx->job.processedFiles);
    }
    if (ctx->job.processedPages > 0) {
        text += L"  " + ui.statusPages + std::to_wstring(ctx->job.processedPages);
    }
    SetWindowTextW(ctx->labelStatus, text.c_str());
}

static void FinishSearchJob(HWND hWnd, SearchCtx* ctx, const SearchUiStrings& ui, bool canceled) {
    if (!ctx) return;
    if (ctx->job.pdf && ctx->job.pdf->ownsDoc && ctx->job.pdf->doc) {
        std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
        FPDF_CloseDocument(ctx->job.pdf->doc);
    }
    ctx->job.pdf.reset();
    ctx->job.active = false;
    ctx->job.canceled = canceled;
    SetIdleStatus(ctx, canceled ? ui.statusCanceled : ui.statusDone);
    KillTimer(hWnd, kSearchTimerId);
    SetSearchControlsEnabled(ctx, true);
    UpdateRunButtonLabel(ctx, ui);

    if (ctx->job.matchCount == 0 && ctx->job.results.empty() && !canceled) {
        SearchResultItem item;
        item.kind = SearchResultKind::Info;
        item.title = ui.msgNoResults;
        item.display = ui.msgNoResults;
        AppendResults(ctx, std::vector<SearchResultItem>{item});
    }

    std::wstring status = ui.statusLabel + L": " + (canceled ? ui.statusCanceled : ui.statusDone);
    status += L"  " + ui.statusHits + std::to_wstring(ctx->job.matchCount);
    if (ctx->job.processedFiles > 0) {
        status += L"  " + ui.statusFiles + std::to_wstring(ctx->job.processedFiles);
    }
    if (ctx->job.processedPages > 0) {
        status += L"  " + ui.statusPages + std::to_wstring(ctx->job.processedPages);
    }
    if (ctx->labelStatus) SetWindowTextW(ctx->labelStatus, status.c_str());
    UpdateSummaryLabel(ctx, ui);
}

static void CancelSearchJob(HWND hWnd, SearchCtx* ctx, bool silent) {
    if (!ctx || !ctx->job.active) return;
    const auto ui = GetSearchUiStrings();
    if (ctx->job.pdf && ctx->job.pdf->ownsDoc && ctx->job.pdf->doc) {
        std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
        FPDF_CloseDocument(ctx->job.pdf->doc);
    }
    ctx->job.pdf.reset();
    ctx->job.active = false;
    ctx->job.canceled = true;
    SetIdleStatus(ctx, ui.statusCanceled);
    KillTimer(hWnd, kSearchTimerId);
    if (silent) return;
    SetSearchControlsEnabled(ctx, true);
    UpdateRunButtonLabel(ctx, ui);
    std::wstring status = ui.statusLabel + L": " + ui.statusCanceled;
    status += L"  " + ui.statusHits + std::to_wstring(ctx->job.matchCount);
    if (ctx->job.processedFiles > 0) {
        status += L"  " + ui.statusFiles + std::to_wstring(ctx->job.processedFiles);
    }
    if (ctx->job.processedPages > 0) {
        status += L"  " + ui.statusPages + std::to_wstring(ctx->job.processedPages);
    }
    if (ctx->labelStatus) SetWindowTextW(ctx->labelStatus, status.c_str());
    UpdateSummaryLabel(ctx, ui);
}

static std::filesystem::path GetWorkspaceClassesPath() {
    if (g_workspaceRoot.empty()) return {};
    std::error_code ec;
    auto classesPath = WorkspaceClassesPath(g_workspaceRoot, g_config);
    if (!std::filesystem::exists(classesPath, ec)) {
        classesPath = g_workspaceRoot;
    }
    if (!std::filesystem::exists(classesPath, ec)) return {};
    return classesPath;
}

static std::vector<std::filesystem::path> CollectLectureRoots(const std::filesystem::path& lecturePath) {
    std::vector<std::filesystem::path> roots;
    if (lecturePath.empty()) return roots;

    std::error_code ec;
    auto norm = [](std::wstring s) {
        s = TrimWhitespace(s);
        std::transform(s.begin(), s.end(), s.begin(), ::towlower);
        return s;
    };

    std::unordered_map<std::wstring, std::filesystem::path> sessionMap;
    for (const auto& entry : std::filesystem::directory_iterator(lecturePath, ec)) {
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(entry.path(), isReparse) && isReparse) continue;
        std::error_code stEc;
        if (!entry.is_directory(stEc) || stEc) continue;
        auto key = norm(entry.path().filename().wstring());
        if (sessionMap.find(key) != sessionMap.end()) continue;
        sessionMap[key] = entry.path();
    }
    for (const auto& kv : sessionMap) {
        roots.push_back(kv.second);
    }
    return roots;
}

static std::vector<std::filesystem::path> CollectSessionRoots() {
    std::vector<std::filesystem::path> roots;
    std::filesystem::path classesPath = GetWorkspaceClassesPath();
    if (classesPath.empty()) return roots;

    std::error_code ec;
    auto cacheName = std::filesystem::path(g_config.cacheDir).filename().wstring();
    for (const auto& lecture : std::filesystem::directory_iterator(classesPath, ec)) {
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(lecture.path(), isReparse) && isReparse) continue;
        std::error_code stEc;
        if (!lecture.is_directory(stEc) || stEc) continue;
        auto lectureName = lecture.path().filename().wstring();
        if (!cacheName.empty() && lectureName == cacheName) continue;

        auto lectureRoots = CollectLectureRoots(lecture.path());
        roots.insert(roots.end(), lectureRoots.begin(), lectureRoots.end());
    }
    return roots;
}

static void AppendUniqueRoots(std::vector<std::filesystem::path>& roots,
                              const std::vector<std::filesystem::path>& additions) {
    for (const auto& path : additions) {
        const std::wstring key = NormalizePathKey(path);
        bool exists = false;
        for (const auto& root : roots) {
            if (NormalizePathKey(root) == key) {
                exists = true;
                break;
            }
        }
        if (!exists) roots.push_back(path);
    }
}

static std::vector<std::filesystem::path> CollectSessionRootsIncludingVisibleLectures() {
    std::vector<std::filesystem::path> roots = CollectSessionRoots();
    for (const auto& lecturePath : g_lectures) {
        if (lecturePath.empty()) continue;
        std::filesystem::path path(lecturePath);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) continue;
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(path, isReparse) && isReparse) continue;
        std::error_code stEc;
        if (!std::filesystem::is_directory(path, stEc) || stEc) continue;
        AppendUniqueRoots(roots, CollectLectureRoots(path));
    }
    return roots;
}

static bool TryGetSelectedLectureRoot(std::filesystem::path& lecturePath) {
    lecturePath.clear();
    if (!g_currentLecturePath.empty()) {
        lecturePath = std::filesystem::path(g_currentLecturePath);
        return true;
    }
    if (!g_hLectureList || g_lectures.empty()) return false;
    int sel = static_cast<int>(SendMessageW(g_hLectureList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_lectures.size())) return false;
    if (g_lectures[static_cast<size_t>(sel)].empty()) return false;
    lecturePath = std::filesystem::path(g_lectures[static_cast<size_t>(sel)]);
    return true;
}

static bool TryGetSelectedSessionRoot(std::filesystem::path& sessionPath) {
    sessionPath.clear();
    if (!g_currentSessionPath.empty()) {
        sessionPath = std::filesystem::path(g_currentSessionPath);
        return true;
    }
    if (!g_hSessionList || g_sessions.empty()) return false;
    int sel = static_cast<int>(SendMessageW(g_hSessionList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_sessions.size())) return false;
    if (g_sessions[static_cast<size_t>(sel)].path.empty()) return false;
    sessionPath = std::filesystem::path(g_sessions[static_cast<size_t>(sel)].path);
    return true;
}

static bool TryGetCurrentSessionRoot(std::filesystem::path& sessionPath) {
    sessionPath.clear();
    if (TryGetSelectedSessionRoot(sessionPath)) return true;

    std::filesystem::path lecturePath;
    if (!g_currentNotePath.empty() &&
        (ExtractLectureSessionFromPath(std::filesystem::path(g_currentNotePath), lecturePath, sessionPath) ||
         ExtractLectureSessionFromVisibleLecturePath(std::filesystem::path(g_currentNotePath), lecturePath, sessionPath)) &&
        !sessionPath.empty()) {
        return true;
    }
    if (!CurrentLogicalPdfPath().empty() &&
        (ExtractLectureSessionFromPath(std::filesystem::path(CurrentLogicalPdfPath()), lecturePath, sessionPath) ||
         ExtractLectureSessionFromVisibleLecturePath(std::filesystem::path(CurrentLogicalPdfPath()), lecturePath, sessionPath)) &&
        !sessionPath.empty()) {
        return true;
    }
    return !sessionPath.empty();
}

static bool TryGetCurrentLectureRoot(std::filesystem::path& lecturePath) {
    lecturePath.clear();
    if (TryGetSelectedLectureRoot(lecturePath)) return true;

    std::filesystem::path selectedSessionPath;
    std::filesystem::path sessionPath;
    if (TryGetSelectedSessionRoot(selectedSessionPath) &&
        (ExtractLectureSessionFromPath(selectedSessionPath, lecturePath, sessionPath) ||
         ExtractLectureSessionFromVisibleLecturePath(selectedSessionPath, lecturePath, sessionPath)) &&
        !lecturePath.empty()) {
        return true;
    }
    if (!g_currentNotePath.empty() &&
        (ExtractLectureSessionFromPath(std::filesystem::path(g_currentNotePath), lecturePath, sessionPath) ||
         ExtractLectureSessionFromVisibleLecturePath(std::filesystem::path(g_currentNotePath), lecturePath, sessionPath)) &&
        !lecturePath.empty()) {
        return true;
    }
    if (!CurrentLogicalPdfPath().empty() &&
        (ExtractLectureSessionFromPath(std::filesystem::path(CurrentLogicalPdfPath()), lecturePath, sessionPath) ||
         ExtractLectureSessionFromVisibleLecturePath(std::filesystem::path(CurrentLogicalPdfPath()), lecturePath, sessionPath)) &&
        !lecturePath.empty()) {
        return true;
    }
    return false;
}

static SearchRange GetSelectedRange(const SearchCtx& ctx) {
    for (size_t i = 0; i < ctx.rangeRadios.size(); ++i) {
        if (SendMessageW(ctx.rangeRadios[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            return static_cast<SearchRange>(i);
        }
    }
    return kInvalidSearchRange;
}

static unsigned GetSelectedTargetMask(const SearchCtx& ctx) {
    unsigned mask = 0;
    for (size_t i = 0; i < ctx.targetChecks.size(); ++i) {
        if (SendMessageW(ctx.targetChecks[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            mask |= (1u << static_cast<unsigned>(i));
        }
    }
    return mask;
}

static bool TargetMaskIncludesNotes(unsigned mask) { return (mask & TargetNote) != 0; }
static bool TargetMaskIncludesPdfs(unsigned mask) { return (mask & TargetPdf) != 0; }
static bool TargetMaskIncludesAnnotations(unsigned mask) { return (mask & TargetAnnot) != 0; }

static bool IsChecked(HWND hWnd) {
    return hWnd && SendMessageW(hWnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static note::SemanticSearchOptions CurrentSearchOptions(const SearchCtx* ctx) {
    note::SemanticSearchOptions options;
    if (!ctx) return options;
    options.normalizeWidthKana = IsChecked(ctx->optNormalizeWidthKana);
    options.ignoreCase = IsChecked(ctx->optIgnoreCase);
    options.ignoreSeparators = options.normalizeWidthKana;
    return options;
}

static void ApplySearchWindowTranslucency(HWND hWnd, const SearchCtx* ctx) {
    if (!hWnd) return;
    const bool translucent = !ctx || IsChecked(ctx->optTranslucent);
    LONG_PTR ex = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
    if ((ex & WS_EX_LAYERED) == 0) {
        SetWindowLongPtrW(hWnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    }
    SetLayeredWindowAttributes(hWnd, 0, static_cast<BYTE>(translucent ? 224 : 255), LWA_ALPHA);
}

static void FocusSearchWindowQuery(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd)) return;
    ShowWindow(hWnd, SW_SHOWNORMAL);
    SetForegroundWindow(hWnd);
    if (auto* ctx = reinterpret_cast<SearchCtx*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA))) {
        if (ctx->edit) {
            SetFocus(ctx->edit);
            SendMessageW(ctx->edit, EM_SETSEL, 0, -1);
        }
    }
}

static bool PrepareSearchRoots(SearchRange range,
                               std::vector<std::filesystem::path>& roots,
                               const SearchUiStrings& ui,
                               SearchCtx* ctx) {
    roots.clear();
    switch (range) {
    case SearchRange::CurrentSession: {
        std::filesystem::path sessionPath;
        std::error_code ec;
        if (!TryGetCurrentSessionRoot(sessionPath) || !std::filesystem::exists(sessionPath, ec) || ec) {
            AppendInfo(ctx, ui.msgNoSession);
            return false;
        }
        roots.push_back(std::move(sessionPath));
        return true;
    }
    case SearchRange::CurrentLecture: {
        std::filesystem::path lecturePath;
        std::error_code ec;
        if (!TryGetCurrentLectureRoot(lecturePath) || !std::filesystem::exists(lecturePath, ec) || ec) {
            AppendInfo(ctx, ui.msgNoLecture);
            return false;
        }
        roots = CollectLectureRoots(lecturePath);
        if (roots.empty()) {
            AppendInfo(ctx, ui.msgNoSearchableFiles);
            return false;
        }
        return true;
    }
    case SearchRange::WholeWorkspace:
        roots = CollectSessionRoots();
        if (roots.empty()) {
            AppendInfo(ctx, ui.msgNoSearchableFiles);
            return false;
        }
        return true;
    case SearchRange::WorkspaceAndTempExternal:
        roots = CollectSessionRootsIncludingVisibleLectures();
        if (roots.empty()) {
            AppendInfo(ctx, ui.msgNoSearchableFiles);
            return false;
        }
        return true;
    default:
        break;
    }
    return true;
}

static void AddScopedStagedDiffsToSearchJob(SearchJob& job) {
    const auto entries = file_output::ListStagedDiffEntries();
    for (const auto& entry : entries) {
        if (!entry.isLatest || entry.targetPath.empty() || entry.stagePath.empty()) continue;
        std::filesystem::path target(entry.targetPath);
        if (!IsSearchPathUnderAnyRoot(target, job.roots)) continue;
        if (entry.kind == file_output::StagedDiffKind::Note && job.includeNotes) {
            bool exists = false;
            const std::wstring key = NormalizePathKey(target);
            for (const auto& staged : job.stagedNoteFiles) {
                if (NormalizePathKey(staged.targetPath) == key) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                job.stagedNoteFiles.push_back(
                    {target, entry.stagePath, entry.revision.value_or(0)});
            }
        } else if (entry.kind == file_output::StagedDiffKind::Clrop && job.includeAnnots) {
            std::error_code ec;
            const bool targetPdfWillBeEnumerated = IsPdfFile(target) && std::filesystem::exists(target, ec) && !ec;
            if (targetPdfWillBeEnumerated) continue;
            bool exists = false;
            const std::wstring key = NormalizePathKey(target);
            for (const auto& staged : job.stagedAnnotFiles) {
                if (NormalizePathKey(staged.targetPath) == key) {
                    exists = true;
                    break;
                }
            }
            if (!exists) job.stagedAnnotFiles.push_back({target, entry.stagePath});
        }
    }
}

static void ResetSearchState(SearchCtx* ctx, const SearchUiStrings& ui) {
    if (!ctx || !ctx->results) return;
    ctx->job = SearchJob{};
    ctx->idleStatusText = ui.statusReady;
    ctx->statusPulse = 0;
    ctx->maxResultWidth = 0;
    ctx->hWheelRemainderPx = 0.0;
    ctx->vWheelRemainder1000 = 0;
    SendMessageW(ctx->results, WM_SETREDRAW, FALSE, 0);
    SendMessageW(ctx->results, LB_RESETCONTENT, 0, 0);
    SendMessageW(ctx->results, LB_SETHORIZONTALEXTENT, 0, 0);
    SendMessageW(ctx->results, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(ctx->results, nullptr, TRUE);
    UpdateRunButtonLabel(ctx, ui);
    UpdateSummaryLabel(ctx, ui);
}

static void AppendInfo(SearchCtx* ctx, const std::wstring& text) {
    if (!ctx) return;
    SearchResultItem item;
    item.kind = SearchResultKind::Info;
    item.title = text;
    item.display = text;
    AppendResults(ctx, std::vector<SearchResultItem>{item});
}

static void ProcessEnumeration(SearchCtx* ctx,
                               const SearchUiStrings& ui,
                               std::vector<SearchResultItem>& out,
                               int stepBudget) {
    if (!ctx) return;
    auto& job = ctx->job;
    if (job.enumerationDone) return;
    std::filesystem::recursive_directory_iterator end;
    std::error_code ec;

    for (int step = 0; step < stepBudget && !job.enumerationDone; ++step) {
        if (!job.iterValid) {
            if (job.rootIndex >= job.roots.size()) {
                job.enumerationDone = true;
                break;
            }
            job.iter = std::filesystem::recursive_directory_iterator(
                job.roots[job.rootIndex],
                std::filesystem::directory_options::skip_permission_denied,
                ec);
            if (ec) {
                job.rootIndex++;
                job.iterValid = false;
                continue;
            }
            job.iterValid = true;
        }

        if (job.iter == end) {
            job.rootIndex++;
            job.iterValid = false;
            continue;
        }

        const auto& entry = *job.iter;
        std::filesystem::path path = entry.path();
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(path, isReparse) && isReparse) {
            job.iter.disable_recursion_pending();
            job.iter.increment(ec);
            if (ec) {
                job.rootIndex++;
                job.iterValid = false;
            }
            continue;
        }
        if (job.includePaths) {
            std::wstring relPath = FormatPathForDisplay(path);
            SearchMatchInfo pathMatch = MatchText(relPath, job.queryTermsLower, job.options);
            if (!pathMatch.matched) {
                pathMatch = MatchText(path.filename().wstring(), job.queryTermsLower, job.options);
            }
            if (pathMatch.matched) {
                SearchResultItem item;
                item.kind = SearchResultKind::Path;
                item.path = path;
                item.hitCount = pathMatch.totalHits;
                item.title = relPath.empty() ? path.wstring() : relPath;
                item.meta = JoinMetaParts({
                    ui.tagPath,
                    FormatResultHitText(ui, pathMatch.totalHits)
                });
                std::error_code typeEc;
                item.preview = std::filesystem::is_directory(path, typeEc) ? L"" : path.filename().wstring();
                item.display = item.title + L" " + item.meta + L" " + item.preview;
                out.push_back(std::move(item));
            }
        }
        std::error_code stEc;
        if (entry.is_regular_file(stEc) && !stEc) {
            if (job.includeNotes && IsNoteFile(path)) {
                if (!HasStagedNoteForTarget(job, path)) {
                    job.noteFiles.push_back(path);
                }
            }
            if ((job.includePdfs || job.includeAnnots) && IsPdfFile(path)) {
                job.pdfFiles.push_back(path);
            }
        }

        job.iter.increment(ec);
        if (ec) {
            job.rootIndex++;
            job.iterValid = false;
        }
    }

    if (job.rootIndex >= job.roots.size() &&
        (!job.iterValid || job.iter == end)) {
        job.enumerationDone = true;
    }
}

static void ProcessNoteFiles(SearchCtx* ctx,
                             const SearchUiStrings& ui,
                             std::vector<SearchResultItem>& out,
                             int fileBudget) {
    if (!ctx) return;
    auto& job = ctx->job;
    int processed = 0;
    while (processed < fileBudget && job.noteIndex < job.noteFiles.size()) {
        const auto& path = job.noteFiles[job.noteIndex++];
        std::vector<SearchResultItem> found;
        SearchNoteFile(path, job.queryTermsLower, job.options, ui, found);
        if (!found.empty()) {
            out.insert(out.end(), found.begin(), found.end());
        }
        job.processedFiles++;
        processed++;
    }
}

static void ProcessStagedNoteFiles(SearchCtx* ctx,
                                   const SearchUiStrings& ui,
                                   std::vector<SearchResultItem>& out,
                                   int fileBudget) {
    if (!ctx) return;
    auto& job = ctx->job;
    int processed = 0;
    while (processed < fileBudget && job.stagedNoteIndex < job.stagedNoteFiles.size()) {
        const auto& staged = job.stagedNoteFiles[job.stagedNoteIndex++];
        std::vector<SearchResultItem> found;
        SearchStagedNoteFile(staged, job.queryTermsLower, job.options, ui, found);
        if (!found.empty()) {
            out.insert(out.end(), found.begin(), found.end());
        }
        job.processedFiles++;
        processed++;
    }
}

static void ProcessStagedAnnotationFiles(SearchCtx* ctx,
                                         const SearchUiStrings& ui,
                                         std::vector<SearchResultItem>& out,
                                         int fileBudget) {
    if (!ctx) return;
    auto& job = ctx->job;
    int processed = 0;
    while (processed < fileBudget && job.stagedAnnotIndex < job.stagedAnnotFiles.size()) {
        const auto& staged = job.stagedAnnotFiles[job.stagedAnnotIndex++];
        std::vector<SearchResultItem> found;
        SearchStagedAnnotationsForPdf(staged.targetPath, staged.stagePath, job.queryTermsLower, job.options, ui, found);
        if (!found.empty()) {
            out.insert(out.end(), found.begin(), found.end());
        }
        job.processedFiles++;
        processed++;
    }
}

static void ProcessPdfPages(SearchCtx* ctx,
                            const SearchUiStrings& ui,
                            std::vector<SearchResultItem>& out,
                            int pageBudget) {
    if (!ctx) return;
    auto& job = ctx->job;
    int processed = 0;
    while (processed < pageBudget) {
        if (!job.pdf) {
            if (job.pdfIndex >= job.pdfFiles.size()) return;
            const auto& path = job.pdfFiles[job.pdfIndex];
            if (!job.includePdfs && job.includeAnnots) {
                std::vector<SearchResultItem> annotResults;
                SearchAnnotationsForPdf(path, job.queryTermsLower, job.options, ui, annotResults);
                if (!annotResults.empty()) {
                    out.insert(out.end(), annotResults.begin(), annotResults.end());
                }
                job.pdfIndex++;
                job.processedFiles++;
                processed++;
                continue;
            }
            std::string utf8 = WideToUTF8(path.wstring());
            FPDF_DOCUMENT doc = nullptr;
            {
                std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
                doc = FPDF_LoadDocument(utf8.c_str(), nullptr);
            }
            if (!doc) {
                job.pdfIndex++;
                job.processedFiles++;
                continue;
            }
            PdfScanState state;
            state.path = path;
            state.doc = doc;
            state.pageIndex = 0;
            {
                std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
                state.pageCount = FPDF_GetPageCount(doc);
            }
            state.display = FormatPathForDisplay(path);
            state.ownsDoc = true;
            state.includeAnnots = job.includeAnnots;
            job.pdf = std::move(state);
        }

        auto& state = *job.pdf;
        if (state.pageIndex >= state.pageCount) {
            if (state.includeAnnots && !state.path.empty()) {
                std::vector<SearchResultItem> annotResults;
                SearchAnnotationsForPdf(state.path, job.queryTermsLower, job.options, ui, annotResults);
                if (!annotResults.empty()) {
                    out.insert(out.end(), annotResults.begin(), annotResults.end());
                }
            }
            if (state.ownsDoc && state.doc) {
                std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
                FPDF_CloseDocument(state.doc);
            }
            job.pdf.reset();
            job.pdfIndex++;
            job.processedFiles++;
            continue;
        }

        SearchPdfPage(state.doc, state.pageIndex, state.display, state.path, job.queryTermsLower, job.options, ui, out);
        state.pageIndex++;
        job.processedPages++;
        processed++;
    }
}

static bool IsSearchJobComplete(const SearchJob& job) {
    if (!job.enumerationDone) return false;
    if (job.noteIndex < job.noteFiles.size()) return false;
    if (job.stagedNoteIndex < job.stagedNoteFiles.size()) return false;
    if (job.pdf) return false;
    if (job.pdfIndex < job.pdfFiles.size()) return false;
    if (job.stagedAnnotIndex < job.stagedAnnotFiles.size()) return false;
    return true;
}

static void ProcessSearchTick(HWND hWnd, SearchCtx* ctx) {
    if (!ctx || !ctx->job.active) return;
    const auto ui = GetSearchUiStrings();
    std::vector<SearchResultItem> newItems;

    ProcessEnumeration(ctx, ui, newItems, kSearchEnumStepsPerTick);
    if (ctx->job.enumerationDone) {
        ProcessNoteFiles(ctx, ui, newItems, kSearchNoteFilesPerTick);
        ProcessStagedNoteFiles(ctx, ui, newItems, kSearchNoteFilesPerTick);
        ProcessPdfPages(ctx, ui, newItems, kSearchPdfPagesPerTick);
        ProcessStagedAnnotationFiles(ctx, ui, newItems, kSearchPdfPagesPerTick);
    }

    if (!newItems.empty()) {
        AppendResults(ctx, newItems);
    }

    ctx->statusPulse++;
    UpdateStatusLabel(ctx, ui);

    if (IsSearchJobComplete(ctx->job)) {
        FinishSearchJob(hWnd, ctx, ui, false);
    }
}

static void OpenSearchResult(HWND hWnd, SearchCtx* ctx, int listIndex) {
    if (!ctx || !ctx->results || listIndex < 0) return;
    size_t idx = static_cast<size_t>(listIndex);
    if (idx >= ctx->job.results.size()) return;
    const SearchResultItem item = ctx->job.results[idx];
    if (item.kind == SearchResultKind::Info) return;

    HWND owner = ctx->owner ? ctx->owner : hWnd;
    ClearOpenedSearchResultMarkers();

    if (item.kind == SearchResultKind::Path) {
        std::error_code ec;
        if (std::filesystem::is_directory(item.path, ec)) {
            return;
        }
        if (IsPdfOrImageFile(item.path)) {
            if (OpenSearchResultFileInCurrentFileList(owner, item.path.wstring())) {
                if (item.pageIndex >= 0 && g_hPdfView) JumpToPage(g_hPdfView, item.pageIndex);
            }
            return;
        }
        if (IsNoteFile(item.path)) {
            if (!OpenSearchResultFileInCurrentFileList(owner, item.path.wstring())) return;
            if (item.lineNumber >= 1) JumpToNoteLine(item.lineNumber);
            return;
        }
        return;
    }

    if (item.kind == SearchResultKind::NoteLine) {
        if (!item.path.empty()) {
            if (!OpenSearchResultFileInCurrentFileList(owner, item.path.wstring())) return;
        }
        if (!CurrentOpenNoteMatchesSnapshot(item.noteSnapshotIdentity)) return;
        if (item.lineNumber >= 1) JumpToNoteLine(item.lineNumber);
        if (item.textStart != std::wstring::npos && item.textEnd > item.textStart) {
            SetNoteSearchResultMarker(item.textStart, item.textEnd);
        }
        return;
    }

    if (item.kind == SearchResultKind::PdfPage || item.kind == SearchResultKind::Annot) {
        if (!item.path.empty()) {
            if (!OpenSearchResultFileInCurrentFileList(owner, item.path.wstring())) return;
        }
        if (g_hPdfView) {
            if (item.kind == SearchResultKind::Annot && item.hasFocusPoint && item.pageIndex >= 0) {
                JumpToPdfPoint(g_hPdfView, item.pageIndex, item.focusX, item.focusY);
                SetPdfSearchPointMarker(item.pageIndex, item.focusX, item.focusY);
            } else if (item.pageIndex >= 0) {
                JumpToPage(g_hPdfView, item.pageIndex);
                if (item.textStart != std::wstring::npos && item.textEnd > item.textStart) {
                    SetPdfSearchTextMarker(item.pageIndex, item.textStart, item.textEnd);
                }
            }
        }
        return;
    }
}

static void RunSearch(HWND hWnd, SearchCtx* ctx) {
    if (!ctx || !ctx->results) return;
    const auto ui = GetSearchUiStrings();
    if (ctx->job.active) {
        CancelSearchJob(hWnd, ctx, false);
    }
    ResetSearchState(ctx, ui);

    std::wstring query = TrimWhitespace(GetWindowTextValue(ctx->edit));
    if (query.empty()) {
        AppendInfo(ctx, ui.msgNeedQuery);
        SetIdleStatus(ctx, ui.msgNeedQuery);
        UpdateStatusLabel(ctx, ui);
        return;
    }
    note::SemanticSearchOptions options = CurrentSearchOptions(ctx);
    SearchQueryGroups queryTermsLower = ParseSearchTermsLower(query, options);
    if (queryTermsLower.empty()) {
        AppendInfo(ctx, ui.msgNeedQuery);
        SetIdleStatus(ctx, ui.msgNeedQuery);
        UpdateStatusLabel(ctx, ui);
        return;
    }

    const SearchRange range = GetSelectedRange(*ctx);
    const unsigned targetMask = GetSelectedTargetMask(*ctx);
    if (range == kInvalidSearchRange) {
        AppendInfo(ctx, ui.msgNeedRange);
        SetIdleStatus(ctx, ui.msgNeedRange);
        UpdateStatusLabel(ctx, ui);
        return;
    }
    if (targetMask == 0) {
        AppendInfo(ctx, ui.msgNeedTarget);
        SetIdleStatus(ctx, ui.msgNeedTarget);
        UpdateStatusLabel(ctx, ui);
        return;
    }
    ctx->job.includeNotes = TargetMaskIncludesNotes(targetMask);
    ctx->job.includePdfs = TargetMaskIncludesPdfs(targetMask);
    ctx->job.includeAnnots = TargetMaskIncludesAnnotations(targetMask);
    if (!PrepareSearchRoots(range, ctx->job.roots, ui, ctx)) {
        if (!ctx->job.results.empty()) {
            const auto& last = ctx->job.results.back();
            if (!last.display.empty()) SetIdleStatus(ctx, last.display);
        }
        UpdateStatusLabel(ctx, ui);
        return;
    }
    AddScopedStagedDiffsToSearchJob(ctx->job);
    ctx->job.enumerationDone = false;

    ctx->job.active = true;
    ctx->job.range = range;
    ctx->job.targetMask = targetMask;
    ctx->job.query = query;
    ctx->job.queryTermsLower = queryTermsLower;
    ctx->job.options = options;
    UpdateRunButtonLabel(ctx, ui);

    if (IsSearchJobComplete(ctx->job)) {
        FinishSearchJob(hWnd, ctx, ui, false);
        return;
    }

    SetSearchControlsEnabled(ctx, false);
    UpdateStatusLabel(ctx, ui);
    SetTimer(hWnd, kSearchTimerId, kSearchTimerIntervalMs, nullptr);
    ProcessSearchTick(hWnd, ctx);
}

static void LayoutSearchWindow(HWND hWnd, SearchCtx* ctx) {
    if (!ctx) return;
    RECT rc{};
    GetClientRect(hWnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    const int pad = 16;
    const int labelH = 18;
    const int rowH = 30;
    const int hintH = 18;
    const int statusH = 18;
    const int gap = 8;
    const int btnW = 110;
    const int radioH = 32;
    const int radioGapY = 6;

    int x = pad;
    int y = pad;
    int contentW = std::max(0, w - pad * 2);
    auto layoutRadioGroup = [&](HWND label, const std::vector<HWND>& radios) {
        if (label) {
            MoveWindow(label, x, y, contentW, labelH, TRUE);
        }
        y += labelH + gap;
        int cols = 1;
        if (contentW >= 700) cols = 3;
        else if (contentW >= 420) cols = 2;
        int groupW = std::max(0, (contentW - gap * (cols - 1)) / cols);
        for (size_t i = 0; i < radios.size(); ++i) {
            int col = static_cast<int>(i % static_cast<size_t>(cols));
            int row = static_cast<int>(i / static_cast<size_t>(cols));
            int rx = x + col * (groupW + gap);
            int ry = y + row * (radioH + radioGapY);
            if (radios[i]) {
                MoveWindow(radios[i], rx, ry, groupW, radioH, TRUE);
            }
        }
        int rows = radios.empty() ? 0 :
                   static_cast<int>((radios.size() + static_cast<size_t>(cols) - 1) / static_cast<size_t>(cols));
        y += rows * (radioH + radioGapY);
        y += gap;
    };

    if (ctx->labelQuery) {
        MoveWindow(ctx->labelQuery, x, y, contentW, labelH, TRUE);
    }
    y += labelH + gap;
    int editW = std::max(0, contentW - btnW - gap);
    if (ctx->edit) {
        MoveWindow(ctx->edit, x, y, editW, rowH, TRUE);
    }
    if (ctx->btnRun) {
        MoveWindow(ctx->btnRun, x + editW + gap, y, btnW, rowH, TRUE);
    }
    y += rowH + gap;
    if (ctx->labelHint) {
        MoveWindow(ctx->labelHint, x, y, contentW, hintH, TRUE);
    }
    y += hintH + gap;
    if (ctx->labelStatus) {
        MoveWindow(ctx->labelStatus, x, y, contentW, statusH, TRUE);
    }
    y += statusH + gap;
    if (ctx->labelSummary) {
        MoveWindow(ctx->labelSummary, x, y, contentW, statusH, TRUE);
    }
    y += statusH + gap;
    layoutRadioGroup(ctx->labelRange, ctx->rangeRadios);
    layoutRadioGroup(ctx->labelTarget, ctx->targetChecks);
    if (ctx->labelOptions) {
        MoveWindow(ctx->labelOptions, x, y, contentW, labelH, TRUE);
    }
    y += labelH + gap;
    const int optCols = contentW >= 700 ? 3 : 1;
    const int optW = std::max(0, (contentW - gap * (optCols - 1)) / optCols);
    const HWND optionButtons[] = {ctx->optNormalizeWidthKana, ctx->optIgnoreCase, ctx->optTranslucent};
    for (int i = 0; i < 3; ++i) {
        int col = i % optCols;
        int row = i / optCols;
        if (optionButtons[i]) {
            MoveWindow(optionButtons[i], x + col * (optW + gap), y + row * (radioH + radioGapY), optW, radioH, TRUE);
        }
    }
    y += ((3 + optCols - 1) / optCols) * (radioH + radioGapY) + gap;
    if (ctx->labelResults) {
        MoveWindow(ctx->labelResults, x, y, contentW, labelH, TRUE);
    }
    y += labelH + gap;
    if (ctx->results) {
        int listH = std::max(0, h - y - pad);
        MoveWindow(ctx->results, x, y, contentW, listH, TRUE);
    }
}

static LRESULT CALLBACK SearchWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<SearchCtx*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto ui = GetSearchUiStrings();
        ctx = new SearchCtx();
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        if (cs) ctx->owner = reinterpret_cast<HWND>(cs->lpCreateParams);
        ctx->idleStatusText = ui.statusReady;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

        ctx->labelQuery = CreateWindowExW(0, L"STATIC", ui.queryLabel.c_str(),
                                          WS_CHILD | WS_VISIBLE,
                                          0, 0, 0, 0, hWnd, nullptr, g_hInst, nullptr);
        ctx->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                    0, 0, 0, 0, hWnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH_QUERY)),
                                    g_hInst, nullptr);
        ctx->labelHint = CreateWindowExW(0, L"STATIC", ui.queryHint.c_str(),
                                         WS_CHILD | WS_VISIBLE,
                                         0, 0, 0, 0, hWnd, nullptr, g_hInst, nullptr);
        ctx->btnRun = CreateWindowExW(0, L"BUTTON", ui.runLabel.c_str(),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, 0, 0, 0, hWnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH_RUN)),
                                      g_hInst, nullptr);
        std::wstring statusText = ui.statusLabel + L": " + ui.statusReady;
        ctx->labelStatus = CreateWindowExW(0, L"STATIC", statusText.c_str(),
                                           WS_CHILD | WS_VISIBLE,
                                           0, 0, 0, 0, hWnd, nullptr, g_hInst, nullptr);
        ctx->labelSummary = CreateWindowExW(0, L"STATIC", BuildSummaryText(ctx->job, ui).c_str(),
                                            WS_CHILD | WS_VISIBLE,
                                            0, 0, 0, 0, hWnd, nullptr, g_hInst, nullptr);
        ctx->labelRange = CreateWindowExW(0, L"STATIC", ui.rangeLabel.c_str(),
                                          WS_CHILD | WS_VISIBLE,
                                          0, 0, 0, 0, hWnd, nullptr, g_hInst, nullptr);
        ctx->rangeRadios.clear();
        ctx->rangeRadios.reserve(ui.ranges.size());
        for (size_t i = 0; i < ui.ranges.size(); ++i) {
            DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON;
            if (i == 0) style |= WS_GROUP;
            HWND radio = CreateWindowExW(0, L"BUTTON", ui.ranges[i].c_str(),
                                         style,
                                         0, 0, 0, 0, hWnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH_RANGE_BASE + static_cast<int>(i))),
                                         g_hInst, nullptr);
            ctx->rangeRadios.push_back(radio);
            EnableSearchOwnerDrawButton(radio);
            if (radio && static_cast<int>(i) == static_cast<int>(ctx->job.range)) {
                SendMessageW(radio, BM_SETCHECK, BST_CHECKED, 0);
            }
        }
        ctx->labelTarget = CreateWindowExW(0, L"STATIC", ui.targetLabel.c_str(),
                                           WS_CHILD | WS_VISIBLE,
                                           0, 0, 0, 0, hWnd, nullptr, g_hInst, nullptr);
        ctx->targetChecks.clear();
        ctx->targetChecks.reserve(ui.targets.size());
        for (size_t i = 0; i < ui.targets.size(); ++i) {
            DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX;
            if (i == 0) style |= WS_GROUP;
            HWND btn = CreateWindowExW(0, L"BUTTON", ui.targets[i].c_str(),
                                       style,
                                       0, 0, 0, 0, hWnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH_TARGET_BASE + static_cast<int>(i))),
                                       g_hInst, nullptr);
            ctx->targetChecks.push_back(btn);
            EnableSearchOwnerDrawButton(btn);
        }
        // Default: search everything (note + PDF + annotations).
        for (HWND btn : ctx->targetChecks) {
            if (btn) SendMessageW(btn, BM_SETCHECK, BST_CHECKED, 0);
        }

        ctx->labelOptions = CreateWindowExW(0, L"STATIC", ui.optionsLabel.c_str(),
                                            WS_CHILD | WS_VISIBLE,
                                            0, 0, 0, 0, hWnd, nullptr, g_hInst, nullptr);
        ctx->optNormalizeWidthKana = CreateWindowExW(0, L"BUTTON", ui.normalizeWidthKanaLabel.c_str(),
                                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                     0, 0, 0, 0, hWnd,
                                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH_OPTION_NORMALIZE)),
                                                     g_hInst, nullptr);
        ctx->optIgnoreCase = CreateWindowExW(0, L"BUTTON", ui.ignoreCaseLabel.c_str(),
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                             0, 0, 0, 0, hWnd,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH_OPTION_IGNORE_CASE)),
                                             g_hInst, nullptr);
        ctx->optTranslucent = CreateWindowExW(0, L"BUTTON", ui.translucentLabel.c_str(),
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                              0, 0, 0, 0, hWnd,
                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH_OPTION_TRANSLUCENT)),
                                              g_hInst, nullptr);
        if (ctx->optNormalizeWidthKana) SendMessageW(ctx->optNormalizeWidthKana, BM_SETCHECK, BST_CHECKED, 0);
        if (ctx->optIgnoreCase) SendMessageW(ctx->optIgnoreCase, BM_SETCHECK, BST_CHECKED, 0);
        if (ctx->optTranslucent) SendMessageW(ctx->optTranslucent, BM_SETCHECK, BST_CHECKED, 0);
        EnableSearchOwnerDrawButton(ctx->optNormalizeWidthKana);
        EnableSearchOwnerDrawButton(ctx->optIgnoreCase);
        EnableSearchOwnerDrawButton(ctx->optTranslucent);
        ApplySearchWindowTranslucency(hWnd, ctx);
        std::wstring resultsText = ui.resultLabel + L"  " + ui.resultHint;
        ctx->labelResults = CreateWindowExW(0, L"STATIC", resultsText.c_str(),
                                            WS_CHILD | WS_VISIBLE,
                                            0, 0, 0, 0, hWnd, nullptr, g_hInst, nullptr);
        ctx->results = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
                                           LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                       0, 0, 0, 0, hWnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH_RESULTS)),
                                       g_hInst, nullptr);

        SetUIFont(ctx->labelQuery);
        SetUIFont(ctx->edit);
        SetUIFont(ctx->labelHint);
        SetUIFont(ctx->btnRun);
        SetUIFont(ctx->labelStatus);
        SetUIFont(ctx->labelSummary);
        SetUIFont(ctx->labelRange);
        for (HWND radio : ctx->rangeRadios) SetUIFont(radio);
        SetUIFont(ctx->labelTarget);
        for (HWND btn : ctx->targetChecks) SetUIFont(btn);
        SetUIFont(ctx->labelOptions);
        SetUIFont(ctx->optNormalizeWidthKana);
        SetUIFont(ctx->optIgnoreCase);
        SetUIFont(ctx->optTranslucent);
        SetUIFont(ctx->labelResults);
        SetUIFont(ctx->results);
        if (ctx->edit) {
            SendMessageW(ctx->edit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(ui.queryCue.c_str()));
            SetWindowSubclass(ctx->edit, SearchEditProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
        }
        if (ctx->results) {
            SetWindowSubclass(ctx->results, SearchResultsProc, 2, reinterpret_cast<DWORD_PTR>(ctx));
        }
        {
            int itemH = CalcListItemHeightFromFont(ctx->results, 3, 18);
            if (itemH > 0) {
                SendMessageW(ctx->results, LB_SETITEMHEIGHT, 0, static_cast<LPARAM>(itemH));
            }
        }

        UpdateRunButtonLabel(ctx, ui);
        UpdateSummaryLabel(ctx, ui);
        UpdateStatusLabel(ctx, ui);
        LayoutSearchWindow(hWnd, ctx);
        ApplyThemeToDialog(hWnd);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        if (mmi) {
            mmi->ptMinTrackSize.x = 620;
            mmi->ptMinTrackSize.y = 560;
            return 0;
        }
        break;
    }
    case WM_SIZE:
        LayoutSearchWindow(hWnd, ctx);
        return 0;
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
        if (dis && dis->CtlID == IDC_SEARCH_RESULTS) {
            DrawSearchResultsItem(ctx, dis);
            return TRUE;
        }
        if (DrawSearchToggleButton(dis)) return TRUE;
        if (DrawThemeButton(dis)) return TRUE;
        break;
    }
    case WM_MEASUREITEM: {
        auto* mi = reinterpret_cast<LPMEASUREITEMSTRUCT>(lParam);
        if (mi && mi->CtlID == IDC_SEARCH_RESULTS && ctx && ctx->results) {
            int itemH = CalcListItemHeightFromFont(ctx->results, 3, 18);
            if (itemH > 0) {
                mi->itemHeight = static_cast<UINT>(itemH);
                return TRUE;
            }
        }
        break;
    }
    case WM_TIMER:
        if (wParam == kSearchTimerId) {
            ProcessSearchTick(hWnd, ctx);
            return 0;
        }
        break;
    case kMsgOpenSearchResult:
        if (ctx) {
            int sel = static_cast<int>(wParam);
            size_t idx = static_cast<size_t>(sel);
            if (sel >= 0 && idx < ctx->job.results.size() &&
                ctx->job.results[idx].kind != SearchResultKind::Info) {
                OpenSearchResult(hWnd, ctx, sel);
            }
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        if (id == IDC_SEARCH_RUN && code == BN_CLICKED) {
            if (ctx && ctx->job.active) {
                CancelSearchJob(hWnd, ctx, false);
            } else {
                RunSearch(hWnd, ctx);
            }
            return 0;
        }
        if (id == IDC_SEARCH_RESULTS && code == LBN_DBLCLK) {
            int sel = static_cast<int>(SendMessageW(ctx->results, LB_GETCURSEL, 0, 0));
            QueueOpenSearchResult(hWnd, sel);
            return 0;
        }
        if (id == IDC_SEARCH_OPTION_TRANSLUCENT && code == BN_CLICKED) {
            ApplySearchWindowTranslucency(hWnd, ctx);
            return 0;
        }
        if ((id >= IDC_SEARCH_RANGE_BASE && id < IDC_SEARCH_RANGE_BASE + kSearchRangeCount) ||
            (id >= IDC_SEARCH_TARGET_BASE && id < IDC_SEARCH_TARGET_BASE + kSearchTargetCount) ||
            (id >= IDC_SEARCH_OPTION_BASE && id < IDC_SEARCH_OPTION_BASE + 3)) {
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        CancelSearchJob(hWnd, ctx, true);
        ClearOpenedSearchResultMarkers();
        delete ctx;
        if (g_hSearchWnd == hWnd) g_hSearchWnd = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace

void ShowSearchWindow(HWND parent) {
    try {
    fault_injection::MaybeThrow(L"ShowSearchWindow:start");
    if (g_hSearchWnd) {
        FocusSearchWindowQuery(g_hSearchWnd);
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SearchWndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_hThemeWindowBrush ? g_hThemeWindowBrush
                                           : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kSearchWndClass;
    wc.hIcon = LoadSearchWindowIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hIconSm = LoadSearchWindowIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    static bool registered = false;
    if (!registered) {
        RegisterClassExW(&wc);
        registered = true;
    }
    const auto ui = GetSearchUiStrings();
    g_hSearchWnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_LAYERED, kSearchWndClass, ui.title.c_str(),
                                   WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_SIZEBOX,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 760, 620,
                                   nullptr, nullptr, g_hInst, parent);
    if (g_hSearchWnd) {
        ShowWindow(g_hSearchWnd, SW_SHOWNORMAL);
        UpdateWindow(g_hSearchWnd);
        FocusSearchWindowQuery(g_hSearchWnd);
    }
    } catch (const std::exception& ex) {
        AppendCrashLogLine("ShowSearchWindow", ex.what());
        ShowSoftNotice(parent ? parent : g_hMainWnd,
                       IsEnglishUi() ? L"Search window could not be opened." : L"検索ウィンドウを開けませんでした。",
                       SoftNoticeKind::Error);
    } catch (...) {
        AppendCrashLogLine("ShowSearchWindow", nullptr);
        ShowSoftNotice(parent ? parent : g_hMainWnd,
                       IsEnglishUi() ? L"Search window could not be opened." : L"検索ウィンドウを開けませんでした。",
                       SoftNoticeKind::Error);
    }
}

void ToggleSearchWindow(HWND parent) {
    if (g_hSearchWnd && IsWindow(g_hSearchWnd)) {
        FocusSearchWindowQuery(g_hSearchWnd);
        return;
    }
    ShowSearchWindow(parent);
}
void ShowSearchWindowWithPreset(HWND parent, int rangeIndex, unsigned targetMask,
                                const std::wstring& initialQuery) {
    try {
    fault_injection::MaybeThrow(L"ShowSearchWindowWithPreset:start");
    // Keep this function independent from internal SearchCtx types by using control IDs.
    constexpr int kSearchRangeCountPublic = 4;
    constexpr int kSearchTargetCountPublic = 3;
    constexpr int IDC_SEARCH_QUERY_PUBLIC = 4101;
    constexpr int IDC_SEARCH_RANGE_BASE_PUBLIC = 4110;
    constexpr int IDC_SEARCH_TARGET_BASE_PUBLIC = 4130;

    ShowSearchWindow(parent);
    if (!g_hSearchWnd) return;

    if (rangeIndex >= 0 && rangeIndex < kSearchRangeCountPublic) {
        for (int i = 0; i < kSearchRangeCountPublic; ++i) {
            HWND radio = GetDlgItem(g_hSearchWnd, IDC_SEARCH_RANGE_BASE_PUBLIC + i);
            if (!radio) continue;
            SendMessageW(radio, BM_SETCHECK, (i == rangeIndex) ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    }

    if (targetMask != 0) {
        for (int i = 0; i < kSearchTargetCountPublic; ++i) {
            HWND btn = GetDlgItem(g_hSearchWnd, IDC_SEARCH_TARGET_BASE_PUBLIC + i);
            if (!btn) continue;
            unsigned bit = 1u << static_cast<unsigned>(i);
            SendMessageW(btn, BM_SETCHECK, (targetMask & bit) ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    }

    HWND edit = GetDlgItem(g_hSearchWnd, IDC_SEARCH_QUERY_PUBLIC);
    if (edit) {
        if (!initialQuery.empty()) {
            SetWindowTextW(edit, initialQuery.c_str());
        }
        SetFocus(edit);
        SendMessageW(edit, EM_SETSEL, 0, -1);
    }
    } catch (const std::exception& ex) {
        AppendCrashLogLine("ShowSearchWindowWithPreset", ex.what());
        ShowSoftNotice(parent ? parent : g_hMainWnd,
                       IsEnglishUi() ? L"Preset search could not be opened." : L"検索プリセットを開けませんでした。",
                       SoftNoticeKind::Error);
    } catch (...) {
        AppendCrashLogLine("ShowSearchWindowWithPreset", nullptr);
        ShowSoftNotice(parent ? parent : g_hMainWnd,
                       IsEnglishUi() ? L"Preset search could not be opened." : L"検索プリセットを開けませんでした。",
                       SoftNoticeKind::Error);
    }
}


