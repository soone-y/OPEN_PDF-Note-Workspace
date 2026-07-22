#include "theme/built_in_theme.h"
#include "clrop/json.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
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

#include "note/note_model.h"
#include "note/note_parser.h"
#include "clrop/types.h"
#include "readonly_viewer/mermaid_subset_parser.h"
#include "readonly_viewer/mermaid_subset_preview.h"

#include <windowsx.h>
#include <algorithm>

WNDPROC g_OriginalTabProc = nullptr;
bool g_isDraggingTab = false;
int g_draggedTabIndex = -1;

LRESULT CALLBACK TabSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include "pdf_preview_panel.h"
#include "text_preview_panel.h"
#include "fpdfview.h"

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
HWND g_hwndTabControl = NULL;
HWND g_hwndEditControl = NULL;
HWND g_hwndDecoratedButton = NULL;
HWND g_hwndRawButton = NULL;
HWND g_hwndHexButton = NULL;
HWND g_hwndDiagramButton = NULL;
HWND g_hwndOpenFileButton = NULL;
HWND g_hwndOpenFolderButton = NULL;
HWND g_hwndPdfRangeButton = NULL;
HWND g_hwndCancelLoadButton = NULL;
HWND g_hwndViewMenuButton = NULL;
HWND g_hwndMain = NULL;
HWND g_hwndPdfPanel = NULL;
HWND g_hwndDetachedDiagram = NULL;
WNDPROC g_originalEditProc = nullptr;
int g_splitX = 250;
bool g_isDraggingSplitter = false;
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
constexpr int kViewMenuButtonId = 114;
constexpr int kDetachedDiagramZoomInButtonId = 201;
constexpr int kDetachedDiagramZoomOutButtonId = 202;
constexpr int kDetachedDiagramResetButtonId = 203;
constexpr wchar_t kDetachedDiagramWindowClass[] = L"PdfNoteDetachedMermaidDiagram";
constexpr UINT kTabContextMenuMessage = WM_APP + 4;
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

enum class ViewMode {
    Decorated,
    Raw,
    Hex,
    Diagram,
    Pdf,
};

enum TextStyle : unsigned int {
    TextStyleNone = 0,
    TextStyleBold = 1 << 0,
    TextStyleItalic = 1 << 1,
    TextStyleCode = 1 << 2,
    TextStyleStrike = 1 << 3,
    TextStyleLink = 1 << 4,
};

struct StyleRun {
    size_t start = 0;
    size_t end = 0;
    unsigned int style = TextStyleNone;
    int headingLevel = 0;
};

struct RenderedDocument {
    std::wstring text;
    std::vector<StyleRun> styles;
};

struct InlineDiagram {
    size_t text_offset = 0;
    textviewer::mermaid::DiagramModel model;
};

std::wstring_view TrimMarkdownLine(std::wstring_view line);

struct ByteRange {
    uintmax_t start = 0;
    uintmax_t end = 0;
};

std::vector<std::wstring> g_filePaths;

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
    ViewMode viewMode = ViewMode::Raw;
    std::vector<int> jpLines;
    int totalLines = 1;
};
std::vector<OpenTab> g_tabs;
std::vector<OpenTab> g_closedTabs;
int g_currentTab = -1;
textviewer::mermaid::MermaidSubsetPreview g_mermaidPreview;
textviewer::mermaid::MermaidSubsetPreview g_detachedMermaidPreview;

struct InlineDiagramPreview {
    size_t text_offset = 0;
    std::unique_ptr<textviewer::mermaid::MermaidSubsetPreview> preview;
};
std::vector<InlineDiagramPreview> g_inlineDiagramPreviews;

WNDPROC g_originalTabProc = nullptr;
int g_draggingTab = -1;
HWND g_hwndHighlightBar = NULL;

void PopulateFileTree(const std::wstring& directoryPath);
void OpenDetachedDiagramWindow();
void UpdateInlineDiagramBounds();
void SetViewerStatus(HWND owner, const std::wstring& message);

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
                if (tab.totalLines > 0 && !tab.jpLines.empty()) {
                    HBRUSH hlBrush = CreateSolidBrush(RGB(255, 165, 0)); // Orange highlight
                    int height = rc.bottom - rc.top;
                    int width = rc.right - rc.left;
                    
                    for (int line : tab.jpLines) {
                        int y = (int)(((long long)line * height) / tab.totalLines);
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
                if (tab.totalLines > 0 && g_hwndEditControl) {
                    RECT rc;
                    GetClientRect(hWnd, &rc);
                    int height = rc.bottom - rc.top;
                    if (height > 0) {
                        int y = GET_Y_LPARAM(lParam);
                        int targetLine = (int)(((long long)y * tab.totalLines) / height);
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
            AppendMenuW(menu, MF_STRING, kTabMenuClose, L"このタブを閉じる");
            AppendMenuW(menu, MF_STRING, kTabMenuCloseOthers, L"他のタブを閉じる");
            AppendMenuW(menu, MF_STRING, kTabMenuCloseToRight, L"右側のタブを閉じる");
            if (g_tabs[index].isPartialText) AppendMenuW(menu, MF_STRING, kTabMenuChangeRange, L"表示範囲を変更...");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, index > 0 ? MF_STRING : MF_STRING | MF_GRAYED,
                        kTabMenuMoveLeft, L"左へ移動");
            AppendMenuW(menu, index + 1 < static_cast<int>(g_tabs.size()) ? MF_STRING : MF_STRING | MF_GRAYED,
                        kTabMenuMoveRight, L"右へ移動");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kTabMenuCloseAll, L"すべてのタブを閉じる");
            if (!g_closedTabs.empty()) AppendMenuW(menu, MF_STRING, kTabMenuReopenClosed, L"閉じたタブを再度開く");
            HMENU tabList = CreatePopupMenu();
            for (size_t tabIndex = 0; tabIndex < g_tabs.size() && tabIndex < 200; ++tabIndex) {
                AppendMenuW(tabList, MF_STRING | (tabIndex == static_cast<size_t>(index) ? MF_CHECKED : 0),
                            kTabMenuListBase + static_cast<UINT>(tabIndex), g_tabs[tabIndex].title.c_str());
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tabList), L"タブ一覧");

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
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            TCHITTESTINFO hti = {0};
            hti.pt.x = x;
            hti.pt.y = y;
            int idx = TabCtrl_HitTest(hWnd, &hti);
            if (idx >= 0) {
                RECT rc;
                TabCtrl_GetItemRect(hWnd, idx, &rc);
                if (x >= rc.right - 20) {
                    return 0;
                }
                g_draggingTab = idx;
                SetCapture(hWnd);
            }
            break;
        }
        case WM_LBUTTONUP: {
            if (GetCapture() == hWnd) {
                ReleaseCapture();
            }
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            TCHITTESTINFO hti = {0};
            hti.pt.x = x;
            hti.pt.y = y;
            int idx = TabCtrl_HitTest(hWnd, &hti);
            
            if (idx >= 0) {
                RECT rc;
                TabCtrl_GetItemRect(hWnd, idx, &rc);
                if (x >= rc.right - 20) {
                    SendMessageW(GetParent(hWnd), WM_APP + 1, idx, 0);
                    g_draggingTab = -1;
                    return 0;
                }
            }
            
            if (g_draggingTab >= 0 && idx >= 0 && idx != g_draggingTab) {
                SendMessageW(GetParent(hWnd), WM_APP + 2, g_draggingTab, idx);
            }
            g_draggingTab = -1;
            break;
        }
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
    wss << L"=== Binary File Preview (Hex Viewer Mode) ===\n\n";

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
    if (g_hwndCancelLoadButton) {
        EnableWindow(g_hwndCancelLoadButton, TRUE);
        ShowWindow(g_hwndCancelLoadButton, SW_SHOW);
    }
    UpdateWindow(owner);
}

void EndCancellableLoad() {
    g_loadInProgress = false;
    if (g_hwndCancelLoadButton) ShowWindow(g_hwndCancelLoadButton, SW_HIDE);
}

std::wstring DecodeTextBytes(const std::vector<unsigned char>& bytes) {
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        const int wsize = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(bytes.data()) + 3,
                                              static_cast<int>(bytes.size()) - 3, nullptr, 0);
        std::wstring result(wsize, 0);
        if (wsize > 0) MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(bytes.data()) + 3,
                                           static_cast<int>(bytes.size()) - 3, result.data(), wsize);
        return result;
    }
    bool is_utf8 = true;
    for (size_t i = 0; i < bytes.size();) {
        const unsigned char c = bytes[i];
        if (c <= 0x7F) { ++i; continue; }
        const size_t tail = c >= 0xC2 && c <= 0xDF ? 1 : (c >= 0xE0 && c <= 0xEF ? 2 : (c >= 0xF0 && c <= 0xF4 ? 3 : 0));
        if (tail == 0 || i + tail >= bytes.size()) { is_utf8 = false; break; }
        bool valid = true;
        for (size_t j = 1; j <= tail; ++j) valid = valid && (bytes[i + j] & 0xC0) == 0x80;
        if (!valid) { is_utf8 = false; break; }
        i += tail + 1;
    }
    const UINT code_page = is_utf8 ? CP_UTF8 : 932;
    const int wsize = MultiByteToWideChar(code_page, 0, reinterpret_cast<const char*>(bytes.data()),
                                          static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring result(wsize, 0);
    if (wsize > 0) MultiByteToWideChar(code_page, 0, reinterpret_cast<const char*>(bytes.data()),
                                       static_cast<int>(bytes.size()), result.data(), wsize);
    return result;
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
        result.text = L"Error: File cannot be accessed.";
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
            result.text = L"Error: Selected range is too large (Limit: 256MB).";
            result.hasError = true;
            return result;
        }
    }

    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) {
        result.text = L"Error: Could not open file.";
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
            result.text = L"読み込みを中止しました。";
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
        result.text = L"Error: Could not read the complete file.";
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
    std::wstring summary = L"選択範囲: " + std::to_wstring(state.start / 10) + L"% ～ " +
        std::to_wstring(state.end / 10) + L"%\r\n推定表示量: " + std::to_wstring(megabytes) +
        L" MB / 約 " + std::to_wstring(estimated_characters) + L" 文字";
    SetWindowTextW(GetDlgItem(hwnd, kRangeSummaryId), summary.c_str());
    std::wstring warning;
    if (bytes > kMaximumRangeLoadBytes) warning = L"この範囲は256MBを超えるため開けません。範囲を狭めてください。";
    else if (available_memory > 0 && estimated_working_set > available_memory / 2)
        warning = L"強い警告: 現在利用できるメモリに対して表示量が大きすぎます。範囲を狭めてください。";
    else if (available_memory > 0 && estimated_working_set > available_memory / 4)
        warning = L"注意: 現在の空きメモリでは表示が遅くなる可能性があります。";
    else if (bytes > 128ull * 1024ull * 1024ull) warning = L"強い警告: 大量のメモリを使用し、表示が遅くなる可能性があります。";
    else if (bytes > 32ull * 1024ull * 1024ull) warning = L"注意: 環境によっては表示に時間がかかります。";
    else warning = L"負荷: 軽い";
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
        CreateWindowExW(0, L"STATIC", L"大きなテキストです。数直線で開く連続範囲を指定してください。",
                        WS_CHILD | WS_VISIBLE, 16, 16, 500, 22, hwnd, nullptr, instance, nullptr);
        CreateWindowExW(0, L"STATIC", L"選択範囲（■ の部分を開きます）", WS_CHILD | WS_VISIBLE, 16, 44, 500, 20, hwnd, nullptr, instance, nullptr);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 64, 500, 22, hwnd, reinterpret_cast<HMENU>(kRangeVisualBarId), instance, nullptr);
        CreateWindowExW(0, L"STATIC", L"開始つまみ", WS_CHILD | WS_VISIBLE, 16, 92, 90, 20, hwnd, nullptr, instance, nullptr);
        HWND start = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                     110, 86, 400, 32, hwnd, reinterpret_cast<HMENU>(kRangeStartSliderId), instance, nullptr);
        CreateWindowExW(0, L"STATIC", L"終了つまみ", WS_CHILD | WS_VISIBLE, 16, 132, 90, 20, hwnd, nullptr, instance, nullptr);
        HWND end = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                   110, 126, 400, 32, hwnd, reinterpret_cast<HMENU>(kRangeEndSliderId), instance, nullptr);
        for (HWND slider : {start, end}) { SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000)); SendMessageW(slider, TBM_SETTICFREQ, 100, 0); }
        SendMessageW(start, TBM_SETPOS, TRUE, state->start);
        SendMessageW(end, TBM_SETPOS, TRUE, state->end);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 170, 500, 42, hwnd, reinterpret_cast<HMENU>(kRangeSummaryId), instance, nullptr);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 218, 500, 36, hwnd, reinterpret_cast<HMENU>(kRangeWarningId), instance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"この範囲を開く", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        284, 264, 130, 28, hwnd, reinterpret_cast<HMENU>(kRangeConfirmId), instance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"キャンセル", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
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
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, kRangeDialogClass, L"表示範囲を指定",
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

void AppendStyledText(RenderedDocument& document, std::wstring_view text, unsigned int style, int headingLevel) {
    if (text.empty()) return;
    const size_t start = document.text.size();
    document.text.append(text);
    const size_t end = document.text.size();
    if (style == TextStyleNone && headingLevel == 0) return;
    if (!document.styles.empty()) {
        StyleRun& previous = document.styles.back();
        if (previous.end == start && previous.style == style && previous.headingLevel == headingLevel) {
            previous.end = end;
            return;
        }
    }
    document.styles.push_back({start, end, style, headingLevel});
}

void RenderInlineMarkdown(
    std::wstring_view source,
    size_t start,
    size_t end,
    RenderedDocument& document,
    unsigned int style,
    int headingLevel) {
    size_t pos = start;
    while (pos < end) {
        if (source[pos] == L'\\' && pos + 1 < end) {
            AppendStyledText(document, source.substr(pos + 1, 1), style, headingLevel);
            pos += 2;
            continue;
        }

        if (source[pos] == L'`') {
            const size_t close = source.find(L'`', pos + 1);
            if (close != std::wstring_view::npos && close < end) {
                AppendStyledText(document, source.substr(pos + 1, close - pos - 1), style | TextStyleCode, headingLevel);
                pos = close + 1;
                continue;
            }
        }

        const bool strongMarker = pos + 1 < end &&
            ((source[pos] == L'*' && source[pos + 1] == L'*') ||
             (source[pos] == L'_' && source[pos + 1] == L'_'));
        if (strongMarker) {
            const std::wstring_view marker = source.substr(pos, 2);
            const size_t close = source.find(marker, pos + 2);
            if (close != std::wstring_view::npos && close < end) {
                RenderInlineMarkdown(source, pos + 2, close, document, style | TextStyleBold, headingLevel);
                pos = close + 2;
                continue;
            }
        }

        if (pos + 1 < end && source[pos] == L'~' && source[pos + 1] == L'~') {
            const size_t close = source.find(L"~~", pos + 2);
            if (close != std::wstring_view::npos && close < end) {
                RenderInlineMarkdown(source, pos + 2, close, document, style | TextStyleStrike, headingLevel);
                pos = close + 2;
                continue;
            }
        }

        if (source[pos] == L'*' || source[pos] == L'_') {
            const wchar_t marker = source[pos];
            const size_t close = source.find(marker, pos + 1);
            if (close != std::wstring_view::npos && close < end && close > pos + 1) {
                RenderInlineMarkdown(source, pos + 1, close, document, style | TextStyleItalic, headingLevel);
                pos = close + 1;
                continue;
            }
        }

        const bool image = source[pos] == L'!' && pos + 1 < end && source[pos + 1] == L'[';
        const bool link = source[pos] == L'[';
        if (image || link) {
            const size_t labelStart = pos + (image ? 2 : 1);
            const size_t labelEnd = source.find(L']', labelStart);
            if (labelEnd != std::wstring_view::npos && labelEnd + 1 < end && source[labelEnd + 1] == L'(') {
                const size_t targetEnd = source.find(L')', labelEnd + 2);
                if (targetEnd != std::wstring_view::npos && targetEnd < end) {
                    if (image) AppendStyledText(document, L"画像: ", style, headingLevel);
                    RenderInlineMarkdown(source, labelStart, labelEnd, document, style | TextStyleLink, headingLevel);
                    pos = targetEnd + 1;
                    continue;
                }
            }
        }

        AppendStyledText(document, source.substr(pos, 1), style, headingLevel);
        ++pos;
    }
}

bool IsHorizontalRule(std::wstring_view line) {
    wchar_t marker = 0;
    int count = 0;
    for (wchar_t c : line) {
        if (iswspace(c)) continue;
        if (c != L'-' && c != L'*' && c != L'_') return false;
        if (marker == 0) marker = c;
        if (c != marker) return false;
        ++count;
    }
    return count >= 3;
}

bool ParseMarkdownTableRow(std::wstring_view line, std::vector<std::wstring>* cells) {
    cells->clear();
    while (!line.empty() && iswspace(line.front())) line.remove_prefix(1);
    while (!line.empty() && iswspace(line.back())) line.remove_suffix(1);
    if (line.empty() || line.find(L'|') == std::wstring_view::npos) return false;
    if (line.front() == L'|') line.remove_prefix(1);
    if (!line.empty() && line.back() == L'|') line.remove_suffix(1);
    size_t start = 0;
    while (start <= line.size()) {
        const size_t end = line.find(L'|', start);
        std::wstring_view cell = line.substr(start, end == std::wstring_view::npos ? line.size() - start : end - start);
        while (!cell.empty() && iswspace(cell.front())) cell.remove_prefix(1);
        while (!cell.empty() && iswspace(cell.back())) cell.remove_suffix(1);
        if (cell.size() > 256 || cells->size() >= 16) return false;
        cells->emplace_back(cell);
        if (end == std::wstring_view::npos) break;
        start = end + 1;
    }
    return !cells->empty();
}

bool IsMarkdownTableSeparator(const std::vector<std::wstring>& cells) {
    if (cells.empty()) return false;
    for (const std::wstring& value : cells) {
        size_t first = 0, last = value.size();
        if (first < last && value[first] == L':') ++first;
        if (first < last && value[last - 1] == L':') --last;
        if (last - first < 3) return false;
        for (size_t index = first; index < last; ++index) if (value[index] != L'-') return false;
    }
    return true;
}

void AppendMarkdownTable(RenderedDocument& document, const std::vector<std::vector<std::wstring>>& rows) {
    if (rows.empty()) return;
    const size_t column_count = rows.front().size();
    std::vector<size_t> widths(column_count, 3);
    for (const auto& row : rows) for (size_t column = 0; column < column_count; ++column)
        widths[column] = std::min<size_t>(32, std::max(widths[column], row[column].size()));
    auto border = [&](wchar_t left, wchar_t join, wchar_t right, wchar_t fill) {
        std::wstring value(1, left);
        for (size_t column = 0; column < column_count; ++column) {
            value.append(widths[column] + 2, fill);
            value += column + 1 == column_count ? right : join;
        }
        return value;
    };
    AppendStyledText(document, border(L'┌', L'┬', L'┐', L'─'), TextStyleCode, 0); document.text.push_back(L'\n');
    for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
        std::wstring line = L"│";
        for (size_t column = 0; column < column_count; ++column) {
            std::wstring cell = rows[row_index][column];
            if (cell.size() > widths[column]) cell = cell.substr(0, widths[column] - 1) + L"…";
            line += L" " + cell + std::wstring(widths[column] - cell.size() + 1, L' ') + L"│";
        }
        AppendStyledText(document, line, TextStyleCode | (row_index == 0 ? TextStyleBold : TextStyleNone), 0);
        document.text.push_back(L'\n');
        if (row_index == 0) { AppendStyledText(document, border(L'├', L'┼', L'┤', L'─'), TextStyleCode, 0); document.text.push_back(L'\n'); }
    }
    AppendStyledText(document, border(L'└', L'┴', L'┘', L'─'), TextStyleCode, 0); document.text.push_back(L'\n');
}

RenderedDocument RenderMarkdown(const std::wstring& source, std::vector<InlineDiagram>* inline_diagrams = nullptr) {
    RenderedDocument document;
    bool inCodeBlock = false;
    bool inMermaidBlock = false;
    std::wstring mermaid_source;
    size_t diagram_number = 0;
    size_t lineStart = 0;
    while (lineStart <= source.size()) {
        size_t lineEnd = source.find(L'\n', lineStart);
        if (lineEnd == std::wstring::npos) lineEnd = source.size();
        size_t contentEnd = lineEnd;
        if (contentEnd > lineStart && source[contentEnd - 1] == L'\r') --contentEnd;
        std::wstring_view line(source.data() + lineStart, contentEnd - lineStart);

        size_t first = 0;
        while (first < line.size() && first < 3 && line[first] == L' ') ++first;
        const std::wstring_view trimmed = TrimMarkdownLine(line);
        if (!inMermaidBlock && !inCodeBlock) {
            std::vector<std::wstring> header;
            const size_t separator_start = lineEnd == source.size() ? source.size() : lineEnd + 1;
            const size_t separator_end = source.find(L'\n', separator_start);
            const size_t separator_limit = separator_end == std::wstring::npos ? source.size() : separator_end;
            std::wstring_view separator(source.data() + separator_start, separator_limit - separator_start);
            if (!separator.empty() && separator.back() == L'\r') separator.remove_suffix(1);
            std::vector<std::wstring> separator_cells;
            if (ParseMarkdownTableRow(line, &header) && ParseMarkdownTableRow(separator, &separator_cells) &&
                header.size() == separator_cells.size() && IsMarkdownTableSeparator(separator_cells)) {
                std::vector<std::vector<std::wstring>> rows;
                rows.push_back(std::move(header));
                size_t next_start = separator_end == std::wstring::npos ? source.size() : separator_end + 1;
                while (next_start < source.size() && rows.size() < 512) {
                    const size_t next_end = source.find(L'\n', next_start);
                    const size_t next_limit = next_end == std::wstring::npos ? source.size() : next_end;
                    std::wstring_view candidate(source.data() + next_start, next_limit - next_start);
                    if (!candidate.empty() && candidate.back() == L'\r') candidate.remove_suffix(1);
                    std::vector<std::wstring> cells;
                    if (!ParseMarkdownTableRow(candidate, &cells) || cells.size() != rows.front().size()) break;
                    rows.push_back(std::move(cells));
                    next_start = next_end == std::wstring::npos ? source.size() : next_end + 1;
                }
                AppendMarkdownTable(document, rows);
                lineStart = next_start;
                continue;
            }
        }
        if (inMermaidBlock) {
            if (trimmed.size() >= 3 && trimmed.substr(0, 3) == L"```") {
                const textviewer::mermaid::ParseResult result = textviewer::mermaid::ParseFlowchart(mermaid_source);
                if (result.can_render() && inline_diagrams) {
                    ++diagram_number;
                    const size_t text_offset = document.text.size();
                    AppendStyledText(document, L"図 " + std::to_wstring(diagram_number) + L"  （右クリックで図表一覧）\n", TextStyleItalic, 0);
                    // Reserve vertical space in the RichEdit document for the child preview.
                    for (int blank_line = 0; blank_line < 15; ++blank_line) document.text.push_back(L'\n');
                    inline_diagrams->push_back({text_offset, result.model});
                } else {
                    AppendStyledText(document, mermaid_source, TextStyleCode, 0);
                    if (!mermaid_source.empty() && mermaid_source.back() != L'\n') document.text.push_back(L'\n');
                }
                mermaid_source.clear();
                inMermaidBlock = false;
            } else {
                mermaid_source.append(line);
                mermaid_source.push_back(L'\n');
            }
        } else if (trimmed == L"```mermaid") {
            inMermaidBlock = true;
            mermaid_source.clear();
        } else if (line.substr(first, 3) == L"```") {
            inCodeBlock = !inCodeBlock;
        } else if (inCodeBlock) {
            AppendStyledText(document, line, TextStyleCode, 0);
            document.text.push_back(L'\n');
        } else {
            int headingLevel = 0;
            size_t contentStart = first;
            while (contentStart + headingLevel < line.size() && headingLevel < 6 &&
                   line[contentStart + headingLevel] == L'#') {
                ++headingLevel;
            }
            if (headingLevel > 0 && contentStart + headingLevel < line.size() &&
                iswspace(line[contentStart + headingLevel])) {
                contentStart += headingLevel;
                while (contentStart < line.size() && iswspace(line[contentStart])) ++contentStart;
                size_t headingEnd = line.size();
                while (headingEnd > contentStart && iswspace(line[headingEnd - 1])) --headingEnd;
                while (headingEnd > contentStart && line[headingEnd - 1] == L'#') --headingEnd;
                while (headingEnd > contentStart && iswspace(line[headingEnd - 1])) --headingEnd;
                RenderInlineMarkdown(line, contentStart, headingEnd, document, TextStyleBold, headingLevel);
                document.text.push_back(L'\n');
            } else if (IsHorizontalRule(line)) {
                AppendStyledText(document, L"────────────────────────", TextStyleNone, 0);
                document.text.push_back(L'\n');
            } else {
                contentStart = 0;
                while (contentStart < line.size() && iswspace(line[contentStart])) ++contentStart;
                unsigned int baseStyle = TextStyleNone;
                if (contentStart < line.size() && line[contentStart] == L'>') {
                    AppendStyledText(document, L"│ ", TextStyleItalic, 0);
                    ++contentStart;
                    if (contentStart < line.size() && line[contentStart] == L' ') ++contentStart;
                    baseStyle |= TextStyleItalic;
                } else if (contentStart + 1 < line.size() &&
                           (line[contentStart] == L'-' || line[contentStart] == L'*' || line[contentStart] == L'+') &&
                           iswspace(line[contentStart + 1])) {
                    AppendStyledText(document, L"• ", TextStyleBold, 0);
                    contentStart += 2;
                    while (contentStart < line.size() && iswspace(line[contentStart])) ++contentStart;
                    if (contentStart + 2 < line.size() && line[contentStart] == L'[' && line[contentStart + 2] == L']') {
                        const wchar_t checked = static_cast<wchar_t>(towlower(line[contentStart + 1]));
                        AppendStyledText(document, checked == L'x' ? L"☑ " : L"☐ ", TextStyleNone, 0);
                        contentStart += 3;
                        while (contentStart < line.size() && iswspace(line[contentStart])) ++contentStart;
                    }
                }
                RenderInlineMarkdown(line, contentStart, line.size(), document, baseStyle, 0);
                document.text.push_back(L'\n');
            }
        }

        if (lineEnd == source.size()) break;
        lineStart = lineEnd + 1;
    }
    return document;
}

bool IsMarkdownPath(const std::wstring& path) {
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    if (extension == L".md" || extension == L".markdown") return true;
    std::wstring filename = std::filesystem::path(path).filename().wstring();
    std::transform(filename.begin(), filename.end(), filename.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return filename == L"readme";
}

std::wstring_view TrimMarkdownLine(std::wstring_view line) {
    while (!line.empty() && iswspace(line.front())) line.remove_prefix(1);
    while (!line.empty() && iswspace(line.back())) line.remove_suffix(1);
    return line;
}

std::vector<std::wstring_view> FindMermaidBlocks(std::wstring_view source) {
    std::vector<std::wstring_view> blocks;
    bool in_mermaid_block = false;
    size_t content_start = 0;
    size_t line_start = 0;
    while (line_start <= source.size()) {
        size_t line_end = source.find(L'\n', line_start);
        if (line_end == std::wstring_view::npos) line_end = source.size();
        std::wstring_view line = source.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        const std::wstring_view trimmed = TrimMarkdownLine(line);
        if (!in_mermaid_block) {
            if (trimmed == L"```mermaid") {
                in_mermaid_block = true;
                content_start = line_end == source.size() ? line_end : line_end + 1;
            }
        } else if (trimmed.size() >= 3 && trimmed.substr(0, 3) == L"```") {
            blocks.push_back(source.substr(content_start, line_start - content_start));
            in_mermaid_block = false;
        }
        if (line_end == source.size()) break;
        line_start = line_end + 1;
    }
    return blocks;
}

bool TryBuildRenderableMermaid(std::wstring_view source, textviewer::mermaid::DiagramModel* out_model) {
    textviewer::mermaid::DiagramModel combined;
    size_t diagram_number = 0;
    for (const std::wstring_view block : FindMermaidBlocks(source)) {
        textviewer::mermaid::ParseResult result = textviewer::mermaid::ParseFlowchart(block);
        if (!result.can_render()) continue;
        ++diagram_number;
        const size_t node_base = combined.nodes.size();
        const size_t group_base = combined.groups.size();
        const size_t wrapper_group = combined.groups.size();
        combined.groups.push_back({L"Diagram " + std::to_wstring(diagram_number), textviewer::mermaid::kNoDiagramGroup});
        for (auto group : result.model.groups) {
            group.parent_group = group.parent_group == textviewer::mermaid::kNoDiagramGroup
                ? wrapper_group : group.parent_group + group_base + 1;
            combined.groups.push_back(std::move(group));
        }
        for (auto node : result.model.nodes) {
            node.group_index = node.group_index == textviewer::mermaid::kNoDiagramGroup
                ? wrapper_group : node.group_index + group_base + 1;
            node.id = L"d" + std::to_wstring(diagram_number) + L"_" + node.id;
            combined.nodes.push_back(std::move(node));
        }
        for (auto edge : result.model.edges) {
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
            CreateWindowExW(0, L"BUTTON", L"拡大", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                            8, 6, 64, 24, hWnd, reinterpret_cast<HMENU>(kDetachedDiagramZoomInButtonId), instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"縮小", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                            78, 6, 64, 24, hWnd, reinterpret_cast<HMENU>(kDetachedDiagramZoomOutButtonId), instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"等倍", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                            148, 6, 64, 24, hWnd, reinterpret_cast<HMENU>(kDetachedDiagramResetButtonId), instance, nullptr);
            if (!g_detachedMermaidPreview.Create(hWnd, instance, 204)) return -1;
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

void OpenDetachedDiagramWindow() {
    if (g_currentTab < 0 || g_currentTab >= static_cast<int>(g_tabs.size())) return;
    textviewer::mermaid::DiagramModel model;
    if (!TryBuildRenderableMermaid(g_tabs[g_currentTab].rawText, &model)) return;

    if (!g_hwndDetachedDiagram) {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = DetachedDiagramWndProc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = kDetachedDiagramWindowClass;
        RegisterClassW(&window_class);
        g_hwndDetachedDiagram = CreateWindowExW(WS_EX_TOOLWINDOW, kDetachedDiagramWindowClass, L"Mermaid 図表",
                                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 700,
                                                 g_hwndMain, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!g_hwndDetachedDiagram) return;
    }
    g_detachedMermaidPreview.SetModel(std::move(model));
    ShowWindow(g_hwndDetachedDiagram, SW_SHOWNORMAL);
    SetForegroundWindow(g_hwndDetachedDiagram);
}

bool IsClropPath(const std::wstring& path) {
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return extension == L".clro" || extension == L".clrop";
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
        case clrop::Item::Kind::Text: return L"テキスト";
        case clrop::Item::Kind::Math: return L"数式";
        case clrop::Item::Kind::MarkerText: return L"テキストマーカー";
        case clrop::Item::Kind::TextColor: return L"文字色";
        case clrop::Item::Kind::MarkerFree: return L"フリーマーカー";
        case clrop::Item::Kind::Line: return L"線";
        case clrop::Item::Kind::Arrow: return L"矢印";
        case clrop::Item::Kind::Wave: return L"波線";
        case clrop::Item::Kind::Freehand: return L"手書き";
        case clrop::Item::Kind::LinkMarker: return L"リンクマーカー";
        case clrop::Item::Kind::Shape: return L"図形";
    }
    return L"注釈";
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
        AppendDecoratedLine(document, L"CLRO を読み取れません", TextStyleBold, 1);
        AppendDecoratedLine(document, L"Raw表示で内容を確認してください。");
        return document;
    }

    const std::string json(tab.bytes.begin(), tab.bytes.end());
    clrop::Document clropDocument;
    std::wstring error;
    if (!clrop::ParseClropFromJson(json, clropDocument, error)) {
        AppendDecoratedLine(document, L"CLRO を解析できません", TextStyleBold, 1);
        AppendDecoratedLine(document, L"Raw表示で内容を確認してください。");
        if (!error.empty()) AppendDecoratedLine(document, error, TextStyleCode);
        return document;
    }

    size_t itemCount = 0;
    std::map<std::wstring, size_t> kindCounts;
    for (const clrop::Page& page : clropDocument.pages) {
        itemCount += page.items.size();
        for (const clrop::Item& item : page.items) ++kindCounts[ClropItemKindLabel(item.kind)];
    }

    AppendDecoratedLine(document, L"CLRO 注釈ファイル", TextStyleBold, 1);
    AppendDecoratedLine(document, L"形式バージョン: " + std::to_wstring(clropDocument.version));
    if (!clropDocument.pdfId.path.empty()) {
        AppendDecoratedLine(document, L"対象PDF: " + clropDocument.pdfId.path);
    }
    if (clropDocument.pdfId.pageCount > 0) {
        AppendDecoratedLine(document, L"PDFページ数: " + std::to_wstring(clropDocument.pdfId.pageCount));
    }
    AppendDecoratedLine(document, L"注釈ページ: " + std::to_wstring(clropDocument.pages.size()) +
        L"　注釈数: " + std::to_wstring(itemCount));

    if (!kindCounts.empty()) {
        AppendDecoratedLine(document, L"注釈の種類", TextStyleBold, 2);
        for (const auto& [kind, count] : kindCounts) {
            AppendDecoratedLine(document, L"• " + kind + L": " + std::to_wstring(count));
        }
    }

    AppendDecoratedLine(document, L"ページ別の注釈", TextStyleBold, 2);
    for (const clrop::Page& page : clropDocument.pages) {
        AppendDecoratedLine(document,
            L"ページ " + std::to_wstring(page.page + 1) + L"（" + std::to_wstring(page.items.size()) + L"件）",
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

void UpdateTOC(const std::wstring& text) {
    TreeView_DeleteAllItems(g_hwndTocTree);
    note::NoteMetadata meta;
    meta.file_name = L"preview.md";
    meta.title = L"preview";
    note::NoteTextModel model = note::MakeNoteTextModel(meta, text, 0);
    note::NoteDocument doc = note::ParseNoteDocument(model);

    HTREEITEM parentStack[10];
    for (int i = 0; i < 10; ++i) parentStack[i] = TVI_ROOT;

    for (const auto& block : doc.blocks) {
        if (block.kind == note::BlockKind::Heading) {
            int level = block.level;
            if (level < 1) level = 1;
            if (level > 6) level = 6;
            
            std::wstring headingText;
            for (size_t i = 0; i < block.inline_count; ++i) {
                const auto& inlineNode = doc.inlines[block.first_inline + i];
                if (inlineNode.kind == note::InlineKind::Text || inlineNode.kind == note::InlineKind::Code) {
                    headingText += text.substr(inlineNode.span.start.value, inlineNode.span.end.value - inlineNode.span.start.value);
                }
            }
            if (headingText.empty()) headingText = L"Heading " + std::to_wstring(level);
            
            TVINSERTSTRUCTW tvis = {0};
            tvis.hParent = parentStack[level - 1];
            tvis.hInsertAfter = TVI_LAST;
            tvis.item.mask = TVIF_TEXT | TVIF_PARAM;
            tvis.item.pszText = (LPWSTR)headingText.c_str();
            tvis.item.lParam = block.loc.line;
            HTREEITEM newItem = TreeView_InsertItem(g_hwndTocTree, &tvis);
            for (int i = level; i < 10; ++i) {
                parentStack[i] = newItem;
            }
        }
    }
}

void ApplyEditorText(const std::wstring& text, const std::vector<StyleRun>& styles, bool monospace) {
    if (!g_hwndEditControl) return;

    SendMessageW(g_hwndEditControl, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(g_hwndEditControl, text.c_str());
    if (g_isRichEdit) {
        SendMessageW(g_hwndEditControl, EM_SETBKGNDCOLOR, 0, RGB(255, 255, 255));
        SendMessageW(g_hwndEditControl, EM_SETSEL, 0, -1);

        CHARFORMAT2W base{};
        base.cbSize = sizeof(base);
        base.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT | CFM_BACKCOLOR;
        base.dwEffects = CFE_AUTOBACKCOLOR;
        base.yHeight = monospace ? 210 : 220;
        base.crTextColor = RGB(35, 39, 47);
        base.crBackColor = RGB(255, 255, 255);
        wcsncpy(base.szFaceName, monospace ? L"Consolas" : L"Segoe UI", LF_FACESIZE - 1);
        base.szFaceName[LF_FACESIZE - 1] = L'\0';
        SendMessageW(g_hwndEditControl, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&base));

        for (const StyleRun& run : styles) {
            if (run.start >= run.end || run.end > text.size()) continue;
            SendMessageW(g_hwndEditControl, EM_SETSEL, static_cast<WPARAM>(run.start), static_cast<LPARAM>(run.end));
            CHARFORMAT2W format{};
            format.cbSize = sizeof(format);
            format.dwMask = CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT | CFM_COLOR | CFM_SIZE;
            format.dwEffects = 0;
            format.crTextColor = RGB(35, 39, 47);
            format.yHeight = 220;
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
    SendMessageW(g_hwndEditControl, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hwndEditControl, NULL, TRUE);
}

void ClearInlineDiagramPreviews() {
    for (InlineDiagramPreview& inline_diagram : g_inlineDiagramPreviews) {
        inline_diagram.preview->SetVisible(false);
    }
    g_inlineDiagramPreviews.clear();
}

void UpdateInlineDiagramBounds() {
    if (!g_hwndEditControl || !g_hwndMain) return;

    RECT edit_rect{};
    GetWindowRect(g_hwndEditControl, &edit_rect);
    POINT edit_origin{edit_rect.left, edit_rect.top};
    ScreenToClient(g_hwndMain, &edit_origin);
    const int preview_width = std::max(120, static_cast<int>(edit_rect.right - edit_rect.left - 28));
    constexpr int kPreviewHeight = 240;

    for (InlineDiagramPreview& inline_diagram : g_inlineDiagramPreviews) {
        const LRESULT position = SendMessageW(g_hwndEditControl, EM_POSFROMCHAR,
                                              static_cast<WPARAM>(inline_diagram.text_offset), 0);
        const int y = static_cast<short>(HIWORD(position));
        const bool is_visible = y + kPreviewHeight >= 0 && y <= edit_rect.bottom - edit_rect.top;
        inline_diagram.preview->SetBounds(edit_origin.x + 12, edit_origin.y + y + 24,
                                          preview_width, kPreviewHeight);
        inline_diagram.preview->SetVisible(is_visible);
        if (is_visible) SetWindowPos(inline_diagram.preview->GetWindow(), HWND_TOP, 0, 0, 0, 0,
                                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void BuildInlineDiagramPreviews(const std::vector<InlineDiagram>& diagrams) {
    ClearInlineDiagramPreviews();
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    for (const InlineDiagram& diagram : diagrams) {
        auto preview = std::make_unique<textviewer::mermaid::MermaidSubsetPreview>();
        if (!preview->Create(g_hwndMain, instance, 0)) continue;
        preview->SetModel(diagram.model);
        preview->SetDetachedWindowTarget(g_hwndMain);
        g_inlineDiagramPreviews.push_back({diagram.text_offset, std::move(preview)});
    }
    UpdateInlineDiagramBounds();
}

LRESULT CALLBACK InlineDiagramEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    const LRESULT result = CallWindowProcW(g_originalEditProc, hwnd, message, wParam, lParam);
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
    if (g_hwndDecoratedButton) {
        SendMessageW(g_hwndDecoratedButton, BM_SETCHECK, mode == ViewMode::Decorated ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (g_hwndRawButton) {
        SendMessageW(g_hwndRawButton, BM_SETCHECK, mode == ViewMode::Raw ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (g_hwndHexButton) {
        SendMessageW(g_hwndHexButton, BM_SETCHECK, mode == ViewMode::Hex ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (g_hwndDiagramButton) {
        SendMessageW(g_hwndDiagramButton, BM_SETCHECK, mode == ViewMode::Diagram ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

void ShowCurrentTab() {
    if (g_currentTab < 0 || g_currentTab >= static_cast<int>(g_tabs.size())) {
        ClearInlineDiagramPreviews();
        g_mermaidPreview.SetVisible(false);
        if (g_hwndPdfPanel) ShowWindow(g_hwndPdfPanel, SW_HIDE);
        if (g_hwndPdfRangeButton) ShowWindow(g_hwndPdfRangeButton, SW_HIDE);
        if (g_hwndEditControl) ShowWindow(g_hwndEditControl, SW_SHOW);
        if (g_hwndHighlightBar) ShowWindow(g_hwndHighlightBar, SW_SHOW);
        ApplyEditorText(L"", {}, false);
        UpdateModeButtons(ViewMode::Raw);
        if (g_hwndDiagramButton) EnableWindow(g_hwndDiagramButton, FALSE);
        TreeView_DeleteAllItems(g_hwndTocTree);
        return;
    }

    OpenTab& tab = g_tabs[g_currentTab];
    textviewer::mermaid::DiagramModel mermaid_model;
    const bool has_renderable_mermaid = !tab.isBinary && IsMarkdownPath(tab.path) &&
        TryBuildRenderableMermaid(tab.rawText, &mermaid_model);
    if (g_hwndDiagramButton) EnableWindow(g_hwndDiagramButton, has_renderable_mermaid ? TRUE : FALSE);
    const bool showDiagram = (tab.viewMode == ViewMode::Diagram && has_renderable_mermaid);
    const bool showPdf = (tab.viewMode == ViewMode::Pdf);
    if (g_hwndPdfRangeButton) ShowWindow(g_hwndPdfRangeButton, showPdf ? SW_SHOW : SW_HIDE);
    if (showPdf) {
        if (g_hwndDecoratedButton) ShowWindow(g_hwndDecoratedButton, SW_HIDE);
        if (g_hwndRawButton) ShowWindow(g_hwndRawButton, SW_HIDE);
        if (g_hwndHexButton) ShowWindow(g_hwndHexButton, SW_HIDE);
        if (g_hwndDiagramButton) ShowWindow(g_hwndDiagramButton, SW_HIDE);
    } else {
        if (g_hwndDecoratedButton) ShowWindow(g_hwndDecoratedButton, SW_SHOW);
        if (g_hwndRawButton) ShowWindow(g_hwndRawButton, SW_SHOW);
        if (g_hwndHexButton) ShowWindow(g_hwndHexButton, SW_SHOW);
        if (g_hwndDiagramButton) ShowWindow(g_hwndDiagramButton, SW_SHOW);
    }
    
    g_mermaidPreview.SetVisible(showDiagram);
    if (g_hwndPdfPanel) {
        ShowWindow(g_hwndPdfPanel, showPdf ? SW_SHOW : SW_HIDE);
    }
    
    if (showDiagram || showPdf) {
        if (g_hwndEditControl) ShowWindow(g_hwndEditControl, SW_HIDE);
        if (g_hwndHighlightBar) ShowWindow(g_hwndHighlightBar, SW_HIDE);
    } else {
        if (g_hwndEditControl) ShowWindow(g_hwndEditControl, SW_SHOW);
        if (g_hwndHighlightBar) ShowWindow(g_hwndHighlightBar, SW_SHOW);
    }
    if (tab.viewMode != ViewMode::Decorated || IsClropPath(tab.path)) {
        ClearInlineDiagramPreviews();
    }
    switch (tab.viewMode) {
        case ViewMode::Decorated: {
            std::vector<InlineDiagram> inline_diagrams;
            const RenderedDocument rendered = IsClropPath(tab.path)
                ? RenderClropDocument(tab)
                : RenderMarkdown(tab.rawText, &inline_diagrams);
            ApplyEditorText(rendered.text, rendered.styles, false);
            BuildInlineDiagramPreviews(inline_diagrams);
            break;
        }
        case ViewMode::Raw:
            if (tab.isBinary && tab.rawText.empty()) {
                ApplyEditorText(L"Binary file. Select Hex to inspect its bytes.", {}, true);
            } else {
                ApplyEditorText(tab.rawText, {}, true);
            }
            break;
        case ViewMode::Hex:
            ApplyEditorText(BuildHexText(tab.bytes), {}, true);
            break;
        case ViewMode::Diagram:
            if (has_renderable_mermaid) {
                g_mermaidPreview.SetModel(std::move(mermaid_model));
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
                }
            }
            break;
    }

    UpdateModeButtons(tab.viewMode);
    if (!tab.isBinary && tab.viewMode != ViewMode::Hex && !IsClropPath(tab.path)) {
        UpdateTOC(tab.rawText);
    } else {
        TreeView_DeleteAllItems(g_hwndTocTree);
    }
    if (g_hwndHighlightBar) InvalidateRect(g_hwndHighlightBar, NULL, TRUE);
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
    BeginCancellableLoad(owner, L"読み込み中…");
    LoadFileResult loaded = LoadTextFile(tab.path, range);
    EndCancellableLoad();
    if (loaded.hasError || loaded.cancelled) { SetViewerStatus(owner, loaded.text); return; }
    tab.rawText = std::move(loaded.text);
    tab.bytes = std::move(loaded.bytes);
    tab.isBinary = loaded.isBinary;
    tab.textRange = *range;
    tab.sourceSize = loaded.source_size;
    tab.viewMode = tab.isBinary ? ViewMode::Hex : (IsMarkdownPath(tab.path) ? ViewMode::Decorated : ViewMode::Raw);
    ParseJapaneseLines(tab);
    SelectTab(index);
}

void SetViewerStatus(HWND owner, const std::wstring& message) {
    const std::wstring title = message.empty() ? L"Text Viewer" : L"Text Viewer - " + message;
    SetWindowTextW(owner, title.c_str());
}

bool IsSupportedLocalPath(const std::wstring& path) {
    if (path.empty() || path.rfind(L"\\\\", 0) == 0) return false;
    const std::filesystem::path filePath(path);
    const std::filesystem::path root = filePath.root_path();
    if (!root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
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
        SetViewerStatus(owner, L"Windows file picker is not available.");
        return {};
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        options |= pickFolder ? FOS_PICKFOLDERS : (FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT);
        dialog->SetOptions(options);
    }
    dialog->SetTitle(pickFolder ? L"閲覧するフォルダーを開く" : L"閲覧するファイルを開く");
    if (!pickFolder) {
        COMDLG_FILTERSPEC filters[] = {
            {L"PDF, Text and Markdown", L"*.pdf;*.txt;*.md;*.markdown;*.json;*.clro;*.sha256"},
            {L"PDF files", L"*.pdf"},
            {L"Text and Markdown", L"*.txt;*.md;*.markdown;*.json;*.clro;*.sha256"},
            {L"All files", L"*.*"},
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
        SetViewerStatus(owner, L"Windows file picker failed.");
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

bool OpenPathInTab(HWND owner, const std::wstring& path) {
    if (!IsSupportedLocalPath(path)) {
        SetViewerStatus(owner, L"Only local, non-link files can be opened.");
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        SetViewerStatus(owner, L"The selected path is not a readable file.");
        return false;
    }

    for (size_t i = 0; i < g_tabs.size(); ++i) {
        if (_wcsicmp(g_tabs[i].path.c_str(), path.c_str()) == 0) {
            TabCtrl_SetCurSel(g_hwndTabControl, static_cast<int>(i));
            g_currentTab = static_cast<int>(i);
            ShowCurrentTab();
            SetViewerStatus(owner, L"");
            return true;
        }
    }

    BeginCancellableLoad(owner, L"読み込み中…");
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
        BeginCancellableLoad(owner, L"選択範囲を読み込み中…");
        loaded = LoadTextFile(path, selected_range);
        EndCancellableLoad();
        if (loaded.cancelled) {
            SetViewerStatus(owner, loaded.text);
            return false;
        }
    }
    OpenTab tab;
    tab.title = std::filesystem::path(path).filename().wstring();
    if (selected_range) tab.title += L" （一部）";
    tab.path = path;
    tab.rawText = std::move(loaded.text);
    tab.bytes = std::move(loaded.bytes);
    tab.isBinary = loaded.isBinary;
    tab.isPdf = loaded.isPdf;
    tab.loadError = loaded.hasError;
    tab.isPartialText = selected_range.has_value() && !tab.loadError;
    tab.textRange = selected_range.value_or(ByteRange{});
    tab.sourceSize = loaded.source_size;
    tab.viewMode = tab.loadError ? ViewMode::Raw :
        (tab.isPdf ? ViewMode::Pdf : (tab.isBinary ? ViewMode::Hex : (IsMarkdownPath(path) ? ViewMode::Decorated : ViewMode::Raw)));
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
    SetViewerStatus(owner, L"");
    return true;
}

void PopulateReleaseDocuments(const std::vector<std::wstring>& paths) {
    TreeView_DeleteAllItems(g_hwndFileTree);
    g_filePaths.clear();
    
    TVINSERTSTRUCTW tvis = {0};
    tvis.hParent = TVI_ROOT;
    tvis.hInsertAfter = TVI_LAST;
    tvis.item.mask = TVIF_TEXT | TVIF_PARAM;
    std::wstring rootName = L"Release Documents";
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
    TreeView_DeleteAllItems(g_hwndFileTree);
    g_filePaths.clear();
    
    TVINSERTSTRUCTW tvis = {0};
    tvis.hParent = TVI_ROOT;
    tvis.hInsertAfter = TVI_LAST;
    tvis.item.mask = TVIF_TEXT | TVIF_PARAM;
    tvis.item.pszText = (LPWSTR)directoryPath.c_str();
    tvis.item.lParam = -1;
    HTREEITEM hRoot = TreeView_InsertItem(g_hwndFileTree, &tvis);

    try {
        for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
            if (entry.is_regular_file()) {
                g_filePaths.push_back(entry.path().wstring());
                size_t index = g_filePaths.size() - 1;
                
                std::wstring filename = entry.path().filename().wstring();
                tvis.hParent = hRoot;
                tvis.item.mask = TVIF_TEXT | TVIF_PARAM;
                tvis.item.pszText = (LPWSTR)filename.c_str();
                tvis.item.lParam = (LPARAM)index;
                TreeView_InsertItem(g_hwndFileTree, &tvis);
            }
        }
    } catch (...) {}
    TreeView_Expand(g_hwndFileTree, hRoot, TVE_EXPAND);
}


LRESULT CALLBACK TabSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            TCHITTESTINFO hitinfo = {};
            hitinfo.pt.x = GET_X_LPARAM(lParam);
            hitinfo.pt.y = GET_Y_LPARAM(lParam);
            int idx = TabCtrl_HitTest(hwnd, &hitinfo);
            if (idx >= 0 && g_tabs.size() > 1) {
                g_isDraggingTab = true;
                g_draggedTabIndex = idx;
                SetCapture(hwnd);
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (g_isDraggingTab) {
                // nothing
            }
            break;
        }
        case WM_LBUTTONUP: {
            if (g_isDraggingTab) {
                g_isDraggingTab = false;
                ReleaseCapture();

                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ClientToScreen(hwnd, &pt);
                HWND targetWnd = WindowFromPoint(pt);
                
                if (targetWnd != g_hwndMain && targetWnd != g_hwndTabControl && targetWnd != g_hwndEditControl && targetWnd != g_hwndTocTree && targetWnd != g_hwndPdfPanel && !IsChild(g_hwndMain, targetWnd)) {
                    if (g_draggedTabIndex >= 0 && g_draggedTabIndex < (int)g_tabs.size()) {
                        std::wstring path = g_tabs[g_draggedTabIndex].path;
                        
                        wchar_t exePath[MAX_PATH];
                        GetModuleFileNameW(NULL, exePath, MAX_PATH);
                        std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" \"" + path + L"\"";
                        
                        STARTUPINFOW si = { sizeof(si) };
                        PROCESS_INFORMATION pi = { 0 };
                        if (CreateProcessW(NULL, &cmdLine[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                            CloseHandle(pi.hProcess);
                            CloseHandle(pi.hThread);
                            
                            g_tabs.erase(g_tabs.begin() + g_draggedTabIndex);
                            TabCtrl_DeleteItem(g_hwndTabControl, g_draggedTabIndex);
                            if (g_tabs.empty()) {
                                PostMessageW(g_hwndMain, WM_CLOSE, 0, 0);
                            } else {
                                if (g_currentTab >= (int)g_tabs.size()) g_currentTab = (int)g_tabs.size() - 1;
                                else if (g_currentTab > g_draggedTabIndex) g_currentTab--;
                                else if (g_currentTab == g_draggedTabIndex) g_currentTab = std::max(0, g_currentTab - 1);
                                
                                TabCtrl_SetCurSel(g_hwndTabControl, g_currentTab);
                                ShowCurrentTab();
                            }
                        }
                    }
                }
            }
            break;
        }
    }
    if (g_OriginalTabProc) {
        return CallWindowProc(g_OriginalTabProc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            HINSTANCE hInst = ((LPCREATESTRUCT)lParam)->hInstance;
            g_hwndMain = hWnd;
            g_hwndPdfPanel = readonly_viewer::CreatePdfPreviewPanel(hWnd, hInst, 1002);
            if (g_hwndPdfPanel) ShowWindow(g_hwndPdfPanel, SW_HIDE);
            g_hwndFileTree = CreateWindowExW(0, WC_TREEVIEWW, L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
                0, 0, 0, 0, hWnd, (HMENU)101, hInst, NULL);
            g_hwndTocTree = CreateWindowExW(0, WC_TREEVIEWW, L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
                0, 0, 0, 0, hWnd, (HMENU)102, hInst, NULL);
            g_hwndTabControl = CreateWindowExW(0, WC_TABCONTROLW, L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_OWNERDRAWFIXED | TCS_FOCUSNEVER | TCS_SCROLLOPPOSITE,
                0, 0, 0, 0, hWnd, (HMENU)103, hInst, NULL);

        g_OriginalTabProc = (WNDPROC)SetWindowLongPtrW(g_hwndTabControl, GWLP_WNDPROC, (LONG_PTR)TabSubclassProc);
            g_originalTabProc = (WNDPROC)SetWindowLongPtrW(g_hwndTabControl, GWLP_WNDPROC, (LONG_PTR)TabProc);
            g_richEditModule = LoadLibraryW(L"Msftedit.dll");
            const wchar_t* editClass = g_richEditModule ? MSFTEDIT_CLASS : L"EDIT";
            g_isRichEdit = g_richEditModule != NULL;
            g_hwndEditControl = CreateWindowExW(0, editClass, L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_WANTRETURN | ES_NOHIDESEL,
                0, 0, 0, 0, hWnd, (HMENU)104, hInst, NULL);
            if (!g_hwndEditControl && g_isRichEdit) {
                g_isRichEdit = false;
                g_hwndEditControl = CreateWindowExW(0, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_WANTRETURN | ES_NOHIDESEL,
                    0, 0, 0, 0, hWnd, (HMENU)104, hInst, NULL);
            }
            if (g_hwndEditControl) {
                g_originalEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                    g_hwndEditControl, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InlineDiagramEditProc)));
            }
            g_hwndHighlightBar = CreateWindowExW(0, L"HighlightBarClass", L"",
                WS_CHILD | WS_VISIBLE,
                0, 0, 0, 0, hWnd, (HMENU)105, hInst, NULL);
            g_hwndOpenFileButton = CreateWindowExW(0, L"BUTTON", L"ファイルを開く  Ctrl+O",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, hWnd, (HMENU)kOpenFileButtonId, hInst, NULL);
            g_hwndOpenFolderButton = CreateWindowExW(0, L"BUTTON", L"フォルダーを開く",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, hWnd, (HMENU)kOpenFolderButtonId, hInst, NULL);
            g_hwndDecoratedButton = CreateWindowExW(0, L"BUTTON", L"本文表示  Ctrl+1",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP,
                0, 0, 0, 0, hWnd, (HMENU)kDecoratedButtonId, hInst, NULL);
            g_hwndRawButton = CreateWindowExW(0, L"BUTTON", L"Raw  Ctrl+2",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                0, 0, 0, 0, hWnd, (HMENU)kRawButtonId, hInst, NULL);
            g_hwndHexButton = CreateWindowExW(0, L"BUTTON", L"Hex  Ctrl+3",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                0, 0, 0, 0, hWnd, (HMENU)kHexButtonId, hInst, NULL);
            g_hwndDiagramButton = CreateWindowExW(0, L"BUTTON", L"図一覧  Ctrl+4",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                0, 0, 0, 0, hWnd, (HMENU)kDiagramButtonId, hInst, NULL);
            g_hwndPdfRangeButton = CreateWindowExW(0, L"BUTTON", L"ページ範囲...",
                WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, hWnd, (HMENU)kPdfRangeButtonId, hInst, NULL);
            g_hwndCancelLoadButton = CreateWindowExW(0, L"BUTTON", L"読み込み中止",
                WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, hWnd, (HMENU)kCancelLoadButtonId, hInst, NULL);
            g_hwndViewMenuButton = CreateWindowExW(0, L"BUTTON", L"表示 ▼",
                WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, hWnd, (HMENU)kViewMenuButtonId, hInst, NULL);
            g_mermaidPreview.Create(hWnd, hInst, 112);
            g_mermaidPreview.SetDetachedWindowTarget(hWnd);
            UpdateModeButtons(ViewMode::Raw);
            DragAcceptFiles(hWnd, TRUE);
            return 0;
        }
        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            int splitX = g_splitX;
            if (splitX > width - 50) splitX = width - 50;
            if (splitX < 50) splitX = 50;

            int leftPaneHeight = height;
            int rightPaneWidth = width - splitX - 5; // 5px for splitter

            int fileTreeHeight = leftPaneHeight / 2;
            int tocTreeHeight = leftPaneHeight - fileTreeHeight;

            if (g_hwndFileTree) MoveWindow(g_hwndFileTree, 0, 0, splitX, fileTreeHeight, TRUE);
            if (g_hwndTocTree) MoveWindow(g_hwndTocTree, 0, fileTreeHeight, splitX, tocTreeHeight, TRUE);

            int tabHeight = 24;
            int modeBarHeight = 36;
            const bool compactControls = rightPaneWidth < 840;
            if (g_hwndTabControl) MoveWindow(g_hwndTabControl, splitX + 5, 0, rightPaneWidth, tabHeight, TRUE);
            if (g_hwndOpenFileButton) {
                SetWindowTextW(g_hwndOpenFileButton, compactControls ? L"開く" : L"ファイルを開く  Ctrl+O");
                MoveWindow(g_hwndOpenFileButton, splitX + 13, tabHeight + 5, compactControls ? 80 : 165, 26, TRUE);
            }
            if (g_hwndOpenFolderButton) ShowWindow(g_hwndOpenFolderButton, compactControls ? SW_HIDE : SW_SHOW);
            if (!compactControls && g_hwndOpenFolderButton) MoveWindow(g_hwndOpenFolderButton, splitX + 184, tabHeight + 5, 140, 26, TRUE);
            const int modeLeft = splitX + (compactControls ? 99 : 330);
            const int modeWidth = compactControls ? 58 : 110;
            if (g_hwndDecoratedButton) { SetWindowTextW(g_hwndDecoratedButton, compactControls ? L"本文" : L"本文表示  Ctrl+1"); MoveWindow(g_hwndDecoratedButton, modeLeft, tabHeight + 5, compactControls ? 64 : 130, 26, TRUE); }
            if (g_hwndRawButton) { SetWindowTextW(g_hwndRawButton, L"Raw"); MoveWindow(g_hwndRawButton, modeLeft + (compactControls ? 70 : 136), tabHeight + 5, modeWidth, 26, TRUE); }
            if (g_hwndHexButton) { SetWindowTextW(g_hwndHexButton, L"Hex"); MoveWindow(g_hwndHexButton, modeLeft + (compactControls ? 134 : 252), tabHeight + 5, modeWidth, 26, TRUE); }
            if (g_hwndDiagramButton) { SetWindowTextW(g_hwndDiagramButton, compactControls ? L"図" : L"図一覧  Ctrl+4"); MoveWindow(g_hwndDiagramButton, modeLeft + (compactControls ? 198 : 368), tabHeight + 5, modeWidth, 26, TRUE); }
            if (g_hwndViewMenuButton) {
                ShowWindow(g_hwndViewMenuButton, compactControls ? SW_SHOW : SW_HIDE);
                MoveWindow(g_hwndViewMenuButton, splitX + 99, tabHeight + 5, 80, 26, TRUE);
                if (compactControls) {
                    ShowWindow(g_hwndDecoratedButton, SW_HIDE); ShowWindow(g_hwndRawButton, SW_HIDE);
                    ShowWindow(g_hwndHexButton, SW_HIDE); ShowWindow(g_hwndDiagramButton, SW_HIDE);
                } else if (g_currentTab >= 0 && g_currentTab < static_cast<int>(g_tabs.size()) &&
                           g_tabs[g_currentTab].viewMode != ViewMode::Pdf) {
                    ShowWindow(g_hwndDecoratedButton, SW_SHOW); ShowWindow(g_hwndRawButton, SW_SHOW);
                    ShowWindow(g_hwndHexButton, SW_SHOW); ShowWindow(g_hwndDiagramButton, SW_SHOW);
                }
            }
            if (g_hwndPdfRangeButton) MoveWindow(g_hwndPdfRangeButton, modeLeft, tabHeight + 5, compactControls ? 130 : 150, 26, TRUE);
            if (g_hwndCancelLoadButton) MoveWindow(g_hwndCancelLoadButton, splitX + 13, tabHeight + 5, compactControls ? 100 : 120, 26, TRUE);
            
            int highlightBarWidth = 12;
            int editWidth = rightPaneWidth - highlightBarWidth;
            const int contentTop = tabHeight + modeBarHeight;
            if (g_hwndEditControl) MoveWindow(g_hwndEditControl, splitX + 5, contentTop, editWidth, height - contentTop, TRUE);
            if (g_hwndHighlightBar) MoveWindow(g_hwndHighlightBar, splitX + 5 + editWidth, contentTop, highlightBarWidth, height - contentTop, TRUE);
            if (g_hwndPdfPanel) MoveWindow(g_hwndPdfPanel, splitX + 5, contentTop, rightPaneWidth, height - contentTop, TRUE);
            g_mermaidPreview.SetBounds(splitX + 5, contentTop, rightPaneWidth, height - contentTop);
            UpdateInlineDiagramBounds();
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case kOpenFileButtonId: {
                    const std::vector<std::wstring> paths = PromptForLocalPaths(hWnd, false);
                    for (const std::wstring& path : paths) OpenPathInTab(hWnd, path);
                    return 0;
                }
                case kOpenFolderButtonId: {
                    const std::vector<std::wstring> paths = PromptForLocalPaths(hWnd, true);
                    if (!paths.empty() && IsSupportedLocalPath(paths.front())) {
                        PopulateFileTree(paths.front());
                        SetViewerStatus(hWnd, L"");
                    } else if (!paths.empty()) {
                        SetViewerStatus(hWnd, L"Only local, non-link folders can be opened.");
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
                case kCancelLoadButtonId:
                    if (g_loadInProgress) {
                        g_loadCancelRequested = true;
                        EnableWindow(g_hwndCancelLoadButton, FALSE);
                    }
                    return 0;
                case kViewMenuButtonId: {
                    HMENU menu = CreatePopupMenu();
                    if (!menu) return 0;
                    AppendMenuW(menu, MF_STRING, kDecoratedButtonId, L"本文表示\tCtrl+1");
                    AppendMenuW(menu, MF_STRING, kRawButtonId, L"Raw\tCtrl+2");
                    AppendMenuW(menu, MF_STRING, kHexButtonId, L"Hex\tCtrl+3");
                    AppendMenuW(menu, MF_STRING | (g_hwndDiagramButton && IsWindowEnabled(g_hwndDiagramButton) ? 0 : MF_GRAYED), kDiagramButtonId, L"図一覧\tCtrl+4");
                    RECT rect{}; GetWindowRect(g_hwndViewMenuButton, &rect);
                    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN, rect.left, rect.bottom, 0, hWnd, nullptr);
                    DestroyMenu(menu);
                    if (command) SendMessageW(hWnd, WM_COMMAND, command, 0);
                    return 0;
                }
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            int xPos = GET_X_LPARAM(lParam);
            if (xPos >= g_splitX && xPos <= g_splitX + 5) {
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
                if (xPos >= g_splitX && xPos <= g_splitX + 5) {
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
            if (lpnmhdr->hwndFrom == g_hwndFileTree && lpnmhdr->code == TVN_SELCHANGEDW) {
                LPNMTREEVIEWW lpnmtv = (LPNMTREEVIEWW)lParam;
                if (lpnmtv->itemNew.hItem && lpnmtv->itemNew.lParam >= 0 && lpnmtv->itemNew.lParam < (LPARAM)g_filePaths.size()) {
                    std::wstring path = g_filePaths[lpnmtv->itemNew.lParam];
                    OpenPathInTab(hWnd, path);
                }
            } else if (lpnmhdr->hwndFrom == g_hwndTabControl && lpnmhdr->code == TCN_SELCHANGE) {
                int idx = TabCtrl_GetCurSel(g_hwndTabControl);
                if (idx >= 0 && idx < (int)g_tabs.size()) {
                    g_currentTab = idx;
                    ShowCurrentTab();
                }
            }
            return 0;
        }
        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT pdis = (LPDRAWITEMSTRUCT)lParam;
            if (pdis->hwndItem == g_hwndTabControl) {
                int tabIdx = pdis->itemID;
                if (tabIdx >= 0 && tabIdx < (int)g_tabs.size()) {
                    HDC hdc = pdis->hDC;
                    RECT rc = pdis->rcItem;
                    
                    const bool isSelected = (pdis->itemState & ODS_SELECTED) != 0;
                    const ThemeColors theme = readonly_viewer::PdfPreviewPanel_GetTheme();
                    HBRUSH hbg = CreateSolidBrush(isSelected ? theme.selectionBg : theme.toolbarBg);
                    FillRect(hdc, &rc, hbg);
                    DeleteObject(hbg);
                    
                    SetBkMode(hdc, TRANSPARENT);
                    SetTextColor(hdc, isSelected ? theme.selectionText : theme.toolbarText);
                    
                    std::wstring title = g_tabs[tabIdx].title;
                    RECT textRc = rc;
                    textRc.left += 5;
                    textRc.right -= 20; 
                    DrawTextW(hdc, title.c_str(), -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    
                    RECT xRc = rc;
                    xRc.left = rc.right - 20;
                    SetTextColor(hdc, theme.buttonBorder);
                    DrawTextW(hdc, L" x ", -1, &xRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
            }
            return 0;
        }
        case textviewer::mermaid::kOpenDetachedDiagramMessage:
            OpenDetachedDiagramWindow();
            return 0;
        case WM_CLOSE:
            if (g_loadInProgress) {
                g_loadCancelRequested = true;
                return 0;
            }
            break;
        case WM_DESTROY:
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
    const HRESULT coInitializeResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialized = SUCCEEDED(coInitializeResult);
    FPDF_InitLibrary();
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_TAB_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    const wchar_t CLASS_NAME[] = L"PdfReadonlyViewerWindow";

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
        L"Read-Only Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768,
        NULL,
        NULL,
        hInstance,
        NULL
    );

    if (hwnd == NULL) {
        FPDF_DestroyLibrary();
        if (comInitialized) CoUninitialize();
        return 0;
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring startup_path;
    if (argv) {
        for (int index = 1; index < argc; ++index) {
            const std::wstring argument = argv[index] ? argv[index] : L"";
            if ((argument == L"--pdf" || argument == L"--open") && index + 1 < argc) {
                startup_path = argv[++index] ? argv[index] : L"";
            } else if (argument == L"--page" || argument == L"--pdf-y-pt" || argument == L"--annotations" ||
                       argument == L"--theme" || argument == L"--theme-id" || argument == L"--theme-inline") {
                if (index + 1 < argc) ++index;
            } else if (argument.rfind(L"--", 0) != 0 && startup_path.empty()) {
                startup_path = argument;
            }
        }
    }
    if (!startup_path.empty()) {
        std::filesystem::path p(startup_path);
        std::wstring dirPath = p.parent_path().wstring();
        if (dirPath.empty()) dirPath = L".";
        PopulateFileTree(dirPath);

        OpenPathInTab(hwnd, startup_path);
    } else {
        wchar_t exePathBuf[MAX_PATH]{};
        const DWORD exePathLength = GetModuleFileNameW(NULL, exePathBuf, MAX_PATH);
        if (exePathLength == 0 || exePathLength >= MAX_PATH) {
            PopulateReleaseDocuments({});
            if (argv) LocalFree(argv);
            DestroyWindow(hwnd);
            FPDF_DestroyLibrary();
            if (comInitialized) CoUninitialize();
            return 0;
        }
        std::filesystem::path exePath(exePathBuf);
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
            std::error_code currentPathEc;
            const std::filesystem::path currentPath = std::filesystem::current_path(currentPathEc);
            if (!currentPathEc) {
                PopulateFileTree(currentPath.wstring());
            } else {
                PopulateReleaseDocuments({});
            }
        }
    }
    if (argv) {
        LocalFree(argv);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (g_hwndEditControl && (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST)) {
            bool isTabTarget = false;
            if (msg.hwnd == g_hwndTabControl || msg.hwnd == g_hwndFileTree || msg.hwnd == g_hwndTocTree || 
                msg.hwnd == g_hwndDecoratedButton || msg.hwnd == g_hwndRawButton || 
                msg.hwnd == g_hwndHexButton || msg.hwnd == g_hwndDiagramButton ||
                msg.hwnd == g_hwndOpenFileButton || msg.hwnd == g_hwndOpenFolderButton) {
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

    FPDF_DestroyLibrary();
    if (comInitialized) CoUninitialize();
    return 0;
}
