#include "theme/built_in_theme.h"
#include "clrop/json.h"
#include "core/text_encoding.h"
#include "core/json_string.h"
#include "core/atomic_write.h"
#include "core/localization.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <algorithm>
#include <cwctype>
#include <limits>

#include "note/note_model.h"
#include "note/note_parser.h"
#include "clrop/types.h"
#include "readonly_viewer/mermaid_subset_preview.h"

#include <windowsx.h>
#include <algorithm>

#include "pdf_preview_panel.h"
#include "reader_chrome.h"
#include "reader_document.h"
#include "pdf_search.h"
#include "ui/window_corners.h"
#include "ui/application_shortcuts.h"
#include "ui/shortcut_input_guard.h"
#include "readonly_viewer/shortcut_reference.h"
#include "text_preview_panel.h"
#include "fpdfview.h"

// The production entry owns the implementation; the comparison entry only
// selects an isolated class/session identity. Existing production callers and
// persisted paths keep their original executable, window and setup names.
#if defined(PDF_NOTE_READER_PROTOTYPE) && PDF_NOTE_READER_PROTOTYPE
constexpr wchar_t kViewerWindowClass[] = L"PdfReadonlyViewerNextWindow";
constexpr wchar_t kViewerSessionFile[] = L"reader_next_setup.json";
#else
constexpr wchar_t kViewerWindowClass[] = L"PdfReadonlyViewerWindow";
constexpr wchar_t kViewerSessionFile[] = L"readonly_setup.json";
#endif

class ScopedComInit {
public:
    ScopedComInit() {
        m_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }
    ~ScopedComInit() {
        if (SUCCEEDED(m_hr)) {
            CoUninitialize();
        }
    }
    bool IsSucceeded() const { return SUCCEEDED(m_hr); }
private:
    HRESULT m_hr;
};

class ScopedPdfiumLibrary {
public:
    ScopedPdfiumLibrary() {
        FPDF_InitLibrary();
    }
    ~ScopedPdfiumLibrary() {
        FPDF_DestroyLibrary();
    }
};

// Keep the standalone viewer visually quiet and close to the main application's
// conventional Win32 frame. DWM attributes are available only on newer Windows;
// loading the API at runtime keeps older systems on their normal system frame.
using DwmSetWindowAttributeFn = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmBorderColor = 34;
constexpr DWORD kDwmCaptionColor = 35;
constexpr DWORD kDwmTextColor = 36;
constexpr DWORD kDwmDoNotRound = 1;

void ApplyNeutralWindowFrame(HWND hwnd) {
    const HRESULT corners = ui::ApplyAuxiliaryWindowCorners(hwnd);
    (void)corners;
    HMODULE dwmapi = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwmapi) return;
    const auto setAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(
        GetProcAddress(dwmapi, "DwmSetWindowAttribute"));
    if (setAttribute) {
        const DWORD cornerPreference = kDwmDoNotRound;
        const COLORREF captionColor = reader_next::kCanvas;
        const COLORREF borderColor = RGB(210, 210, 210);
        const COLORREF textColor = RGB(32, 32, 32);
        setAttribute(hwnd, kDwmWindowCornerPreference, &cornerPreference, sizeof(cornerPreference));
        setAttribute(hwnd, kDwmCaptionColor, &captionColor, sizeof(captionColor));
        setAttribute(hwnd, kDwmBorderColor, &borderColor, sizeof(borderColor));
        setAttribute(hwnd, kDwmTextColor, &textColor, sizeof(textColor));
    }
    FreeLibrary(dwmapi);
}

std::wstring UTF8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, &s[0], (int)s.size(), NULL, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, &s[0], (int)s.size(), &result[0], size);
    return result;
}

std::string WideToUTF8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, &w[0], (int)w.size(), NULL, 0, NULL, NULL);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, &w[0], (int)w.size(), &result[0], size, NULL, NULL);
    return result;
}

HWND g_hwndFileTree = NULL;
HWND g_hwndTocTree = NULL;
HWND g_hwndFileTreeLabel = NULL;
HWND g_hwndTocTreeLabel = NULL;
HWND g_hwndTabControl = NULL;
HWND g_hwndEditControl = NULL;
HWND g_hwndMain = NULL;
HWND g_hwndPdfPanel = NULL;
HWND g_hwndDetachedDiagram = NULL;
HWND g_hwndToggleLeftPaneButton = NULL;
HWND g_hwndOpenFolderButton = NULL;
WNDPROC g_originalEditProc = nullptr;
int g_splitX = 280;
bool g_isDraggingSplitter = false;
bool g_leftPaneVisible = true;
bool g_persistSessionEnabled = false;
bool g_sessionStateLoaded = false;
bool g_isRichEdit = false;
HMODULE g_richEditModule = NULL;

constexpr int kDecoratedButtonId = 106;
constexpr int kRawButtonId = 107;
constexpr int kHexButtonId = 108;
constexpr int kOpenFileButtonId = 109;
constexpr int kOpenFolderButtonId = 110;
constexpr int kDiagramButtonId = 111;
constexpr int kPdfRangeButtonId = 112;
constexpr int kCancelLoadButtonId = 113;
constexpr int kToggleLeftPaneButtonId = 116;
constexpr int kPersistSessionMenuId = 117;
constexpr int kPathMarkTabPersistentMenuId = 118;
constexpr int kPathMarkTabTemporaryMenuId = 119;
constexpr int kPathMarkFolderPersistentMenuId = 120;
constexpr int kPathMarkFolderTemporaryMenuId = 121;
constexpr int kLicensesMenuId = 122;
constexpr int kShortcutReferenceMenuId = 123;
constexpr int kDetachedDiagramZoomInButtonId = 201;
constexpr int kDetachedDiagramZoomOutButtonId = 202;
constexpr int kDetachedDiagramResetButtonId = 203;
constexpr int kDetachedDiagramFitButtonId = 205;
constexpr wchar_t kDetachedDiagramWindowClass[] = L"PdfNoteDetachedMermaidDiagram";
constexpr UINT kTabContextMenuMessage = WM_APP + 4;
constexpr UINT kCloseEmptyAfterTabTransferMessage = WM_APP + 5;
constexpr UINT kTabMenuSelect = 401;
constexpr UINT kTabMenuClose = 402;
constexpr UINT kTabMenuCloseOthers = 403;
constexpr UINT kTabMenuCloseToRight = 404;
constexpr UINT kTabMenuCloseAll = 405;
constexpr UINT kTabMenuMoveLeft = 406;
constexpr UINT kTabMenuMoveRight = 407;
constexpr UINT kTabMenuChangeRange = 408;
constexpr UINT kTabMenuReopenClosed = 409;
constexpr UINT kTabMenuListBase = 500;
constexpr UINT kTabMenuMoveToNewWindow = 700;
constexpr UINT kTabMenuMoveToWindowBase = 720;
constexpr ULONG_PTR kCopyDataOpenReadonlyTab = 0x504E5254; // "PNRT"
// Same trusted sender and path contract as PNRT; a single '0'..'3' prefix
// carries the transient view angle. A rejected transfer retains the source tab.
constexpr ULONG_PTR kCopyDataOpenRotatedReadonlyTab = 0x504E5252; // "PNRR"
constexpr ULONG_PTR kCopyDataTransferAck = 0x504E414B; // "PNAK"

enum class ViewMode {
    Decorated,
    Raw,
    Hex,
    Diagram,
    Pdf,
};

std::vector<std::wstring> g_filePaths;
std::vector<std::wstring> g_directoryPaths;

struct FileSnapshot {
    uintmax_t size = 0;
    std::filesystem::file_time_type lastWriteTime{};
};

struct ReaderViewPosition {
    POINT scroll{};
    CHARRANGE selection{};
    LONG anchor = 0, anchor_y = 0;
    bool captured = false;
};

struct OpenTab {
    std::wstring title;
    std::wstring path;
    std::wstring rawText;
    std::vector<unsigned char> bytes;
    bool isBinary = false;
    bool isPdf = false;
    bool loadError = false;
    bool isPartialText = false;
    ByteRange textRange{};
    uintmax_t sourceSize = 0;
    std::optional<FileSnapshot> sourceSnapshot;
    ViewMode viewMode = ViewMode::Raw;
    std::vector<int> jpLines;
    int totalLines = 1;
    bool persistent = false;
    ReaderViewPosition readingPositions[5];
    reader_next::PdfViewState pdfPosition;
    textviewer::mermaid::DiagramViewState diagramPosition;
    std::vector<textviewer::mermaid::DiagramViewState> inlineDiagramPositions;
    std::shared_ptr<RenderedDocument> reading;
    std::vector<InlineDiagram> diagrams;
    std::optional<textviewer::mermaid::DiagramModel> combinedDiagram;
};
std::vector<OpenTab> g_tabs;
std::vector<OpenTab> g_closedTabs;
std::vector<HWND> g_tabTransferTargets;
struct PendingTabTransfer { ULONG_PTR token; std::wstring path; };
std::vector<PendingTabTransfer> g_pendingTabTransfers;
ULONG_PTR g_nextTabTransferToken = 1;
int g_currentTab = -1;
std::wstring g_sessionFolder;
bool g_sessionFolderPersistent = false;
textviewer::mermaid::MermaidSubsetPreview g_mermaidPreview;
textviewer::mermaid::MermaidSubsetPreview g_detachedMermaidPreview;

struct InlineDiagramPreview {
    size_t text_offset = 0;
    size_t reserved_length = 0;
    size_t diagram_index = 0;
    std::unique_ptr<textviewer::mermaid::MermaidSubsetPreview> preview;
};
std::vector<InlineDiagramPreview> g_inlineDiagramPreviews;

WNDPROC g_originalTabProc = nullptr;
int g_draggingTab = -1;
POINT g_tabDragOrigin{};
bool g_tabDragActive = false;
int g_tabPressedClose = -1;
std::vector<std::wstring> g_folderHistory;
std::wstring g_browseFolder;
bool g_browseBack = false;
bool g_syncFileSelection = false;
HWND g_hwndHighlightBar = NULL;
reader_next::SearchOverview g_searchOverview;
void RefreshSearchOverview();

void PopulateFileTree(const std::wstring& directoryPath);
void OpenDetachedDiagramWindow(HWND source);
void UpdateInlineDiagramBounds();
void SetViewerStatus(HWND owner, const std::wstring& message);
void UpdateReaderWindowTitle(HWND owner);
bool OpenPathInTab(HWND owner, const std::wstring& path);
bool MoveTabToNewWindow(HWND owner, int index);
bool MoveTabToWindow(HWND owner, int index, HWND target);
void CollectOtherViewerWindows(HWND owner);
bool IsTrustedReadonlyViewerWindow(HWND hwnd);
void SendTabTransferAcknowledgement(HWND source, ULONG_PTR token);
void RelayoutViewer();
[[nodiscard]] bool SaveReadonlySession(std::wstring* error = nullptr);
void RestoreReadonlySession(HWND owner);

void BuildViewerMenu(HWND hwnd) {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU paths = CreatePopupMenu();
    HMENU settings = CreatePopupMenu();
    if (!bar || !file || !view || !paths || !settings) return;
    auto shortcutText = [](const wchar_t* id, application_shortcuts::Action action) {
        return application_shortcuts::MenuHint(localization::Text(id), action, application_shortcuts::Surface::Reader);
    };
    using ShortcutAction = application_shortcuts::Action;
    AppendMenuW(file, MF_STRING, kOpenFileButtonId, shortcutText(L"readonly.menu.open_file", ShortcutAction::OpenFile).c_str());
    AppendMenuW(file, MF_STRING, kOpenFolderButtonId, localization::Text(L"readonly.menu.open_folder").c_str());
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, kTabMenuClose, shortcutText(L"readonly.tab.close", ShortcutAction::CloseFile).c_str());
    AppendMenuW(file, MF_STRING, kTabMenuCloseAll, shortcutText(L"readonly.menu.close_all_tabs", ShortcutAction::CloseAll).c_str());
    AppendMenuW(file, MF_STRING | MF_GRAYED, readonly_viewer::kCmdPdfExportAnnotatedPdf, shortcutText(L"shortcut.action.export_pdf", ShortcutAction::Export).c_str());
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, SC_CLOSE, localization::Text(L"readonly.menu.exit").c_str());
    AppendMenuW(view, MF_STRING, kToggleLeftPaneButtonId, localization::Text(L"readonly.menu.toggle_list_pane").c_str());
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, kDecoratedButtonId, localization::Text(L"readonly.menu.decorated_view").c_str());
    AppendMenuW(view, MF_STRING, kRawButtonId, localization::Text(L"readonly.menu.raw_shortcut").c_str());
    AppendMenuW(view, MF_STRING, kHexButtonId, localization::Text(L"readonly.menu.hex_shortcut").c_str());
    AppendMenuW(view, MF_STRING, kDiagramButtonId, localization::Text(L"readonly.menu.diagram_list").c_str());
    AppendMenuW(view, MF_STRING, kPdfRangeButtonId, localization::Text(L"readonly.menu.pdf_page_range").c_str());
    AppendMenuW(view, MF_STRING | MF_GRAYED, readonly_viewer::kCmdPdfJumpPage, shortcutText(L"shortcut.action.jump", ShortcutAction::JumpPage).c_str());
    AppendMenuW(view, MF_STRING | MF_GRAYED, readonly_viewer::kCmdPdfFitPage, shortcutText(L"shortcut.action.fit_page", ShortcutAction::FitPage).c_str());
    AppendMenuW(view, MF_STRING | MF_GRAYED, readonly_viewer::kCmdPdfToggleAnnots, shortcutText(L"shortcut.action.annotations", ShortcutAction::ToggleAnnotations).c_str());
    AppendMenuW(view, MF_STRING | MF_GRAYED, readonly_viewer::kCmdPdfToggleGrayscale,
                localization::Text(L"readonly.pdf.menu.grayscale").c_str());
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING | MF_GRAYED, readonly_viewer::kCmdPdfRotateLeft,
                shortcutText(L"readonly.pdf.menu.rotate_left", ShortcutAction::RotateLeft).c_str());
    AppendMenuW(view, MF_STRING | MF_GRAYED, readonly_viewer::kCmdPdfRotateRight,
                shortcutText(L"readonly.pdf.menu.rotate_right", ShortcutAction::RotateRight).c_str());
    AppendMenuW(view, MF_STRING | MF_GRAYED, readonly_viewer::kCmdPdfRotateReset,
                shortcutText(L"readonly.pdf.menu.rotate_reset", ShortcutAction::ResetRotation).c_str());
    AppendMenuW(paths, MF_STRING, kPathMarkTabPersistentMenuId, localization::Text(L"readonly.menu.persist_current_tab").c_str());
    AppendMenuW(paths, MF_STRING, kPathMarkTabTemporaryMenuId, localization::Text(L"readonly.menu.mark_current_tab_temporary").c_str());
    AppendMenuW(paths, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(paths, MF_STRING, kPathMarkFolderPersistentMenuId, localization::Text(L"readonly.menu.persist_current_folder").c_str());
    AppendMenuW(paths, MF_STRING, kPathMarkFolderTemporaryMenuId, localization::Text(L"readonly.menu.mark_current_folder_temporary").c_str());
    AppendMenuW(settings, MF_STRING, kPersistSessionMenuId, localization::Text(L"readonly.menu.restore_previous_session").c_str());
    AppendMenuW(settings, MF_STRING, kShortcutReferenceMenuId, localization::Text(L"shortcut.reference.title").c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), localization::Text(L"readonly.menu.file").c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), localization::Text(L"readonly.menu.view").c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(paths), localization::Text(L"readonly.menu.path_management").c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(settings), localization::Text(L"readonly.menu.settings").c_str());
    AppendMenuW(bar, MF_STRING, kLicensesMenuId, localization::Text(L"reader_next.licenses").c_str());
    SetMenu(hwnd, bar);
}

bool HasActivePdfPreview() {
    return g_hwndPdfPanel && g_currentTab >= 0 &&
        g_currentTab < static_cast<int>(g_tabs.size()) &&
        g_tabs[g_currentTab].viewMode == ViewMode::Pdf &&
        readonly_viewer::PdfPreviewPanel_GetPdfPath(g_hwndPdfPanel) == g_tabs[g_currentTab].path;
}

void UpdatePdfViewMenu(HWND hwnd) {
    HMENU bar = GetMenu(hwnd);
    HMENU view = bar ? GetSubMenu(bar, 1) : nullptr;
    if (!view) return;
    for (UINT command : {readonly_viewer::kCmdPdfRotateLeft, readonly_viewer::kCmdPdfRotateRight,
                         readonly_viewer::kCmdPdfRotateReset, readonly_viewer::kCmdPdfJumpPage, readonly_viewer::kCmdPdfFitPage, readonly_viewer::kCmdPdfToggleAnnots}) {
        const bool enabled = HasActivePdfPreview() && (command != readonly_viewer::kCmdPdfRotateReset ||
            readonly_viewer::PdfPreviewPanel_GetRotation(g_hwndPdfPanel) != 0);
        EnableMenuItem(view, command, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    }
    EnableMenuItem(bar, readonly_viewer::kCmdPdfExportAnnotatedPdf, MF_BYCOMMAND | (HasActivePdfPreview() ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(view, readonly_viewer::kCmdPdfToggleAnnots, MF_BYCOMMAND |
                  (readonly_viewer::PdfPreviewPanel_AreAnnotationsVisible(g_hwndPdfPanel) ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(view, readonly_viewer::kCmdPdfToggleGrayscale,
                   MF_BYCOMMAND | (HasActivePdfPreview() ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(view, readonly_viewer::kCmdPdfToggleGrayscale,
                  MF_BYCOMMAND | (readonly_viewer::PdfPreviewPanel_IsGrayscaleEnabled(g_hwndPdfPanel)
                      ? MF_CHECKED : MF_UNCHECKED));
}

void ParseJapaneseLines(OpenTab& tab) {
    tab.jpLines.clear();
    tab.totalLines = 1;
    if (tab.isBinary || tab.rawText.empty()) return;
    
    int currentLine = 0;
    bool hasJp = false;
    for (size_t i = 0; i < tab.rawText.size(); ++i) {
        wchar_t c = tab.rawText[i];
        if (c == L'\n') {
            if (hasJp) tab.jpLines.push_back(currentLine);
            currentLine++;
            hasJp = false;
        } else if (!hasJp) {
            if (c >= 0x3040 && c <= 0x9FFF) hasJp = true;
        }
    }
    if (hasJp) tab.jpLines.push_back(currentLine);
    tab.totalLines = currentLine + 1;
}

LRESULT CALLBACK HighlightBarProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            
            RECT rc;
            GetClientRect(hWnd, &rc);
            HBRUSH bgBrush = CreateSolidBrush(RGB(245, 245, 245));
            FillRect(hdc, &rc, bgBrush);
            DeleteObject(bgBrush);
            
            if (g_currentTab >= 0 && g_currentTab < (int)g_tabs.size()) {
                const auto& tab = g_tabs[g_currentTab];
                const auto& lines = g_searchOverview.Active() ? g_searchOverview.Lines() : tab.jpLines;
                const int totalLines = g_searchOverview.Active() ? g_searchOverview.TotalLines() : tab.totalLines;
                if (totalLines > 0 && !lines.empty()) {
                    HBRUSH hlBrush = CreateSolidBrush(RGB(255, 165, 0)); // Orange highlight
                    int height = rc.bottom - rc.top;
                    int width = rc.right - rc.left;
                    
                    for (int line : lines) {
                        int y = (int)(((long long)line * height) / totalLines);
                        RECT hRc = {0, y, width, y + 2};
                        FillRect(hdc, &hRc, hlBrush);
                    }
                    DeleteObject(hlBrush);
                }
            }
            
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (g_currentTab >= 0 && g_currentTab < (int)g_tabs.size()) {
                const auto& tab = g_tabs[g_currentTab];
                const int totalLines = g_searchOverview.Active() ? g_searchOverview.TotalLines() : tab.totalLines;
                if (totalLines > 0 && g_hwndEditControl) {
                    RECT rc;
                    GetClientRect(hWnd, &rc);
                    int height = rc.bottom - rc.top;
                    if (height > 0) {
                        int y = GET_Y_LPARAM(lParam);
                        int targetLine = std::clamp(static_cast<int>((static_cast<long long>(y) * totalLines) / height), 0, totalLines - 1);
                        int currentLine = SendMessageW(g_hwndEditControl, EM_GETFIRSTVISIBLELINE, 0, 0);
                        SendMessageW(g_hwndEditControl, EM_LINESCROLL, 0, targetLine - currentLine);
                        UpdateInlineDiagramBounds();
                    }
                }
            }
            return 0;
        }
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

LRESULT CALLBACK TabProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_RBUTTONUP: {
            const POINT client_point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            TCHITTESTINFO hit_test{};
            hit_test.pt = client_point;
            const int index = TabCtrl_HitTest(hWnd, &hit_test);
            if (index < 0 || index >= static_cast<int>(g_tabs.size())) return 0;

            SendMessageW(GetParent(hWnd), kTabContextMenuMessage, kTabMenuSelect, index);
            HMENU menu = CreatePopupMenu();
            if (!menu) return 0;
            AppendMenuW(menu, MF_STRING, kTabMenuClose, localization::Text(L"readonly.tab.close").c_str());
            AppendMenuW(menu, MF_STRING, kTabMenuCloseOthers, localization::Text(L"readonly.tab.close_others").c_str());
            AppendMenuW(menu, MF_STRING, kTabMenuCloseToRight, localization::Text(L"readonly.tab.close_to_right").c_str());
            if (g_tabs[index].isPartialText) AppendMenuW(menu, MF_STRING, kTabMenuChangeRange, localization::Text(L"readonly.tab.change_range").c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, index > 0 ? MF_STRING : MF_STRING | MF_GRAYED,
                        kTabMenuMoveLeft, localization::Text(L"readonly.tab.move_left").c_str());
            AppendMenuW(menu, index + 1 < static_cast<int>(g_tabs.size()) ? MF_STRING : MF_STRING | MF_GRAYED,
                        kTabMenuMoveRight, localization::Text(L"readonly.tab.move_right").c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kTabMenuMoveToNewWindow, localization::Text(L"readonly.tab.move_to_new_window").c_str());
            CollectOtherViewerWindows(GetParent(hWnd));
            HMENU transferMenu = CreatePopupMenu();
            for (size_t targetIndex = 0; targetIndex < g_tabTransferTargets.size(); ++targetIndex) {
                wchar_t title[160]{};
                GetWindowTextW(g_tabTransferTargets[targetIndex], title,
                               static_cast<int>(sizeof(title) / sizeof(title[0])));
                std::wstring label = title[0] ? title : localization::Text(L"readonly.tab.viewer_window");
                label += localization::Text(L"readonly.tab.other_window_suffix");
                AppendMenuW(transferMenu, MF_STRING,
                            kTabMenuMoveToWindowBase + static_cast<UINT>(targetIndex), label.c_str());
            }
            AppendMenuW(menu, MF_POPUP | (g_tabTransferTargets.empty() ? MF_GRAYED : 0),
                        reinterpret_cast<UINT_PTR>(transferMenu), localization::Text(L"readonly.tab.move_to_other_window").c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kTabMenuCloseAll, localization::Text(L"readonly.menu.close_all_tabs").c_str());
            if (!g_closedTabs.empty()) AppendMenuW(menu, MF_STRING, kTabMenuReopenClosed, localization::Text(L"readonly.tab.reopen_closed").c_str());
            HMENU tabList = CreatePopupMenu();
            for (size_t tabIndex = 0; tabIndex < g_tabs.size() && tabIndex < 200; ++tabIndex) {
                AppendMenuW(tabList, MF_STRING | (tabIndex == static_cast<size_t>(index) ? MF_CHECKED : 0),
                            kTabMenuListBase + static_cast<UINT>(tabIndex), g_tabs[tabIndex].title.c_str());
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tabList), localization::Text(L"readonly.tab.list").c_str());

            POINT screen_point = client_point;
            ClientToScreen(hWnd, &screen_point);
            const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                                screen_point.x, screen_point.y, 0, GetParent(hWnd), nullptr);
            DestroyMenu(menu);
            if (command >= kTabMenuListBase && command < kTabMenuListBase + g_tabs.size()) {
                SendMessageW(GetParent(hWnd), kTabContextMenuMessage, kTabMenuSelect, command - kTabMenuListBase);
            } else if (command != 0) SendMessageW(GetParent(hWnd), kTabContextMenuMessage, command, index);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            TCHITTESTINFO hit{}; hit.pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int index = TabCtrl_HitTest(hWnd, &hit);
            g_draggingTab = index; g_tabDragOrigin = hit.pt; g_tabDragActive = false;
            g_tabPressedClose = -1;
            if (index >= 0) {
                RECT rect{}; TabCtrl_GetItemRect(hWnd, index, &rect);
                if (hit.pt.x >= rect.right - 20) g_tabPressedClose = index;
                if (g_tabPressedClose < 0) CallWindowProcW(g_originalTabProc, hWnd, message, wParam, lParam);
                SetCapture(hWnd); return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (g_draggingTab >= 0 && g_tabPressedClose < 0 && GetCapture() == hWnd) {
                const int dx = abs(GET_X_LPARAM(lParam) - g_tabDragOrigin.x);
                const int dy = abs(GET_Y_LPARAM(lParam) - g_tabDragOrigin.y);
                if (dx >= std::max(6, GetSystemMetrics(SM_CXDRAG)) || dy >= std::max(6, GetSystemMetrics(SM_CYDRAG))) {
                    if (!g_tabDragActive) SetViewerStatus(GetParent(hWnd), localization::Text(L"reader_next.drag_hint"));
                    g_tabDragActive = true;
                }
                if (g_tabDragActive) SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP: {
            const int dragged = g_draggingTab, close = g_tabPressedClose;
            const bool active = g_tabDragActive;
            g_draggingTab = g_tabPressedClose = -1; g_tabDragActive = false;
            if (GetCapture() == hWnd) ReleaseCapture();
            TCHITTESTINFO hit{}; hit.pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int index = TabCtrl_HitTest(hWnd, &hit);
            const HWND owner = GetParent(hWnd);
            if (close >= 0 && close == index) {
                RECT rect{}; TabCtrl_GetItemRect(hWnd, index, &rect);
                if (hit.pt.x >= rect.right - 20) SendMessageW(owner, WM_APP + 1, index, 0);
            } else if (active && dragged >= 0) {
                POINT screen = hit.pt; ClientToScreen(hWnd, &screen);
                HWND pointed = WindowFromPoint(screen);
                HWND target = pointed ? GetAncestor(pointed, GA_ROOT) : nullptr;
                if (target == owner) {
                    if (index >= 0 && index != dragged) SendMessageW(owner, WM_APP + 2, dragged, index);
                    SetViewerStatus(owner, L"");
                } else {
                    const bool moved = IsTrustedReadonlyViewerWindow(target)
                        ? MoveTabToWindow(owner, dragged, target) : MoveTabToNewWindow(owner, dragged);
                    SetViewerStatus(owner, localization::Text(moved ? L"reader_next.tab_moved" : L"reader_next.transfer_failed"));
                }
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            g_draggingTab = g_tabPressedClose = -1; g_tabDragActive = false;
            break;
    }
    return CallWindowProcW(g_originalTabProc, hWnd, message, wParam, lParam);
}

struct LoadFileResult {
    std::wstring text;
    std::vector<unsigned char> bytes;
    uintmax_t source_size = 0;
    bool isBinary = false;
    bool isPdf = false;
    bool requires_range_selection = false;
    bool cancelled = false;
    bool hasError = false;
};

constexpr uintmax_t kDirectTextLoadBytes = 50ull * 1024ull * 1024ull;
constexpr uintmax_t kMaximumRangeLoadBytes = 256ull * 1024ull * 1024ull;

std::wstring BuildHexText(const std::vector<unsigned char>& bytes) {
    std::wstringstream wss;
    wss << localization::Text(L"readonly.hex.preview_heading");

    const size_t displaySize = std::min<size_t>(bytes.size(), 65536);
    for (size_t i = 0; i < displaySize; i += 16) {
        wss << std::hex << std::setw(8) << std::setfill(L'0') << i << L"  ";
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < displaySize) {
                wss << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<int>(bytes[i + j]) << L" ";
            } else {
                wss << L"   ";
            }
            if (j == 7) wss << L" ";
        }

        wss << L" |";
        for (size_t j = 0; j < 16 && i + j < displaySize; ++j) {
            const unsigned char c = bytes[i + j];
            wss << ((c >= 32 && c <= 126) ? static_cast<wchar_t>(c) : L'.');
        }
        wss << L"|\n";
    }

    if (bytes.size() > displaySize) {
        wss << std::dec << L"\n... (truncated " << (bytes.size() - displaySize) << L" bytes) ...\n";
    }
    return wss.str();
}


bool IsPdfPath(const std::wstring& path) {
    if (path.length() >= 4) {
        std::wstring ext = path.substr(path.length() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(), std::towlower);
        if (ext == L".pdf") return true;
    }
    return false;
}

bool IsLikelyBinary(const std::vector<unsigned char>& bytes) {
    for (size_t i = 0; i < bytes.size() && i < 8192; ++i) {
        if (bytes[i] == 0) return true;
    }
    return false;
}

bool g_loadInProgress = false;
bool g_loadCancelRequested = false;
enum class ViewerExitRequest {
    None,
    UserClose,
    SystemEndSession,
};
ViewerExitRequest g_viewerExitRequest = ViewerExitRequest::None;
bool g_systemEndSessionSessionSaved = false;

void PumpLoadCancellation() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            g_loadCancelRequested = true;
            PostQuitMessage(static_cast<int>(message.wParam));
            return;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void BeginCancellableLoad(HWND owner, const std::wstring& status) {
    g_loadCancelRequested = false;
    g_loadInProgress = true;
    SetViewerStatus(owner, status);
    UpdateWindow(owner);
}

void EndCancellableLoad() {
    g_loadInProgress = false;
    if (g_viewerExitRequest == ViewerExitRequest::UserClose && g_hwndMain && IsWindow(g_hwndMain)) {
        PostMessageW(g_hwndMain, WM_CLOSE, 0, 0);
    }
}

std::wstring DecodeTextBytes(const std::vector<unsigned char>& bytes) {
    text_encoding::DecodedText decoded;
    std::wstring error;
    text_encoding::ErrorCode encodingCode = text_encoding::ErrorCode::None;
    const std::string_view source(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text_encoding::DecodeExternalText(source, &decoded, &error, &encodingCode)) {
        return decoded.text;
    }
    return localization::Format(L"readonly.error.decoding", {
        {L"DETAIL", localization::Text(text_encoding::ErrorTextId(encodingCode))}});
}

LoadFileResult LoadTextFile(const std::wstring& path, const std::optional<ByteRange>& requested_range = std::nullopt) {
    if (IsPdfPath(path)) {
        LoadFileResult res;
        res.isBinary = true;
        res.isPdf = true;
        res.hasError = false;
        return res;
    }

    LoadFileResult result;
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        result.text = localization::Text(L"readonly.error.file_access_failed");
        result.hasError = true;
        return result;
    }
    result.source_size = size;
    if (!requested_range && size > kDirectTextLoadBytes) {
        result.requires_range_selection = true;
        return result;
    }

    uintmax_t read_start = 0;
    uintmax_t read_end = size;
    if (requested_range) {
        read_start = std::min(requested_range->start, size);
        read_end = std::min(std::max(requested_range->end, read_start), size);
        if (read_end - read_start > kMaximumRangeLoadBytes) {
            result.text = localization::Text(L"readonly.error.range_too_large");
            result.hasError = true;
            return result;
        }
    }

    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) {
        result.text = localization::Text(L"readonly.error.open_failed");
        result.hasError = true;
        return result;
    }

    if (read_start > 0) {
        file.seekg(static_cast<std::streamoff>(read_start));
        uintmax_t skipped = 0;
        char character = 0;
        while (skipped < 64 * 1024 && file.get(character)) {
            ++skipped;
            if (character == '\n') break;
        }
        read_start += skipped; // Begin at the next full line without allocating an arbitrarily long line.
    }
    const uintmax_t requested_bytes = read_end > read_start ? read_end - read_start : 0;
    result.bytes.resize(static_cast<size_t>(requested_bytes));
    size_t total_read = 0;
    while (total_read < result.bytes.size()) {
        if (g_loadInProgress) PumpLoadCancellation();
        if (g_loadCancelRequested) {
            result.bytes.clear();
            result.cancelled = true;
            result.text = localization::Text(L"readonly.status.load_canceled");
            return result;
        }
        const size_t chunk = std::min<size_t>(result.bytes.size() - total_read, 1024 * 1024);
        file.read(reinterpret_cast<char*>(result.bytes.data() + total_read), static_cast<std::streamsize>(chunk));
        const size_t read_count = static_cast<size_t>(file.gcount());
        total_read += read_count;
        if (read_count != chunk) break;
    }
    result.bytes.resize(total_read);
    if (file.bad()) {
        result.bytes.clear();
        result.text = localization::Text(L"readonly.error.read_failed");
        result.hasError = true;
        return result;
    }
    result.isBinary = IsLikelyBinary(result.bytes);
    if (result.isBinary) {
        return result;
    }
    result.text = DecodeTextBytes(result.bytes);
    return result;
}

struct RangeDialogState {
    uintmax_t file_size = 0;
    int start = 0;
    int end = 1;
    bool accepted = false;
    ByteRange selected{};
};

constexpr int kRangeStartSliderId = 601;
constexpr int kRangeEndSliderId = 602;
constexpr int kRangeSummaryId = 603;
constexpr int kRangeWarningId = 604;
constexpr int kRangeConfirmId = 605;
constexpr int kRangeCancelId = 606;
constexpr int kRangeVisualBarId = 607;
constexpr wchar_t kRangeDialogClass[] = L"PdfNoteLargeTextRangeDialog";

void UpdateRangeDialog(HWND hwnd, RangeDialogState& state) {
    state.start = std::clamp(state.start, 0, 999);
    state.end = std::clamp(state.end, state.start + 1, 1000);
    const uintmax_t start = state.file_size * static_cast<uintmax_t>(state.start) / 1000;
    const uintmax_t end = state.file_size * static_cast<uintmax_t>(state.end) / 1000;
    const uintmax_t bytes = end - start;
    const uintmax_t megabytes = (bytes + 1024 * 1024 - 1) / (1024 * 1024);
    const uintmax_t estimated_characters = bytes / 2;
    MEMORYSTATUSEX memory_status{};
    memory_status.dwLength = sizeof(memory_status);
    const uintmax_t available_memory = GlobalMemoryStatusEx(&memory_status) ? memory_status.ullAvailPhys : 0;
    const uintmax_t estimated_working_set = bytes * 3;
    std::wstring range_bar = L"0%  [";
    for (int part = 0; part < 40; ++part) {
        const int position = part * 1000 / 40;
        range_bar += position >= state.start && position < state.end ? L"■" : L"─";
    }
    range_bar += L"]  100%";
    SetWindowTextW(GetDlgItem(hwnd, kRangeVisualBarId), range_bar.c_str());
    std::wstring summary = localization::Format(L"readonly.range.summary", {{L"START", std::to_wstring(state.start / 10)}, {L"END", std::to_wstring(state.end / 10)}, {L"MB", std::to_wstring(megabytes)}, {L"CHARS", std::to_wstring(estimated_characters)}});
    SetWindowTextW(GetDlgItem(hwnd, kRangeSummaryId), summary.c_str());
    std::wstring warning;
    if (bytes > kMaximumRangeLoadBytes) warning = localization::Text(L"readonly.range.too_large");
    else if (available_memory > 0 && estimated_working_set > available_memory / 2)
        warning = localization::Text(L"readonly.range.memory_critical");
    else if (available_memory > 0 && estimated_working_set > available_memory / 4)
        warning = localization::Text(L"readonly.range.memory_low");
    else if (bytes > 128ull * 1024ull * 1024ull) warning = localization::Text(L"readonly.range.memory_heavy");
    else if (bytes > 32ull * 1024ull * 1024ull) warning = localization::Text(L"readonly.range.memory_slow");
    else warning = localization::Text(L"readonly.range.memory_light");
    SetWindowTextW(GetDlgItem(hwnd, kRangeWarningId), warning.c_str());
    EnableWindow(GetDlgItem(hwnd, kRangeConfirmId), bytes <= kMaximumRangeLoadBytes &&
                 (available_memory == 0 || estimated_working_set <= available_memory / 2));
}

LRESULT CALLBACK LargeTextRangeDialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<RangeDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<RangeDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(hwnd, message, wParam, lParam);
    if (message == WM_CREATE) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        CreateWindowExW(0, L"STATIC", localization::Text(L"readonly.range.large_text_help").c_str(),
                        WS_CHILD | WS_VISIBLE, 16, 16, 500, 22, hwnd, nullptr, instance, nullptr);
        CreateWindowExW(0, L"STATIC", localization::Text(L"readonly.range.selection_label").c_str(), WS_CHILD | WS_VISIBLE, 16, 44, 500, 20, hwnd, nullptr, instance, nullptr);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 64, 500, 22, hwnd, reinterpret_cast<HMENU>(kRangeVisualBarId), instance, nullptr);
        CreateWindowExW(0, L"STATIC", localization::Text(L"readonly.range.start_handle").c_str(), WS_CHILD | WS_VISIBLE, 16, 92, 90, 20, hwnd, nullptr, instance, nullptr);
        HWND start = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                     110, 86, 400, 32, hwnd, reinterpret_cast<HMENU>(kRangeStartSliderId), instance, nullptr);
        CreateWindowExW(0, L"STATIC", localization::Text(L"readonly.range.end_handle").c_str(), WS_CHILD | WS_VISIBLE, 16, 132, 90, 20, hwnd, nullptr, instance, nullptr);
        HWND end = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                   110, 126, 400, 32, hwnd, reinterpret_cast<HMENU>(kRangeEndSliderId), instance, nullptr);
        for (HWND slider : {start, end}) { SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000)); SendMessageW(slider, TBM_SETTICFREQ, 100, 0); }
        SendMessageW(start, TBM_SETPOS, TRUE, state->start);
        SendMessageW(end, TBM_SETPOS, TRUE, state->end);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 170, 500, 42, hwnd, reinterpret_cast<HMENU>(kRangeSummaryId), instance, nullptr);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 218, 500, 36, hwnd, reinterpret_cast<HMENU>(kRangeWarningId), instance, nullptr);
        CreateWindowExW(0, L"BUTTON", localization::Text(L"readonly.range.open").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        284, 264, 130, 28, hwnd, reinterpret_cast<HMENU>(kRangeConfirmId), instance, nullptr);
        CreateWindowExW(0, L"BUTTON", localization::Text(L"readonly.common.cancel").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        424, 264, 90, 28, hwnd, reinterpret_cast<HMENU>(kRangeCancelId), instance, nullptr);
        UpdateRangeDialog(hwnd, *state);
        return 0;
    }
    if (message == WM_HSCROLL) {
        const HWND slider = reinterpret_cast<HWND>(lParam);
        if (GetDlgCtrlID(slider) == kRangeStartSliderId) state->start = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        if (GetDlgCtrlID(slider) == kRangeEndSliderId) state->end = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        if (state->start >= state->end) {
            if (GetDlgCtrlID(slider) == kRangeStartSliderId) state->end = std::min(1000, state->start + 1);
            else state->start = std::max(0, state->end - 1);
            SendMessageW(GetDlgItem(hwnd, kRangeStartSliderId), TBM_SETPOS, TRUE, state->start);
            SendMessageW(GetDlgItem(hwnd, kRangeEndSliderId), TBM_SETPOS, TRUE, state->end);
        }
        UpdateRangeDialog(hwnd, *state);
        return 0;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wParam) == kRangeConfirmId) {
            state->selected = {state->file_size * static_cast<uintmax_t>(state->start) / 1000,
                               state->file_size * static_cast<uintmax_t>(state->end) / 1000};
            state->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == kRangeCancelId) { DestroyWindow(hwnd); return 0; }
    }
    if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

std::optional<ByteRange> ChooseLargeTextRange(HWND owner, uintmax_t file_size, const std::optional<ByteRange>& current = std::nullopt) {
    static ATOM atom = 0;
    if (!atom) {
        WNDCLASSW wc{}; wc.lpfnWndProc = LargeTextRangeDialogProc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); wc.lpszClassName = kRangeDialogClass;
        atom = RegisterClassW(&wc);
    }
    if (!atom) return std::nullopt;
    RangeDialogState state{}; state.file_size = file_size;
    state.start = current ? static_cast<int>(current->start * 1000 / file_size) : 0;
    state.end = current ? static_cast<int>(current->end * 1000 / file_size) :
        std::max(1, std::min(1000, static_cast<int>((16ull * 1024ull * 1024ull * 1000ull) / file_size)));
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, kRangeDialogClass, localization::Text(L"readonly.range.title").c_str(),
                                  WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 540, 340,
                                  owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!dialog) return std::nullopt;
    EnableWindow(owner, FALSE);
    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    EnableWindow(owner, TRUE); SetForegroundWindow(owner);
    return state.accepted ? std::optional<ByteRange>(state.selected) : std::nullopt;
}

struct FileChangeDialogState {
    bool reload = false;
};

constexpr int kFileChangeKeepButtonId = 621;
constexpr int kFileChangeReloadButtonId = 622;
constexpr wchar_t kFileChangeDialogClass[] = L"PdfNoteReadonlyFileChangedDialog";

LRESULT CALLBACK FileChangeDialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<FileChangeDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<FileChangeDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(hwnd, message, wParam, lParam);
    if (message == WM_CREATE) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        CreateWindowExW(0, L"STATIC", localization::Text(L"readonly.file_change.detected").c_str(),
                        WS_CHILD | WS_VISIBLE, 18, 18, 470, 22, hwnd, nullptr, instance, nullptr);
        CreateWindowExW(0, L"STATIC", localization::Text(L"readonly.file_change.reload_explanation").c_str(),
                        WS_CHILD | WS_VISIBLE, 18, 48, 470, 40, hwnd, nullptr, instance, nullptr);
        CreateWindowExW(0, L"BUTTON", localization::Text(L"readonly.file_change.keep_current").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        222, 118, 125, 30, hwnd, reinterpret_cast<HMENU>(kFileChangeKeepButtonId), instance, nullptr);
        CreateWindowExW(0, L"BUTTON", localization::Text(L"readonly.file_change.reload").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        357, 118, 115, 30, hwnd, reinterpret_cast<HMENU>(kFileChangeReloadButtonId), instance, nullptr);
        return 0;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wParam) == kFileChangeReloadButtonId) state->reload = true;
        if (LOWORD(wParam) == kFileChangeKeepButtonId || LOWORD(wParam) == kFileChangeReloadButtonId) {
            DestroyWindow(hwnd);
            return 0;
        }
    }
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
    if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool ConfirmReloadChangedFile(HWND owner) {
    static ATOM atom = 0;
    if (!atom) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = FileChangeDialogProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kFileChangeDialogClass;
        atom = RegisterClassW(&wc);
    }
    if (!atom) return false;
    FileChangeDialogState state{};
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, kFileChangeDialogClass, localization::Text(L"readonly.file_change.title").c_str(),
                                  WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 510, 195, owner, nullptr,
                                  GetModuleHandleW(nullptr), &state);
    if (!dialog) return false;
    EnableWindow(owner, FALSE);
    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return state.reload;
}

bool IsMarkdownPath(const std::wstring& path) {
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    if (extension == L".md" || extension == L".markdown" || extension == L".clro") return true;
    std::wstring filename = std::filesystem::path(path).filename().wstring();
    std::transform(filename.begin(), filename.end(), filename.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return filename == L"readme";
}


[[nodiscard]] bool TryBuildRenderableMermaid(const std::vector<InlineDiagram>& diagrams, textviewer::mermaid::DiagramModel* out_model) {
    textviewer::mermaid::DiagramModel combined;
    if (!diagrams.empty()) combined.direction = diagrams.front().model.direction;
    size_t diagram_number = 0;
    for (const auto& diagram : diagrams) {
        const auto& model = diagram.model;
        if (combined.nodes.size() + model.nodes.size() > 256 || combined.edges.size() + model.edges.size() > 512 ||
            combined.groups.size() + model.groups.size() + 1 > 64) return false;
        ++diagram_number;
        const size_t node_base = combined.nodes.size();
        const size_t group_base = combined.groups.size();
        const size_t wrapper_group = combined.groups.size();
        combined.groups.push_back({L"Diagram " + std::to_wstring(diagram_number), textviewer::mermaid::kNoDiagramGroup});
        for (auto group : model.groups) {
            group.parent_group = group.parent_group == textviewer::mermaid::kNoDiagramGroup
                ? wrapper_group : group.parent_group + group_base + 1;
            combined.groups.push_back(std::move(group));
        }
        for (auto node : model.nodes) {
            node.group_index = node.group_index == textviewer::mermaid::kNoDiagramGroup
                ? wrapper_group : node.group_index + group_base + 1;
            node.id = L"d" + std::to_wstring(diagram_number) + L"_" + node.id;
            combined.nodes.push_back(std::move(node));
        }
        for (auto edge : model.edges) {
            edge.from_node += node_base;
            edge.to_node += node_base;
            combined.edges.push_back(std::move(edge));
        }
    }
    if (combined.nodes.empty()) return false;
    *out_model = std::move(combined);
    return true;
}

LRESULT CALLBACK DetachedDiagramWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            const HINSTANCE instance = reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance;
            (void)ui::ApplyAuxiliaryWindowCorners(hWnd);
            CreateWindowExW(0, L"BUTTON", localization::Text(L"readonly.diagram.zoom_in").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                            8, 6, 64, 24, hWnd, reinterpret_cast<HMENU>(kDetachedDiagramZoomInButtonId), instance, nullptr);
            CreateWindowExW(0, L"BUTTON", localization::Text(L"readonly.diagram.zoom_out").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                            78, 6, 64, 24, hWnd, reinterpret_cast<HMENU>(kDetachedDiagramZoomOutButtonId), instance, nullptr);
            CreateWindowExW(0, L"BUTTON", localization::Text(L"readonly.diagram.zoom_reset").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                            148, 6, 64, 24, hWnd, reinterpret_cast<HMENU>(kDetachedDiagramResetButtonId), instance, nullptr);
            CreateWindowExW(0, L"BUTTON", localization::Text(L"readonly.diagram.fit").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                            218, 6, 96, 24, hWnd, reinterpret_cast<HMENU>(kDetachedDiagramFitButtonId), instance, nullptr);
            if (!g_detachedMermaidPreview.Create(hWnd, instance, 204)) return -1;
            g_detachedMermaidPreview.SetVisible(true);
            return 0;
        }
        case WM_SIZE: {
            const int width = LOWORD(lParam);
            const int height = HIWORD(lParam);
            g_detachedMermaidPreview.SetBounds(6, 36, std::max(0, width - 12), std::max(0, height - 42));
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kDetachedDiagramZoomInButtonId:
                    g_detachedMermaidPreview.ZoomIn();
                    return 0;
                case kDetachedDiagramZoomOutButtonId:
                    g_detachedMermaidPreview.ZoomOut();
                    return 0;
                case kDetachedDiagramResetButtonId:
                    g_detachedMermaidPreview.ResetView();
                    return 0;
                case kDetachedDiagramFitButtonId:
                    g_detachedMermaidPreview.FitToWindow();
                    return 0;
            }
            break;
        case WM_CLOSE:
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            g_hwndDetachedDiagram = nullptr;
            return 0;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

void OpenDetachedDiagramWindow(HWND source) {
    if (g_currentTab < 0 || g_currentTab >= static_cast<int>(g_tabs.size())) return;
    const auto& tab = g_tabs[g_currentTab];
    const textviewer::mermaid::DiagramModel* model = nullptr;
    if (source == g_mermaidPreview.GetWindow() && tab.combinedDiagram) model = &*tab.combinedDiagram;
    for (const auto& diagram : g_inlineDiagramPreviews) {
        if (diagram.preview->GetWindow() == source && diagram.diagram_index < tab.diagrams.size())
            model = &tab.diagrams[diagram.diagram_index].model;
    }
    if (!model) return;

    if (!g_hwndDetachedDiagram) {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = DetachedDiagramWndProc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = kDetachedDiagramWindowClass;
        RegisterClassW(&window_class);
        g_hwndDetachedDiagram = CreateWindowExW(WS_EX_TOOLWINDOW, kDetachedDiagramWindowClass, localization::Text(L"readonly.diagram.title").c_str(),
                                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 700,
                                                 g_hwndMain, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!g_hwndDetachedDiagram) return;
    }
    g_detachedMermaidPreview.SetModel(*model);
    g_detachedMermaidPreview.SetVisible(true);
    ShowWindow(g_hwndDetachedDiagram, SW_SHOWNORMAL);
    SetForegroundWindow(g_hwndDetachedDiagram);
}

bool IsClropPath(const std::wstring& path) {
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return extension == L".clrop";
}

void AppendDecoratedLine(RenderedDocument& document,
                         std::wstring_view text,
                         unsigned int style = TextStyleNone,
                         int headingLevel = 0) {
    AppendStyledText(document, text, style, headingLevel);
    document.text.push_back(L'\n');
}

std::wstring ClropItemKindLabel(clrop::Item::Kind kind) {
    switch (kind) {
        case clrop::Item::Kind::Text: return localization::Text(L"readonly.clrop.kind.text");
        case clrop::Item::Kind::Math: return localization::Text(L"readonly.clrop.kind.math");
        case clrop::Item::Kind::MarkerText: return localization::Text(L"readonly.clrop.kind.text_marker");
        case clrop::Item::Kind::TextColor: return localization::Text(L"readonly.clrop.kind.text_color");
        case clrop::Item::Kind::MarkerFree: return localization::Text(L"readonly.clrop.kind.free_marker");
        case clrop::Item::Kind::Line: return localization::Text(L"readonly.clrop.kind.line");
        case clrop::Item::Kind::Arrow: return localization::Text(L"readonly.clrop.kind.arrow");
        case clrop::Item::Kind::Wave: return localization::Text(L"readonly.clrop.kind.wave");
        case clrop::Item::Kind::Freehand: return localization::Text(L"readonly.clrop.kind.freehand");
        case clrop::Item::Kind::LinkMarker: return localization::Text(L"readonly.clrop.kind.link_marker");
        case clrop::Item::Kind::Shape: return localization::Text(L"readonly.clrop.kind.shape");
    }
    return localization::Text(L"readonly.clrop.kind.annotation");
}

std::wstring ClropItemPreview(const clrop::Item& item) {
    std::wstring preview = item.content;
    if (preview.empty() && !item.lines.empty()) {
        for (size_t i = 0; i < item.lines.size(); ++i) {
            if (i != 0) preview += L" / ";
            preview += item.lines[i];
        }
    }
    for (wchar_t& c : preview) {
        if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    }
    constexpr size_t kPreviewLimit = 160;
    if (preview.size() > kPreviewLimit) {
        preview.resize(kPreviewLimit);
        preview += L"…";
    }
    return preview;
}

RenderedDocument RenderClropDocument(const OpenTab& tab) {
    RenderedDocument document;
    if (tab.bytes.empty()) {
        AppendDecoratedLine(document, localization::Text(L"readonly.clrop.read_failed"), TextStyleBold, 1);
        AppendDecoratedLine(document, localization::Text(L"readonly.clrop.use_raw_view"));
        return document;
    }

    const std::string json(tab.bytes.begin(), tab.bytes.end());
    clrop::Document clropDocument;
    std::wstring error;
    if (!clrop::ParseClropFromJson(json, clropDocument, error)) {
        AppendDecoratedLine(document, localization::Text(L"readonly.clrop.parse_failed"), TextStyleBold, 1);
        AppendDecoratedLine(document, localization::Text(L"readonly.clrop.use_raw_view"));
        if (!error.empty()) AppendDecoratedLine(document, error, TextStyleCode);
        return document;
    }

    size_t itemCount = 0;
    std::map<std::wstring, size_t> kindCounts;
    for (const clrop::Page& page : clropDocument.pages) {
        itemCount += page.items.size();
        for (const clrop::Item& item : page.items) ++kindCounts[ClropItemKindLabel(item.kind)];
    }

    AppendDecoratedLine(document, localization::Text(L"readonly.clrop.title"), TextStyleBold, 1);
    AppendDecoratedLine(document, localization::Text(L"readonly.clrop.format_version") + std::to_wstring(clropDocument.version));
    if (!clropDocument.pdfId.path.empty()) {
        AppendDecoratedLine(document, localization::Text(L"readonly.clrop.target_pdf") + clropDocument.pdfId.path);
    }
    if (clropDocument.pdfId.pageCount > 0) {
        AppendDecoratedLine(document, localization::Text(L"readonly.clrop.pdf_page_count") + std::to_wstring(clropDocument.pdfId.pageCount));
    }
    AppendDecoratedLine(document, localization::Format(L"readonly.clrop.page_and_annotation_count", {{L"PAGES", std::to_wstring(clropDocument.pages.size())}, {L"ITEMS", std::to_wstring(itemCount)}}));

    if (!kindCounts.empty()) {
    AppendDecoratedLine(document, localization::Text(L"readonly.clrop.annotation_types"), TextStyleBold, 2);
        for (const auto& [kind, count] : kindCounts) {
            AppendDecoratedLine(document, L"• " + kind + L": " + std::to_wstring(count));
        }
    }

    AppendDecoratedLine(document, localization::Text(L"readonly.clrop.annotations_by_page"), TextStyleBold, 2);
    for (const clrop::Page& page : clropDocument.pages) {
        AppendDecoratedLine(document,
            localization::Format(L"readonly.clrop.page_item_count", {{L"PAGE", std::to_wstring(page.page + 1)}, {L"COUNT", std::to_wstring(page.items.size())}}),
            TextStyleBold,
            3);
        for (const clrop::Item& item : page.items) {
            std::wstring line = L"• " + ClropItemKindLabel(item.kind);
            const std::wstring preview = ClropItemPreview(item);
            if (!preview.empty()) line += L": " + preview;
            AppendDecoratedLine(document, line);
        }
    }
    return document;
}

[[nodiscard]] LONG FindPublishedHeading(const ReaderHeading& heading, LONG* nextOffset) {
    if (heading.title.empty()) return -1;
    FINDTEXTEXW found{}; found.chrg = {*nextOffset, -1};
    found.lpstrText = const_cast<LPWSTR>(heading.title.c_str());
    while (SendMessageW(g_hwndEditControl, EM_FINDTEXTEXW, FR_DOWN | FR_MATCHCASE,
                        reinterpret_cast<LPARAM>(&found)) >= 0) {
        const CHARRANGE character{found.chrgText.cpMin, found.chrgText.cpMin + 1};
        SendMessageW(g_hwndEditControl, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&character));
        CHARFORMAT2W format{}; format.cbSize = sizeof(format);
        SendMessageW(g_hwndEditControl, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
        if ((format.dwMask & CFM_SIZE) && format.yHeight == ReaderHeadingFontHalfPoints(heading.level) * 10) {
            *nextOffset = found.chrgText.cpMax;
            return found.chrgText.cpMin;
        }
        found.chrg.cpMin = found.chrgText.cpMax;
    }
    return -1;
}

// The UI thread owns this nesting: helpers must not resume painting halfway
// through publishing a tab. WM_SETREDRAW also changes WS_VISIBLE, so preserve
// visibility on helper exits and explicitly select it at the publication end.
unsigned int g_readerRedrawDepth = 0;
class ReaderRedrawScope {
public:
    explicit ReaderRedrawScope(HWND hwnd) : hwnd_(hwnd), visible_(hwnd && IsWindowVisible(hwnd)) {
        if (hwnd_ && g_readerRedrawDepth++ == 0) SendMessageW(hwnd_, WM_SETREDRAW, FALSE, 0);
    }
    ReaderRedrawScope(const ReaderRedrawScope&) = delete;
    ReaderRedrawScope& operator=(const ReaderRedrawScope&) = delete;
    ~ReaderRedrawScope() { Resume(visible_); }
    void Resume(bool visible) {
        if (!hwnd_) return;
        if (--g_readerRedrawDepth == 0) {
            SendMessageW(hwnd_, WM_SETREDRAW, TRUE, 0);
            ShowWindow(hwnd_, visible ? SW_SHOWNA : SW_HIDE);
            RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        }
        hwnd_ = nullptr;
    }
private:
    HWND hwnd_;
    bool visible_;
};

bool g_updatingToc = false;
void UpdateTOC(const RenderedDocument& document, const std::wstring* rawSource) {
    g_updatingToc = true;
    SendMessageW(g_hwndTocTree, WM_SETREDRAW, FALSE, 0);
    CHARRANGE originalSelection{};
    POINT originalScroll{};
    ReaderRedrawScope editRedraw(g_isRichEdit && !rawSource ? g_hwndEditControl : nullptr);
    if (g_isRichEdit && !rawSource) {
        SendMessageW(g_hwndEditControl, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&originalSelection));
        SendMessageW(g_hwndEditControl, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&originalScroll));
    }
    TreeView_DeleteAllItems(g_hwndTocTree);
    HTREEITEM parents[7]{}; std::fill(std::begin(parents), std::end(parents), TVI_ROOT);
    LONG nextOffset = 0, sourceLine = 0;
    size_t sourceCursor = 0;
    for (const auto& heading : document.headings) {
        TVINSERTSTRUCTW item{};
        const int level = std::clamp(heading.level, 1, 6);
        item.hParent = parents[level - 1]; item.hInsertAfter = TVI_LAST;
        item.item.mask = TVIF_TEXT | TVIF_PARAM;
        item.item.pszText = const_cast<LPWSTR>(heading.title.c_str());
        // RTF tables have native cell delimiters: resolve the prepared headings
        // in forward order, rather than reparsing the source after publication.
        const LONG offset = g_isRichEdit && !rawSource ? FindPublishedHeading(heading, &nextOffset) : -1;
        if (rawSource) {
            // RichEdit collapses CRLF into one native character. Resolve the
            // prepared source line in its coordinate system without reparsing.
            const size_t limit = std::min(heading.sourceOffset, rawSource->size());
            while (sourceCursor < limit) if ((*rawSource)[sourceCursor++] == L'\n') ++sourceLine;
            item.item.lParam = SendMessageW(g_hwndEditControl, EM_LINEINDEX, sourceLine, 0);
        } else item.item.lParam = offset;
        HTREEITEM inserted = TreeView_InsertItem(g_hwndTocTree, &item);
        if (item.hParent != TVI_ROOT) TreeView_Expand(g_hwndTocTree, item.hParent, TVE_EXPAND);
        for (int i = level; i < 7; ++i) parents[i] = inserted;
    }
    if (g_isRichEdit && !rawSource) {
        SendMessageW(g_hwndEditControl, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&originalSelection));
        SendMessageW(g_hwndEditControl, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&originalScroll));
    }
    SendMessageW(g_hwndTocTree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hwndTocTree, nullptr, FALSE);
    g_updatingToc = false;
}

struct ReaderRtfStream { const std::string* bytes; size_t offset = 0; };
DWORD CALLBACK ReadReaderRtf(DWORD_PTR cookie, LPBYTE buffer, LONG count, LONG* copied) {
    auto& stream = *reinterpret_cast<ReaderRtfStream*>(cookie);
    const size_t size = std::min(static_cast<size_t>(std::max(0L, count)), stream.bytes->size() - stream.offset);
    memcpy(buffer, stream.bytes->data() + stream.offset, size);
    stream.offset += size; *copied = static_cast<LONG>(size); return 0;
}
[[nodiscard]] bool ApplyReadingDocument(const RenderedDocument& document) {
    RECT rect{}; SendMessageW(g_hwndEditControl, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&rect));
    HDC dc = GetDC(g_hwndEditControl);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(g_hwndEditControl, dc);
    const std::string rtf = BuildReaderRtf(document, MulDiv(std::max(160L, rect.right - rect.left - 8), 1440, dpi));
    ReaderRtfStream data{&rtf}; EDITSTREAM stream{};
    stream.dwCookie = reinterpret_cast<DWORD_PTR>(&data); stream.pfnCallback = ReadReaderRtf;
    ReaderRedrawScope editRedraw(g_hwndEditControl);
    SendMessageW(g_hwndEditControl, EM_SETBKGNDCOLOR, 0, reader_next::kPaper);
    SendMessageW(g_hwndEditControl, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&stream));
    SendMessageW(g_hwndEditControl, EM_SETSEL, 0, 0);
    SendMessageW(g_hwndEditControl, EM_SCROLLCARET, 0, 0);
    return stream.dwError == 0 && data.offset == rtf.size();
}

void PrepareReading(OpenTab& tab) {
    if (tab.reading || tab.isBinary || tab.isPdf || IsClropPath(tab.path)) return;
    tab.reading = std::make_shared<RenderedDocument>(RenderMarkdown(tab.rawText, g_isRichEdit ? &tab.diagrams : nullptr));
    if (!tab.diagrams.empty()) {
        textviewer::mermaid::DiagramModel model;
        if (TryBuildRenderableMermaid(tab.diagrams, &model)) tab.combinedDiagram = std::move(model);
    }
    const auto count = reinterpret_cast<UINT_PTR>(GetPropW(g_hwndMain, L"ReaderNextPreparationCount"));
    SetPropW(g_hwndMain, L"ReaderNextPreparationCount", reinterpret_cast<HANDLE>(count + 1));
}

void ApplyEditorText(const std::wstring& text, const std::vector<StyleRun>& styles, bool monospace) {
    if (!g_hwndEditControl) return;

    ReaderRedrawScope editRedraw(g_hwndEditControl);
    SetWindowTextW(g_hwndEditControl, text.c_str());
    if (g_isRichEdit) {
        SendMessageW(g_hwndEditControl, EM_SETBKGNDCOLOR, 0, reader_next::kPaper);
        SendMessageW(g_hwndEditControl, EM_SETSEL, 0, -1);

        CHARFORMAT2W base{};
        base.cbSize = sizeof(base);
        base.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT | CFM_BACKCOLOR;
        base.dwEffects = CFE_AUTOBACKCOLOR;
        base.yHeight = monospace ? 230 : 250;
        base.crTextColor = RGB(35, 39, 47);
        base.crBackColor = reader_next::kPaper;
        wcsncpy(base.szFaceName, monospace ? L"Consolas" : L"Yu Gothic UI", LF_FACESIZE - 1);
        base.szFaceName[LF_FACESIZE - 1] = L'\0';
        SendMessageW(g_hwndEditControl, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&base));
        PARAFORMAT2 paragraph{};
        paragraph.cbSize = sizeof(paragraph);
        paragraph.dwMask = PFM_LINESPACING | PFM_SPACEAFTER;
        paragraph.bLineSpacingRule = 5; paragraph.dyLineSpacing = 24;
        paragraph.dySpaceAfter = monospace ? 0 : 60;
        SendMessageW(g_hwndEditControl, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&paragraph));

        for (const StyleRun& run : styles) {
            if (run.start >= run.end || run.end > text.size()) continue;
            SendMessageW(g_hwndEditControl, EM_SETSEL, static_cast<WPARAM>(run.start), static_cast<LPARAM>(run.end));
            CHARFORMAT2W format{};
            format.cbSize = sizeof(format);
            format.dwMask = CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT | CFM_COLOR | CFM_SIZE;
            format.dwEffects = 0;
            format.crTextColor = RGB(35, 39, 47);
            format.yHeight = 250;
            if ((run.style & TextStyleBold) != 0) format.dwEffects |= CFE_BOLD;
            if ((run.style & TextStyleItalic) != 0) format.dwEffects |= CFE_ITALIC;
            if ((run.style & TextStyleStrike) != 0) format.dwEffects |= CFE_STRIKEOUT;
            if ((run.style & TextStyleLink) != 0) format.crTextColor = RGB(31, 93, 166);
            if (run.headingLevel > 0) {
                static const LONG headingSizes[] = {0, 420, 350, 300, 260, 240, 220};
                format.yHeight = headingSizes[std::min(run.headingLevel, 6)];
                format.crTextColor = RGB(24, 55, 89);
            }
            if ((run.style & TextStyleCode) != 0) {
                format.dwMask |= CFM_FACE | CFM_BACKCOLOR;
                format.crBackColor = RGB(242, 244, 247);
                wcsncpy(format.szFaceName, L"Consolas", LF_FACESIZE - 1);
                format.szFaceName[LF_FACESIZE - 1] = L'\0';
            }
            SendMessageW(g_hwndEditControl, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
        }

        SendMessageW(g_hwndEditControl, EM_SETSEL, 0, 0);
        SendMessageW(g_hwndEditControl, EM_SCROLLCARET, 0, 0);
    }
}

void ClearInlineDiagramPreviews() {
    for (InlineDiagramPreview& inline_diagram : g_inlineDiagramPreviews) {
        inline_diagram.preview->SetVisible(false);
    }
    g_inlineDiagramPreviews.clear();
}

void UpdateInlineDiagramBounds() {
    if (!g_hwndEditControl) return;
    RECT rect{}; GetClientRect(g_hwndEditControl, &rect);
    RECT format{}; SendMessageW(g_hwndEditControl, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&format));
    for (auto& diagram : g_inlineDiagramPreviews) {
        POINTL start{}, next{}, labelBottom{};
        // RichEdit 4.1 uses a POINTL output pointer, unlike the packed EDIT API.
        SendMessageW(g_hwndEditControl, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&start), diagram.text_offset);
        SendMessageW(g_hwndEditControl, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&next), diagram.text_offset + diagram.reserved_length);
        SendMessageW(g_hwndEditControl, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&labelBottom),
            diagram.text_offset + diagram.reserved_length - 15);
        const int top = labelBottom.y + reader_next::Px(g_hwndMain, 4);
        const int height = std::max(0, static_cast<int>(next.y) - top - reader_next::Px(g_hwndMain, 8));
        diagram.preview->SetBounds(format.left, top, std::max(0L, format.right - format.left), height);
        diagram.preview->SetVisible(height > 0 && top < rect.bottom && top + height > 0);
    }
}

void BuildInlineDiagramPreviews(const OpenTab& tab) {
    ClearInlineDiagramPreviews();
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    size_t diagramNumber = 0;
    for (const InlineDiagram& diagram : tab.diagrams) {
        ++diagramNumber;
        auto preview = std::make_unique<textviewer::mermaid::MermaidSubsetPreview>();
        if (!preview->Create(g_hwndEditControl, instance, 0)) continue;
        preview->SetModel(diagram.model);
        if (diagramNumber <= tab.inlineDiagramPositions.size())
            preview->RestoreViewState(tab.inlineDiagramPositions[diagramNumber - 1]);
        preview->SetDetachedWindowTarget(g_hwndMain);
        preview->SetDocumentScrollTarget(g_hwndEditControl);
        FINDTEXTEXW location{}; location.chrg = {0, -1};
        auto label = localization::Format(L"readonly.render.diagram_label", {{L"NUMBER", std::to_wstring(diagramNumber)}});
        location.lpstrText = label.data();
        const LRESULT offset = SendMessageW(g_hwndEditControl, EM_FINDTEXTEXW, FR_DOWN, reinterpret_cast<LPARAM>(&location));
        if (offset < 0) continue;
        g_inlineDiagramPreviews.push_back({static_cast<size_t>(location.chrgText.cpMin),
            static_cast<size_t>(label.size() + 16), diagramNumber - 1, std::move(preview)});
    }
    UpdateInlineDiagramBounds();
}

LRESULT CALLBACK InlineDiagramEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    // A read-only control must never delegate attempted edits to the native beep path.
    if (message == WM_CHAR && wParam != 1 && wParam != 3) return 0;
    if (message == WM_PASTE || message == WM_CUT || message == WM_CLEAR ||
        (message == WM_KEYDOWN && (wParam == VK_BACK || wParam == VK_DELETE))) return 0;
    const LRESULT result = CallWindowProcW(g_originalEditProc, hwnd, message, wParam, lParam);
    if (message == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL))
        RefreshSearchOverview();
    switch (message) {
        case WM_VSCROLL:
        case WM_MOUSEWHEEL:
        case WM_SIZE:
        case WM_KEYDOWN:
            UpdateInlineDiagramBounds();
            break;
    }
    return result;
}

void UpdateModeButtons(ViewMode mode) {
    const bool hasTab = g_currentTab >= 0 && g_currentTab < static_cast<int>(g_tabs.size());
    const bool pdf = hasTab && g_tabs[g_currentTab].isPdf;
    const bool binary = hasTab && g_tabs[g_currentTab].isBinary;
    const int ids[] = {kDecoratedButtonId, kRawButtonId, kHexButtonId, kDiagramButtonId};
    const ViewMode modes[] = {ViewMode::Decorated, ViewMode::Raw, ViewMode::Hex, ViewMode::Diagram};
    const bool diagram = hasTab && g_tabs[g_currentTab].combinedDiagram.has_value();
    for (int i = 0; i < 4; ++i) {
        HWND button = GetDlgItem(g_hwndMain, ids[i]);
        ShowWindow(button, pdf ? SW_HIDE : SW_SHOW);
        EnableWindow(button, hasTab && (i == 2 || (!binary && (i != 3 || diagram))));
        reader_next::SetActive(button, hasTab && mode == modes[i]);
    }
    ShowWindow(GetDlgItem(g_hwndMain, kPdfRangeButtonId), pdf ? SW_SHOW : SW_HIDE);
    EnableWindow(GetDlgItem(g_hwndMain, reader_next::kSearchId), hasTab && mode != ViewMode::Diagram);
    for (UINT id : {reader_next::kZoomInId, reader_next::kZoomOutId, reader_next::kZoomResetId})
        EnableWindow(GetDlgItem(g_hwndMain, id), hasTab && mode != ViewMode::Diagram);
    InvalidateRect(g_hwndMain, nullptr, FALSE);
}

void CancelReaderSearch();
void ContinuePdfSearch();
void FindReaderText(bool previous);
void ZoomReader(UINT command);
[[nodiscard]] bool CreateReaderControls(HWND owner);
[[nodiscard]] bool HandleReaderKey(const MSG& message);
unsigned int g_searchGeneration = 0;
std::wstring g_displayedPath;
ViewMode g_displayedMode = ViewMode::Raw;
const RenderedDocument* g_displayedReading = nullptr;
int g_displayedReadingWidth = 0;
bool g_reflowQueued = false;
bool g_publishingReaderView = false;
void InvalidateReaderPreparation(OpenTab& tab) {
    tab.reading.reset(); tab.diagrams.clear(); tab.combinedDiagram.reset();
    for (auto& position : tab.readingPositions) position = {};
    tab.pdfPosition = {}; tab.diagramPosition = {}; tab.inlineDiagramPositions.clear();
    if (g_displayedPath == tab.path) { g_displayedPath.clear(); g_displayedReading = nullptr; }
}
void RememberReaderView() {
    for (auto& tab : g_tabs) if (tab.path == g_displayedPath) {
        if (g_displayedMode == ViewMode::Pdf) {
            tab.pdfPosition = reader_next::CapturePdfView(g_hwndPdfPanel, tab.path); break;
        }
        if (g_displayedMode == ViewMode::Diagram) {
            tab.diagramPosition = g_mermaidPreview.GetViewState(); break;
        }
        if (!g_isRichEdit || !IsWindowVisible(g_hwndEditControl)) break;
        auto& position = tab.readingPositions[static_cast<int>(g_displayedMode)];
        SendMessageW(g_hwndEditControl, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&position.scroll));
        SendMessageW(g_hwndEditControl, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&position.selection));
        position.anchor = static_cast<LONG>(SendMessageW(g_hwndEditControl, EM_LINEINDEX,
            SendMessageW(g_hwndEditControl, EM_GETFIRSTVISIBLELINE, 0, 0), 0));
        POINTL anchor{};
        SendMessageW(g_hwndEditControl, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&anchor), position.anchor);
        position.anchor_y = anchor.y;
        if (g_displayedMode == ViewMode::Decorated) {
            tab.inlineDiagramPositions.resize(tab.diagrams.size());
            for (const auto& diagram : g_inlineDiagramPreviews)
                if (diagram.diagram_index < tab.inlineDiagramPositions.size())
                    tab.inlineDiagramPositions[diagram.diagram_index] = diagram.preview->GetViewState();
        }
        position.captured = true; break;
    }
}
void RestoreReaderPosition(const ReaderViewPosition& position) {
    const CHARRANGE selection = position.captured ? position.selection : CHARRANGE{};
    POINT scroll = position.captured ? position.scroll : POINT{};
    SendMessageW(g_hwndEditControl, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    SendMessageW(g_hwndEditControl, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    if (position.captured && position.anchor >= 0) {
        POINTL anchor{};
        SendMessageW(g_hwndEditControl, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&anchor), position.anchor);
        scroll.y = std::max(0L, scroll.y + anchor.y - position.anchor_y);
        SendMessageW(g_hwndEditControl, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    }
    UpdateInlineDiagramBounds();
    // Native scrolling can reuse stale pixels from the preceding document.
    // Publish a complete restored viewport without waiting for a wheel event.
    RedrawWindow(g_hwndEditControl, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}
void ShowCurrentTab(bool rememberView = true) {
    if (rememberView) RememberReaderView();
    struct PublicationGuard {
        bool previous;
        PublicationGuard() : previous(g_publishingReaderView) { g_publishingReaderView = true; }
        ~PublicationGuard() { g_publishingReaderView = previous; }
    } publication;
    CancelReaderSearch();
    KillTimer(g_hwndMain, reader_next::kSearchOverviewTimer);
    g_searchOverview.Clear();
    if (g_currentTab < 0 || g_currentTab >= static_cast<int>(g_tabs.size())) {
        ClearInlineDiagramPreviews();
        g_mermaidPreview.SetVisible(false);
        if (g_hwndPdfPanel) ShowWindow(g_hwndPdfPanel, SW_HIDE);
        ApplyEditorText(L"", {}, false);
        if (g_hwndEditControl) ShowWindow(g_hwndEditControl, SW_HIDE);
        if (g_hwndHighlightBar) ShowWindow(g_hwndHighlightBar, SW_HIDE);
        UpdateModeButtons(ViewMode::Raw);
        TreeView_DeleteAllItems(g_hwndTocTree);
        g_displayedPath.clear(); g_displayedReading = nullptr;
        UpdateReaderWindowTitle(g_hwndMain);
        RelayoutViewer();
        RefreshSearchOverview();
        return;
    }

    ReaderRedrawScope editRedraw(g_hwndEditControl);
    OpenTab& tab = g_tabs[g_currentTab];
    bool newlyOpenedPdf = false;
    PrepareReading(tab);
    SendMessageW(g_hwndEditControl, EM_SETTARGETDEVICE, 0, tab.viewMode == ViewMode::Decorated ? 0 : 1);
    const bool has_renderable_mermaid = tab.combinedDiagram.has_value();
    RelayoutViewer();
    RECT viewport{}; SendMessageW(g_hwndEditControl, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&viewport));
    const int readingWidth = viewport.right - viewport.left;
    const bool reuse = tab.path == g_displayedPath && tab.viewMode == g_displayedMode &&
        tab.viewMode == ViewMode::Decorated && tab.reading && tab.reading.get() == g_displayedReading &&
        (tab.reading->tables.empty() || readingWidth == g_displayedReadingWidth);
    const bool showDiagram = (tab.viewMode == ViewMode::Diagram && has_renderable_mermaid);
    const bool showPdf = (tab.viewMode == ViewMode::Pdf);
    
    g_mermaidPreview.SetVisible(showDiagram);
    if (g_hwndPdfPanel) {
        ShowWindow(g_hwndPdfPanel, showPdf ? SW_SHOW : SW_HIDE);
    }
    
    if (showDiagram || showPdf) {
        if (g_hwndEditControl) ShowWindow(g_hwndEditControl, SW_HIDE);
        if (g_hwndHighlightBar) ShowWindow(g_hwndHighlightBar, SW_HIDE);
    } else {
        if (g_hwndHighlightBar) ShowWindow(g_hwndHighlightBar, SW_SHOW);
    }
    if (tab.viewMode != ViewMode::Decorated || IsClropPath(tab.path)) {
        ClearInlineDiagramPreviews();
    }
    switch (tab.viewMode) {
        case ViewMode::Decorated: {
            if (IsClropPath(tab.path)) {
                const RenderedDocument rendered = RenderClropDocument(tab);
                ApplyEditorText(rendered.text, rendered.styles, false);
            } else if (tab.reading && !reuse) {
                if (!g_isRichEdit) ApplyEditorText(tab.reading->text, {}, false);
                else if (!ApplyReadingDocument(*tab.reading))
                    SetViewerStatus(g_hwndMain, localization::Text(L"reader_next.render_failed"));
                BuildInlineDiagramPreviews(tab);
            }
            break;
        }
        case ViewMode::Raw:
            if (tab.isBinary && tab.rawText.empty()) {
                ApplyEditorText(localization::Text(L"readonly.binary.use_hex"), {}, true);
            } else {
                ApplyEditorText(tab.rawText, {}, true);
            }
            break;
        case ViewMode::Hex:
            ApplyEditorText(BuildHexText(tab.bytes), {}, true);
            break;
        case ViewMode::Diagram:
            if (has_renderable_mermaid) {
                g_mermaidPreview.SetModel(*tab.combinedDiagram);
                g_mermaidPreview.RestoreViewState(tab.diagramPosition);
                g_mermaidPreview.SetVisible(true);
            } else {
                tab.viewMode = ViewMode::Decorated;
                const RenderedDocument rendered = RenderMarkdown(tab.rawText);
                ApplyEditorText(rendered.text, rendered.styles, false);
            }
            break;
        case ViewMode::Pdf:
            if (g_hwndPdfPanel) {
                if (readonly_viewer::PdfPreviewPanel_GetPdfPath(g_hwndPdfPanel) != tab.path) {
                    readonly_viewer::PdfPreviewPanel_OpenPdf(g_hwndPdfPanel, tab.path, L"");
                    newlyOpenedPdf = true;
                }
            }
            break;
    }

    UpdateModeButtons(tab.viewMode);
    if (!reuse) {
        if (tab.reading && (tab.viewMode == ViewMode::Decorated || tab.viewMode == ViewMode::Raw))
            UpdateTOC(*tab.reading, tab.viewMode == ViewMode::Raw ? &tab.rawText : nullptr);
        else TreeView_DeleteAllItems(g_hwndTocTree);
    }
    g_displayedPath = tab.path; g_displayedMode = tab.viewMode;
    g_displayedReading = tab.reading.get(); g_displayedReadingWidth = readingWidth;
    SetPropW(g_hwndMain, L"ReaderNextReadingWidth", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(readingWidth)));
    UpdateReaderWindowTitle(g_hwndMain);
    // Reflect the active document when switching tabs without opening it again.
    HTREEITEM root = TreeView_GetRoot(g_hwndFileTree);
    for (HTREEITEM item = TreeView_GetChild(g_hwndFileTree, root); item; item = TreeView_GetNextSibling(g_hwndFileTree, item)) {
        TVITEMW data{}; data.mask = TVIF_PARAM; data.hItem = item;
        if (TreeView_GetItem(g_hwndFileTree, &data) && data.lParam >= 0 && data.lParam < static_cast<LPARAM>(g_filePaths.size()) &&
            _wcsicmp(g_filePaths[data.lParam].c_str(), tab.path.c_str()) == 0) {
            g_syncFileSelection = true; TreeView_SelectItem(g_hwndFileTree, item);
            TreeView_EnsureVisible(g_hwndFileTree, item); g_syncFileSelection = false; break;
        }
    }
    if (g_hwndHighlightBar) InvalidateRect(g_hwndHighlightBar, NULL, TRUE);
    RelayoutViewer();
    if (newlyOpenedPdf) {
        if (tab.pdfPosition.captured) {
            if (!reader_next::RestorePdfView(g_hwndPdfPanel, tab.pdfPosition))
                SetViewerStatus(g_hwndMain, localization::Text(L"reader_next.fit_failed"));
        } else if (!reader_next::FitPdfReadingWidth(g_hwndPdfPanel))
            SetViewerStatus(g_hwndMain, localization::Text(L"reader_next.fit_failed"));
    }
    // Enable native layout before restoring coordinates; none of the inner
    // text/outline helpers may expose the temporary selection at the top.
    editRedraw.Resume(!showPdf && !showDiagram);
    if (g_isRichEdit && !showPdf && !showDiagram) {
        const auto& position = tab.readingPositions[static_cast<int>(tab.viewMode)];
        // Preserve the text at the viewport edge when wrapping changes, rather
        // than applying the old pixel offset to different lines. No disk state.
        RestoreReaderPosition(position);
    }
    RefreshSearchOverview();
}

void RebuildTabControl() {
    if (!g_hwndTabControl) return;
    TabCtrl_DeleteAllItems(g_hwndTabControl);
    for (size_t index = 0; index < g_tabs.size(); ++index) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<LPWSTR>(g_tabs[index].title.c_str());
        TabCtrl_InsertItem(g_hwndTabControl, static_cast<int>(index), &item);
    }
}

void RememberClosedTab(OpenTab tab) {
    constexpr size_t kClosedTabHistoryLimit = 12;
    g_closedTabs.push_back(std::move(tab));
    if (g_closedTabs.size() > kClosedTabHistoryLimit) g_closedTabs.erase(g_closedTabs.begin());
}

void SelectTab(int index) {
    if (index < 0 || index >= static_cast<int>(g_tabs.size())) {
        g_currentTab = -1;
    } else {
        g_currentTab = index;
        TabCtrl_SetCurSel(g_hwndTabControl, index);
    }
    ShowCurrentTab();
    InvalidateRect(g_hwndTabControl, nullptr, TRUE);
}

void CloseTabAt(int index) {
    if (index < 0 || index >= static_cast<int>(g_tabs.size())) return;
    const int previously_selected = g_currentTab;
    RememberClosedTab(std::move(g_tabs[index]));
    g_tabs.erase(g_tabs.begin() + index);
    RebuildTabControl();
    if (g_tabs.empty()) {
        SelectTab(-1);
    } else if (previously_selected == index) {
        SelectTab(std::min(index, static_cast<int>(g_tabs.size()) - 1));
    } else {
        SelectTab(previously_selected > index ? previously_selected - 1 : previously_selected);
    }
    SetViewerStatus(g_hwndMain, L"");
}

void CloseOtherTabs(int index) {
    if (index < 0 || index >= static_cast<int>(g_tabs.size())) return;
    OpenTab selected = std::move(g_tabs[index]);
    for (int tab_index = 0; tab_index < static_cast<int>(g_tabs.size()); ++tab_index) {
        if (tab_index != index) RememberClosedTab(std::move(g_tabs[tab_index]));
    }
    g_tabs.clear();
    g_tabs.push_back(std::move(selected));
    RebuildTabControl();
    SelectTab(0);
}

void CloseTabsToRight(int index) {
    if (index < 0 || index >= static_cast<int>(g_tabs.size())) return;
    for (int tab_index = index + 1; tab_index < static_cast<int>(g_tabs.size()); ++tab_index)
        RememberClosedTab(std::move(g_tabs[tab_index]));
    g_tabs.erase(g_tabs.begin() + index + 1, g_tabs.end());
    RebuildTabControl();
    SelectTab(index);
}

void CloseAllTabs() {
    for (OpenTab& tab : g_tabs) RememberClosedTab(std::move(tab));
    g_tabs.clear();
    RebuildTabControl();
    SelectTab(-1);
}

void ReopenLastClosedTab() {
    if (g_closedTabs.empty()) return;
    g_tabs.push_back(std::move(g_closedTabs.back()));
    g_closedTabs.pop_back();
    RebuildTabControl();
    SelectTab(static_cast<int>(g_tabs.size()) - 1);
}

void MoveTab(int from_index, int to_index) {
    if (from_index < 0 || to_index < 0 || from_index >= static_cast<int>(g_tabs.size()) ||
        to_index >= static_cast<int>(g_tabs.size()) || from_index == to_index) return;
    OpenTab tab = std::move(g_tabs[from_index]);
    g_tabs.erase(g_tabs.begin() + from_index);
    g_tabs.insert(g_tabs.begin() + to_index, std::move(tab));
    RebuildTabControl();
    SelectTab(to_index);
}

void ChangePartialTextRange(HWND owner, int index) {
    if (index < 0 || index >= static_cast<int>(g_tabs.size())) return;
    OpenTab& tab = g_tabs[index];
    if (!tab.isPartialText || tab.sourceSize == 0) return;
    const std::optional<ByteRange> range = ChooseLargeTextRange(owner, tab.sourceSize, tab.textRange);
    if (!range) return;
    BeginCancellableLoad(owner, localization::Text(L"readonly.status.loading"));
    LoadFileResult loaded = LoadTextFile(tab.path, range);
    EndCancellableLoad();
    if (loaded.hasError || loaded.cancelled) { SetViewerStatus(owner, loaded.text); return; }
    tab.rawText = std::move(loaded.text);
    tab.bytes = std::move(loaded.bytes);
    InvalidateReaderPreparation(tab);
    tab.isBinary = loaded.isBinary;
    tab.textRange = *range;
    tab.sourceSize = loaded.source_size;
    tab.viewMode = tab.isBinary ? ViewMode::Hex : (IsMarkdownPath(tab.path) ? ViewMode::Decorated : ViewMode::Raw);
    ParseJapaneseLines(tab);
    SelectTab(index);
}

void SetViewerStatus(HWND owner, const std::wstring& message) {
    SetWindowTextW(GetDlgItem(owner, 817), message.empty() ? localization::Text(L"reader_next.ready").c_str() : message.c_str());
    UpdateReaderWindowTitle(owner);
}

void UpdateReaderWindowTitle(HWND owner) {
#if defined(PDF_NOTE_READER_PROTOTYPE) && PDF_NOTE_READER_PROTOTYPE
    std::wstring title = localization::Text(L"reader_next.title");
#else
    std::wstring title = localization::Text(L"readonly.window.title");
#endif
    if (!g_tabs.empty()) {
        title += localization::Format(L"readonly.window.tab_count", {{L"COUNT", std::to_wstring(g_tabs.size())}});
    }
    if (g_currentTab >= 0 && g_currentTab < static_cast<int>(g_tabs.size())) {
        title += L" - " + g_tabs[static_cast<size_t>(g_currentTab)].title;
    }
    SetWindowTextW(owner, title.c_str());
}

bool IsSupportedLocalPath(const std::wstring& path) {
    if (path.empty() || path.rfind(L"\\\\", 0) == 0) return false;
    const std::filesystem::path filePath(path);
    const std::filesystem::path root = filePath.root_path();
    if (!root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE) return false;
    std::filesystem::path current = root;
    const std::filesystem::path relativePath = filePath.relative_path();
    for (auto it = relativePath.begin(); it != relativePath.end(); ++it) {
        current /= *it;
        const std::wstring openPath = ToExtendedWin32PathIfAbsoluteLocal(current);
        const DWORD attributes = GetFileAttributesW(openPath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
    }
    return true;
}

struct StoredReadonlySession {
    bool persistenceEnabled = false;
    std::wstring folder;
    bool folderPersistent = false;
    std::vector<std::pair<std::wstring, bool>> tabs;
    int selectedTab = -1;
};

std::filesystem::path ViewerExecutablePath() {
    std::vector<wchar_t> buffer(512, L'\0');
    for (;;) {
        const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) return {};
        if (size + 1 < buffer.size()) return std::filesystem::path(std::wstring(buffer.data(), size));
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2, L'\0');
    }
}

std::filesystem::path ViewerExeDirectory() {
    return ViewerExecutablePath().parent_path();
}

std::string EscapeJsonString(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { const char hex[] = "0123456789abcdef"; out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
            else out += static_cast<char>(c);
        }
    }
    return out;
}

size_t SkipJsonString(const std::string& text, size_t pos) {
    if (pos >= text.size() || text[pos] != '"') return std::string::npos;
    for (++pos; pos < text.size(); ++pos) {
        if (text[pos] == '\\') { ++pos; continue; }
        if (text[pos] == '"') return pos + 1;
    }
    return std::string::npos;
}

size_t SkipJsonValue(const std::string& text, size_t pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    if (pos >= text.size()) return std::string::npos;
    if (text[pos] == '"') return SkipJsonString(text, pos);
    if (text[pos] != '{' && text[pos] != '[') {
        while (pos < text.size() && text[pos] != ',' && text[pos] != '}' && text[pos] != ']') ++pos;
        return pos;
    }
    const char open = text[pos], close = open == '{' ? '}' : ']';
    int depth = 0;
    for (; pos < text.size(); ++pos) {
        if (text[pos] == '"') { pos = SkipJsonString(text, pos); if (pos == std::string::npos) return pos; --pos; continue; }
        if (text[pos] == open) ++depth;
        else if (text[pos] == close && --depth == 0) return pos + 1;
    }
    return std::string::npos;
}

bool JsonObjectMember(const std::string& object, const char* key, std::string* value) {
    if (!value) return false;
    const std::string quoted = std::string("\"") + key + "\"";
    size_t pos = 0;
    while ((pos = object.find(quoted, pos)) != std::string::npos) {
        const size_t afterKey = pos + quoted.size();
        size_t colon = afterKey;
        while (colon < object.size() && std::isspace(static_cast<unsigned char>(object[colon]))) ++colon;
        if (colon >= object.size() || object[colon] != ':') { pos = afterKey; continue; }
        ++colon;
        while (colon < object.size() && std::isspace(static_cast<unsigned char>(object[colon]))) ++colon;
        const size_t end = SkipJsonValue(object, colon);
        if (end == std::string::npos) return false;
        *value = object.substr(colon, end - colon);
        return true;
    }
    return false;
}

bool JsonStringMember(const std::string& object, const char* key, std::wstring* value) {
    std::string raw;
    if (!value || !JsonObjectMember(object, key, &raw) || raw.size() < 2 || raw.front() != '"' || raw.back() != '"') return false;
    std::string decoded;
    size_t pos = 0;
    if (!json_string::DecodeToken(raw, &pos, &decoded) || pos != raw.size()) return false;
    *value = UTF8ToWide(decoded);
    return true;
}

bool JsonBoolMember(const std::string& object, const char* key, bool* value) {
    std::string raw;
    if (!value || !JsonObjectMember(object, key, &raw)) return false;
    if (raw == "true") { *value = true; return true; }
    if (raw == "false") { *value = false; return true; }
    return false;
}

bool JsonIntMember(const std::string& object, const char* key, int* value) {
    std::string raw;
    if (!value || !JsonObjectMember(object, key, &raw)) return false;
    try { const long long parsed = std::stoll(raw); if (parsed < -1 || parsed > std::numeric_limits<int>::max()) return false; *value = static_cast<int>(parsed); return true; }
    catch (...) { return false; }
}

bool ParseReadonlySession(const std::string& json, StoredReadonlySession* out) {
    if (!out || json.size() > 4 * 1024 * 1024) return false;
    StoredReadonlySession parsed;
    std::string root = json, nested;
    if (JsonObjectMember(json, "readonlyViewer", &nested)) root = nested;
    if (!JsonBoolMember(root, "persistenceEnabled", &parsed.persistenceEnabled) &&
        !JsonBoolMember(root, "enabled", &parsed.persistenceEnabled)) return false;
    JsonIntMember(root, "selectedTab", &parsed.selectedTab);
    std::string folder;
    if (JsonObjectMember(root, "folder", &folder)) {
        JsonStringMember(folder, "path", &parsed.folder);
        JsonBoolMember(folder, "persistent", &parsed.folderPersistent);
    }
    std::string tabs;
    if (JsonObjectMember(root, "tabs", &tabs) && tabs.size() >= 2 && tabs.front() == '[') {
        for (size_t pos = 1; pos < tabs.size();) {
            while (pos < tabs.size() && (std::isspace(static_cast<unsigned char>(tabs[pos])) || tabs[pos] == ',')) ++pos;
            if (pos >= tabs.size() || tabs[pos] == ']') break;
            const size_t end = SkipJsonValue(tabs, pos); if (end == std::string::npos) return false;
            const std::string item = tabs.substr(pos, end - pos);
            std::wstring path; bool persistent = false;
            if (JsonStringMember(item, "path", &path) && JsonBoolMember(item, "persistent", &persistent)) parsed.tabs.emplace_back(std::move(path), persistent);
            pos = end;
        }
    }
    *out = std::move(parsed);
    return true;
}

bool ReadReadonlySessionFile(const std::filesystem::path& path, StoredReadonlySession* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return ParseReadonlySession(text, out);
}

[[nodiscard]] bool SaveReadonlySession(std::wstring* error) {
    // A command-line document/folder starts a temporary viewing session. Do
    // not replace previously saved paths when this instance never loaded them
    // and the user has not explicitly changed the restore preference.
    if (!g_sessionStateLoaded) return true;
    const std::filesystem::path dir = ViewerExeDirectory();
    if (dir.empty()) { if (error) *error = localization::Text(L"readonly.settings.path_unavailable"); return false; }
    std::ostringstream json;
    json << "{\n  \"format\": \"readonly_viewer_session_v1\",\n  \"persistenceEnabled\": "
         << (g_persistSessionEnabled ? "true" : "false") << ",\n  \"folder\": {\"path\": \"";
    if (g_persistSessionEnabled && g_sessionFolderPersistent) json << EscapeJsonString(WideToUTF8(g_sessionFolder));
    json << "\", \"persistent\": " << (g_persistSessionEnabled && g_sessionFolderPersistent ? "true" : "false") << "},\n"
         << "  \"selectedTab\": " << (g_persistSessionEnabled ? g_currentTab : -1) << ",\n  \"tabs\": [";
    bool first = true;
    if (g_persistSessionEnabled) for (const OpenTab& tab : g_tabs) if (tab.persistent) {
        if (!first) json << ',';
        first = false;
        json << "\n    {\"path\": \"" << EscapeJsonString(WideToUTF8(tab.path)) << "\", \"persistent\": true}";
    }
    if (!first) json << '\n';
    json << "  ]\n}\n";
    return atomic_write::AtomicWriteUtf8(dir / kViewerSessionFile, json.str(), dir, error);
}

void RestoreReadonlySession(HWND owner) {
    const std::filesystem::path dir = ViewerExeDirectory();
    if (dir.empty()) return;
    StoredReadonlySession shared, privateState;
    const std::filesystem::path sharedSetup = dir / L"pdf_note_workspace_setup.json";
    const std::filesystem::path legacySharedSetup = dir / L"pdf_workspace_setup.json";
    std::error_code sharedSetupEc;
    const bool canonicalSetupExists = std::filesystem::exists(sharedSetup, sharedSetupEc) && !sharedSetupEc;
    const bool hasShared = ReadReadonlySessionFile(
        canonicalSetupExists ? sharedSetup : legacySharedSetup, &shared);
    const bool hasPrivate = ReadReadonlySessionFile(dir / kViewerSessionFile, &privateState);
    std::error_code privateExistsEc;
    const bool privateExists = std::filesystem::exists(dir / kViewerSessionFile, privateExistsEc);
    // A malformed/inaccessible existing preference file is recoverable input,
    // not permission to save defaults over it on normal shutdown.
    g_sessionStateLoaded = hasPrivate || (!privateExistsEc && !privateExists);
    // The dedicated file is the user's explicit choice.  It can disable restoration
    // even when a previously distributed/shared setup still contains old paths.
    if (hasPrivate ? !privateState.persistenceEnabled : (!hasShared || !shared.persistenceEnabled)) return;
    g_persistSessionEnabled = true;
    const StoredReadonlySession* folderState =
        hasPrivate && privateState.folderPersistent ? &privateState : (hasShared ? &shared : nullptr);
    if (folderState && folderState->folderPersistent && IsSupportedLocalPath(folderState->folder)) {
        std::error_code ec;
        if (std::filesystem::is_directory(folderState->folder, ec) && !ec) {
            g_sessionFolder = folderState->folder;
            g_sessionFolderPersistent = true;
            PopulateFileTree(folderState->folder);
        }
    }
    std::vector<std::wstring> restored;
    const auto restoreTabs = [&](const StoredReadonlySession& state) {
        for (const auto& [path, persistent] : state.tabs) {
            if (!persistent || !IsSupportedLocalPath(path)) continue;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec) || ec) continue;
            const std::wstring normalized = std::filesystem::absolute(path, ec).lexically_normal().wstring();
            if (ec || std::find_if(restored.begin(), restored.end(), [&](const std::wstring& item) { return _wcsicmp(item.c_str(), normalized.c_str()) == 0; }) != restored.end()) continue;
            if (OpenPathInTab(owner, normalized)) { g_tabs.back().persistent = true; restored.push_back(normalized); }
        }
    };
    if (hasShared && shared.persistenceEnabled) restoreTabs(shared);
    if (hasPrivate) restoreTabs(privateState);
    const int selected = hasPrivate ? privateState.selectedTab : shared.selectedTab;
    if (selected >= 0 && selected < static_cast<int>(g_tabs.size())) SelectTab(selected);
}

std::filesystem::path InitialDialogFolder() {
    if (g_currentTab >= 0 && g_currentTab < static_cast<int>(g_tabs.size())) {
        const std::filesystem::path candidate = std::filesystem::path(g_tabs[g_currentTab].path).parent_path();
        if (!candidate.empty() && IsSupportedLocalPath(candidate.wstring())) return candidate;
    }
    std::error_code ec;
    const std::filesystem::path current = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path() : current;
}

std::vector<std::wstring> PromptForLocalPaths(HWND owner, bool pickFolder) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))) || !dialog) {
        SetViewerStatus(owner, localization::Text(L"readonly.picker.unavailable"));
        return {};
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        options |= pickFolder ? FOS_PICKFOLDERS : (FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT);
        dialog->SetOptions(options);
    }
    dialog->SetTitle(localization::Text(pickFolder ? L"readonly.picker.open_folder" : L"readonly.picker.open_file").c_str());
    if (!pickFolder) {
        const std::wstring filterDocuments = localization::Text(L"readonly.picker.filter_documents");
        const std::wstring filterPdf = localization::Text(L"readonly.picker.filter_pdf");
        const std::wstring filterText = localization::Text(L"readonly.picker.filter_text");
        const std::wstring filterAll = localization::Text(L"readonly.picker.filter_all");
        COMDLG_FILTERSPEC filters[] = {
            {filterDocuments.c_str(), L"*.pdf;*.txt;*.md;*.markdown;*.json;*.clro;*.sha256"},
            {filterPdf.c_str(), L"*.pdf"},
            {filterText.c_str(), L"*.txt;*.md;*.markdown;*.json;*.clro;*.sha256"},
            {filterAll.c_str(), L"*.*"},
        };
        dialog->SetFileTypes(static_cast<UINT>(sizeof(filters) / sizeof(filters[0])), filters);
        dialog->SetFileTypeIndex(1);
    }

    const std::filesystem::path initialFolder = InitialDialogFolder();
    if (!initialFolder.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initialFolder.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    const HRESULT showResult = dialog->Show(owner);
    if (showResult == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        dialog->Release();
        return {};
    }
    if (FAILED(showResult)) {
        dialog->Release();
        SetViewerStatus(owner, localization::Text(L"readonly.picker.failed"));
        return {};
    }

    std::vector<std::wstring> paths;
    if (pickFolder) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR raw = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
                paths.emplace_back(raw);
                CoTaskMemFree(raw);
            }
            item->Release();
        }
    } else {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dialog->GetResults(&items)) && items) {
            DWORD count = 0;
            if (SUCCEEDED(items->GetCount(&count))) {
                for (DWORD i = 0; i < count; ++i) {
                    IShellItem* item = nullptr;
                    if (FAILED(items->GetItemAt(i, &item)) || !item) continue;
                    PWSTR raw = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
                        paths.emplace_back(raw);
                        CoTaskMemFree(raw);
                    }
                    item->Release();
                }
            }
            items->Release();
        }
    }
    dialog->Release();
    return paths;
}

std::optional<FileSnapshot> CaptureFileSnapshot(const std::wstring& path) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) return std::nullopt;
    const std::filesystem::file_time_type lastWriteTime = std::filesystem::last_write_time(path, ec);
    if (ec) return std::nullopt;
    return FileSnapshot{size, lastWriteTime};
}

bool HasSourceFileChanged(const OpenTab& tab) {
    const std::optional<FileSnapshot> current = CaptureFileSnapshot(tab.path);
    return !current || !tab.sourceSnapshot || current->size != tab.sourceSnapshot->size ||
        current->lastWriteTime != tab.sourceSnapshot->lastWriteTime;
}

bool ReloadOpenTab(HWND owner, int index) {
    if (index < 0 || index >= static_cast<int>(g_tabs.size())) return false;
    const OpenTab previous = g_tabs[static_cast<size_t>(index)];
    if (!IsSupportedLocalPath(previous.path)) {
        SetViewerStatus(owner, localization::Text(L"readonly.file.local_only"));
        return false;
    }

    std::optional<ByteRange> selectedRange;
    if (previous.isPartialText) selectedRange = previous.textRange;
    BeginCancellableLoad(owner, localization::Text(L"readonly.status.loading_updated_file"));
    LoadFileResult loaded = LoadTextFile(previous.path, selectedRange);
    EndCancellableLoad();
    if (loaded.cancelled || loaded.hasError) {
        SetViewerStatus(owner, loaded.text.empty() ? localization::Text(L"readonly.status.reload_failed") : loaded.text);
        return false;
    }
    if (loaded.requires_range_selection) {
        selectedRange = ChooseLargeTextRange(owner, loaded.source_size);
        if (!selectedRange) { SetViewerStatus(owner, L""); return false; }
        BeginCancellableLoad(owner, localization::Text(L"readonly.status.loading_selected_range"));
        loaded = LoadTextFile(previous.path, selectedRange);
        EndCancellableLoad();
        if (loaded.cancelled || loaded.hasError) {
            SetViewerStatus(owner, loaded.text.empty() ? localization::Text(L"readonly.status.reload_failed") : loaded.text);
            return false;
        }
    }

    OpenTab refreshed = previous;
    refreshed.rawText = std::move(loaded.text);
    refreshed.bytes = std::move(loaded.bytes);
    InvalidateReaderPreparation(refreshed);
    refreshed.isBinary = loaded.isBinary;
    refreshed.isPdf = loaded.isPdf;
    refreshed.loadError = false;
    refreshed.isPartialText = selectedRange.has_value();
    refreshed.textRange = selectedRange.value_or(ByteRange{});
    refreshed.sourceSize = loaded.source_size;
    refreshed.sourceSnapshot = CaptureFileSnapshot(refreshed.path);
    refreshed.title = std::filesystem::path(refreshed.path).filename().wstring();
    if (refreshed.isPartialText) refreshed.title += localization::Text(L"readonly.tab.partial_suffix");
    ParseJapaneseLines(refreshed);
    g_tabs[static_cast<size_t>(index)] = std::move(refreshed);
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<LPWSTR>(g_tabs[static_cast<size_t>(index)].title.c_str());
    TabCtrl_SetItem(g_hwndTabControl, index, &item);
    if (g_tabs[static_cast<size_t>(index)].isPdf && g_hwndPdfPanel) {
        readonly_viewer::PdfPreviewPanel_ClosePdf(g_hwndPdfPanel);
    }
    if (g_currentTab == index) ShowCurrentTab();
    SetViewerStatus(owner, L"");
    return true;
}

bool ConfirmAndReloadChangedTab(HWND owner, int index) {
    if (index < 0 || index >= static_cast<int>(g_tabs.size()) || !HasSourceFileChanged(g_tabs[static_cast<size_t>(index)])) {
        return false;
    }
    if (!ConfirmReloadChangedFile(owner)) {
        SetViewerStatus(owner, localization::Text(L"readonly.status.external_change_kept"));
        return false;
    }
    return ReloadOpenTab(owner, index);
}

bool OpenPathInTab(HWND owner, const std::wstring& path) {
    if (!IsSupportedLocalPath(path)) {
        SetViewerStatus(owner, localization::Text(L"readonly.file.local_only"));
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        SetViewerStatus(owner, localization::Text(L"readonly.file.unreadable"));
        return false;
    }

    for (size_t i = 0; i < g_tabs.size(); ++i) {
        if (_wcsicmp(g_tabs[i].path.c_str(), path.c_str()) == 0) {
            TabCtrl_SetCurSel(g_hwndTabControl, static_cast<int>(i));
            g_currentTab = static_cast<int>(i);
            ConfirmAndReloadChangedTab(owner, static_cast<int>(i));
            ShowCurrentTab();
            if (!HasSourceFileChanged(g_tabs[i])) SetViewerStatus(owner, L"");
            return true;
        }
    }

    BeginCancellableLoad(owner, localization::Text(L"readonly.status.loading"));
    LoadFileResult loaded = LoadTextFile(path);
    EndCancellableLoad();
    if (loaded.cancelled) {
        SetViewerStatus(owner, loaded.text);
        return false;
    }
    std::optional<ByteRange> selected_range;
    if (loaded.requires_range_selection) {
        selected_range = ChooseLargeTextRange(owner, loaded.source_size);
        if (!selected_range) {
            SetViewerStatus(owner, L"");
            return false;
        }
        BeginCancellableLoad(owner, localization::Text(L"readonly.status.loading_selected_range"));
        loaded = LoadTextFile(path, selected_range);
        EndCancellableLoad();
        if (loaded.cancelled) {
            SetViewerStatus(owner, loaded.text);
            return false;
        }
    }
    if (loaded.hasError) {
        SetViewerStatus(owner, loaded.text.empty() ? localization::Text(L"readonly.status.reload_failed") : loaded.text);
        return false;
    }
    OpenTab tab;
    tab.title = std::filesystem::path(path).filename().wstring();
    if (selected_range) tab.title += localization::Text(L"readonly.tab.partial_suffix");
    tab.path = path;
    tab.rawText = std::move(loaded.text);
    tab.bytes = std::move(loaded.bytes);
    tab.isBinary = loaded.isBinary;
    tab.isPdf = loaded.isPdf;
    tab.loadError = loaded.hasError;
    tab.isPartialText = selected_range.has_value() && !tab.loadError;
    tab.textRange = selected_range.value_or(ByteRange{});
    tab.sourceSize = loaded.source_size;
    tab.sourceSnapshot = CaptureFileSnapshot(path);
    tab.viewMode = tab.loadError ? ViewMode::Raw :
        (tab.isPdf ? ViewMode::Pdf : (tab.isBinary ? ViewMode::Hex : (IsMarkdownPath(path) || IsClropPath(path) ? ViewMode::Decorated : ViewMode::Raw)));
    ParseJapaneseLines(tab);
    g_tabs.push_back(std::move(tab));

    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<LPWSTR>(g_tabs.back().title.c_str());
    const int newIndex = static_cast<int>(g_tabs.size() - 1);
    TabCtrl_InsertItem(g_hwndTabControl, newIndex, &item);
    TabCtrl_SetCurSel(g_hwndTabControl, newIndex);
    g_currentTab = newIndex;
    ShowCurrentTab();
    if (g_tabs[newIndex].isPdf) {
        const auto loadedPdf = reader_next::StartPdfSearch(false);
        if (loadedPdf.path != path || loadedPdf.pageCount <= 0) return false;
    }
    SetViewerStatus(owner, L"");
    return true;
}

BOOL CALLBACK CollectOtherViewerWindowsProc(HWND hwnd, LPARAM ownerParam) {
    const HWND owner = reinterpret_cast<HWND>(ownerParam);
    if (!hwnd || hwnd == owner || !IsWindowVisible(hwnd)) return TRUE;
    wchar_t className[64]{};
    if (GetClassNameW(hwnd, className, static_cast<int>(sizeof(className) / sizeof(className[0]))) <= 0 ||
        wcscmp(className, kViewerWindowClass) != 0) return TRUE;
    g_tabTransferTargets.push_back(hwnd);
    return TRUE;
}

void CollectOtherViewerWindows(HWND owner) {
    g_tabTransferTargets.clear();
    EnumWindows(CollectOtherViewerWindowsProc, reinterpret_cast<LPARAM>(owner));
}

bool IsTrustedReadonlyViewerWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return false;
    std::vector<wchar_t> senderPath(512, L'\0');
    std::wstring senderImagePath;
    for (;;) {
        DWORD senderPathLength = static_cast<DWORD>(senderPath.size());
        if (QueryFullProcessImageNameW(process, 0, senderPath.data(), &senderPathLength)) {
            senderImagePath.assign(senderPath.data(), senderPathLength);
            break;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER || senderPath.size() >= 32768) break;
        senderPath.resize(senderPath.size() * 2, L'\0');
    }
    CloseHandle(process);
    const std::filesystem::path currentPath = ViewerExecutablePath();
    return !senderImagePath.empty() && !currentPath.empty() &&
           _wcsicmp(senderImagePath.c_str(), currentPath.c_str()) == 0;
}

void SendTabTransferAcknowledgement(HWND source, ULONG_PTR token) {
    if (!IsTrustedReadonlyViewerWindow(source)) return;
    COPYDATASTRUCT data{};
    data.dwData = kCopyDataTransferAck;
    data.cbData = sizeof(token);
    data.lpData = &token;
    SendMessageTimeoutW(source, WM_COPYDATA, reinterpret_cast<WPARAM>(g_hwndMain),
                        reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 3000, nullptr);
}

int TabViewRotation(int index) {
    if (index < 0 || index >= static_cast<int>(g_tabs.size()) || !g_tabs[index].isPdf) return 0;
    return index == g_currentTab && HasActivePdfPreview()
        ? readonly_viewer::PdfPreviewPanel_GetRotation(g_hwndPdfPanel) : g_tabs[index].pdfPosition.rotation;
}

bool MoveTabToWindow(HWND owner, int index, HWND target) {
    if (index < 0 || index >= static_cast<int>(g_tabs.size()) || !IsWindow(target)) return false;
    const std::wstring& path = g_tabs[static_cast<size_t>(index)].path;
    if (!IsSupportedLocalPath(path)) return false;
    const std::wstring payload = std::wstring(1, static_cast<wchar_t>(L'0' + TabViewRotation(index))) + path;
    COPYDATASTRUCT data{};
    data.dwData = kCopyDataOpenRotatedReadonlyTab;
    data.cbData = static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t));
    data.lpData = const_cast<wchar_t*>(payload.c_str());
    DWORD_PTR received = 0;
    if (!SendMessageTimeoutW(target, WM_COPYDATA, reinterpret_cast<WPARAM>(owner),
                             reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 3000, &received) ||
        received == 0) return false;
    CloseTabAt(index);
    // Defer normal shutdown until the receiving IPC and drag handler return.
    // The queued handler rechecks emptiness so a newly opened tab is retained.
    if (g_tabs.empty()) PostMessageW(owner, kCloseEmptyAfterTabTransferMessage, 0, 0);
    return true;
}

bool MoveTabToNewWindow(HWND owner, int index) {
    if (index < 0 || index >= static_cast<int>(g_tabs.size())) return false;
    const std::filesystem::path exePath = ViewerExecutablePath();
    if (exePath.empty()) return false;
    const ULONG_PTR token = g_nextTabTransferToken++;
    const std::wstring params = L"--open \"" + g_tabs[static_cast<size_t>(index)].path +
        L"\" --transfer-source " + std::to_wstring(reinterpret_cast<UINT_PTR>(owner)) +
        L" --transfer-token " + std::to_wstring(token) +
        L" --transfer-rotation " + std::to_wstring(TabViewRotation(index));
    g_pendingTabTransfers.push_back({token, g_tabs[static_cast<size_t>(index)].path});
    const HINSTANCE result = ShellExecuteW(owner, L"open", exePath.c_str(), params.c_str(), exePath.parent_path().c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        g_pendingTabTransfers.erase(std::remove_if(g_pendingTabTransfers.begin(), g_pendingTabTransfers.end(),
            [token](const auto& transfer) { return transfer.token == token; }), g_pendingTabTransfers.end());
        return false;
    }
    return true;
}

void PopulateReleaseDocuments(const std::vector<std::wstring>& paths) {
    TreeView_DeleteAllItems(g_hwndFileTree);
    g_filePaths.clear();
    
    TVINSERTSTRUCTW tvis = {0};
    tvis.hParent = TVI_ROOT;
    tvis.hInsertAfter = TVI_LAST;
    tvis.item.mask = TVIF_TEXT | TVIF_PARAM;
    std::wstring rootName = localization::Text(L"readonly.tree.release_documents");
    tvis.item.pszText = (LPWSTR)rootName.c_str();
    tvis.item.lParam = -1;
    HTREEITEM hRoot = TreeView_InsertItem(g_hwndFileTree, &tvis);

    for (const auto& path : paths) {
        std::error_code pathEc;
        if (std::filesystem::is_regular_file(path, pathEc) && !pathEc) {
            g_filePaths.push_back(path);
            size_t index = g_filePaths.size() - 1;
            std::wstring filename = std::filesystem::path(path).filename().wstring();
            tvis.hParent = hRoot;
            tvis.item.pszText = (LPWSTR)filename.c_str();
            tvis.item.lParam = (LPARAM)index;
            TreeView_InsertItem(g_hwndFileTree, &tvis);
        } else {
            pathEc.clear();
            if (!std::filesystem::is_directory(path, pathEc) || pathEc) continue;
            std::wstring dirName = std::filesystem::path(path).filename().wstring();
            tvis.hParent = hRoot;
            tvis.item.pszText = (LPWSTR)dirName.c_str();
            tvis.item.lParam = -1;
            HTREEITEM hDir = TreeView_InsertItem(g_hwndFileTree, &tvis);

            std::vector<std::filesystem::path> files;
            std::error_code iterEc;
            for (std::filesystem::recursive_directory_iterator it(
                     path, std::filesystem::directory_options::skip_permission_denied, iterEc), end;
                 !iterEc && it != end; it.increment(iterEc)) {
                std::error_code entryEc;
                if (it->is_regular_file(entryEc) && !entryEc) {
                    files.push_back(it->path());
                }
            }
            std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
                return left.generic_wstring() < right.generic_wstring();
            });

            std::map<std::wstring, HTREEITEM> directoryNodes;
            for (const auto& file : files) {
                const std::filesystem::path relative = file.lexically_relative(path);
                HTREEITEM parent = hDir;
                std::filesystem::path accumulated;
                for (const auto& component : relative.parent_path()) {
                    accumulated /= component;
                    const std::wstring key = accumulated.generic_wstring();
                    const auto existing = directoryNodes.find(key);
                    if (existing != directoryNodes.end()) {
                        parent = existing->second;
                        continue;
                    }

                    const std::wstring directoryName = component.wstring();
                    tvis.hParent = parent;
                    tvis.item.pszText = const_cast<LPWSTR>(directoryName.c_str());
                    tvis.item.lParam = -1;
                    parent = TreeView_InsertItem(g_hwndFileTree, &tvis);
                    directoryNodes.emplace(key, parent);
                }

                g_filePaths.push_back(file.wstring());
                size_t index = g_filePaths.size() - 1;
                std::wstring filename = file.filename().wstring();
                tvis.hParent = parent;
                tvis.item.pszText = const_cast<LPWSTR>(filename.c_str());
                tvis.item.lParam = (LPARAM)index;
                TreeView_InsertItem(g_hwndFileTree, &tvis);
            }
            TreeView_Expand(g_hwndFileTree, hDir, TVE_EXPAND);
        }
    }
    TreeView_Expand(g_hwndFileTree, hRoot, TVE_EXPAND);
}

void PopulateFileTree(const std::wstring& directoryPath) {
    if (!g_browseFolder.empty() && _wcsicmp(g_browseFolder.c_str(), directoryPath.c_str()) != 0 && !g_browseBack) {
        if (g_folderHistory.size() >= 64) g_folderHistory.erase(g_folderHistory.begin());
        g_folderHistory.push_back(g_browseFolder);
    }
    g_browseFolder = directoryPath;
    EnableWindow(GetDlgItem(g_hwndMain, reader_next::kFolderBackId), !g_folderHistory.empty());
    const auto currentParent = std::filesystem::path(directoryPath).parent_path();
    EnableWindow(GetDlgItem(g_hwndMain, reader_next::kFolderUpId), !currentParent.empty() && currentParent != std::filesystem::path(directoryPath));
    // Browsing another folder (including local license documents) must not
    // replace the path the user explicitly marked for session restoration.
    if (!g_sessionFolderPersistent) g_sessionFolder = directoryPath;
    if (!g_persistSessionEnabled) g_sessionFolderPersistent = false;
    TreeView_DeleteAllItems(g_hwndFileTree);
    g_filePaths.clear();
    g_directoryPaths.clear();
    
    TVINSERTSTRUCTW tvis = {0};
    tvis.hParent = TVI_ROOT;
    tvis.hInsertAfter = TVI_LAST;
    tvis.item.mask = TVIF_TEXT | TVIF_PARAM;
    std::wstring rootLabel = std::filesystem::path(directoryPath).filename().wstring();
    if (rootLabel.empty()) rootLabel = std::filesystem::path(directoryPath).root_path().wstring();
    tvis.item.pszText = rootLabel.data();
    tvis.item.lParam = -1;
    tvis.item.mask |= TVIF_STATE; tvis.item.stateMask = TVIS_BOLD; tvis.item.state = TVIS_BOLD;
    HTREEITEM hRoot = TreeView_InsertItem(g_hwndFileTree, &tvis);
    const auto parentName = std::filesystem::path(directoryPath).parent_path().filename().wstring();
    const auto breadcrumb = parentName.empty() ? rootLabel : parentName + L" / " + rootLabel;
    SetWindowTextW(g_hwndFileTreeLabel, breadcrumb.c_str());

    const auto addDirectory = [&](const std::wstring& path, const std::wstring& label) {
        g_directoryPaths.push_back(path);
        tvis.hParent = hRoot;
        tvis.item.mask = TVIF_TEXT | TVIF_PARAM;
        tvis.item.pszText = const_cast<LPWSTR>(label.c_str());
        tvis.item.lParam = -static_cast<LPARAM>(g_directoryPaths.size()) - 1;
        TreeView_InsertItem(g_hwndFileTree, &tvis);
    };

    std::error_code ec;
    const std::filesystem::path current(directoryPath);
    const std::filesystem::path parent = current.parent_path();
    if (!parent.empty() && parent != current && IsSupportedLocalPath(parent.wstring())) {
        addDirectory(parent.wstring(), localization::Text(L"reader_next.parent_folder"));
    }
    std::vector<std::filesystem::path> directories;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(current, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (it->is_directory(entryEc) && !entryEc && IsSupportedLocalPath(it->path().wstring())) directories.push_back(it->path());
        else if (it->is_regular_file(entryEc) && !entryEc) files.push_back(it->path());
    }
    const auto byName = [](const std::filesystem::path& left, const std::filesystem::path& right) {
        return _wcsicmp(left.filename().c_str(), right.filename().c_str()) < 0;
    };
    std::sort(directories.begin(), directories.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    for (const auto& directory : directories) addDirectory(directory.wstring(), directory.filename().wstring());
    for (const auto& file : files) {
        g_filePaths.push_back(file.wstring());
        tvis.hParent = hRoot;
        tvis.item.mask = TVIF_TEXT | TVIF_PARAM;
        std::wstring filename = file.filename().wstring();
        tvis.item.pszText = const_cast<LPWSTR>(filename.c_str());
        tvis.item.lParam = static_cast<LPARAM>(g_filePaths.size() - 1);
        TreeView_InsertItem(g_hwndFileTree, &tvis);
    }
    TreeView_Expand(g_hwndFileTree, hRoot, TVE_EXPAND);
    g_syncFileSelection = true; TreeView_SelectItem(g_hwndFileTree, hRoot); g_syncFileSelection = false;
}


void LayoutToolbarControls(HWND hWnd, int splitX, int rightPaneWidth, int height) {
    const auto px = [hWnd](int n) { return reader_next::Px(hWnd, n); };
    const int gap = px(12), x = splitX + gap, width = std::max(0, rightPaneWidth - gap * 2);
    MoveWindow(g_hwndTabControl, x, px(4), width, px(28), TRUE);
    int modeX = x;
    for (int id : {kDecoratedButtonId, kRawButtonId, kHexButtonId, kDiagramButtonId}) {
        MoveWindow(GetDlgItem(hWnd, id), modeX, px(40), px(70), px(28), TRUE); modeX += px(74);
    }
    MoveWindow(GetDlgItem(hWnd, kPdfRangeButtonId), x, px(40), px(150), px(28), TRUE);
    int zoomX = x + width - px(192);
    MoveWindow(GetDlgItem(hWnd, reader_next::kZoomOutId), zoomX, px(40), px(42), px(28), TRUE);
    MoveWindow(GetDlgItem(hWnd, reader_next::kZoomResetId), zoomX + px(46), px(40), px(88), px(28), TRUE);
    MoveWindow(GetDlgItem(hWnd, reader_next::kZoomInId), zoomX + px(138), px(40), px(42), px(28), TRUE);
    MoveWindow(GetDlgItem(hWnd, reader_next::kSearchId), x, px(76), std::max(px(80), width - px(156)), px(28), TRUE);
    MoveWindow(GetDlgItem(hWnd, reader_next::kFindPreviousId), x + width - px(148), px(76), px(70), px(28), TRUE);
    MoveWindow(GetDlgItem(hWnd, reader_next::kFindNextId), x + width - px(74), px(76), px(70), px(28), TRUE);
    MoveWindow(GetDlgItem(hWnd, 818), x + width * 2 / 3, std::max(0, height - px(28)), width / 3, px(22), TRUE);
    MoveWindow(GetDlgItem(hWnd, 817), x, std::max(0, height - px(28)), width * 2 / 3 - px(8), px(22), TRUE);
    const int top = px(116), contentHeight = std::max(0, height - top - px(36));
    // Limit line length in rendered reading mode; raw and hex retain their working width.
    const bool reading = g_currentTab >= 0 && g_currentTab < static_cast<int>(g_tabs.size()) &&
                         g_tabs[g_currentTab].viewMode == ViewMode::Decorated;
    const int pageWidth = reading ? std::min(width, px(940)) : width;
    const int pageX = x + (width - pageWidth) / 2;
    MoveWindow(g_hwndEditControl, pageX, top, std::max(0, pageWidth - px(12)), contentHeight, TRUE);
    MoveWindow(g_hwndHighlightBar, pageX + pageWidth - px(12), top, px(12), contentHeight, TRUE);
    if (g_isRichEdit) {
        RECT inset{px(28), px(18), std::max(px(28), pageWidth - px(58)), std::max(px(22), contentHeight - px(20))};
        SendMessageW(g_hwndEditControl, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&inset));
    }
    if (g_hwndPdfPanel) MoveWindow(g_hwndPdfPanel, x, top, width, contentHeight, TRUE);
    g_mermaidPreview.SetBounds(x, top, width, contentHeight);
    UpdateInlineDiagramBounds();
    if (g_currentTab >= 0 && g_currentTab < static_cast<int>(g_tabs.size())) {
        const OpenTab& tab = g_tabs[g_currentTab];
        std::wstring detail = tab.title + L"   ·   " + (tab.isPdf ? L"PDF" : (tab.isBinary ? L"HEX" : L"TEXT"));
        if (tab.isPartialText) detail += L"   ·   " + localization::Text(L"reader_next.partial");
        SetWindowTextW(GetDlgItem(hWnd, 818), detail.c_str());
    } else SetWindowTextW(GetDlgItem(hWnd, 818), localization::Text(L"reader_next.empty_detail").c_str());
}

void RelayoutViewer() {
    if (!g_hwndMain) return;
    RECT client{};
    GetClientRect(g_hwndMain, &client);
    SendMessageW(g_hwndMain, WM_SIZE, SIZE_RESTORED,
                 MAKELPARAM(client.right - client.left, client.bottom - client.top));
}

namespace {
reader_next::PdfSearchCursor g_pdfFind;
std::wstring g_findQuery;
int g_findRemaining = 0;
bool g_pdfFindActive = false;
int g_textZoom = 100;
}

void CancelReaderSearch() {
    ++g_searchGeneration;
    g_pdfFindActive = false;
    g_findRemaining = 0;
    g_findQuery.clear();
    g_pdfFind = {};
}

void RefreshSearchOverview() {
    // A query/view change cancels the old scan before any new results appear.
    // Only completed results are published; failures leave the search band empty.
    KillTimer(g_hwndMain, reader_next::kSearchOverviewTimer);
    wchar_t query[257]{};
    if (g_isRichEdit && g_currentTab >= 0 && g_currentTab < static_cast<int>(g_tabs.size()) &&
        g_tabs[g_currentTab].viewMode != ViewMode::Pdf && g_tabs[g_currentTab].viewMode != ViewMode::Diagram)
        GetWindowTextW(GetDlgItem(g_hwndMain, reader_next::kSearchId), query, 257);
    RECT band{}; GetClientRect(g_hwndHighlightBar, &band);
    const bool ready = g_searchOverview.Reset(g_hwndEditControl, query, band.bottom - band.top);
    if (!ready || (g_searchOverview.Active() && !SetTimer(g_hwndMain, reader_next::kSearchOverviewTimer, 120, nullptr)))
        SetViewerStatus(g_hwndMain, localization::Text(L"reader_next.search_failed"));
    if (g_hwndHighlightBar) InvalidateRect(g_hwndHighlightBar, nullptr, TRUE);
}

void ContinuePdfSearch() {
    if (!g_pdfFindActive) return;
    const int searchedPage = g_pdfFind.page;
    const auto result = reader_next::SearchPdfPage(g_hwndPdfPanel, g_findQuery, g_pdfFind);
    if (result == reader_next::SearchStep::Found) {
        g_pdfFindActive = false;
        SetViewerStatus(g_hwndMain, localization::Format(L"reader_next.pdf_found", {{L"PAGE", std::to_wstring(searchedPage + 1)}}));
    } else if (result == reader_next::SearchStep::NextPage && --g_findRemaining > 0) {
        if (!PostMessageW(g_hwndMain, reader_next::kSearchStepMessage, g_searchGeneration, 0)) {
            g_pdfFindActive = false;
            SetViewerStatus(g_hwndMain, localization::Text(L"reader_next.search_failed"));
        }
    } else {
        g_pdfFindActive = false;
        SetViewerStatus(g_hwndMain, localization::Text(result == reader_next::SearchStep::NextPage ? L"reader_next.find_none" : L"reader_next.search_failed"));
    }
}

void FindReaderText(bool previous) {
    if (g_currentTab < 0 || g_currentTab >= static_cast<int>(g_tabs.size())) return;
    wchar_t buffer[257]{};
    GetWindowTextW(GetDlgItem(g_hwndMain, reader_next::kSearchId), buffer, 257);
    const std::wstring query(buffer);
    if (query.empty()) { SetViewerStatus(g_hwndMain, localization::Text(L"reader_next.find_empty")); return; }
    const auto mode = g_tabs[g_currentTab].viewMode;
    if (mode == ViewMode::Pdf) {
        if (g_pdfFindActive) return;
        if (g_findQuery != query) g_pdfFind = reader_next::StartPdfSearch(previous);
        else if (g_pdfFind.previous != previous && g_pdfFind.resultPage >= 0) {
            g_pdfFind.previous = previous;
            g_pdfFind.page = g_pdfFind.resultPage;
            g_pdfFind.character = previous ? g_pdfFind.resultCharacter - 1 : g_pdfFind.resultCharacter + g_pdfFind.resultLength;
            if (previous && g_pdfFind.resultCharacter == 0) {
                g_pdfFind.page = (g_pdfFind.page + g_pdfFind.pageCount - 1) % g_pdfFind.pageCount;
                g_pdfFind.character = -1;
            }
        }
        g_findQuery = query;
        g_findRemaining = g_pdfFind.pageCount + 1;
        g_pdfFindActive = true;
        ++g_searchGeneration;
        SetViewerStatus(g_hwndMain, localization::Text(L"reader_next.searching"));
        ContinuePdfSearch();
        return;
    }
    if (mode == ViewMode::Diagram || !g_isRichEdit) {
        SetViewerStatus(g_hwndMain, localization::Text(L"reader_next.find_unavailable")); return;
    }
    CHARRANGE selected{};
    SendMessageW(g_hwndEditControl, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selected));
    const LONG length = static_cast<LONG>(SendMessageW(g_hwndEditControl, WM_GETTEXTLENGTH, 0, 0));
    FINDTEXTEXW find{};
    find.lpstrText = const_cast<wchar_t*>(query.c_str());
    // RichEdit's Unicode coordinates are UTF-16 code units. They never index UTF-8 bytes.
    find.chrg = previous ? CHARRANGE{selected.cpMin, 0} : CHARRANGE{selected.cpMax, -1};
    const WPARAM flags = previous ? 0 : FR_DOWN;
    LRESULT match = SendMessageW(g_hwndEditControl, EM_FINDTEXTEXW, flags, reinterpret_cast<LPARAM>(&find));
    if (match < 0) {
        find.chrg = previous ? CHARRANGE{length, selected.cpMin} : CHARRANGE{0, selected.cpMax};
        match = SendMessageW(g_hwndEditControl, EM_FINDTEXTEXW, flags, reinterpret_cast<LPARAM>(&find));
    }
    if (match >= 0) {
        SendMessageW(g_hwndEditControl, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&find.chrgText));
        SendMessageW(g_hwndEditControl, EM_SCROLLCARET, 0, 0);
        UpdateInlineDiagramBounds();
    }
    SetViewerStatus(g_hwndMain, localization::Text(match >= 0 ? L"reader_next.find_found" : L"reader_next.find_none"));
}

void ZoomReader(UINT command) {
    if (g_currentTab < 0 || g_currentTab >= static_cast<int>(g_tabs.size())) return;
    if (g_tabs[g_currentTab].viewMode == ViewMode::Diagram) {
        if (command == reader_next::kZoomResetId) g_mermaidPreview.ResetView();
        else if (command == reader_next::kZoomInId) g_mermaidPreview.ZoomIn();
        else g_mermaidPreview.ZoomOut();
        return;
    }
    if (g_tabs[g_currentTab].viewMode == ViewMode::Pdf) {
        if (command == reader_next::kZoomResetId) {
            if (!reader_next::FitPdfReadingWidth(g_hwndPdfPanel))
                SetViewerStatus(g_hwndMain, localization::Text(L"reader_next.fit_failed"));
            return;
        }
        const UINT pdf = command == reader_next::kZoomInId ? readonly_viewer::kCmdPdfZoomIn :
            command == reader_next::kZoomOutId ? readonly_viewer::kCmdPdfZoomOut : readonly_viewer::kCmdPdfZoomReset;
        SendMessageW(g_hwndPdfPanel, WM_COMMAND, pdf, 0);
    } else if (g_isRichEdit) {
        g_textZoom = command == reader_next::kZoomResetId ? 100 :
            std::clamp(g_textZoom + (command == reader_next::kZoomInId ? 10 : -10), 60, 200);
        if (!SendMessageW(g_hwndEditControl, EM_SETZOOM, g_textZoom, 100)) return;
        UpdateInlineDiagramBounds();
        SetViewerStatus(g_hwndMain, std::to_wstring(g_textZoom) + L"%");
        RefreshSearchOverview();
    }
}

ui::ShortcutInputGuard g_readerShortcutInput;

bool CreateReaderControls(HWND owner) {
    bool controlsCreated = true;
    const auto button = [owner](UINT id, const wchar_t* key) {
        return reader_next::Button(owner, id, localization::Text(key).c_str());
    };
    controlsCreated &= button(kOpenFileButtonId, L"reader_next.open") != nullptr;
    controlsCreated &= button(kDecoratedButtonId, L"reader_next.reading") != nullptr;
    controlsCreated &= button(kRawButtonId, L"reader_next.raw") != nullptr;
    controlsCreated &= button(kHexButtonId, L"reader_next.hex") != nullptr;
    controlsCreated &= button(kDiagramButtonId, L"reader_next.diagram") != nullptr;
    controlsCreated &= button(kPdfRangeButtonId, L"reader_next.range") != nullptr;
    controlsCreated &= button(reader_next::kFolderBackId, L"reader_next.folder_back") != nullptr;
    controlsCreated &= button(reader_next::kFolderUpId, L"reader_next.folder_up") != nullptr;
    EnableWindow(GetDlgItem(owner, reader_next::kFolderBackId), FALSE);
    EnableWindow(GetDlgItem(owner, reader_next::kFolderUpId), FALSE);
    controlsCreated &= button(reader_next::kFindPreviousId, L"reader_next.previous") != nullptr;
    controlsCreated &= button(reader_next::kFindNextId, L"reader_next.next") != nullptr;
    controlsCreated &= reader_next::Button(owner, reader_next::kZoomOutId, L"−") != nullptr;
    controlsCreated &= button(reader_next::kZoomResetId, L"reader_next.reset") != nullptr;
    controlsCreated &= reader_next::Button(owner, reader_next::kZoomInId, L"+") != nullptr;
    HWND search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0, owner, reinterpret_cast<HMENU>(reader_next::kSearchId), GetModuleHandleW(nullptr), nullptr);
    controlsCreated &= search != nullptr;
    SendMessageW(search, EM_SETLIMITTEXT, 256, 0);
    SendMessageW(search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(localization::Text(L"reader_next.search").c_str()));
    for (int id : {817, 818}) {
        HWND label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS,
            0, 0, 0, 0, owner, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        controlsCreated &= label != nullptr;
        reader_next::StyleControl(label);
    }
    for (HWND control : {g_hwndToggleLeftPaneButton, g_hwndOpenFolderButton}) {
        SetWindowLongPtrW(control, GWL_STYLE, (GetWindowLongPtrW(control, GWL_STYLE) & ~BS_TYPEMASK) | BS_OWNERDRAW);
        reader_next::StyleControl(control);
    }
    SetWindowTextW(g_hwndOpenFolderButton, localization::Text(L"reader_next.folder").c_str());
    for (HWND control : {g_hwndTabControl, g_hwndFileTreeLabel, g_hwndTocTreeLabel, search}) reader_next::StyleControl(control);
    reader_next::StyleControl(g_hwndFileTree, true);
    reader_next::StyleControl(g_hwndTocTree, true);
    TabCtrl_SetItemSize(g_hwndTabControl, reader_next::Px(owner, 180), reader_next::Px(owner, 32));
    const bool shortcutInputObserved = g_readerShortcutInput.Attach(search);
    (void)shortcutInputObserved; // Fail-closed for shortcuts in search, not for reading/text entry.
    SetViewerStatus(owner, L"");
    return controlsCreated;
}

bool HandleReaderKey(const MSG& message) {
    if ((message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN) || !IsWindowEnabled(g_hwndMain) ||
        (message.hwnd != g_hwndMain && !IsChild(g_hwndMain, message.hwnd))) return false;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (g_readerShortcutInput.Suppress(GetFocus())) return false;
    if (ctrl && alt && (GetKeyState(VK_RMENU) & 0x8000)) return false;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) return false;
    UINT command = 0;
    if (const auto* binding = application_shortcuts::Resolve(static_cast<UINT>(message.wParam), ctrl, alt, shift,
                                                            application_shortcuts::Surface::Reader)) {
        // PDF gestures must not replace selection/navigation in search fields
        // or the reader's selectable text view. Explicit Open/Close/Export remain available.
        const HWND focus = GetFocus();
        const bool textInput = focus && (focus == g_hwndEditControl || focus == GetDlgItem(g_hwndMain, reader_next::kSearchId));
        if (!application_shortcuts::MayRun(*binding, textInput, false, false)) return false;
        if ((message.lParam & 0x40000000) != 0) return true;
        if (binding->pdfOnly && !HasActivePdfPreview()) return true;
        using Action = application_shortcuts::Action;
        switch (binding->action) {
        case Action::OpenFile: command = kOpenFileButtonId; break;
        case Action::CloseFile: CloseTabAt(g_currentTab); return true;
        case Action::CloseAll: command = kTabMenuCloseAll; break;
        case Action::JumpPage: command = readonly_viewer::kCmdPdfJumpPage; break;
        case Action::FitPage: command = readonly_viewer::kCmdPdfFitPage; break;
        case Action::ToggleAnnotations: command = readonly_viewer::kCmdPdfToggleAnnots; break;
        case Action::RotateLeft: command = readonly_viewer::kCmdPdfRotateLeft; break;
        case Action::RotateRight: command = readonly_viewer::kCmdPdfRotateRight; break;
        case Action::ResetRotation: command = readonly_viewer::kCmdPdfRotateReset; break;
        case Action::Export:
            if (!HasActivePdfPreview()) return true;
            command = readonly_viewer::kCmdPdfExportAnnotatedPdf; break;
        default: return false;
        }
    }
    if (!command && ctrl && !alt) {
        if (message.wParam == 'O') command = shift ? kOpenFolderButtonId : kOpenFileButtonId;
        else if (message.wParam == 'B') command = kToggleLeftPaneButtonId;
        else if (message.wParam == 'F') { SetFocus(GetDlgItem(g_hwndMain, reader_next::kSearchId)); return true; }
        else if (shift && message.wParam == 'T') { ReopenLastClosedTab(); return true; }
        else if (message.wParam == VK_TAB && !g_tabs.empty()) {
            SelectTab((g_currentTab + (shift ? static_cast<int>(g_tabs.size()) - 1 : 1)) % static_cast<int>(g_tabs.size())); return true;
        } else if (message.wParam >= '1' && message.wParam <= '4') {
            command = message.wParam == '1' ? kDecoratedButtonId : message.wParam == '2' ? kRawButtonId :
                message.wParam == '3' ? kHexButtonId : kDiagramButtonId;
        } else if (message.wParam == VK_OEM_PLUS || message.wParam == VK_ADD) command = reader_next::kZoomInId;
        else if (message.wParam == VK_OEM_MINUS || message.wParam == VK_SUBTRACT) command = reader_next::kZoomOutId;
        else if (message.wParam == '0') command = reader_next::kZoomResetId;
        else if (message.wParam == 'R' && HasActivePdfPreview() && GetFocus() != GetDlgItem(g_hwndMain, reader_next::kSearchId)) command = shift
            ? readonly_viewer::kCmdPdfRotateLeft : readonly_viewer::kCmdPdfRotateRight;
    }
    if (!ctrl && !alt && (message.wParam == VK_F3 || (message.wParam == VK_RETURN && message.hwnd == GetDlgItem(g_hwndMain, reader_next::kSearchId)))) {
        FindReaderText(shift); return true;
    }
    if (!ctrl && !alt && message.wParam == VK_ESCAPE) {
        CancelReaderSearch(); SetViewerStatus(g_hwndMain, localization::Text(L"reader_next.search_canceled"));
        if (g_currentTab >= 0) SetFocus(g_tabs[g_currentTab].isPdf ? g_hwndPdfPanel : g_hwndEditControl);
        return true;
    }
    if (command) {
        HWND button = GetDlgItem(g_hwndMain, command);
        if (!button || IsWindowEnabled(button)) SendMessageW(g_hwndMain, WM_COMMAND, command, 0);
        return true;
    }
    // Keep native Tab traversal and visible focus. Enter in search is consumed above.
    if (message.wParam == VK_TAB && !ctrl && !alt) {
        HWND next = GetNextDlgTabItem(g_hwndMain, GetFocus(), shift);
        if (next) SetFocus(next);
        return true;
    }
    return false;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            HINSTANCE hInst = ((LPCREATESTRUCT)lParam)->hInstance;
            g_hwndMain = hWnd;
            ApplyNeutralWindowFrame(hWnd);
            reader_next::Initialize(hWnd);
            g_splitX = reader_next::Px(hWnd, 280);
            BuildViewerMenu(hWnd);
            reader_next::ConfigurePdfReadingStyle();
            g_hwndPdfPanel = readonly_viewer::CreatePdfPreviewPanel(hWnd, hInst, 1002);
            if (g_hwndPdfPanel) ShowWindow(g_hwndPdfPanel, SW_HIDE);
            g_hwndToggleLeftPaneButton = CreateWindowExW(0, L"BUTTON", localization::Text(L"readonly.pane.close_list").c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, hWnd, (HMENU)kToggleLeftPaneButtonId, hInst, NULL);
            g_hwndOpenFolderButton = CreateWindowExW(0, L"BUTTON", localization::Text(L"readonly.menu.open_folder").c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, hWnd, (HMENU)kOpenFolderButtonId, hInst, NULL);
            g_hwndFileTree = CreateWindowExW(0, WC_TREEVIEWW, L"",
                WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
                0, 0, 0, 0, hWnd, (HMENU)101, hInst, NULL);
            g_hwndTocTree = CreateWindowExW(0, WC_TREEVIEWW, L"",
                WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
                0, 0, 0, 0, hWnd, (HMENU)102, hInst, NULL);
            g_hwndFileTreeLabel = CreateWindowExW(0, L"STATIC", localization::Text(L"readonly.pane.files").c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                0, 0, 0, 0, hWnd, nullptr, hInst, nullptr);
            g_hwndTocTreeLabel = CreateWindowExW(0, L"STATIC", localization::Text(L"readonly.pane.table_of_contents").c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                0, 0, 0, 0, hWnd, nullptr, hInst, nullptr);
            g_hwndTabControl = CreateWindowExW(0, WC_TABCONTROLW, L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_OWNERDRAWFIXED | TCS_FIXEDWIDTH | TCS_FOCUSNEVER | TCS_SCROLLOPPOSITE,
                0, 0, 0, 0, hWnd, (HMENU)103, hInst, NULL);
            g_originalTabProc = (WNDPROC)SetWindowLongPtrW(g_hwndTabControl, GWLP_WNDPROC, (LONG_PTR)TabProc);
            g_richEditModule = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            const wchar_t* editClass = g_richEditModule ? MSFTEDIT_CLASS : L"EDIT";
            g_isRichEdit = g_richEditModule != NULL;
            g_hwndEditControl = CreateWindowExW(0, editClass, L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_WANTRETURN | ES_NOHIDESEL,
                0, 0, 0, 0, hWnd, (HMENU)104, hInst, NULL);
            if (!g_hwndEditControl && g_isRichEdit) {
                g_isRichEdit = false;
                g_hwndEditControl = CreateWindowExW(0, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_WANTRETURN | ES_NOHIDESEL,
                    0, 0, 0, 0, hWnd, (HMENU)104, hInst, NULL);
            }
            if (g_hwndEditControl) {
                g_originalEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                    g_hwndEditControl, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InlineDiagramEditProc)));
            }
            g_hwndHighlightBar = CreateWindowExW(0, L"HighlightBarClass", L"",
                WS_CHILD | WS_VISIBLE,
                0, 0, 0, 0, hWnd, (HMENU)105, hInst, NULL);
            if (!g_mermaidPreview.Create(hWnd, hInst, 820)) {
                SetViewerStatus(hWnd, localization::Text(L"readonly.status.diagram_preview_failed"));
            }
            g_mermaidPreview.SetDetachedWindowTarget(hWnd);
            if (!CreateReaderControls(hWnd)) return -1;
            UpdateModeButtons(ViewMode::Raw);
            DragAcceptFiles(hWnd, TRUE);
            return 0;
        }
        case WM_SIZE: {
            // Snapshot before native wrapping changes. Publication owns its own
            // restore and must not capture an intermediate/new document here.
            if (!g_publishingReaderView) RememberReaderView();
            const auto px = [hWnd](int n) { return reader_next::Px(hWnd, n); };
            const int width = LOWORD(lParam), height = HIWORD(lParam);
            int split = g_leftPaneVisible ? std::clamp(g_splitX, px(210), std::max(px(210), width - px(530))) : px(128);
            if (g_leftPaneVisible) g_splitX = split;
            MoveWindow(g_hwndToggleLeftPaneButton, px(12), px(44), split - px(24), px(28), TRUE);
            SetWindowTextW(g_hwndToggleLeftPaneButton, localization::Text(g_leftPaneVisible ? L"reader_next.focus" : L"reader_next.show_pane").c_str());
            MoveWindow(GetDlgItem(hWnd, kOpenFileButtonId), px(12), px(80), split - px(24), px(28), TRUE);
            MoveWindow(g_hwndOpenFolderButton, px(12), px(114), split - px(24), px(28), TRUE);
            for (HWND control : {g_hwndFileTree, g_hwndTocTree, g_hwndFileTreeLabel, g_hwndTocTreeLabel})
                ShowWindow(control, g_leftPaneVisible ? SW_SHOW : SW_HIDE);
            if (g_leftPaneVisible) {
                int treeHeight = std::max(0, height - px(220));
                int fileHeight = treeHeight * 55 / 100;
                MoveWindow(g_hwndFileTreeLabel, px(18), px(160), split - px(104), px(24), TRUE);
                MoveWindow(g_hwndFileTree, px(12), px(192), split - px(24), fileHeight, TRUE);
                MoveWindow(g_hwndTocTreeLabel, px(18), px(202) + fileHeight, split - px(36), px(24), TRUE);
                MoveWindow(g_hwndTocTree, px(12), px(234) + fileHeight, split - px(24), std::max(0, height - px(250) - fileHeight), TRUE);
            }
            MoveWindow(GetDlgItem(hWnd, reader_next::kFolderBackId), split - px(78), px(156), px(30), px(28), TRUE);
            MoveWindow(GetDlgItem(hWnd, reader_next::kFolderUpId), split - px(42), px(156), px(30), px(28), TRUE);
            for (UINT id : {reader_next::kFolderBackId, reader_next::kFolderUpId})
                ShowWindow(GetDlgItem(hWnd, id), g_leftPaneVisible ? SW_SHOW : SW_HIDE);
            LayoutToolbarControls(hWnd, split, width - split, height);
            if (!g_publishingReaderView && g_isRichEdit && g_currentTab >= 0 &&
                g_displayedPath == g_tabs[g_currentTab].path && IsWindowVisible(g_hwndEditControl))
                RestoreReaderPosition(g_tabs[g_currentTab].readingPositions[static_cast<int>(g_displayedMode)]);
            if (g_currentTab >= 0 && g_tabs[g_currentTab].reading && !g_tabs[g_currentTab].reading->tables.empty() && !g_reflowQueued)
                g_reflowQueued = PostMessageW(hWnd, WM_APP + 21, 0, 0) != FALSE;
            if (!g_publishingReaderView) RefreshSearchOverview();
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = {reader_next::Px(hWnd, 900), reader_next::Px(hWnd, 600)};
            return 0;
        }
        case WM_DPICHANGED: {
            reader_next::Initialize(hWnd);
            EnumChildWindows(hWnd, [](HWND child, LPARAM) -> BOOL { reader_next::StyleControl(child); return TRUE; }, 0);
            reader_next::StyleControl(g_hwndFileTree, true);
            reader_next::StyleControl(g_hwndTocTree, true);
            const RECT& rect = *reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hWnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_PAINT:
            reader_next::Paint(hWnd, g_leftPaneVisible ? g_splitX : reader_next::Px(hWnd, 128), reader_next::Px(hWnd, 110), g_tabs.empty());
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_CTLCOLORSTATIC:
            return reinterpret_cast<LRESULT>(reader_next::ControlBrush(reinterpret_cast<HWND>(lParam), reinterpret_cast<HDC>(wParam)));
        case WM_APP + 21: {
            g_reflowQueued = false;
            RECT viewport{}; SendMessageW(g_hwndEditControl, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&viewport));
            if (g_currentTab >= 0 && g_tabs[g_currentTab].viewMode == ViewMode::Decorated &&
                g_displayedPath == g_tabs[g_currentTab].path && viewport.right - viewport.left != g_displayedReadingWidth)
                ShowCurrentTab(false);
            return 0;
        }
        case reader_next::kSearchStepMessage:
            if (wParam == g_searchGeneration) ContinuePdfSearch();
            return 0;
        case WM_TIMER:
            if (wParam == reader_next::kSearchOverviewTimer) {
                const auto result = g_searchOverview.Step(g_hwndEditControl);
                if (result == reader_next::SearchOverview::Result::Pending) {
                    if (!SetTimer(hWnd, reader_next::kSearchOverviewTimer, 10, nullptr)) {
                        KillTimer(hWnd, reader_next::kSearchOverviewTimer);
                        SetViewerStatus(hWnd, localization::Text(L"reader_next.search_failed"));
                    }
                } else {
                    KillTimer(hWnd, reader_next::kSearchOverviewTimer);
                    if (result == reader_next::SearchOverview::Result::Failed)
                        SetViewerStatus(hWnd, localization::Text(L"reader_next.search_failed"));
                    InvalidateRect(g_hwndHighlightBar, nullptr, TRUE);
                }
                return 0;
            }
            break;
        case WM_INITMENUPOPUP:
            if (GetMenu(hWnd) && (reinterpret_cast<HMENU>(wParam) == GetSubMenu(GetMenu(hWnd), 0) ||
                                 reinterpret_cast<HMENU>(wParam) == GetSubMenu(GetMenu(hWnd), 1)))
                UpdatePdfViewMenu(hWnd);
            break;
        case WM_COMMAND: {
            const UINT command = LOWORD(wParam);
            if (command == kLicensesMenuId) {
                const auto exeDirectory = ViewerExeDirectory();
                const auto directory = exeDirectory / L"licenses";
                if (!exeDirectory.empty() && OpenPathInTab(hWnd, (directory / L"README.txt").wstring())) {
                    PopulateFileTree(directory.wstring());
                    ShowCurrentTab();
                    SetViewerStatus(hWnd, L"");
                } else {
                    SetViewerStatus(hWnd, localization::Text(L"reader_next.licenses_missing"));
                }
                return 0;
            }
            if (command == reader_next::kFolderBackId || command == reader_next::kFolderUpId) {
                std::wstring destination;
                if (command == reader_next::kFolderBackId && !g_folderHistory.empty()) {
                    destination = g_folderHistory.back(); g_folderHistory.pop_back(); g_browseBack = true;
                } else if (command == reader_next::kFolderUpId && !g_browseFolder.empty()) {
                    destination = std::filesystem::path(g_browseFolder).parent_path().wstring();
                }
                if (!destination.empty() && IsSupportedLocalPath(destination)) PopulateFileTree(destination);
                g_browseBack = false; return 0;
            }
            if (command == kTabMenuCloseAll) { CloseAllTabs(); SetViewerStatus(hWnd, L""); return 0; }
            if (command == SC_CLOSE) { PostMessageW(hWnd, WM_CLOSE, 0, 0); return 0; }
            if (command == kDecoratedButtonId || command == kRawButtonId || command == kHexButtonId || command == kDiagramButtonId) {
                HWND button = GetDlgItem(hWnd, command);
                if (!IsWindowEnabled(button) || !IsWindowVisible(button)) return 0;
            }
            if (command == reader_next::kSearchId && HIWORD(wParam) == EN_CHANGE) {
                CancelReaderSearch();
                RefreshSearchOverview();
            }
            if (command == reader_next::kFindNextId || command == reader_next::kFindPreviousId) {
                FindReaderText(command == reader_next::kFindPreviousId); return 0;
            }
            if (command >= reader_next::kZoomInId && command <= reader_next::kZoomResetId) {
                ZoomReader(command); return 0;
            }
            switch (command) {
                case kToggleLeftPaneButtonId:
                    g_leftPaneVisible = !g_leftPaneVisible;
                    {
                        RECT client{};
                        GetClientRect(hWnd, &client);
                        SendMessageW(hWnd, WM_SIZE, 0,
                                     MAKELPARAM(client.right - client.left, client.bottom - client.top));
                    }
                    return 0;
                case kPersistSessionMenuId: {
                    g_sessionStateLoaded = true;
                    g_persistSessionEnabled = !g_persistSessionEnabled;
                    HMENU menu = GetMenu(hWnd);
                    HMENU settings = menu ? GetSubMenu(menu, 3) : nullptr;
                    if (settings) CheckMenuItem(settings, kPersistSessionMenuId,
                                                MF_BYCOMMAND | (g_persistSessionEnabled ? MF_CHECKED : MF_UNCHECKED));
                    if (!g_persistSessionEnabled) {
                        g_sessionFolderPersistent = false;
                        for (OpenTab& tab : g_tabs) tab.persistent = false;
                        std::wstring error;
                        if (!SaveReadonlySession(&error)) SetViewerStatus(hWnd, localization::Text(L"readonly.status.session_disable_save_failed"));
                    }
                    DrawMenuBar(hWnd);
                    SetViewerStatus(hWnd, localization::Text(g_persistSessionEnabled ? L"readonly.status.session_restore_enabled" : L"readonly.status.session_restore_disabled"));
                    return 0;
                }
                case kPathMarkTabPersistentMenuId:
                    if (!g_persistSessionEnabled || g_currentTab < 0 || g_currentTab >= static_cast<int>(g_tabs.size())) {
                        SetViewerStatus(hWnd, localization::Text(L"readonly.status.enable_session_restore_first")); return 0;
                    }
                    g_tabs[g_currentTab].persistent = true;
                    SetViewerStatus(hWnd, localization::Text(L"readonly.status.current_tab_persisted"));
                    return 0;
                case kPathMarkTabTemporaryMenuId:
                    if (g_currentTab >= 0 && g_currentTab < static_cast<int>(g_tabs.size())) g_tabs[g_currentTab].persistent = false;
                    SetViewerStatus(hWnd, localization::Text(L"readonly.status.current_tab_temporary"));
                    return 0;
                case kPathMarkFolderPersistentMenuId:
                    if (!g_persistSessionEnabled || g_browseFolder.empty()) {
                        SetViewerStatus(hWnd, localization::Text(L"readonly.status.enable_restore_and_open_folder")); return 0;
                    }
                    g_sessionFolder = g_browseFolder;
                    g_sessionFolderPersistent = true;
                    SetViewerStatus(hWnd, localization::Text(L"readonly.status.current_folder_persisted"));
                    return 0;
                case kPathMarkFolderTemporaryMenuId:
                    g_sessionFolderPersistent = false;
                    SetViewerStatus(hWnd, localization::Text(L"readonly.status.current_folder_temporary"));
                    return 0;
                case kOpenFileButtonId: {
                    const std::vector<std::wstring> paths = PromptForLocalPaths(hWnd, false);
                    for (const std::wstring& path : paths) OpenPathInTab(hWnd, path);
                    return 0;
                }
                case kTabMenuClose:
                    CloseTabAt(g_currentTab);
                    return 0;
                case kShortcutReferenceMenuId:
                    if (!readonly_viewer::shortcut_reference::Show(hWnd))
                        SetViewerStatus(hWnd, localization::Text(L"shortcut.reference.failed"));
                    return 0;
                case kOpenFolderButtonId: {
                    const std::vector<std::wstring> paths = PromptForLocalPaths(hWnd, true);
                    if (!paths.empty() && IsSupportedLocalPath(paths.front())) {
                        // A selected folder is represented by the left file tree.  Reopen it
                        // when necessary so the newly selected root is immediately visible.
                        g_leftPaneVisible = true;
                        RECT client{};
                        GetClientRect(hWnd, &client);
                        SendMessageW(hWnd, WM_SIZE, SIZE_RESTORED,
                                     MAKELPARAM(client.right - client.left, client.bottom - client.top));
                        PopulateFileTree(paths.front());
                        SetViewerStatus(hWnd, L"");
                    } else if (!paths.empty()) {
                        SetViewerStatus(hWnd, localization::Text(L"readonly.folder.local_only"));
                    }
                    return 0;
                }
                case kDecoratedButtonId:
                    if (g_currentTab < 0 || g_currentTab >= static_cast<int>(g_tabs.size())) return 0;
                    g_tabs[g_currentTab].viewMode = ViewMode::Decorated;
                    ShowCurrentTab();
                    return 0;
                case kRawButtonId:
                    if (g_currentTab < 0 || g_currentTab >= static_cast<int>(g_tabs.size())) return 0;
                    g_tabs[g_currentTab].viewMode = ViewMode::Raw;
                    ShowCurrentTab();
                    return 0;
                case kHexButtonId:
                    if (g_currentTab < 0 || g_currentTab >= static_cast<int>(g_tabs.size())) return 0;
                    g_tabs[g_currentTab].viewMode = ViewMode::Hex;
                    ShowCurrentTab();
                    return 0;
                case kDiagramButtonId:
                    if (g_currentTab < 0 || g_currentTab >= static_cast<int>(g_tabs.size())) return 0;
                    g_tabs[g_currentTab].viewMode = ViewMode::Diagram;
                    ShowCurrentTab();
                    return 0;
                case kPdfRangeButtonId:
                    if (g_hwndPdfPanel) readonly_viewer::PdfPreviewPanel_ChoosePageRange(g_hwndPdfPanel);
                    return 0;
                case readonly_viewer::kCmdPdfToggleGrayscale:
                    if (HasActivePdfPreview()) {
                        SendMessageW(g_hwndPdfPanel, WM_COMMAND, readonly_viewer::kCmdPdfToggleGrayscale, 0);
                        UpdatePdfViewMenu(hWnd);
                    }
                    return 0;
                case readonly_viewer::kCmdPdfRotateLeft:
                case readonly_viewer::kCmdPdfRotateRight:
                case readonly_viewer::kCmdPdfRotateReset:
                case readonly_viewer::kCmdPdfToggleAnnots:
                case readonly_viewer::kCmdPdfExportAnnotatedPdf:
                case readonly_viewer::kCmdPdfJumpPage:
                case readonly_viewer::kCmdPdfFitPage:
                    if (HasActivePdfPreview()) {
                        if (command == readonly_viewer::kCmdPdfJumpPage) readonly_viewer::PdfPreviewPanel_PromptJumpPage(g_hwndPdfPanel);
                        else SendMessageW(g_hwndPdfPanel, WM_COMMAND, command, 0);
                        UpdatePdfViewMenu(hWnd);
                    }
                    return 0;
                case kCancelLoadButtonId:
                    if (g_loadInProgress) {
                        g_loadCancelRequested = true;
                    }
                    return 0;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            int xPos = GET_X_LPARAM(lParam);
            if (g_leftPaneVisible && xPos >= g_splitX && xPos <= g_splitX + 5) {
                g_isDraggingSplitter = true;
                SetCapture(hWnd);
            }
            return 0;
        }
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wParam);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                if (length == 0) continue;
                std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
                if (DragQueryFileW(drop, i, buffer.data(), static_cast<UINT>(buffer.size())) == 0) continue;
                const std::wstring path(buffer.data());
                std::error_code ec;
                if (IsSupportedLocalPath(path) && std::filesystem::is_directory(path, ec) && !ec) {
                    PopulateFileTree(path);
                    SetViewerStatus(hWnd, L"");
                } else {
                    OpenPathInTab(hWnd, path);
                }
            }
            DragFinish(drop);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_isDraggingSplitter) {
                int xPos = GET_X_LPARAM(lParam);
                int width = 0;
                RECT rc;
                if (GetClientRect(hWnd, &rc)) {
                    width = rc.right - rc.left;
                }
                if (xPos < 50) xPos = 50;
                if (width > 0 && xPos > width - 50) xPos = width - 50;
                g_splitX = xPos;
                SendMessage(hWnd, WM_SIZE, 0, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
            } else {
                int xPos = GET_X_LPARAM(lParam);
                if (g_leftPaneVisible && xPos >= g_splitX && xPos <= g_splitX + 5) {
                    SetCursor(LoadCursor(NULL, IDC_SIZEWE));
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_isDraggingSplitter) {
                g_isDraggingSplitter = false;
                ReleaseCapture();
            }
            return 0;
        }
        case WM_NOTIFY: {
            LPNMHDR lpnmhdr = (LPNMHDR)lParam;
            if ((lpnmhdr->hwndFrom == g_hwndFileTree || lpnmhdr->hwndFrom == g_hwndTocTree) && lpnmhdr->code == NM_CUSTOMDRAW)
                return reader_next::DrawTree(*reinterpret_cast<NMTVCUSTOMDRAW*>(lParam));
            if (lpnmhdr->hwndFrom == g_hwndFileTree && lpnmhdr->code == TVN_SELCHANGEDW) {
                LPNMTREEVIEWW lpnmtv = (LPNMTREEVIEWW)lParam;
                if (lpnmtv->itemNew.hItem && !g_syncFileSelection) {
                    const LPARAM itemPath = lpnmtv->itemNew.lParam;
                    if (itemPath >= 0 && itemPath < static_cast<LPARAM>(g_filePaths.size())) {
                        OpenPathInTab(hWnd, g_filePaths[static_cast<size_t>(itemPath)]);
                    } else if (itemPath <= -2) {
                        const size_t directoryIndex = static_cast<size_t>(-itemPath - 2);
                        if (directoryIndex < g_directoryPaths.size()) {
                            PopulateFileTree(g_directoryPaths[directoryIndex]);
                            SetViewerStatus(hWnd, L"");
                        }
                    }
                }
            } else if (lpnmhdr->hwndFrom == g_hwndTocTree && lpnmhdr->code == TVN_SELCHANGEDW) {
                const auto* selected = reinterpret_cast<const NMTREEVIEWW*>(lParam);
                if (!g_updatingToc && selected->itemNew.hItem && selected->itemNew.lParam >= 0 && IsWindowVisible(g_hwndEditControl)) {
                    SendMessageW(g_hwndEditControl, EM_SETSEL, selected->itemNew.lParam, selected->itemNew.lParam);
                    SendMessageW(g_hwndEditControl, EM_SCROLLCARET, 0, 0);
                    UpdateInlineDiagramBounds();
                }
            } else if (lpnmhdr->hwndFrom == g_hwndTabControl && lpnmhdr->code == TCN_SELCHANGE) {
                int idx = TabCtrl_GetCurSel(g_hwndTabControl);
                if (idx >= 0 && idx < (int)g_tabs.size()) {
                    g_currentTab = idx;
                    ConfirmAndReloadChangedTab(hWnd, idx);
                    ShowCurrentTab();
                }
            }
            return 0;
        }
        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT pdis = (LPDRAWITEMSTRUCT)lParam;
            if (reader_next::DrawButton(*pdis)) return TRUE;
            if (pdis->hwndItem == g_hwndTabControl) {
                int tabIdx = pdis->itemID;
                if (tabIdx >= 0 && tabIdx < (int)g_tabs.size()) {
                    reader_next::DrawTab(*pdis, g_tabs[tabIdx].title, (pdis->itemState & ODS_SELECTED) != 0);
                }
                return TRUE;
            }
            break;
        }
        case WM_APP + 1: {
            CloseTabAt(static_cast<int>(wParam));
            return 0;
        }
        case WM_APP + 2: {
            MoveTab(static_cast<int>(wParam), static_cast<int>(lParam));
            return 0;
        }
        case WM_COPYDATA: {
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            const HWND sender = reinterpret_cast<HWND>(wParam);
            if (!data || !IsTrustedReadonlyViewerWindow(sender)) return FALSE;
            if (data->dwData == kCopyDataTransferAck) {
                if (!data->lpData || data->cbData != sizeof(ULONG_PTR)) return FALSE;
                const ULONG_PTR token = *static_cast<const ULONG_PTR*>(data->lpData);
                const auto pending = std::find_if(g_pendingTabTransfers.begin(), g_pendingTabTransfers.end(),
                    [token](const PendingTabTransfer& item) { return item.token == token; });
                if (pending == g_pendingTabTransfers.end()) return FALSE;
                for (int tabIndex = 0; tabIndex < static_cast<int>(g_tabs.size()); ++tabIndex) {
                    if (_wcsicmp(g_tabs[static_cast<size_t>(tabIndex)].path.c_str(), pending->path.c_str()) == 0) {
                        CloseTabAt(tabIndex);
                        if (g_tabs.empty()) PostMessageW(hWnd, kCloseEmptyAfterTabTransferMessage, 0, 0);
                        break;
                    }
                }
                g_pendingTabTransfers.erase(pending);
                return TRUE;
            }
            const bool rotated = data->dwData == kCopyDataOpenRotatedReadonlyTab;
            if ((data->dwData != kCopyDataOpenReadonlyTab && !rotated) || !data->lpData ||
                data->cbData < sizeof(wchar_t) || (data->cbData % sizeof(wchar_t)) != 0) {
                return FALSE;
            }
            size_t charCount = data->cbData / sizeof(wchar_t);
            const auto* pathData = static_cast<const wchar_t*>(data->lpData);
            int rotation = 0;
            if (rotated) {
                if (charCount < 3 || pathData[0] < L'0' || pathData[0] > L'3') return FALSE;
                rotation = pathData[0] - L'0';
                ++pathData;
                --charCount;
            }
            if (charCount > 32768 || pathData[charCount - 1] != L'\0' ||
                std::find(pathData, pathData + charCount - 1, L'\0') != pathData + charCount - 1) {
                return FALSE;
            }
            if (!OpenPathInTab(hWnd, std::wstring(pathData, pathData + charCount - 1)))
                return FALSE;
            if (rotated && HasActivePdfPreview()) readonly_viewer::PdfPreviewPanel_SetRotation(g_hwndPdfPanel, rotation);
            return TRUE;
        }
        case kTabContextMenuMessage: {
            const int index = static_cast<int>(lParam);
            switch (static_cast<UINT>(wParam)) {
                case kTabMenuSelect:
                    SelectTab(index);
                    break;
                case kTabMenuClose:
                    CloseTabAt(index);
                    break;
                case kTabMenuCloseOthers:
                    CloseOtherTabs(index);
                    break;
                case kTabMenuCloseToRight:
                    CloseTabsToRight(index);
                    break;
                case kTabMenuCloseAll:
                    CloseAllTabs();
                    break;
                case kTabMenuMoveLeft:
                    MoveTab(index, index - 1);
                    break;
                case kTabMenuMoveRight:
                    MoveTab(index, index + 1);
                    break;
                case kTabMenuChangeRange:
                    ChangePartialTextRange(hWnd, index);
                    break;
                case kTabMenuReopenClosed:
                    ReopenLastClosedTab();
                    break;
                case kTabMenuMoveToNewWindow:
                    MoveTabToNewWindow(hWnd, index);
                    break;
                default:
                    if (static_cast<UINT>(wParam) >= kTabMenuMoveToWindowBase) {
                        const size_t targetIndex = static_cast<UINT>(wParam) - kTabMenuMoveToWindowBase;
                        if (targetIndex < g_tabTransferTargets.size()) {
                            MoveTabToWindow(hWnd, index, g_tabTransferTargets[targetIndex]);
                        }
                    }
                    break;
            }
            return 0;
        }
        case textviewer::mermaid::kOpenDetachedDiagramMessage:
            OpenDetachedDiagramWindow(reinterpret_cast<HWND>(wParam));
            return 0;
        case WM_QUERYENDSESSION:
            // Reply promptly; session persistence belongs to WM_ENDSESSION.
            g_viewerExitRequest = ViewerExitRequest::SystemEndSession;
            return TRUE;
        case WM_ENDSESSION:
            if (wParam == TRUE) {
                g_viewerExitRequest = ViewerExitRequest::SystemEndSession;
                g_loadCancelRequested = true;
                if (g_persistSessionEnabled && !g_systemEndSessionSessionSaved) {
                    std::wstring ignored;
                    (void)SaveReadonlySession(&ignored);
                    g_systemEndSessionSessionSaved = true;
                }
            } else if (g_viewerExitRequest == ViewerExitRequest::SystemEndSession) {
                g_viewerExitRequest = ViewerExitRequest::None;
                g_loadCancelRequested = false;
            }
            return 0;
        case kCloseEmptyAfterTabTransferMessage:
            if (g_tabs.empty()) SendMessageW(hWnd, WM_CLOSE, 0, 0);
            return 0;
        case WM_CLOSE:
            if (g_loadInProgress) {
                g_viewerExitRequest = ViewerExitRequest::UserClose;
                g_loadCancelRequested = true;
                return 0;
            }
            DestroyWindow(hWnd);
            return 0;
        case WM_DESTROY:
            if (g_persistSessionEnabled && !g_systemEndSessionSessionSaved) {
                std::wstring ignored;
                const bool saved = SaveReadonlySession(&ignored);
                (void)saved;
            }
            CancelReaderSearch();
            reader_next::Cleanup();
            KillTimer(hWnd, reader_next::kSearchOverviewTimer);
            g_searchOverview.Clear();
            DragAcceptFiles(hWnd, FALSE);
            g_hwndMain = nullptr;
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    using SetDpi = BOOL(WINAPI*)(HANDLE);
    const auto setDpi = reinterpret_cast<SetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (setDpi) setDpi(reinterpret_cast<HANDLE>(-4));
    ui::ScopedAuxiliaryWindowCorners cornerScope;
    ScopedComInit scopedCom;
    ScopedPdfiumLibrary scopedPdfium;
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_TAB_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    const wchar_t* CLASS_NAME = kViewerWindowClass;

    WNDCLASSW hc = { };
    hc.lpfnWndProc   = HighlightBarProc;
    hc.hInstance     = hInstance;
    hc.lpszClassName = L"HighlightBarClass";
    hc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    hc.hbrBackground = NULL;
    RegisterClassW(&hc);

    WNDCLASSW wc = { };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);

    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        localization::Text(L"readonly.window.title").c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860,
        NULL,
        NULL,
        hInstance,
        NULL
    );

    if (hwnd == NULL) {
        return 0;
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring startup_path;
    std::wstring startup_folder;
    int startupPageIndex = -1;
    double startupPdfYPt = 0.0;
    bool hasStartupPdfY = false;
    HWND transferSource = nullptr;
    ULONG_PTR transferToken = 0;
    int transferRotation = 0;
    if (argv) {
        for (int index = 1; index < argc; ++index) {
            const std::wstring argument = argv[index] ? argv[index] : L"";
            if ((argument == L"--pdf" || argument == L"--open") && index + 1 < argc) {
                startup_path = argv[++index] ? argv[index] : L"";
            } else if (argument == L"--folder" && index + 1 < argc) {
                startup_folder = argv[++index] ? argv[index] : L"";
            } else if (argument == L"--transfer-source" && index + 1 < argc) {
                try { transferSource = reinterpret_cast<HWND>(static_cast<UINT_PTR>(std::stoull(argv[++index]))); }
                catch (...) { transferSource = nullptr; }
            } else if (argument == L"--transfer-token" && index + 1 < argc) {
                try { transferToken = static_cast<ULONG_PTR>(std::stoull(argv[++index])); }
                catch (...) { transferToken = 0; }
            } else if (argument == L"--transfer-rotation" && index + 1 < argc) {
                const std::wstring value = argv[++index] ? argv[index] : L"";
                if (value.size() == 1 && value[0] >= L'0' && value[0] <= L'3') transferRotation = value[0] - L'0';
            } else if (argument == L"--page" && index + 1 < argc) {
                try {
                    const std::wstring value = argv[++index] ? argv[index] : L"";
                    size_t parsed = 0;
                    const int oneBased = std::stoi(value, &parsed);
                    if (parsed == value.size() && oneBased > 0) startupPageIndex = oneBased - 1;
                } catch (...) {
                }
            } else if (argument == L"--pdf-y-pt" && index + 1 < argc) {
                try {
                    const std::wstring value = argv[++index] ? argv[index] : L"";
                    size_t parsed = 0;
                    const double yPt = std::stod(value, &parsed);
                    if (parsed == value.size() && std::isfinite(yPt)) {
                        startupPdfYPt = std::clamp(yPt, -50000.0, 50000.0);
                        hasStartupPdfY = true;
                    }
                } catch (...) {
                }
            } else if (argument == L"--annotations" || argument == L"--theme" ||
                       argument == L"--theme-id" || argument == L"--theme-inline") {
                if (index + 1 < argc) ++index;
            } else if (argument.rfind(L"--", 0) != 0 && startup_path.empty()) {
                startup_path = argument;
            }
        }
    }
    if (!startup_folder.empty()) {
        std::error_code folderEc;
        if (IsSupportedLocalPath(startup_folder) &&
            std::filesystem::is_directory(startup_folder, folderEc) && !folderEc) {
            PopulateFileTree(startup_folder);
            SetViewerStatus(hwnd, L"");
        }
    } else if (!startup_path.empty()) {
        std::filesystem::path p(startup_path);
        std::wstring dirPath = p.parent_path().wstring();
        if (dirPath.empty()) dirPath = L".";
        PopulateFileTree(dirPath);

        const bool opened = OpenPathInTab(hwnd, startup_path);
        if (opened && g_hwndPdfPanel && startupPageIndex >= 0) {
            readonly_viewer::PdfPreviewPanel_JumpToPdfLocation(g_hwndPdfPanel,
                                                                startupPageIndex,
                                                                startupPdfYPt,
                                                                hasStartupPdfY);
        }
        if (opened && transferSource && transferToken != 0) {
            if (HasActivePdfPreview() && IsTrustedReadonlyViewerWindow(transferSource))
                readonly_viewer::PdfPreviewPanel_SetRotation(g_hwndPdfPanel, transferRotation);
            SendTabTransferAcknowledgement(transferSource, transferToken);
        }
    } else {
        RestoreReadonlySession(hwnd);
        HMENU menu = GetMenu(hwnd);
        HMENU settings = menu ? GetSubMenu(menu, 3) : nullptr;
        if (settings) CheckMenuItem(settings, kPersistSessionMenuId,
                                    MF_BYCOMMAND | (g_persistSessionEnabled ? MF_CHECKED : MF_UNCHECKED));
        if (!g_tabs.empty() || !g_sessionFolder.empty()) {
            // A restored session already chose the folder/tab state; never replace it
            // with a release-directory scan.
        } else {
        const std::filesystem::path exePath = ViewerExecutablePath();
        if (exePath.empty()) {
            PopulateReleaseDocuments({});
            if (argv) LocalFree(argv);
            DestroyWindow(hwnd);
            return 0;
        }
        const std::filesystem::path exeDir = exePath.parent_path();
        const std::filesystem::path rootsToScan[] = {
            exeDir,
            exeDir.parent_path(),
        };

        std::vector<std::wstring> dirsToScan;
        std::error_code existsEc;
        auto tryAddPath = [&](const std::filesystem::path& candidate) {
            existsEc.clear();
            if (candidate.empty()) return;
            if (!std::filesystem::exists(candidate, existsEc) || existsEc) return;
            const std::wstring value = candidate.wstring();
            if (std::find(dirsToScan.begin(), dirsToScan.end(), value) == dirsToScan.end()) {
                dirsToScan.push_back(value);
            }
        };

        for (const auto& releaseRoot : rootsToScan) {
            tryAddPath(releaseRoot / L"docs");
            tryAddPath(releaseRoot / L"licenses");
            tryAddPath(releaseRoot / L"README.md");
        }

        if (!dirsToScan.empty()) {
            PopulateReleaseDocuments(dirsToScan);
        } else {
            PopulateReleaseDocuments({});
        }
        }
    }
    if (argv) {
        LocalFree(argv);
    }

    ShowCurrentTab();
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (HandleReaderKey(msg)) continue;
        if (g_hwndEditControl && (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST)) {
            bool isTabTarget = false;
            if (msg.hwnd == g_hwndTabControl || msg.hwnd == g_hwndFileTree || msg.hwnd == g_hwndTocTree) {
                isTabTarget = true;
            }
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB) {
                isTabTarget = true;
            }
            if (!isTabTarget && !IsChild(hwnd, msg.hwnd)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
