#include "pdf_preview_panel.h"
#include "theme/built_in_theme.h"
#include "clrop/json.h"
#include "resources/app_resource.h"
#include "fpdfview.h"
#include "fpdf_text.h"
#include "fpdf_edit.h"
#include "fpdf_ppo.h"
#include "fpdf_save.h"
#include <windows.h>
#include <windowsx.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace readonly_viewer {
namespace pdf_panel {





namespace {

constexpr const wchar_t* kWindowClassName = L"PdfPreviewPanelClass";
constexpr int kDefaultWidth = 980;
constexpr int kDefaultHeight = 720;
constexpr int kMinRenderPx = 16;
constexpr int kMaxRenderPx = 12000;
constexpr size_t kMaxRenderedPageBytes = 64ull * 1024ull * 1024ull;
constexpr int kMagnifierRadiusPx = 60;
constexpr double kMagnifierZoom = 2.0;
constexpr int kContinuousPageGapPx = 24;
constexpr int kWheelScrollStepPx = 96;
constexpr int kKeyScrollStepPx = 72;
constexpr unsigned long long kMaxInputFileBytes = 1024ull * 1024ull * 1024ull;
constexpr unsigned long long kMaxSetupJsonBytes = 256ull * 1024ull;
constexpr unsigned long long kMaxReadonlySettingsJsonBytes = 64ull * 1024ull;
// ReadOnlyViewerWndProc timers:
//   0x7101 kFileWatchTimerId
//   0x7102 kTextSelectionAutoScrollTimerId
constexpr UINT_PTR kFileWatchTimerId = 0x7101;
constexpr UINT kFileWatchIntervalMs = 1000;
constexpr UINT_PTR kTextSelectionAutoScrollTimerId = 0x7102;
constexpr UINT kTextSelectionAutoScrollIntervalMs = 16;
constexpr double kTextSelectionAutoScrollMaxSpeedPx = 28.0;
constexpr ULONGLONG kReloadDebounceMs = 700;

HICON LoadReadonlyViewerIcon(HINSTANCE instance, int width, int height) {
    HICON icon = static_cast<HICON>(LoadImageW(instance,
                                               MAKEINTRESOURCEW(IDI_READONLY_VIEWER_ICON),
                                               IMAGE_ICON,
                                               width,
                                               height,
                                               LR_DEFAULTCOLOR | LR_SHARED));
    if (icon) return icon;
    return LoadIconW(nullptr, IDI_APPLICATION);
}

constexpr UINT kCmdOpen = 1001;
constexpr UINT kCmdNewWindow = 1002;
constexpr UINT kCmdLaunchMain = 1003;
constexpr UINT kCmdPdfInfo = 1004;
constexpr UINT kCmdExit = 1005;
constexpr UINT kCmdCloseAllViewers = 1006;
constexpr UINT kCmdExportAnnotatedPdf = 1007;
constexpr UINT kCmdPageRange = 1008;
constexpr UINT kCmdToggleAnnots = 1101;
constexpr UINT kCmdToggleMagnifier = 1102;
constexpr UINT kCmdTempMarker = 1103;
constexpr UINT kCmdTempPen = 1104;
constexpr UINT kCmdClearTempDrawing = 1105;
constexpr UINT kCmdToggleGrayscale = 1106;
constexpr UINT kCmdZoomIn = 1111;
constexpr UINT kCmdZoomOut = 1112;
constexpr UINT kCmdZoomReset = 1113;
constexpr UINT kCmdThemeBase = 1200;

struct LoadedPdf {
    std::wstring path;
    FPDF_DOCUMENT document = nullptr;
    int pageCount = 0;

    LoadedPdf() = default;
    LoadedPdf(const LoadedPdf&) = delete;
    LoadedPdf& operator=(const LoadedPdf&) = delete;

    ~LoadedPdf() {
        Close();
    }

    void Close() {
        if (document) {
            FPDF_CloseDocument(document);
            document = nullptr;
        }
        pageCount = 0;
        path.clear();
    }
};

struct LoadedClrop {
    std::wstring path;
    clrop::Document document;
};

struct RenderedPage {
    int pageIndex = -1;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<unsigned char> pixels;

    void Clear() {
        pageIndex = -1;
        width = 0;
        height = 0;
        stride = 0;
        pixels.clear();
    }

    bool Matches(int requestedPageIndex, int requestedWidth, int requestedHeight) const {
        return pageIndex == requestedPageIndex &&
               width == requestedWidth &&
               height == requestedHeight &&
               !pixels.empty();
    }
};

struct PageLayout {
    int pageIndex = -1;
    double pageWidthPt = 1.0;
    double pageHeightPt = 1.0;
    int width = 0;
    int height = 0;
    double docTop = 0.0;
    double docBottom = 0.0;
    RECT rect{};
};

enum class TempDrawTool {
    None,
    Marker,
    Pen
};

struct TempStroke {
    TempDrawTool tool = TempDrawTool::None;
    int pageIndex = -1;
    std::vector<double> path;
};

struct CommandLineOptions {
    std::wstring pdfPath;
    std::wstring clropPath;
    std::wstring themeId = theme::kDefaultBuiltInThemeId;
    std::wstring inlineThemeSpec;
    int initialPage = 0;
    double initialZoom = 1.0;
    double initialPanX = 0.0;
    double initialPanY = 0.0;
    double initialPdfYPt = 0.0;
    bool hasInitialPdfY = false;
    bool showAnnotations = true;
    bool showMagnifier = false;
};

struct FileMetadata {
    ULONGLONG size = 0;
    FILETIME lastWriteTime{};

    bool Equals(const FileMetadata& other) const {
        return size == other.size &&
               CompareFileTime(&lastWriteTime, &other.lastWriteTime) == 0;
    }
};

struct ViewerState {
    ThemeColors theme = theme::BuiltInThemeOrDefault(theme::kDefaultBuiltInThemeId);
    std::vector<ThemeColors> themeCatalog = theme::MakeBuiltInThemeCatalog();
    std::wstring status = L"PDF / CLROP read-only viewer";
    std::unique_ptr<LoadedPdf> pdf;
    std::unique_ptr<LoadedClrop> clrop;
    RenderedPage rendered;
    std::vector<RenderedPage> renderedPages;
    int currentPage = 0;
    // Zero-based, inclusive range currently participating in continuous view.
    // pageRangeLast < 0 means the complete document.
    int pageRangeFirst = 0;
    int pageRangeLast = -1;
    FileMetadata pdfWatchedMetadata{};
    FileMetadata pdfPendingMetadata{};
    FileMetadata clropWatchedMetadata{};
    FileMetadata clropPendingMetadata{};
    bool hasPdfWatchedMetadata = false;
    bool hasClropWatchedMetadata = false;
    bool pdfReloadPending = false;
    bool clropReloadPending = false;
    ULONGLONG pdfPendingSinceTick = 0;
    ULONGLONG clropPendingSinceTick = 0;
    bool showAnnotations = true;
    bool showPdfInfo = false;
    bool showMagnifier = false;
    bool showGrayscale = false;
    double zoomFactor = 1.0;
    double panX = 0.0;
    double panY = 0.0;
    double scrollY = 0.0;
    bool panning = false;
    bool drawingTempStroke = false;
    TempDrawTool tempTool = TempDrawTool::None;
    TempStroke activeTempStroke;
    std::vector<TempStroke> tempStrokes;
    POINT panStart{};
    double panStartX = 0.0;
    double panStartY = 0.0;
    POINT lastMouse{0, 0};
    int selectedPage = -1;
    int selectedItem = -1;
    bool selectingRect = false;
    bool hasRectSelection = false;
    POINT selectionAnchor{};
    POINT selectionCurrent{};
    int selectionPage = -1;
    double selectionLeftPt = 0.0;
    double selectionTopPt = 0.0;
    double selectionRightPt = 0.0;
    double selectionBottomPt = 0.0;
    std::wstring selectedText;
};

ViewerState g_state;
bool g_useNativeFileDialogs = false;

std::wstring CurrentExePath();
std::optional<std::wstring> PromptNativeOpenPath(HWND owner);

void ClearRenderCaches() {
    g_state.rendered.Clear();
    g_state.renderedPages.clear();
}

void ClearTextSelection() {
    g_state.selectingRect = false;
    g_state.hasRectSelection = false;
    g_state.selectionPage = -1;
    g_state.selectionLeftPt = 0.0;
    g_state.selectionTopPt = 0.0;
    g_state.selectionRightPt = 0.0;
    g_state.selectionBottomPt = 0.0;
    g_state.selectedText.clear();
}

int FirstDisplayedPage() {
    if (!g_state.pdf || g_state.pdf->pageCount <= 0) return 0;
    return std::clamp(g_state.pageRangeFirst, 0, g_state.pdf->pageCount - 1);
}

int LastDisplayedPage() {
    if (!g_state.pdf || g_state.pdf->pageCount <= 0) return -1;
    const int first = FirstDisplayedPage();
    const int requestedLast = g_state.pageRangeLast < 0 ? g_state.pdf->pageCount - 1 : g_state.pageRangeLast;
    return std::clamp(requestedLast, first, g_state.pdf->pageCount - 1);
}

bool IsDisplayedPage(int pageIndex) {
    return pageIndex >= FirstDisplayedPage() && pageIndex <= LastDisplayedPage();
}

std::wstring FileNameFromPath(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return path;
    return path.substr(pos + 1);
}

std::wstring ToAbsolutePath(const std::wstring& path) {
    if (path.empty()) return {};

    DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return path;

    std::wstring out(needed, L'\0');
    DWORD written = GetFullPathNameW(path.c_str(), needed, out.data(), nullptr);
    if (written == 0 || written >= needed) return path;
    out.resize(written);
    return out;
}

bool IsOptionName(const std::wstring& value) {
    return value.rfind(L"--", 0) == 0 || value.rfind(L"/", 0) == 0;
}

bool TryParsePositiveInt(const std::wstring& value, int& out) {
    if (value.empty()) return false;
    int result = 0;
    for (wchar_t ch : value) {
        if (ch < L'0' || ch > L'9') return false;
        const int digit = ch - L'0';
        if (result > (INT_MAX - digit) / 10) return false;
        result = result * 10 + digit;
    }
    out = result;
    return true;
}

bool TryParseDouble(const std::wstring& value, double& out) {
    if (value.empty()) return false;
    try {
        size_t parsed = 0;
        double result = std::stod(value, &parsed);
        if (parsed != value.size() || !std::isfinite(result)) return false;
        out = result;
        return true;
    } catch (...) {
        return false;
    }
}

bool TryParseOnOff(const std::wstring& value, bool& out) {
    std::wstring v = value;
    std::transform(v.begin(), v.end(), v.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    if (v == L"1" || v == L"true" || v == L"on" || v == L"yes") {
        out = true;
        return true;
    }
    if (v == L"0" || v == L"false" || v == L"off" || v == L"no") {
        out = false;
        return true;
    }
    return false;
}

CommandLineOptions ParseCommandLine() {
    CommandLineOptions options;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return options;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i] ? argv[i] : L"";
        if (arg == L"--pdf" && i + 1 < argc) {
            options.pdfPath = argv[++i] ? argv[i] : L"";
        } else if (arg == L"--clrop" && i + 1 < argc) {
            options.clropPath = argv[++i] ? argv[i] : L"";
        } else if ((arg == L"--theme" || arg == L"--theme-id") && i + 1 < argc) {
            options.themeId = argv[++i] ? argv[i] : L"";
        } else if (arg == L"--theme-inline" && i + 1 < argc) {
            options.inlineThemeSpec = argv[++i] ? argv[i] : L"";
        } else if (arg == L"--page" && i + 1 < argc) {
            int pageOneBased = 0;
            if (TryParsePositiveInt(argv[++i] ? argv[i] : L"", pageOneBased) && pageOneBased > 0) {
                options.initialPage = pageOneBased - 1;
            }
        } else if (arg == L"--zoom" && i + 1 < argc) {
            double zoom = 1.0;
            if (TryParseDouble(argv[++i] ? argv[i] : L"", zoom)) {
                options.initialZoom = std::clamp(zoom, 0.25, 4.0);
            }
        } else if (arg == L"--pan-x" && i + 1 < argc) {
            double pan = 0.0;
            if (TryParseDouble(argv[++i] ? argv[i] : L"", pan)) {
                options.initialPanX = std::clamp(pan, -50000.0, 50000.0);
            }
        } else if (arg == L"--pan-y" && i + 1 < argc) {
            double pan = 0.0;
            if (TryParseDouble(argv[++i] ? argv[i] : L"", pan)) {
                options.initialPanY = std::clamp(pan, -50000.0, 50000.0);
            }
        } else if ((arg == L"--pdf-y-pt" || arg == L"--pdf-y") && i + 1 < argc) {
            double yPt = 0.0;
            if (TryParseDouble(argv[++i] ? argv[i] : L"", yPt)) {
                options.initialPdfYPt = std::clamp(yPt, -50000.0, 50000.0);
                options.hasInitialPdfY = true;
            }
        } else if ((arg == L"--annotations" || arg == L"--annots") && i + 1 < argc) {
            bool show = true;
            if (TryParseOnOff(argv[++i] ? argv[i] : L"", show)) {
                options.showAnnotations = show;
            }
        } else if (arg == L"--magnifier" && i + 1 < argc) {
            bool show = false;
            if (TryParseOnOff(argv[++i] ? argv[i] : L"", show)) {
                options.showMagnifier = show;
            }
        } else if (!IsOptionName(arg) && options.pdfPath.empty()) {
            options.pdfPath = arg;
        }
    }

    LocalFree(argv);
    options.pdfPath = ToAbsolutePath(options.pdfPath);
    options.clropPath = ToAbsolutePath(options.clropPath);
    return options;
}

bool ReadFileMetadataShared(const std::wstring& path, FileMetadata& out) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }
    LARGE_INTEGER size{};
    size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
    size.LowPart = data.nFileSizeLow;
    if (size.QuadPart < 0) return false;
    out.size = static_cast<ULONGLONG>(size.QuadPart);
    out.lastWriteTime = data.ftLastWriteTime;
    return true;
}

std::wstring ClropPathForPdf(const std::wstring& pdfPath) {
    if (pdfPath.empty()) return {};
    std::filesystem::path p(pdfPath);
    p.replace_extension(L".clrop");
    return p.wstring();
}

bool HasExtensionCaseInsensitive(const std::wstring& path, const wchar_t* ext) {
    std::filesystem::path p(path);
    std::wstring actual = p.extension().wstring();
    std::wstring expected = ext ? ext : L"";
    std::transform(actual.begin(), actual.end(), actual.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    std::transform(expected.begin(), expected.end(), expected.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return actual == expected;
}

bool FileExistsRegular(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::filesystem::path& path) {
    if (path.empty()) return false;
    DWORD attrs = GetFileAttributesW(path.wstring().c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

COLORREF HexToColor(const std::string& s, COLORREF fallback = RGB(255, 230, 180)) {
    if (s.size() == 7 && s[0] == '#') {
        auto hex = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
            if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
            return -1;
        };
        const int r1 = hex(s[1]), r2 = hex(s[2]);
        const int g1 = hex(s[3]), g2 = hex(s[4]);
        const int b1 = hex(s[5]), b2 = hex(s[6]);
        if (r1 >= 0 && r2 >= 0 && g1 >= 0 && g2 >= 0 && b1 >= 0 && b2 >= 0) {
            return RGB(r1 * 16 + r2, g1 * 16 + g2, b1 * 16 + b2);
        }
    }
    return fallback;
}

bool TryParseHexColorW(const std::wstring& value, COLORREF& out) {
    if (value.size() != 7 || value[0] != L'#') return false;
    auto hex = [](wchar_t ch) -> int {
        if (ch >= L'0' && ch <= L'9') return static_cast<int>(ch - L'0');
        if (ch >= L'a' && ch <= L'f') return 10 + static_cast<int>(ch - L'a');
        if (ch >= L'A' && ch <= L'F') return 10 + static_cast<int>(ch - L'A');
        return -1;
    };
    const int r1 = hex(value[1]), r2 = hex(value[2]);
    const int g1 = hex(value[3]), g2 = hex(value[4]);
    const int b1 = hex(value[5]), b2 = hex(value[6]);
    if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0) return false;
    out = RGB(r1 * 16 + r2, g1 * 16 + g2, b1 * 16 + b2);
    return true;
}

void ApplyInlineThemePair(ThemeColors& theme, const std::wstring& key, COLORREF value) {
    if (key == L"windowBg") theme.windowBg = value;
    else if (key == L"windowText") theme.windowText = value;
    else if (key == L"panelBg") theme.panelBg = value;
    else if (key == L"panelText") theme.panelText = value;
    else if (key == L"menuBg") theme.menuBg = value;
    else if (key == L"menuText") theme.menuText = value;
    else if (key == L"menuSelBg") theme.menuSelBg = value;
    else if (key == L"menuSelText") theme.menuSelText = value;
    else if (key == L"toolbarBg") theme.toolbarBg = value;
    else if (key == L"toolbarText") theme.toolbarText = value;
    else if (key == L"buttonBg") theme.buttonBg = value;
    else if (key == L"buttonText") theme.buttonText = value;
    else if (key == L"buttonBorder") theme.buttonBorder = value;
    else if (key == L"buttonHot") theme.buttonHot = value;
    else if (key == L"buttonPressed") theme.buttonPressed = value;
    else if (key == L"splitterBg") theme.splitterBg = value;
    else if (key == L"splitterLine") theme.splitterLine = value;
    else if (key == L"pdfBg") theme.pdfBg = value;
    else if (key == L"pdfPageBg") theme.pdfPageBg = value;
    else if (key == L"noteBg") theme.noteBg = value;
    else if (key == L"noteText") theme.noteText = value;
    else if (key == L"selectionBg") theme.selectionBg = value;
    else if (key == L"selectionText") theme.selectionText = value;
    else if (key == L"accent") theme.accent = value;
}

bool ApplyInlineThemeSpec(ThemeColors& base, const std::wstring& spec) {
    if (spec.empty()) return false;
    bool applied = false;
    size_t start = 0;
    while (start < spec.size()) {
        size_t end = spec.find_first_of(L";,", start);
        if (end == std::wstring::npos) end = spec.size();
        std::wstring token = spec.substr(start, end - start);
        size_t eq = token.find(L'=');
        if (eq != std::wstring::npos) {
            std::wstring key = token.substr(0, eq);
            std::wstring valueText = token.substr(eq + 1);
            COLORREF color{};
            if (TryParseHexColorW(valueText, color)) {
                ApplyInlineThemePair(base, key, color);
                applied = true;
            }
        }
        start = end + 1;
    }
    if (applied) base.name = L"inline";
    return applied;
}

std::wstring ColorToHex(COLORREF color) {
    wchar_t buf[8]{};
    swprintf(buf, 8, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    return buf;
}

std::wstring BuildInlineThemeSpec(const ThemeColors& theme) {
    std::wstring spec;
    auto add = [&](const wchar_t* key, COLORREF value) {
        if (!spec.empty()) spec += L";";
        spec += key;
        spec += L"=";
        spec += ColorToHex(value);
    };
    add(L"windowBg", theme.windowBg);
    add(L"windowText", theme.windowText);
    add(L"panelBg", theme.panelBg);
    add(L"panelText", theme.panelText);
    add(L"menuBg", theme.menuBg);
    add(L"menuText", theme.menuText);
    add(L"menuSelBg", theme.menuSelBg);
    add(L"menuSelText", theme.menuSelText);
    add(L"toolbarBg", theme.toolbarBg);
    add(L"toolbarText", theme.toolbarText);
    add(L"buttonBg", theme.buttonBg);
    add(L"buttonText", theme.buttonText);
    add(L"buttonBorder", theme.buttonBorder);
    add(L"buttonHot", theme.buttonHot);
    add(L"buttonPressed", theme.buttonPressed);
    add(L"splitterBg", theme.splitterBg);
    add(L"splitterLine", theme.splitterLine);
    add(L"pdfBg", theme.pdfBg);
    add(L"pdfPageBg", theme.pdfPageBg);
    add(L"noteBg", theme.noteBg);
    add(L"noteText", theme.noteText);
    add(L"selectionBg", theme.selectionBg);
    add(L"selectionText", theme.selectionText);
    add(L"accent", theme.accent);
    return spec;
}

std::wstring FormatZoomArg(double value) {
    wchar_t buf[32]{};
    swprintf(buf, 32, L"%.4f", std::clamp(value, 0.25, 4.0));
    return buf;
}

std::wstring FormatPixelArg(double value) {
    wchar_t buf[32]{};
    swprintf(buf, 32, L"%.2f", std::clamp(value, -50000.0, 50000.0));
    return buf;
}

std::wstring Utf8ToWideLocal(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                  utf8.data(), static_cast<int>(utf8.size()),
                                  nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), len);
    return out;
}

std::wstring QuoteArg(const std::wstring& value) {
    std::wstring out = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"') out += L'\\';
        out += ch;
    }
    out += L"\"";
    return out;
}

std::wstring PdfiumErrorText(unsigned long err) {
    switch (err) {
    case FPDF_ERR_SUCCESS:
        return L"no error";
    case FPDF_ERR_UNKNOWN:
        return L"unknown PDF error";
    case FPDF_ERR_FILE:
        return L"file cannot be opened";
    case FPDF_ERR_FORMAT:
        return L"invalid PDF format";
    case FPDF_ERR_PASSWORD:
        return L"password-protected PDF";
    case FPDF_ERR_SECURITY:
        return L"PDF security restriction";
    case FPDF_ERR_PAGE:
        return L"page load error";
    default:
        return L"PDF error";
    }
}

bool ReadFileBytesShared(const std::wstring& path, std::vector<unsigned char>& out) {
    out.clear();
    if (path.empty()) return false;

    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        return false;
    }

    const auto maxSize = std::min<unsigned long long>(
        static_cast<unsigned long long>(SIZE_MAX),
        kMaxInputFileBytes);
    if (static_cast<unsigned long long>(size.QuadPart) > maxSize) {
        CloseHandle(file);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    size_t total = 0;
    while (total < out.size()) {
        const size_t remaining = out.size() - total;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1u << 20));
        DWORD read = 0;
        if (!ReadFile(file, out.data() + total, chunk, &read, nullptr) || read == 0) {
            CloseHandle(file);
            out.clear();
            return false;
        }
        total += read;
    }

    CloseHandle(file);
    return true;
}

bool ReadFileBytesSharedLimited(const std::wstring& path,
                                unsigned long long maxBytes,
                                std::vector<unsigned char>& out) {
    out.clear();
    if (path.empty()) return false;

    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > maxBytes) {
        CloseHandle(file);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    size_t total = 0;
    while (total < out.size()) {
        const size_t remaining = out.size() - total;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1u << 20));
        DWORD read = 0;
        if (!ReadFile(file, out.data() + total, chunk, &read, nullptr) || read == 0) {
            CloseHandle(file);
            out.clear();
            return false;
        }
        total += read;
    }

    CloseHandle(file);
    return true;
}

std::optional<std::string> ParseJsonStringFieldLocal(const std::string& json, const std::string& key) {
    const std::string quotedKey = "\"" + key + "\"";
    size_t pos = json.find(quotedKey);
    if (pos == std::string::npos) return std::nullopt;
    pos += quotedKey.size();
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size() || json[pos] != ':') return std::nullopt;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size() || json[pos] != '"') return std::nullopt;
    ++pos;

    std::string value;
    while (pos < json.size()) {
        char ch = json[pos++];
        if (ch == '"') return value;
        if (ch != '\\') {
            value.push_back(ch);
            continue;
        }
        if (pos >= json.size()) return std::nullopt;
        char esc = json[pos++];
        switch (esc) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<bool> ParseJsonBoolFieldLocal(const std::string& json, const std::string& key) {
    const std::string quotedKey = "\"" + key + "\"";
    size_t pos = json.find(quotedKey);
    if (pos == std::string::npos) return std::nullopt;
    pos += quotedKey.size();
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size() || json[pos] != ':') return std::nullopt;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (json.compare(pos, 4, "true") == 0) return true;
    if (json.compare(pos, 5, "false") == 0) return false;
    return std::nullopt;
}

std::filesystem::path ReadonlyViewerSettingsPath() {
    const std::filesystem::path exePath(CurrentExePath());
    const std::filesystem::path exeDir = exePath.parent_path();
    if (exeDir.empty()) return {};
    return exeDir / L"readonly_viewer_settings.json";
}

bool WriteUtf8FileAtomicLocal(const std::filesystem::path& path, const std::string& data) {
    if (path.empty() || path.parent_path().empty()) return false;
    std::filesystem::path temp = path;
    temp += L".tmp";

    HANDLE file = CreateFileW(temp.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    const char* cursor = data.data();
    size_t remaining = data.size();
    bool ok = true;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(file, cursor, chunk, &written, nullptr) || written != chunk) {
            ok = false;
            break;
        }
        cursor += written;
        remaining -= written;
    }
    if (ok) ok = FlushFileBuffers(file) != 0;
    if (!CloseHandle(file)) ok = false;
    if (!ok) {
        DeleteFileW(temp.c_str());
        return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

void LoadReadonlyViewerSettings() {
    const std::filesystem::path path = ReadonlyViewerSettingsPath();
    if (path.empty() || !FileExistsRegular(path.wstring())) {
        g_useNativeFileDialogs = false;
        return;
    }
    std::vector<unsigned char> bytes;
    if (!ReadFileBytesSharedLimited(path.wstring(), kMaxReadonlySettingsJsonBytes, bytes)) return;
    std::string json(bytes.begin(), bytes.end());
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    if (json.size() >= 3 &&
        static_cast<unsigned char>(json[0]) == bom[0] &&
        static_cast<unsigned char>(json[1]) == bom[1] &&
        static_cast<unsigned char>(json[2]) == bom[2]) {
        json.erase(0, 3);
    }
    if (auto enabled = ParseJsonBoolFieldLocal(json, "useNativeFileDialogs")) {
        g_useNativeFileDialogs = *enabled;
    }
    if (auto enabled = ParseJsonBoolFieldLocal(json, "showGrayscale")) {
        g_state.showGrayscale = *enabled;
    }
}

void SaveReadonlyViewerSettings() {
    const std::filesystem::path path = ReadonlyViewerSettingsPath();
    if (path.empty()) return;
    std::string json;
    json += "{\n";
    json += "  \"format\": \"pdf_clrop_viewer_settings_v1\",\n";
    json += "  \"useNativeFileDialogs\": ";
    json += g_useNativeFileDialogs ? "true" : "false";
    json += ",\n";
    json += "  \"showGrayscale\": ";
    json += g_state.showGrayscale ? "true" : "false";
    json += "\n}\n";
    (void)WriteUtf8FileAtomicLocal(path, json);
}

std::optional<std::filesystem::path> ReadSetupWorkspaceRootForOpenDialog(
    const std::filesystem::path& exeDir) {
    const std::filesystem::path setup = exeDir / L"pdf_workspace_setup.json";
    if (!FileExistsRegular(setup.wstring())) return std::nullopt;

    std::vector<unsigned char> bytes;
    if (!ReadFileBytesSharedLimited(setup.wstring(), kMaxSetupJsonBytes, bytes)) return std::nullopt;

    std::string json(bytes.begin(), bytes.end());
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    if (json.size() >= 3 &&
        static_cast<unsigned char>(json[0]) == bom[0] &&
        static_cast<unsigned char>(json[1]) == bom[1] &&
        static_cast<unsigned char>(json[2]) == bom[2]) {
        json.erase(0, 3);
    }

    auto rootValue = ParseJsonStringFieldLocal(json, "workspaceRoot");
    if (!rootValue || rootValue->empty()) return std::nullopt;
    std::filesystem::path root = Utf8ToWideLocal(*rootValue);
    if (root.empty()) return std::nullopt;
    if (root.is_relative()) root = exeDir / root;
    if (!DirectoryExists(root)) return std::nullopt;
    return root;
}

std::filesystem::path InitialOpenDialogFolder() {
    const std::filesystem::path exePath(CurrentExePath());
    const std::filesystem::path exeDir = exePath.parent_path();
    if (exeDir.empty()) return {};
    if (auto workspaceRoot = ReadSetupWorkspaceRootForOpenDialog(exeDir)) {
        return *workspaceRoot;
    }
    return exeDir;
}

struct OpenPathPromptState {
    HWND edit = nullptr;
    std::wstring initial;
    std::wstring result;
    bool accepted = false;
    bool useNativeAfterClose = false;
    bool done = false;
};

LRESULT CALLBACK OpenPathPromptWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OpenPathPromptState* state =
        reinterpret_cast<OpenPathPromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<OpenPathPromptState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        CreateWindowExW(0, L"STATIC", L"PDF / CLROP ファイルのパス",
                        WS_CHILD | WS_VISIBLE, 12, 10, 584, 20,
                        hwnd, nullptr, create->hInstance, nullptr);
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", state->initial.c_str(),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      12, 34, 584, 25, hwnd,
                                      reinterpret_cast<HMENU>(101), create->hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"OS標準で開く",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        12, 72, 120, 28, hwnd,
                        reinterpret_cast<HMENU>(102), create->hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"開く",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        412, 72, 86, 28, hwnd,
                        reinterpret_cast<HMENU>(IDOK), create->hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"キャンセル",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        510, 72, 86, 28, hwnd,
                        reinterpret_cast<HMENU>(IDCANCEL), create->hInstance, nullptr);
        if (state->edit) {
            SetFocus(state->edit);
            SendMessageW(state->edit, EM_SETSEL,
                         static_cast<WPARAM>(state->initial.size()),
                         static_cast<LPARAM>(state->initial.size()));
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK && state && state->edit) {
            const int length = GetWindowTextLengthW(state->edit);
            std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(state->edit, buffer.data(), length + 1);
            state->result.assign(buffer.data());
            state->accepted = !state->result.empty();
            state->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == 102 && state) {
            state->useNativeAfterClose = true;
            state->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL && state) {
            state->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (state) state->done = true;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::optional<std::wstring> PromptLocalOpenPath(HWND owner) {
    OpenPathPromptState state;
    const std::filesystem::path initialFolder = InitialOpenDialogFolder();
    if (!initialFolder.empty()) {
        state.initial = initialFolder.wstring();
        if (!state.initial.empty() && state.initial.back() != L'\\') state.initial += L"\\";
    }
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = OpenPathPromptWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"PdfReadonlyViewerOpenPathPrompt";
    RegisterClassW(&wc);
    if (owner) EnableWindow(owner, FALSE);
    HWND prompt = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"読み込むファイルを開く",
                                  WS_CAPTION | WS_POPUPWINDOW | WS_VISIBLE,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 624, 146,
                                  owner, nullptr, instance, &state);
    if (!prompt) {
        if (owner) EnableWindow(owner, TRUE);
        return std::nullopt;
    }
    MSG msg{};
    while (!state.done) {
        const BOOL status = GetMessageW(&msg, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) PostQuitMessage(static_cast<int>(msg.wParam));
            state.done = true;
            break;
        }
        if (!IsDialogMessageW(prompt, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    if (state.useNativeAfterClose) {
        return PromptNativeOpenPath(owner);
    }
    if (!state.accepted) return std::nullopt;
    while (!state.result.empty() && iswspace(state.result.front())) state.result.erase(state.result.begin());
    while (!state.result.empty() && iswspace(state.result.back())) state.result.pop_back();
    if (state.result.size() >= 2 &&
        state.result.front() == L'"' && state.result.back() == L'"') {
        state.result = state.result.substr(1, state.result.size() - 2);
    }
    if (state.result.empty()) return std::nullopt;
    if (state.result.rfind(L"\\\\", 0) == 0) {
        g_state.status = L"Network/device paths are not supported.";
        if (owner) InvalidateRect(owner, nullptr, FALSE);
        return std::nullopt;
    }
    std::filesystem::path selectedPath(state.result);
    std::filesystem::path root = selectedPath.root_path();
    if (!root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE) {
        g_state.status = L"Network drive paths are not supported.";
        if (owner) InvalidateRect(owner, nullptr, FALSE);
        return std::nullopt;
    }
    const DWORD attributes = GetFileAttributesW(state.result.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        g_state.status = L"Reparse point paths are not supported.";
        if (owner) InvalidateRect(owner, nullptr, FALSE);
        return std::nullopt;
    }
    return state.result;
}

std::optional<std::wstring> PromptNativeOpenPath(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        g_state.status = L"Windows file picker is not available.";
        if (owner) InvalidateRect(owner, nullptr, FALSE);
        return std::nullopt;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST;
        dialog->SetOptions(options);
    }
    dialog->SetTitle(L"読み込むファイルを開く");
    COMDLG_FILTERSPEC filters[] = {
        { L"PDF / CLROP", L"*.pdf;*.clrop" },
        { L"PDF", L"*.pdf" },
        { L"CLROP", L"*.clrop" },
        { L"All files", L"*.*" },
    };
    dialog->SetFileTypes(static_cast<UINT>(sizeof(filters) / sizeof(filters[0])), filters);
    dialog->SetFileTypeIndex(1);

    const std::filesystem::path initialFolder = InitialOpenDialogFolder();
    if (!initialFolder.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initialFolder.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    hr = dialog->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        dialog->Release();
        return std::nullopt;
    }
    if (FAILED(hr)) {
        dialog->Release();
        g_state.status = L"Windows file picker failed.";
        if (owner) InvalidateRect(owner, nullptr, FALSE);
        return std::nullopt;
    }

    std::optional<std::wstring> result;
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && item) {
        PWSTR raw = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
            result = std::wstring(raw);
            CoTaskMemFree(raw);
        }
        item->Release();
    }
    dialog->Release();
    if (!result || result->empty()) return std::nullopt;

    if (result->rfind(L"\\\\", 0) == 0) {
        g_state.status = L"Network/device paths are not supported.";
        if (owner) InvalidateRect(owner, nullptr, FALSE);
        return std::nullopt;
    }
    std::filesystem::path selectedPath(*result);
    std::filesystem::path root = selectedPath.root_path();
    if (!root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE) {
        g_state.status = L"Network drive paths are not supported.";
        if (owner) InvalidateRect(owner, nullptr, FALSE);
        return std::nullopt;
    }
    const DWORD attributes = GetFileAttributesW(result->c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        g_state.status = L"Selected file was not found.";
        if (owner) InvalidateRect(owner, nullptr, FALSE);
        return std::nullopt;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        g_state.status = L"Reparse point paths are not supported.";
        if (owner) InvalidateRect(owner, nullptr, FALSE);
        return std::nullopt;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        g_state.status = L"Select a PDF or CLROP file.";
        if (owner) InvalidateRect(owner, nullptr, FALSE);
        return std::nullopt;
    }
    return result;
}

struct PageRangePromptState {
    int pageCount = 0;
    int firstPage = 1;
    int lastPage = 1;
    HWND summary = nullptr;
    bool accepted = false;
    bool done = false;
};

void UpdateStatusFromPdf();

void UpdatePageRangePrompt(HWND hwnd, PageRangePromptState* state, bool showError = false) {
    if (!state || !state->summary) return;
    BOOL startOk = FALSE;
    BOOL endOk = FALSE;
    const UINT start = GetDlgItemInt(hwnd, 201, &startOk, FALSE);
    const UINT end = GetDlgItemInt(hwnd, 202, &endOk, FALSE);
    if (!startOk || !endOk || start == 0 || end == 0 || start > end || end > static_cast<UINT>(state->pageCount)) {
        SetWindowTextW(state->summary, showError ? L"1 から総ページ数までの連続範囲を指定してください。" : L"開始・終了ページを指定してください。");
        return;
    }
    const UINT count = end - start + 1;
    SetWindowTextW(state->summary,
                   (L"全 " + std::to_wstring(state->pageCount) + L" ページ中、" +
                    std::to_wstring(start) + L" ～ " + std::to_wstring(end) + L" ページ（" +
                    std::to_wstring(count) + L" ページ）を連続表示します。範囲外は描画しません。").c_str());
}

LRESULT CALLBACK PageRangePromptWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<PageRangePromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<PageRangePromptState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        CreateWindowExW(0, L"STATIC", L"表示するページの連続範囲", WS_CHILD | WS_VISIBLE,
                        14, 12, 390, 20, hwnd, nullptr, create->hInstance, nullptr);
        CreateWindowExW(0, L"STATIC", L"開始", WS_CHILD | WS_VISIBLE,
                        14, 44, 42, 22, hwnd, nullptr, create->hInstance, nullptr);
        HWND start = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(state->firstPage).c_str(),
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                                     58, 40, 86, 26, hwnd, reinterpret_cast<HMENU>(201), create->hInstance, nullptr);
        CreateWindowExW(0, L"STATIC", L"終了", WS_CHILD | WS_VISIBLE,
                        162, 44, 42, 22, hwnd, nullptr, create->hInstance, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(state->lastPage).c_str(),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                        206, 40, 86, 26, hwnd, reinterpret_cast<HMENU>(202), create->hInstance, nullptr);
        state->summary = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                         14, 82, 500, 44, hwnd, nullptr, create->hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"この範囲を表示", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        296, 138, 118, 28, hwnd, reinterpret_cast<HMENU>(IDOK), create->hInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"キャンセル", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        426, 138, 90, 28, hwnd, reinterpret_cast<HMENU>(IDCANCEL), create->hInstance, nullptr);
        UpdatePageRangePrompt(hwnd, state);
        if (start) SetFocus(start);
        return 0;
    }
    case WM_COMMAND:
        if ((LOWORD(wParam) == 201 || LOWORD(wParam) == 202) && HIWORD(wParam) == EN_CHANGE) {
            UpdatePageRangePrompt(hwnd, state);
            return 0;
        }
        if (LOWORD(wParam) == IDOK && state) {
            BOOL startOk = FALSE;
            BOOL endOk = FALSE;
            const UINT start = GetDlgItemInt(hwnd, 201, &startOk, FALSE);
            const UINT end = GetDlgItemInt(hwnd, 202, &endOk, FALSE);
            if (!startOk || !endOk || start == 0 || end == 0 || start > end || end > static_cast<UINT>(state->pageCount)) {
                UpdatePageRangePrompt(hwnd, state, true);
                return 0;
            }
            state->firstPage = static_cast<int>(start);
            state->lastPage = static_cast<int>(end);
            state->accepted = true;
            state->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL && state) {
            state->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (state) state->done = true;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool PromptPageRange(HWND owner, int pageCount, int& firstPage, int& lastPage) {
    if (!owner || pageCount <= 0) return false;
    PageRangePromptState state;
    state.pageCount = pageCount;
    state.firstPage = std::clamp(firstPage, 1, pageCount);
    state.lastPage = std::clamp(lastPage, state.firstPage, pageCount);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = PageRangePromptWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"PdfReadonlyViewerPageRangePrompt";
    RegisterClassW(&wc);
    EnableWindow(owner, FALSE);
    HWND prompt = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"PDF の表示範囲",
                                  WS_CAPTION | WS_POPUPWINDOW | WS_VISIBLE,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 540, 210,
                                  owner, nullptr, instance, &state);
    if (!prompt) {
        EnableWindow(owner, TRUE);
        return false;
    }
    MSG msg{};
    while (!state.done) {
        const BOOL status = GetMessageW(&msg, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) PostQuitMessage(static_cast<int>(msg.wParam));
            state.done = true;
            break;
        }
        if (!IsDialogMessageW(prompt, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    if (!state.accepted) return false;
    firstPage = state.firstPage;
    lastPage = state.lastPage;
    return true;
}

void ChoosePageRange(HWND hwnd) {
    if (!g_state.pdf || g_state.pdf->pageCount <= 0) return;
    int first = FirstDisplayedPage() + 1;
    int last = LastDisplayedPage() + 1;
    if (!PromptPageRange(hwnd, g_state.pdf->pageCount, first, last)) return;
    g_state.pageRangeFirst = first - 1;
    g_state.pageRangeLast = last - 1;
    g_state.currentPage = g_state.pageRangeFirst;
    g_state.scrollY = 0.0;
    g_state.panY = 0.0;
    ClearTextSelection();
    ClearRenderCaches();
    UpdateStatusFromPdf();
    InvalidateRect(hwnd, nullptr, FALSE);
}

void UpdateStatusFromPdf() {
    if (!g_state.pdf || !g_state.pdf->document || g_state.pdf->pageCount <= 0) return;
    g_state.status = FileNameFromPath(g_state.pdf->path) +
                     L"  -  page " +
                     std::to_wstring(g_state.currentPage + 1) +
                     L" / " +
                     std::to_wstring(g_state.pdf->pageCount) +
                     L"  (表示 " + std::to_wstring(FirstDisplayedPage() + 1) + L"～" +
                     std::to_wstring(LastDisplayedPage() + 1) + L")";
}

bool LoadPdfReadOnly(const std::wstring& path, std::unique_ptr<LoadedPdf>& out, std::wstring& status) {
    out.reset();
    if (path.empty()) {
        status = L"PDF / CLROP read-only viewer";
        return false;
    }

    auto pdf = std::make_unique<LoadedPdf>();
    pdf->path = path;
    const int utf8_size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
    if (utf8_size <= 0) {
        status = L"Could not encode PDF path: " + FileNameFromPath(path);
        return false;
    }
    std::string utf8_path(static_cast<size_t>(utf8_size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), utf8_path.data(), utf8_size, nullptr, nullptr);
    // Keep the source file on disk: do not duplicate a potentially huge PDF in process memory.
    pdf->document = FPDF_LoadDocument(utf8_path.c_str(), nullptr);
    if (!pdf->document) {
        status = L"Could not open PDF (" + PdfiumErrorText(FPDF_GetLastError()) + L"): " +
                 FileNameFromPath(path);
        return false;
    }

    pdf->pageCount = FPDF_GetPageCount(pdf->document);
    if (pdf->pageCount <= 0) {
        status = L"PDF has no pages: " + FileNameFromPath(path);
        return false;
    }

    status = FileNameFromPath(path) + L"  -  page 1 / " + std::to_wstring(pdf->pageCount);
    out = std::move(pdf);
    return true;
}

bool LoadClropReadOnly(const std::wstring& path, std::unique_ptr<LoadedClrop>& out, std::wstring& err) {
    out.reset();
    if (path.empty() || !FileExistsRegular(path)) {
        err.clear();
        return false;
    }

    auto loaded = std::make_unique<LoadedClrop>();
    loaded->path = path;
    clrop::LoadFileFailureKind failureKind = clrop::LoadFileFailureKind::None;
    if (!clrop::LoadClropFile(path, loaded->document, err, &failureKind)) {
        out.reset();
        return false;
    }
    out = std::move(loaded);
    return true;
}

std::wstring ResolvePdfPathForClrop(const std::wstring& clropPath, const clrop::Document& doc) {
    if (!doc.pdfId.path.empty() && FileExistsRegular(doc.pdfId.path)) {
        return ToAbsolutePath(doc.pdfId.path);
    }

    if (!doc.pdfId.path.empty()) {
        std::filesystem::path relativeCandidate = std::filesystem::path(clropPath).parent_path() / doc.pdfId.path;
        if (FileExistsRegular(relativeCandidate.wstring())) {
            return ToAbsolutePath(relativeCandidate.wstring());
        }
    }

    std::filesystem::path adjacent(clropPath);
    adjacent.replace_extension(L".pdf");
    if (FileExistsRegular(adjacent.wstring())) {
        return ToAbsolutePath(adjacent.wstring());
    }
    return {};
}

void LoadAdjacentClropForPdf(HWND hwnd, const std::wstring& explicitClropPath = {}) {
    std::wstring clropPath = explicitClropPath;
    if (clropPath.empty() && g_state.pdf) {
        clropPath = ClropPathForPdf(g_state.pdf->path);
    }

    std::unique_ptr<LoadedClrop> loaded;
    std::wstring err;
    if (LoadClropReadOnly(clropPath, loaded, err)) {
        g_state.clrop = std::move(loaded);
        g_state.hasClropWatchedMetadata =
            ReadFileMetadataShared(g_state.clrop->path, g_state.clropWatchedMetadata);
        g_state.clropReloadPending = false;
        g_state.selectedPage = -1;
        g_state.selectedItem = -1;
        ClearTextSelection();
        UpdateStatusFromPdf();
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    g_state.clrop.reset();
    g_state.hasClropWatchedMetadata = false;
    g_state.clropReloadPending = false;
    g_state.selectedPage = -1;
    g_state.selectedItem = -1;
    ClearTextSelection();
    if (!clropPath.empty() && FileExistsRegular(clropPath) && !err.empty()) {
        g_state.status = L"Could not read CLROP; showing PDF only";
    } else {
        UpdateStatusFromPdf();
    }
    if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
}

void ScrollToPageTop(HWND hwnd, int pageIndex, double pageOffsetY);

void SetCurrentPage(HWND hwnd, int pageIndex) {
    if (!g_state.pdf || g_state.pdf->pageCount <= 0) return;
    const int clamped = std::clamp(pageIndex, FirstDisplayedPage(), LastDisplayedPage());
    ScrollToPageTop(hwnd, clamped, 0.0);
}

void AdjustZoom(HWND hwnd, double factor) {
    const double next = std::clamp(g_state.zoomFactor * factor, 0.25, 4.0);
    if (std::abs(next - g_state.zoomFactor) < 0.0001) return;
    g_state.zoomFactor = next;
    if (g_state.zoomFactor <= 1.0001) {
        g_state.panX = 0.0;
        g_state.panY = 0.0;
    }
    ClearRenderCaches();
    ScrollToPageTop(hwnd, g_state.currentPage, 0.0);
}

void ResetZoom(HWND hwnd) {
    if (std::abs(g_state.zoomFactor - 1.0) < 0.0001 &&
        std::abs(g_state.panX) < 0.0001) {
        return;
    }
    g_state.zoomFactor = 1.0;
    g_state.panX = 0.0;
    g_state.panY = 0.0;
    ClearRenderCaches();
    ScrollToPageTop(hwnd, g_state.currentPage, 0.0);
}

void ReplaceLoadedPdf(HWND hwnd, std::unique_ptr<LoadedPdf> next, const FileMetadata& metadata) {
    if (!next || !next->document) return;
    const int previousPage = g_state.currentPage;
    g_state.pdf = std::move(next);
    g_state.pageRangeFirst = std::clamp(g_state.pageRangeFirst, 0, g_state.pdf->pageCount - 1);
    if (g_state.pageRangeLast >= 0) {
        g_state.pageRangeLast = std::clamp(g_state.pageRangeLast, g_state.pageRangeFirst, g_state.pdf->pageCount - 1);
    }
    g_state.currentPage = std::clamp(previousPage, FirstDisplayedPage(), LastDisplayedPage());
    g_state.pdfWatchedMetadata = metadata;
    g_state.hasPdfWatchedMetadata = true;
    g_state.pdfReloadPending = false;
    ClearTextSelection();
    ClearRenderCaches();
    UpdateStatusFromPdf();
    LoadAdjacentClropForPdf(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void TryReloadChangedPdf(HWND hwnd) {
    if (!g_state.pdf || g_state.pdf->path.empty() || !g_state.pdfReloadPending) return;

    const ULONGLONG now = GetTickCount64();
    if (now - g_state.pdfPendingSinceTick < kReloadDebounceMs) {
        return;
    }

    FileMetadata latest{};
    if (!ReadFileMetadataShared(g_state.pdf->path, latest)) {
        return;
    }
    if (!latest.Equals(g_state.pdfPendingMetadata)) {
        g_state.pdfPendingMetadata = latest;
        g_state.pdfPendingSinceTick = now;
        return;
    }

    std::unique_ptr<LoadedPdf> next;
    std::wstring status;
    if (LoadPdfReadOnly(g_state.pdf->path, next, status)) {
        ReplaceLoadedPdf(hwnd, std::move(next), latest);
    } else {
        g_state.status = L"Could not reload updated PDF; keeping current view";
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void TryReloadChangedClrop(HWND hwnd) {
    if (!g_state.clrop || g_state.clrop->path.empty() || !g_state.clropReloadPending) return;

    const ULONGLONG now = GetTickCount64();
    if (now - g_state.clropPendingSinceTick < kReloadDebounceMs) {
        return;
    }

    FileMetadata latest{};
    if (!ReadFileMetadataShared(g_state.clrop->path, latest)) {
        return;
    }
    if (!latest.Equals(g_state.clropPendingMetadata)) {
        g_state.clropPendingMetadata = latest;
        g_state.clropPendingSinceTick = now;
        return;
    }

    std::unique_ptr<LoadedClrop> next;
    std::wstring err;
    if (LoadClropReadOnly(g_state.clrop->path, next, err)) {
        g_state.clrop = std::move(next);
        g_state.clropWatchedMetadata = latest;
        g_state.hasClropWatchedMetadata = true;
        g_state.clropReloadPending = false;
        g_state.selectedPage = -1;
        g_state.selectedItem = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
    } else {
        g_state.status = L"Could not reload updated CLROP; keeping current annotations";
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void CheckWatchedFiles(HWND hwnd) {
    if (!g_state.pdf || g_state.pdf->path.empty()) return;

    FileMetadata latest{};
    if (!ReadFileMetadataShared(g_state.pdf->path, latest)) {
        return;
    }

    if (!g_state.hasPdfWatchedMetadata) {
        g_state.pdfWatchedMetadata = latest;
        g_state.hasPdfWatchedMetadata = true;
        return;
    }

    if (!latest.Equals(g_state.pdfWatchedMetadata)) {
        if (!g_state.pdfReloadPending || !latest.Equals(g_state.pdfPendingMetadata)) {
            g_state.pdfPendingMetadata = latest;
            g_state.pdfPendingSinceTick = GetTickCount64();
            g_state.pdfReloadPending = true;
        }
    }

    TryReloadChangedPdf(hwnd);

    if (g_state.clrop && !g_state.clrop->path.empty()) {
        FileMetadata clropLatest{};
        if (ReadFileMetadataShared(g_state.clrop->path, clropLatest)) {
            if (!g_state.hasClropWatchedMetadata) {
                g_state.clropWatchedMetadata = clropLatest;
                g_state.hasClropWatchedMetadata = true;
            } else if (!clropLatest.Equals(g_state.clropWatchedMetadata)) {
                if (!g_state.clropReloadPending || !clropLatest.Equals(g_state.clropPendingMetadata)) {
                    g_state.clropPendingMetadata = clropLatest;
                    g_state.clropPendingSinceTick = GetTickCount64();
                    g_state.clropReloadPending = true;
                }
            }
        } else {
            g_state.clrop.reset();
            g_state.hasClropWatchedMetadata = false;
            g_state.clropReloadPending = false;
            g_state.selectedPage = -1;
            g_state.selectedItem = -1;
            UpdateStatusFromPdf();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    } else if (g_state.pdf) {
        const std::wstring candidate = ClropPathForPdf(g_state.pdf->path);
        if (FileExistsRegular(candidate)) {
            LoadAdjacentClropForPdf(hwnd, candidate);
        }
    }
    TryReloadChangedClrop(hwnd);
}

void ApplyReadonlyGrayscaleToBgra(std::vector<unsigned char>& pixels) {
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const unsigned int b = pixels[i + 0];
        const unsigned int g = pixels[i + 1];
        const unsigned int r = pixels[i + 2];
        const unsigned char gray = static_cast<unsigned char>((r * 77u + g * 150u + b * 29u + 128u) >> 8);
        pixels[i + 0] = gray;
        pixels[i + 1] = gray;
        pixels[i + 2] = gray;
    }
}

bool RenderPageToCache(FPDF_DOCUMENT document, int pageIndex, int width, int height, RenderedPage& out) {
    if (!document || pageIndex < 0 || width < kMinRenderPx || height < kMinRenderPx) return false;
    const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixel_count > kMaxRenderedPageBytes / 4u) return false;

    FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
    if (!page) return false;

    std::vector<unsigned char> pixels(pixel_count * 4u);
    const int stride = width * 4;
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA, pixels.data(), stride);
    if (!bitmap) {
        FPDF_ClosePage(page);
        return false;
    }

    FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xFFFFFFFFu);
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0, FPDF_ANNOT | FPDF_LCD_TEXT);
    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);

    if (g_state.showGrayscale) {
        ApplyReadonlyGrayscaleToBgra(pixels);
    }

    out.pageIndex = pageIndex;
    out.width = width;
    out.height = height;
    out.stride = stride;
    out.pixels = std::move(pixels);
    return true;
}

RECT ViewerAvailableRect(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    RECT avail = client;
    avail.left += 48;
    avail.right -= 48;
    avail.top += 58;
    avail.bottom -= 32;
    return avail;
}

bool PageSizePt(int pageIndex, double& pageW, double& pageH) {
    pageW = 1.0;
    pageH = 1.0;
    if (!g_state.pdf || !g_state.pdf->document || pageIndex < 0 ||
        pageIndex >= g_state.pdf->pageCount) {
        return false;
    }
    double w = 0.0;
    double h = 0.0;
    if (!FPDF_GetPageSizeByIndex(g_state.pdf->document, pageIndex, &w, &h) ||
        w <= 0.0 || h <= 0.0) {
        return false;
    }
    pageW = std::max(1.0, w);
    pageH = std::max(1.0, h);
    return true;
}

bool BuildPageLayoutForDocTop(HWND hwnd, int pageIndex, double docTop, PageLayout& out) {
    RECT avail = ViewerAvailableRect(hwnd);
    if (avail.right <= avail.left || avail.bottom <= avail.top) return false;

    double pageW = 1.0;
    double pageH = 1.0;
    if (!PageSizePt(pageIndex, pageW, pageH)) return false;

    const double availW = static_cast<double>(avail.right - avail.left);
    const double availH = static_cast<double>(avail.bottom - avail.top);
    const double scale = std::min(availW / pageW, availH / pageH) *
                         std::clamp(g_state.zoomFactor, 0.25, 4.0);
    const int width = std::clamp(static_cast<int>(std::lround(pageW * scale)), kMinRenderPx, kMaxRenderPx);
    const int height = std::clamp(static_cast<int>(std::lround(pageH * scale)), kMinRenderPx, kMaxRenderPx);

    out.pageIndex = pageIndex;
    out.pageWidthPt = pageW;
    out.pageHeightPt = pageH;
    out.width = width;
    out.height = height;
    out.docTop = docTop;
    out.docBottom = docTop + static_cast<double>(height);
    out.rect.left = avail.left + ((avail.right - avail.left) - width) / 2 +
                    static_cast<int>(std::lround(g_state.panX));
    out.rect.top = avail.top + static_cast<int>(std::lround(docTop - g_state.scrollY));
    out.rect.right = out.rect.left + width;
    out.rect.bottom = out.rect.top + height;
    return true;
}

double ContinuousDocumentHeight(HWND hwnd) {
    if (!g_state.pdf || g_state.pdf->pageCount <= 0) return 0.0;
    double y = 0.0;
    const int first = FirstDisplayedPage();
    const int last = LastDisplayedPage();
    for (int i = first; i <= last; ++i) {
        PageLayout layout{};
        if (!BuildPageLayoutForDocTop(hwnd, i, y, layout)) continue;
        y += static_cast<double>(layout.height);
        if (i < last) y += kContinuousPageGapPx;
    }
    return y;
}

double MaxContinuousScrollY(HWND hwnd) {
    RECT avail = ViewerAvailableRect(hwnd);
    const double viewH = std::max<LONG>(0, avail.bottom - avail.top);
    return std::max(0.0, ContinuousDocumentHeight(hwnd) - viewH);
}

void ClampContinuousScroll(HWND hwnd) {
    const double maxY = MaxContinuousScrollY(hwnd);
    g_state.scrollY = std::clamp(g_state.scrollY, 0.0, maxY);
}

void AdjustActiveSelectionForScroll(double scrollDeltaY) {
    if (!g_state.selectingRect || std::abs(scrollDeltaY) < 0.001) return;
    // The anchor belongs to the document, while selectionCurrent follows the
    // mouse cursor in the client area. Keep the anchor at the same document
    // position when the view scrolls during a drag.
    g_state.selectionAnchor.y -= static_cast<LONG>(std::lround(scrollDeltaY));
}

std::vector<PageLayout> BuildVisiblePageLayouts(HWND hwnd) {
    std::vector<PageLayout> layouts;
    if (!g_state.pdf || g_state.pdf->pageCount <= 0) return layouts;
    RECT avail = ViewerAvailableRect(hwnd);
    if (avail.right <= avail.left || avail.bottom <= avail.top) return layouts;

    const double viewTop = g_state.scrollY;
    const double viewBottom = g_state.scrollY + static_cast<double>(avail.bottom - avail.top);
    double y = 0.0;
    const int first = FirstDisplayedPage();
    const int last = LastDisplayedPage();
    for (int i = first; i <= last; ++i) {
        PageLayout layout{};
        if (!BuildPageLayoutForDocTop(hwnd, i, y, layout)) continue;
        if (layout.docBottom >= viewTop - kContinuousPageGapPx &&
            layout.docTop <= viewBottom + kContinuousPageGapPx) {
            layouts.push_back(layout);
        }
        y += static_cast<double>(layout.height);
        if (i < last) y += kContinuousPageGapPx;
    }
    return layouts;
}

bool PageLayoutForIndex(HWND hwnd, int pageIndex, PageLayout& out) {
    if (!g_state.pdf || !IsDisplayedPage(pageIndex)) return false;
    double y = 0.0;
    const int first = FirstDisplayedPage();
    const int last = LastDisplayedPage();
    for (int i = first; i <= last; ++i) {
        PageLayout layout{};
        if (!BuildPageLayoutForDocTop(hwnd, i, y, layout)) return false;
        if (i == pageIndex) {
            out = layout;
            return true;
        }
        y += static_cast<double>(layout.height);
        if (i < last) y += kContinuousPageGapPx;
    }
    return false;
}

void SyncCurrentPageFromScroll(HWND hwnd) {
    if (!g_state.pdf || g_state.pdf->pageCount <= 0) return;
    RECT avail = ViewerAvailableRect(hwnd);
    const double centerY = g_state.scrollY + std::max<LONG>(0, avail.bottom - avail.top) / 2.0;
    int bestPage = std::clamp(g_state.currentPage, FirstDisplayedPage(), LastDisplayedPage());
    double bestDistance = DBL_MAX;
    double y = 0.0;
    double bestDocTop = 0.0;
    const int first = FirstDisplayedPage();
    const int last = LastDisplayedPage();
    for (int i = first; i <= last; ++i) {
        PageLayout layout{};
        if (!BuildPageLayoutForDocTop(hwnd, i, y, layout)) continue;
        const double pageCenter = (layout.docTop + layout.docBottom) / 2.0;
        const double distance = std::abs(pageCenter - centerY);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestPage = i;
            bestDocTop = layout.docTop;
        }
        y += static_cast<double>(layout.height);
        if (i < last) y += kContinuousPageGapPx;
    }
    if (bestPage != g_state.currentPage) {
        g_state.currentPage = bestPage;
        UpdateStatusFromPdf();
    }
    g_state.panY = bestDocTop - g_state.scrollY;
}

void ScrollContinuousBy(HWND hwnd, double deltaY) {
    if (!g_state.pdf || g_state.pdf->pageCount <= 0) return;
    const double before = g_state.scrollY;
    g_state.scrollY += deltaY;
    ClampContinuousScroll(hwnd);
    AdjustActiveSelectionForScroll(g_state.scrollY - before);
    SyncCurrentPageFromScroll(hwnd);
    if (std::abs(g_state.scrollY - before) > 0.001) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void ScrollContinuousTo(HWND hwnd, double scrollY) {
    if (!g_state.pdf || g_state.pdf->pageCount <= 0) return;
    const double before = g_state.scrollY;
    g_state.scrollY = scrollY;
    ClampContinuousScroll(hwnd);
    AdjustActiveSelectionForScroll(g_state.scrollY - before);
    SyncCurrentPageFromScroll(hwnd);
    if (std::abs(g_state.scrollY - before) > 0.001) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void ScrollToPageTop(HWND hwnd, int pageIndex, double pageOffsetY) {
    PageLayout layout{};
    if (!PageLayoutForIndex(hwnd, pageIndex, layout)) return;
    g_state.currentPage = std::clamp(pageIndex, FirstDisplayedPage(), LastDisplayedPage());
    g_state.scrollY = layout.docTop - pageOffsetY;
    ClampContinuousScroll(hwnd);
    SyncCurrentPageFromScroll(hwnd);
    UpdateStatusFromPdf();
    InvalidateRect(hwnd, nullptr, FALSE);
}

void ScrollToPdfY(HWND hwnd, int pageIndex, double yPt) {
    PageLayout layout{};
    if (!PageLayoutForIndex(hwnd, pageIndex, layout)) return;

    RECT avail = ViewerAvailableRect(hwnd);
    const double viewHeight = static_cast<double>(std::max<LONG>(1, avail.bottom - avail.top));
    const double pageHeightPt = std::max(1.0, layout.pageHeightPt);
    const double pageHeightPx = std::max(1.0, static_cast<double>(layout.rect.bottom - layout.rect.top));
    const double yFromTopPt = std::clamp(pageHeightPt - yPt, 0.0, pageHeightPt);
    const double yFromTopPx = yFromTopPt * pageHeightPx / pageHeightPt;

    g_state.currentPage = std::clamp(pageIndex, FirstDisplayedPage(), LastDisplayedPage());
    g_state.scrollY = layout.docTop + yFromTopPx - (viewHeight * 0.35);
    ClampContinuousScroll(hwnd);
    SyncCurrentPageFromScroll(hwnd);
    UpdateStatusFromPdf();
    InvalidateRect(hwnd, nullptr, FALSE);
}

RenderedPage* EnsureRenderedPage(int pageIndex, int width, int height) {
    for (auto& page : g_state.renderedPages) {
        if (page.Matches(pageIndex, width, height)) return &page;
    }
    g_state.renderedPages.erase(
        std::remove_if(g_state.renderedPages.begin(), g_state.renderedPages.end(),
                       [&](const RenderedPage& page) {
                           return page.pageIndex == pageIndex &&
                                  (page.width != width || page.height != height);
                       }),
        g_state.renderedPages.end());
    RenderedPage rendered;
    if (!RenderPageToCache(g_state.pdf ? g_state.pdf->document : nullptr,
                           pageIndex,
                           width,
                           height,
                           rendered)) {
        return nullptr;
    }
    g_state.renderedPages.push_back(std::move(rendered));
    return &g_state.renderedPages.back();
}

void PruneRenderedPageCache(const std::vector<PageLayout>& visible) {
    g_state.renderedPages.erase(
        std::remove_if(g_state.renderedPages.begin(), g_state.renderedPages.end(),
                       [&](const RenderedPage& cached) {
                           return std::none_of(visible.begin(), visible.end(),
                                               [&](const PageLayout& layout) {
                                                   return cached.Matches(layout.pageIndex,
                                                                         layout.width,
                                                                         layout.height);
                                               });
                       }),
        g_state.renderedPages.end());
}

void FillRectAlpha(HDC hdc, const RECT& r, COLORREF color, BYTE alpha) {
    if (r.right <= r.left || r.bottom <= r.top || alpha == 0) return;
    if (alpha == 255) {
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(hdc, &r, brush);
        DeleteObject(brush);
        return;
    }

    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    HDC mem = CreateCompatibleDC(hdc);
    if (!mem) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(mem);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HBRUSH brush = CreateSolidBrush(color);
    RECT local{0, 0, w, h};
    FillRect(mem, &local, brush);
    DeleteObject(brush);

    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = alpha;
    AlphaBlend(hdc, r.left, r.top, w, h, mem, 0, 0, w, h, bf);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void DrawPolylineAlpha(HDC hdc, const std::vector<POINT>& pts, int penWidth, COLORREF color, BYTE alpha) {
    if (pts.size() < 2 || penWidth <= 0 || alpha == 0) return;
    if (alpha >= 250) {
        HPEN pen = CreatePen(PS_SOLID, penWidth, color);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        Polyline(hdc, pts.data(), static_cast<int>(pts.size()));
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        return;
    }

    LONG minX = pts.front().x;
    LONG minY = pts.front().y;
    LONG maxX = pts.front().x;
    LONG maxY = pts.front().y;
    for (const POINT& pt : pts) {
        minX = std::min(minX, pt.x);
        minY = std::min(minY, pt.y);
        maxX = std::max(maxX, pt.x);
        maxY = std::max(maxY, pt.y);
    }
    RECT bounds{minX, minY, maxX, maxY};
    InflateRect(&bounds, penWidth + 4, penWidth + 4);
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;

    const int w = bounds.right - bounds.left;
    const int h = bounds.bottom - bounds.top;
    HDC mem = CreateCompatibleDC(hdc);
    if (!mem) return;
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    if (!bmp) {
        DeleteDC(mem);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, hdc, bounds.left, bounds.top, SRCCOPY);

    std::vector<POINT> localPts;
    localPts.reserve(pts.size());
    for (const POINT& pt : pts) {
        localPts.push_back(POINT{pt.x - bounds.left, pt.y - bounds.top});
    }
    HPEN pen = CreatePen(PS_SOLID, penWidth, color);
    HGDIOBJ oldPen = SelectObject(mem, pen);
    Polyline(mem, localPts.data(), static_cast<int>(localPts.size()));
    SelectObject(mem, oldPen);
    DeleteObject(pen);

    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = alpha;
    AlphaBlend(hdc, bounds.left, bounds.top, w, h, mem, 0, 0, w, h, bf);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void DrawRenderedPage(HDC hdc, const RenderedPage& page, const RECT& dst) {
    if (page.pixels.empty()) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = page.width;
    bmi.bmiHeader.biHeight = -page.height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(hdc,
                  dst.left,
                  dst.top,
                  dst.right - dst.left,
                  dst.bottom - dst.top,
                  0,
                  0,
                  page.width,
                  page.height,
                  page.pixels.data(),
                  &bmi,
                  DIB_RGB_COLORS,
                  SRCCOPY);
}

POINT PdfPointToClient(const RECT& pageRect, double pageWidthPt, double pageHeightPt, double xPt, double yPt) {
    const double w = std::max(1.0, pageWidthPt);
    const double h = std::max(1.0, pageHeightPt);
    POINT pt{};
    pt.x = static_cast<LONG>(std::lround(pageRect.left + (xPt / w) * (pageRect.right - pageRect.left)));
    pt.y = static_cast<LONG>(std::lround(pageRect.top + ((h - yPt) / h) * (pageRect.bottom - pageRect.top)));
    return pt;
}

bool ClientPointToPdfPoint(const PageLayout& layout, POINT clientPt, double& outXPt, double& outYPt) {
    if (layout.rect.right <= layout.rect.left || layout.rect.bottom <= layout.rect.top) return false;
    const double relX = static_cast<double>(clientPt.x - layout.rect.left) /
                        static_cast<double>(layout.rect.right - layout.rect.left);
    const double relY = static_cast<double>(clientPt.y - layout.rect.top) /
                        static_cast<double>(layout.rect.bottom - layout.rect.top);
    outXPt = std::clamp(relX, 0.0, 1.0) * std::max(1.0, layout.pageWidthPt);
    outYPt = (1.0 - std::clamp(relY, 0.0, 1.0)) * std::max(1.0, layout.pageHeightPt);
    return true;
}

RECT NormalizeClientRect(POINT a, POINT b) {
    RECT r{};
    r.left = std::min(a.x, b.x);
    r.right = std::max(a.x, b.x);
    r.top = std::min(a.y, b.y);
    r.bottom = std::max(a.y, b.y);
    return r;
}

bool RectHasArea(const RECT& r, int minPx = 2) {
    return (r.right - r.left) >= minPx && (r.bottom - r.top) >= minPx;
}

bool IntersectsPdfRect(double aLeft, double aTop, double aRight, double aBottom,
                       double bLeft, double bTop, double bRight, double bBottom) {
    const double left = std::max(std::min(aLeft, aRight), std::min(bLeft, bRight));
    const double right = std::min(std::max(aLeft, aRight), std::max(bLeft, bRight));
    const double bottom = std::max(std::min(aTop, aBottom), std::min(bTop, bBottom));
    const double top = std::min(std::max(aTop, aBottom), std::max(bTop, bBottom));
    return left <= right && bottom <= top;
}

bool ItemBoundsPdf(const clrop::Item& item, double& left, double& top, double& right, double& bottom) {
    bool hasPoint = false;
    auto addPt = [&](double x, double y) {
        if (!hasPoint) {
            left = right = x;
            top = bottom = y;
            hasPoint = true;
        } else {
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::max(top, y);
            bottom = std::min(bottom, y);
        }
    };

    if (item.bbox) {
        const auto& b = *item.bbox;
        addPt(b[0], b[1]);
        addPt(b[0] + b[2], b[1] - b[3]);
    }
    if (item.p1) addPt((*item.p1)[0], (*item.p1)[1]);
    if (item.p2) addPt((*item.p2)[0], (*item.p2)[1]);
    for (size_t i = 0; i + 1 < item.quads.size(); i += 2) addPt(item.quads[i], item.quads[i + 1]);
    for (size_t i = 0; i + 1 < item.path.size(); i += 2) addPt(item.path[i], item.path[i + 1]);
    return hasPoint;
}

std::wstring ClropItemSelectableText(const clrop::Item& item) {
    if (!item.content.empty()) return item.content;
    if (item.kind == clrop::Item::Kind::Math && !item.mathKind.empty()) {
        return Utf8ToWideLocal(item.mathKind);
    }
    return {};
}

std::wstring ExtractAnnotationTextInSelection(int pageIndex,
                                              double left,
                                              double top,
                                              double right,
                                              double bottom) {
    if (!g_state.showAnnotations || !g_state.clrop) return {};
    std::wstring out;
    for (const auto& page : g_state.clrop->document.pages) {
        if (page.page != pageIndex) continue;
        for (const auto& item : page.items) {
            std::wstring text = ClropItemSelectableText(item);
            if (text.empty()) continue;
            double itemLeft = 0.0, itemTop = 0.0, itemRight = 0.0, itemBottom = 0.0;
            if (!ItemBoundsPdf(item, itemLeft, itemTop, itemRight, itemBottom)) continue;
            if (!IntersectsPdfRect(left, top, right, bottom, itemLeft, itemTop, itemRight, itemBottom)) continue;
            if (!out.empty()) out += L"\r\n";
            out += text;
        }
    }
    return out;
}

std::wstring ExtractPdfTextInSelection(int pageIndex,
                                       double left,
                                       double top,
                                       double right,
                                       double bottom) {
    if (!g_state.pdf || !g_state.pdf->document || pageIndex < 0 || pageIndex >= g_state.pdf->pageCount) return {};
    FPDF_PAGE page = FPDF_LoadPage(g_state.pdf->document, pageIndex);
    if (!page) return {};
    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage) {
        FPDF_ClosePage(page);
        return {};
    }

    const double l = std::min(left, right);
    const double r = std::max(left, right);
    const double t = std::max(top, bottom);
    const double b = std::min(top, bottom);
    const int count = FPDFText_GetBoundedText(textPage, l, t, r, b, nullptr, 0);
    std::wstring out;
    if (count > 0) {
        std::vector<unsigned short> buffer(static_cast<size_t>(count) + 1, 0);
        const int copied = FPDFText_GetBoundedText(textPage, l, t, r, b, buffer.data(), static_cast<int>(buffer.size()));
        const int len = std::max(0, std::min(count, copied > 0 ? copied : count));
        out.assign(reinterpret_cast<const wchar_t*>(buffer.data()),
                   reinterpret_cast<const wchar_t*>(buffer.data()) + len);
        while (!out.empty() && out.back() == L'\0') out.pop_back();
    }

    FPDFText_ClosePage(textPage);
    FPDF_ClosePage(page);
    return out;
}

std::wstring BuildSelectedText(int pageIndex,
                               double left,
                               double top,
                               double right,
                               double bottom) {
    std::wstring body = ExtractPdfTextInSelection(pageIndex, left, top, right, bottom);
    std::wstring ann = ExtractAnnotationTextInSelection(pageIndex, left, top, right, bottom);
    if (body.empty()) return ann;
    if (ann.empty()) return body;
    return body + L"\r\n" + ann;
}

std::wstring CurrentSelectedAnnotationText() {
    if (!g_state.clrop || g_state.selectedPage < 0 || g_state.selectedItem < 0) return {};
    for (const auto& page : g_state.clrop->document.pages) {
        if (page.page != g_state.selectedPage) continue;
        if (g_state.selectedItem >= 0 && g_state.selectedItem < static_cast<int>(page.items.size())) {
            return ClropItemSelectableText(page.items[static_cast<size_t>(g_state.selectedItem)]);
        }
    }
    return {};
}

void ShowReadonlyCopySuccessStatus(HWND hwnd) {
    g_state.status = L"Copied text.";
    InvalidateRect(hwnd, nullptr, FALSE);
}

bool CopyUnicodeTextToClipboard(HWND hwnd, const std::wstring& text) {
    if (text.empty()) return false;
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hmem) {
        CloseClipboard();
        return false;
    }
    void* data = GlobalLock(hmem);
    if (!data) {
        GlobalFree(hmem);
        CloseClipboard();
        return false;
    }
    memcpy(data, text.c_str(), bytes);
    GlobalUnlock(hmem);
    if (!SetClipboardData(CF_UNICODETEXT, hmem)) {
        GlobalFree(hmem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    ShowReadonlyCopySuccessStatus(hwnd);
    return true;
}

bool CopyCurrentSelection(HWND hwnd) {
    std::wstring text = g_state.selectedText;
    if (text.empty()) text = CurrentSelectedAnnotationText();
    return CopyUnicodeTextToClipboard(hwnd, text);
}

struct PdfFileWriter {
    FPDF_FILEWRITE base{};
    HANDLE file = INVALID_HANDLE_VALUE;
    bool ok = true;
};

int WritePdfBlock(FPDF_FILEWRITE* writer, const void* data, unsigned long size) {
    auto* self = reinterpret_cast<PdfFileWriter*>(writer);
    if (!self || self->file == INVALID_HANDLE_VALUE || !data) return 0;
    const char* cursor = static_cast<const char*>(data);
    unsigned long remaining = size;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<unsigned long>(remaining, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(self->file, cursor, chunk, &written, nullptr) || written != chunk) {
            self->ok = false;
            return 0;
        }
        cursor += written;
        remaining -= chunk;
    }
    return 1;
}

std::optional<std::filesystem::path> PromptAnnotatedPdfSavePath(HWND owner) {
    if (!g_state.pdf) return std::nullopt;
    IFileSaveDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) return std::nullopt;

    const COMDLG_FILTERSPEC filters[] = {
        {L"PDF files (*.pdf)", L"*.pdf"},
        {L"All files (*.*)", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"pdf");
    dialog->SetTitle(L"注釈合成PDFを書き出し");

    std::filesystem::path source(g_state.pdf->path);
    std::wstring fileName = source.stem().wstring() + L"_annotated.pdf";
    dialog->SetFileName(fileName.c_str());
    if (!source.parent_path().empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(source.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
            dialog->SetDefaultFolder(folder);
            folder->Release();
        }
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT);
    }
    hr = dialog->Show(owner);
    if (FAILED(hr)) {
        dialog->Release();
        return std::nullopt;
    }

    IShellItem* item = nullptr;
    hr = dialog->GetResult(&item);
    dialog->Release();
    if (FAILED(hr) || !item) return std::nullopt;
    PWSTR raw = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    item->Release();
    if (FAILED(hr) || !raw) return std::nullopt;
    std::filesystem::path out(raw);
    CoTaskMemFree(raw);
    return out;
}

bool SameAbsolutePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ecA, ecB;
    std::filesystem::path ca = std::filesystem::weakly_canonical(a, ecA);
    std::filesystem::path cb = std::filesystem::weakly_canonical(b, ecB);
    if (ecA) ca = std::filesystem::absolute(a, ecA);
    if (ecB) cb = std::filesystem::absolute(b, ecB);
    std::wstring sa = ca.wstring();
    std::wstring sb = cb.wstring();
    std::transform(sa.begin(), sa.end(), sa.begin(), ::towlower);
    std::transform(sb.begin(), sb.end(), sb.begin(), ::towlower);
    return sa == sb;
}

COLORREF ClropColor(const clrop::Item& item) {
    return HexToColor(item.color);
}

BYTE ClropAlphaByte(const clrop::Item& item, double fallback) {
    const double value = item.alpha > 0.0 ? item.alpha : fallback;
    return static_cast<BYTE>(std::clamp(value, 0.0, 1.0) * 255.0);
}

void SetPathStroke(FPDF_PAGEOBJECT obj, COLORREF color, BYTE alpha, double widthPt) {
    FPDFPageObj_SetStrokeColor(obj, GetRValue(color), GetGValue(color), GetBValue(color), alpha);
    FPDFPageObj_SetStrokeWidth(obj, static_cast<float>(std::max(0.5, widthPt)));
    FPDFPath_SetDrawMode(obj, FPDF_FILLMODE_NONE, true);
}

void AddPdfRectObject(FPDF_PAGE page, double left, double bottom, double width, double height,
                      COLORREF color, BYTE alpha, bool fill, double strokeWidth) {
    FPDF_PAGEOBJECT obj = FPDFPageObj_CreateNewRect(static_cast<float>(left),
                                                   static_cast<float>(bottom),
                                                   static_cast<float>(width),
                                                   static_cast<float>(height));
    if (!obj) return;
    if (fill) {
        FPDFPageObj_SetFillColor(obj, GetRValue(color), GetGValue(color), GetBValue(color), alpha);
        FPDFPath_SetDrawMode(obj, FPDF_FILLMODE_ALTERNATE, false);
    } else {
        SetPathStroke(obj, color, alpha, strokeWidth);
    }
    FPDFPage_InsertObject(page, obj);
}

void AddPdfLineObject(FPDF_PAGE page, double x1, double y1, double x2, double y2,
                      COLORREF color, BYTE alpha, double widthPt) {
    FPDF_PAGEOBJECT obj = FPDFPageObj_CreateNewPath(static_cast<float>(x1), static_cast<float>(y1));
    if (!obj) return;
    FPDFPath_LineTo(obj, static_cast<float>(x2), static_cast<float>(y2));
    SetPathStroke(obj, color, alpha, widthPt);
    FPDFPage_InsertObject(page, obj);
}

void AddPdfArrowHead(FPDF_PAGE page, double x1, double y1, double x2, double y2,
                     COLORREF color, BYTE alpha, double widthPt, bool doubleHead) {
    const double dx = x2 - x1;
    const double dy = y2 - y1;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.001) return;
    const double ux = dx / len;
    const double uy = dy / len;
    const double px = -uy;
    const double py = ux;
    const double head = std::clamp(widthPt * 5.0, 8.0, 18.0);
    const double spread = head * 0.45;
    AddPdfLineObject(page, x2, y2, x2 - ux * head + px * spread, y2 - uy * head + py * spread,
                     color, alpha, widthPt);
    AddPdfLineObject(page, x2, y2, x2 - ux * head - px * spread, y2 - uy * head - py * spread,
                     color, alpha, widthPt);
    if (doubleHead) {
        AddPdfLineObject(page, x1, y1, x1 + ux * head + px * spread, y1 + uy * head + py * spread,
                         color, alpha, widthPt);
        AddPdfLineObject(page, x1, y1, x1 + ux * head - px * spread, y1 + uy * head - py * spread,
                         color, alpha, widthPt);
    }
}

void AddPdfPolylineObject(FPDF_PAGE page, const std::vector<double>& path,
                          COLORREF color, BYTE alpha, double widthPt) {
    if (path.size() < 4) return;
    FPDF_PAGEOBJECT obj = FPDFPageObj_CreateNewPath(static_cast<float>(path[0]), static_cast<float>(path[1]));
    if (!obj) return;
    for (size_t i = 2; i + 1 < path.size(); i += 2) {
        FPDFPath_LineTo(obj, static_cast<float>(path[i]), static_cast<float>(path[i + 1]));
    }
    SetPathStroke(obj, color, alpha, widthPt);
    FPDFPage_InsertObject(page, obj);
}

void AddPdfTextObject(FPDF_DOCUMENT doc, FPDF_PAGE page, const clrop::Item& item, FPDF_FONT font) {
    if (!font || !item.bbox) return;
    std::wstring text = item.content;
    if (text.empty() && item.kind == clrop::Item::Kind::Math) text = Utf8ToWideLocal(item.mathKind);
    if (text.empty()) return;
    const auto& b = *item.bbox;
    const double pt = item.pt > 0.0 ? item.pt : 12.0;
    FPDF_PAGEOBJECT obj = FPDFPageObj_CreateTextObj(doc, font, static_cast<float>(pt));
    if (!obj) return;
    FPDFText_SetText(obj, reinterpret_cast<FPDF_WIDESTRING>(text.c_str()));
    COLORREF color = ClropColor(item);
    FPDFPageObj_SetFillColor(obj, GetRValue(color), GetGValue(color), GetBValue(color), 255);
    FS_MATRIX m{};
    m.a = 1.0f;
    m.d = 1.0f;
    m.e = static_cast<float>(b[0]);
    m.f = static_cast<float>(b[1] - pt);
    FPDFPageObj_SetMatrix(obj, &m);
    FPDFPage_InsertObject(page, obj);
}

void AddClropItemToPdfPage(FPDF_DOCUMENT doc, FPDF_PAGE page, const clrop::Item& item, FPDF_FONT font) {
    COLORREF color = ClropColor(item);
    switch (item.kind) {
    case clrop::Item::Kind::MarkerText:
    case clrop::Item::Kind::TextColor: {
        BYTE alpha = ClropAlphaByte(item, item.kind == clrop::Item::Kind::TextColor ? 0.22 : 0.35);
        for (size_t qi = 0; qi + 7 < item.quads.size(); qi += 8) {
            double left = item.quads[qi], right = item.quads[qi], top = item.quads[qi + 1], bottom = item.quads[qi + 1];
            for (size_t k = 2; k < 8; k += 2) {
                left = std::min(left, item.quads[qi + k]);
                right = std::max(right, item.quads[qi + k]);
                top = std::max(top, item.quads[qi + k + 1]);
                bottom = std::min(bottom, item.quads[qi + k + 1]);
            }
            AddPdfRectObject(page, left, bottom, right - left, top - bottom, color, alpha, true, 0.0);
        }
        break;
    }
    case clrop::Item::Kind::Line:
    case clrop::Item::Kind::Arrow:
    case clrop::Item::Kind::Wave:
        if (item.p1 && item.p2) {
            const double width = item.width > 0 ? item.width : 2.0;
            AddPdfLineObject(page, (*item.p1)[0], (*item.p1)[1], (*item.p2)[0], (*item.p2)[1],
                             color, 255, width);
            if (item.kind == clrop::Item::Kind::Arrow) {
                AddPdfArrowHead(page, (*item.p1)[0], (*item.p1)[1], (*item.p2)[0], (*item.p2)[1],
                                color, 255, width, item.arrowHead == "double");
            }
        }
        break;
    case clrop::Item::Kind::MarkerFree:
    case clrop::Item::Kind::Freehand:
        AddPdfPolylineObject(page, item.path, color, ClropAlphaByte(item, 1.0), item.width > 0 ? item.width : 2.0);
        break;
    case clrop::Item::Kind::Shape:
        if (item.bbox) {
            const auto& b = *item.bbox;
            const bool fill = (item.shapeDrawMode == "fill" || item.shapeDrawMode == "filled");
            AddPdfRectObject(page, b[0], b[1] - b[3], b[2], b[3], color, ClropAlphaByte(item, fill ? 0.30 : 1.0),
                             fill, item.width > 0 ? item.width : 2.0);
        }
        break;
    case clrop::Item::Kind::Text:
    case clrop::Item::Kind::Math:
        AddPdfTextObject(doc, page, item, font);
        break;
    default:
        break;
    }
}

bool SavePdfDocumentAtomic(FPDF_DOCUMENT doc, const std::filesystem::path& outPath, std::wstring& err) {
    if (!doc || outPath.empty() || outPath.parent_path().empty()) {
        err = L"Invalid output path.";
        return false;
    }
    std::filesystem::path temp = outPath;
    temp += L".tmp.";
    temp += std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        err = L"Failed to create temporary output file.";
        return false;
    }
    PdfFileWriter writer{};
    writer.base.version = 1;
    writer.base.WriteBlock = &WritePdfBlock;
    writer.file = file;
    bool ok = !!FPDF_SaveAsCopy(doc, &writer.base, 0);
    if (ok) ok = writer.ok;
    if (ok) ok = FlushFileBuffers(file) != 0;
    if (!CloseHandle(file)) ok = false;
    writer.file = INVALID_HANDLE_VALUE;
    if (!ok) {
        DeleteFileW(temp.c_str());
        err = L"Failed to write PDF.";
        return false;
    }
    if (!MoveFileExW(temp.c_str(), outPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        err = L"Failed to replace output PDF.";
        return false;
    }
    return true;
}

bool ExportAnnotatedPdf(HWND hwnd) {
    if (!g_state.pdf || !g_state.pdf->document) {
        g_state.status = L"No PDF is open.";
        InvalidateRect(hwnd, nullptr, FALSE);
        return false;
    }
    if (!g_state.clrop) {
        g_state.status = L"No CLROP annotations are loaded.";
        InvalidateRect(hwnd, nullptr, FALSE);
        return false;
    }
    auto outPath = PromptAnnotatedPdfSavePath(hwnd);
    if (!outPath) return false;
    if (SameAbsolutePath(*outPath, std::filesystem::path(g_state.pdf->path))) {
        g_state.status = L"Export blocked: output path is the original PDF.";
        InvalidateRect(hwnd, nullptr, FALSE);
        return false;
    }

    FPDF_DOCUMENT dest = FPDF_CreateNewDocument();
    if (!dest) {
        g_state.status = L"Failed to create output PDF.";
        InvalidateRect(hwnd, nullptr, FALSE);
        return false;
    }
    const int pageCount = FPDF_GetPageCount(g_state.pdf->document);
    std::string range = "1-" + std::to_string(pageCount);
    if (!FPDF_ImportPages(dest, g_state.pdf->document, range.c_str(), 0)) {
        FPDF_CloseDocument(dest);
        g_state.status = L"Failed to copy PDF pages.";
        InvalidateRect(hwnd, nullptr, FALSE);
        return false;
    }
    FPDF_FONT font = FPDFText_LoadStandardFont(dest, "Helvetica");
    for (const auto& annPage : g_state.clrop->document.pages) {
        if (annPage.page < 0 || annPage.page >= pageCount) continue;
        FPDF_PAGE page = FPDF_LoadPage(dest, annPage.page);
        if (!page) continue;
        for (const auto& item : annPage.items) {
            AddClropItemToPdfPage(dest, page, item, font);
        }
        FPDFPage_GenerateContent(page);
        FPDF_ClosePage(page);
    }
    if (font) FPDFFont_Close(font);

    std::wstring err;
    bool ok = SavePdfDocumentAtomic(dest, *outPath, err);
    FPDF_CloseDocument(dest);
    g_state.status = ok ? (L"Exported annotated PDF: " + outPath->filename().wstring())
                        : (L"Export failed: " + err);
    InvalidateRect(hwnd, nullptr, FALSE);
    return ok;
}

RECT ItemBoundsClient(const clrop::Item& item, const RECT& pageRect, double pageWidthPt, double pageHeightPt) {
    bool hasPoint = false;
    LONG minX = 0, minY = 0, maxX = 0, maxY = 0;
    auto addPt = [&](double x, double y) {
        POINT pt = PdfPointToClient(pageRect, pageWidthPt, pageHeightPt, x, y);
        if (!hasPoint) {
            minX = maxX = pt.x;
            minY = maxY = pt.y;
            hasPoint = true;
        } else {
            minX = std::min(minX, pt.x);
            maxX = std::max(maxX, pt.x);
            minY = std::min(minY, pt.y);
            maxY = std::max(maxY, pt.y);
        }
    };

    if (item.bbox) {
        const auto& b = *item.bbox;
        addPt(b[0], b[1]);
        addPt(b[0] + b[2], b[1] - b[3]);
    }
    if (item.p1) addPt((*item.p1)[0], (*item.p1)[1]);
    if (item.p2) addPt((*item.p2)[0], (*item.p2)[1]);
    for (size_t i = 0; i + 1 < item.quads.size(); i += 2) addPt(item.quads[i], item.quads[i + 1]);
    for (size_t i = 0; i + 1 < item.path.size(); i += 2) addPt(item.path[i], item.path[i + 1]);

    RECT r{};
    if (hasPoint) {
        r.left = minX;
        r.top = minY;
        r.right = maxX;
        r.bottom = maxY;
        InflateRect(&r, 4, 4);
    }
    return r;
}

void DrawArrowHead(HDC hdc, POINT p1, POINT p2, int sizePx, bool doubleHead) {
    const double dx = static_cast<double>(p2.x - p1.x);
    const double dy = static_cast<double>(p2.y - p1.y);
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.1) return;
    const double ux = dx / len;
    const double uy = dy / len;
    const double px = -uy;
    const double py = ux;
    POINT pts[3] = {
        p2,
        { static_cast<LONG>(std::lround(p2.x - ux * sizePx + px * sizePx * 0.45)),
          static_cast<LONG>(std::lround(p2.y - uy * sizePx + py * sizePx * 0.45)) },
        { static_cast<LONG>(std::lround(p2.x - ux * sizePx - px * sizePx * 0.45)),
          static_cast<LONG>(std::lround(p2.y - uy * sizePx - py * sizePx * 0.45)) },
    };
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Polyline(hdc, pts, 3);
    if (doubleHead) {
        POINT startPts[3] = {
            p1,
            { static_cast<LONG>(std::lround(p1.x + ux * sizePx + px * sizePx * 0.45)),
              static_cast<LONG>(std::lround(p1.y + uy * sizePx + py * sizePx * 0.45)) },
            { static_cast<LONG>(std::lround(p1.x + ux * sizePx - px * sizePx * 0.45)),
              static_cast<LONG>(std::lround(p1.y + uy * sizePx - py * sizePx * 0.45)) },
        };
        Polyline(hdc, startPts, 3);
    }
    SelectObject(hdc, oldBrush);
}

void TintRenderedTextRect(HDC hdc, const RECT& rect, COLORREF color,
                          const RECT& pageRect, const RenderedPage* rendered) {
    if (!rendered || rendered->pixels.empty() || rendered->width <= 0 || rendered->height <= 0 ||
        rendered->stride <= 0 || rect.left >= rect.right || rect.top >= rect.bottom) {
        return;
    }
    RECT clipped{};
    if (!IntersectRect(&clipped, &rect, &pageRect)) return;
    const int w = rect.right - rect.left;
    const int h = rect.bottom - rect.top;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    std::vector<DWORD> overlay(static_cast<size_t>(w) * h, 0);
    const double pageW = std::max(1L, pageRect.right - pageRect.left);
    const double pageH = std::max(1L, pageRect.bottom - pageRect.top);
    for (int y = clipped.top; y < clipped.bottom; ++y) {
        const int srcY = std::clamp(
            static_cast<int>((y - pageRect.top) * rendered->height / pageH), 0, rendered->height - 1);
        const unsigned char* src =
            rendered->pixels.data() + static_cast<size_t>(srcY) * rendered->stride;
        DWORD* dst = overlay.data() + static_cast<size_t>(y - rect.top) * w;
        for (int x = clipped.left; x < clipped.right; ++x) {
            const int srcX = std::clamp(
                static_cast<int>((x - pageRect.left) * rendered->width / pageW), 0, rendered->width - 1);
            const size_t srcIndex = static_cast<size_t>(srcX) * 4;
            const BYTE b = src[srcIndex + 0];
            const BYTE g = src[srcIndex + 1];
            const BYTE red = src[srcIndex + 2];
            const int maxc = std::max<int>(red, std::max<int>(g, b));
            const int minc = std::min<int>(red, std::min<int>(g, b));
            const bool darkText = maxc <= 210;
            const bool saturatedText = maxc - minc >= 40;
            if (!darkText && !saturatedText) continue;
            const BYTE alpha = static_cast<BYTE>(
                std::clamp(darkText ? 255 - maxc : 255 - minc, 1, 255));
            const double a = alpha / 255.0;
            const BYTE outR = static_cast<BYTE>(std::lround(GetRValue(color) * a));
            const BYTE outG = static_cast<BYTE>(std::lround(GetGValue(color) * a));
            const BYTE outB = static_cast<BYTE>(std::lround(GetBValue(color) * a));
            dst[x - rect.left] = (static_cast<DWORD>(alpha) << 24) |
                (static_cast<DWORD>(outR) << 16) |
                (static_cast<DWORD>(outG) << 8) |
                static_cast<DWORD>(outB);
        }
    }
    HDC mem = CreateCompatibleDC(hdc);
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap) {
        memcpy(bits, overlay.data(), overlay.size() * sizeof(DWORD));
        HGDIOBJ old = SelectObject(mem, bitmap);
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(hdc, rect.left, rect.top, w, h, mem, 0, 0, w, h, blend);
        SelectObject(mem, old);
        DeleteObject(bitmap);
    }
    DeleteDC(mem);
}

void DrawClropItem(HDC hdc,
                   const clrop::Item& item,
                   const RECT& pageRect,
                   double pageWidthPt,
                   double pageHeightPt,
                   const RenderedPage* rendered,
                   bool selected) {
    const COLORREF color = HexToColor(item.color);
    const RECT bounds = ItemBoundsClient(item, pageRect, pageWidthPt, pageHeightPt);
    if (bounds.left == bounds.right || bounds.top == bounds.bottom) return;

    switch (item.kind) {
    case clrop::Item::Kind::TextColor: {
        for (size_t qi = 0; qi + 7 < item.quads.size(); qi += 8) {
            RECT qr{};
            bool first = true;
            for (size_t k = 0; k < 8; k += 2) {
                const POINT pt = PdfPointToClient(pageRect, pageWidthPt, pageHeightPt,
                                                  item.quads[qi + k], item.quads[qi + k + 1]);
                if (first) {
                    qr.left = qr.right = pt.x;
                    qr.top = qr.bottom = pt.y;
                    first = false;
                } else {
                    qr.left = std::min(qr.left, pt.x);
                    qr.right = std::max(qr.right, pt.x);
                    qr.top = std::min(qr.top, pt.y);
                    qr.bottom = std::max(qr.bottom, pt.y);
                }
            }
            TintRenderedTextRect(hdc, qr, color, pageRect, rendered);
        }
        break;
    }
    case clrop::Item::Kind::MarkerText: {
        BYTE alpha = static_cast<BYTE>(std::clamp(item.alpha > 0.0 ? item.alpha : 0.35, 0.0, 1.0) * 255.0);
        if (!item.quads.empty()) {
            for (size_t qi = 0; qi + 7 < item.quads.size(); qi += 8) {
                RECT qr{};
                bool first = true;
                for (size_t k = 0; k < 8; k += 2) {
                    POINT pt = PdfPointToClient(pageRect, pageWidthPt, pageHeightPt,
                                                item.quads[qi + k], item.quads[qi + k + 1]);
                    if (first) {
                        qr.left = qr.right = pt.x;
                        qr.top = qr.bottom = pt.y;
                        first = false;
                    } else {
                        qr.left = std::min(qr.left, pt.x);
                        qr.right = std::max(qr.right, pt.x);
                        qr.top = std::min(qr.top, pt.y);
                        qr.bottom = std::max(qr.bottom, pt.y);
                    }
                }
                FillRectAlpha(hdc, qr, color, alpha);
            }
        } else {
            FillRectAlpha(hdc, bounds, color, alpha);
        }
        break;
    }
    case clrop::Item::Kind::Line:
    case clrop::Item::Kind::Arrow:
    case clrop::Item::Kind::Wave: {
        if (!item.p1 || !item.p2) break;
        POINT p1 = PdfPointToClient(pageRect, pageWidthPt, pageHeightPt, (*item.p1)[0], (*item.p1)[1]);
        POINT p2 = PdfPointToClient(pageRect, pageWidthPt, pageHeightPt, (*item.p2)[0], (*item.p2)[1]);
        int penWidth = std::max(1, static_cast<int>(std::lround(std::max(1.0, item.width))));
        HPEN pen = CreatePen(PS_SOLID, penWidth, color);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        MoveToEx(hdc, p1.x, p1.y, nullptr);
        LineTo(hdc, p2.x, p2.y);
        if (item.kind == clrop::Item::Kind::Arrow) {
            DrawArrowHead(hdc, p1, p2, std::max(8, penWidth * 5), item.arrowHead == "double");
        }
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        break;
    }
    case clrop::Item::Kind::MarkerFree:
    case clrop::Item::Kind::Freehand: {
        if (item.path.size() < 4) break;
        std::vector<POINT> pts;
        for (size_t i = 0; i + 1 < item.path.size(); i += 2) {
            pts.push_back(PdfPointToClient(pageRect, pageWidthPt, pageHeightPt, item.path[i], item.path[i + 1]));
        }
        int penWidth = std::max(1, static_cast<int>(std::lround(std::max(1.0, item.width))));
        HPEN pen = CreatePen(PS_SOLID, penWidth, color);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        Polyline(hdc, pts.data(), static_cast<int>(pts.size()));
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        break;
    }
    case clrop::Item::Kind::Text:
    case clrop::Item::Kind::Math: {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, color);
        int fontPx = std::max(8, static_cast<int>(std::lround((item.pt > 0 ? item.pt : 12.0) * (pageRect.right - pageRect.left) / pageWidthPt / 1.35)));
        LOGFONTW lf{};
        lf.lfHeight = -fontPx;
        const std::wstring face = item.font.empty() ? L"Segoe UI" : item.font;
        wcsncpy_s(lf.lfFaceName, face.c_str(), LF_FACESIZE - 1);
        HFONT font = CreateFontIndirectW(&lf);
        HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;
        RECT textRect = bounds;
        InflateRect(&textRect, -2, -2);
        std::wstring text = item.content;
        if (text.empty()) text = Utf8ToWideLocal(item.mathKind);
        DrawTextW(hdc, text.c_str(), -1, &textRect, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        if (oldFont) SelectObject(hdc, oldFont);
        if (font) DeleteObject(font);
        break;
    }
    case clrop::Item::Kind::Shape: {
        HPEN pen = CreatePen(PS_SOLID, std::max(1, static_cast<int>(std::lround(item.width > 0 ? item.width : 2.0))), color);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        const bool fill = (item.shapeDrawMode == "fill" || item.shapeDrawMode == "filled");
        const bool ellipseShape = item.shapeKind == "ellipse" || item.shapeKind == "circle";
        const bool rotatedEllipseShape = item.shapeKind == "rotated_ellipse";
        auto buildRotatedEllipse = [&]() {
            constexpr int kSegs = 96;
            constexpr double kDefaultAngle = 0.78539816339744830962; // 45 degrees for legacy rotated_ellipse data
            const double angle = (item.shapeRotation && std::isfinite(*item.shapeRotation)) ? *item.shapeRotation : kDefaultAngle;
            std::vector<POINT> pts;
            const int w = bounds.right - bounds.left;
            const int h = bounds.bottom - bounds.top;
            if (w <= 0 || h <= 0) return pts;
            const double cx = (static_cast<double>(bounds.left) + static_cast<double>(bounds.right)) * 0.5;
            const double cy = (static_cast<double>(bounds.top) + static_cast<double>(bounds.bottom)) * 0.5;
            const double rx = std::max(0.5, static_cast<double>(w) * 0.5);
            const double ry = std::max(0.5, static_cast<double>(h) * 0.5);
            const double c = std::cos(angle);
            const double s = std::sin(angle);
            const double extentX = std::sqrt((rx * c) * (rx * c) + (ry * s) * (ry * s));
            const double extentY = std::sqrt((rx * s) * (rx * s) + (ry * c) * (ry * c));
            const double fit = std::min(rx / std::max(0.5, extentX), ry / std::max(0.5, extentY));
            pts.reserve(kSegs);
            for (int i = 0; i < kSegs; ++i) {
                const double t = (2.0 * 3.14159265358979323846 * i) / kSegs;
                const double x = std::cos(t) * rx * fit;
                const double y = std::sin(t) * ry * fit;
                pts.push_back(POINT{
                    static_cast<LONG>(std::lround(cx + x * c - y * s)),
                    static_cast<LONG>(std::lround(cy + x * s + y * c))
                });
            }
            return pts;
        };
        if (fill && !(ellipseShape || rotatedEllipseShape)) {
            BYTE alpha = static_cast<BYTE>(std::clamp(item.alpha > 0.0 ? item.alpha : 0.30, 0.0, 1.0) * 255.0);
            FillRectAlpha(hdc, bounds, color, alpha);
        } else {
            HBRUSH fillBrush = nullptr;
            HGDIOBJ oldBrush = nullptr;
            if (fill) {
                fillBrush = CreateSolidBrush(color);
                oldBrush = SelectObject(hdc, fillBrush);
            } else {
                oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
            }
            if (ellipseShape) {
                Ellipse(hdc, bounds.left, bounds.top, bounds.right, bounds.bottom);
            } else if (rotatedEllipseShape) {
                auto pts = buildRotatedEllipse();
                if (!pts.empty()) {
                    Polygon(hdc, pts.data(), static_cast<int>(pts.size()));
                }
            } else {
                Rectangle(hdc, bounds.left, bounds.top, bounds.right, bounds.bottom);
            }
            SelectObject(hdc, oldBrush);
            if (fillBrush) DeleteObject(fillBrush);
        }
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        break;
    }
    case clrop::Item::Kind::LinkMarker: {
        const int radius = 8;
        POINT center{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
        HPEN pen = CreatePen(PS_SOLID, 2, color);
        HBRUSH brush = CreateSolidBrush(color);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, brush);
        Ellipse(hdc, center.x - radius, center.y - radius, center.x + radius, center.y + radius);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
        break;
    }
    default:
        break;
    }

    if (selected) {
        HPEN pen = CreatePen(PS_DOT, 1, g_state.theme.accent);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, bounds.left, bounds.top, bounds.right, bounds.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }
}

void DrawClropAnnotations(HDC hdc,
                          int pageIndex,
                          const RECT& pageRect,
                          double pageWidthPt,
                          double pageHeightPt,
                          const RenderedPage* rendered) {
    if (!g_state.showAnnotations || !g_state.clrop) return;
    for (const auto& page : g_state.clrop->document.pages) {
        if (page.page != pageIndex) continue;
        for (size_t i = 0; i < page.items.size(); ++i) {
            const bool selected = (g_state.selectedPage == page.page &&
                                   g_state.selectedItem == static_cast<int>(i));
            DrawClropItem(hdc, page.items[i], pageRect, pageWidthPt, pageHeightPt, rendered, selected);
        }
    }
}

bool PageLayoutAtPoint(HWND hwnd, POINT pt, PageLayout& out);

void DrawTextSelection(HDC hdc, const PageLayout& layout) {
    if (g_state.hasRectSelection && g_state.selectionPage == layout.pageIndex) {
        POINT p1 = PdfPointToClient(layout.rect,
                                    layout.pageWidthPt,
                                    layout.pageHeightPt,
                                    g_state.selectionLeftPt,
                                    g_state.selectionTopPt);
        POINT p2 = PdfPointToClient(layout.rect,
                                    layout.pageWidthPt,
                                    layout.pageHeightPt,
                                    g_state.selectionRightPt,
                                    g_state.selectionBottomPt);
        RECT r = NormalizeClientRect(p1, p2);
        if (RectHasArea(r)) {
            FillRectAlpha(hdc, r, g_state.theme.selectionBg, 70);
            HPEN pen = CreatePen(PS_SOLID, 1, g_state.theme.accent);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(hdc, r.left, r.top, r.right, r.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
        }
    }

    if (g_state.selectingRect) {
        RECT r = NormalizeClientRect(g_state.selectionAnchor, g_state.selectionCurrent);
        if (RectHasArea(r)) {
            FillRectAlpha(hdc, r, g_state.theme.selectionBg, 55);
            HPEN pen = CreatePen(PS_DOT, 1, g_state.theme.accent);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(hdc, r.left, r.top, r.right, r.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
        }
    }
}

bool BeginTextSelection(HWND hwnd, POINT pt) {
    if (g_state.tempTool != TempDrawTool::None) return false;
    PageLayout layout{};
    if (!PageLayoutAtPoint(hwnd, pt, layout)) return false;
    g_state.selectingRect = true;
    g_state.hasRectSelection = false;
    g_state.selectedText.clear();
    g_state.selectedPage = -1;
    g_state.selectedItem = -1;
    g_state.selectionPage = layout.pageIndex;
    g_state.selectionAnchor = pt;
    g_state.selectionCurrent = pt;
    SetCapture(hwnd);
    SetTimer(hwnd,
             kTextSelectionAutoScrollTimerId,
             kTextSelectionAutoScrollIntervalMs,
             nullptr);
    return true;
}

POINT ClampSelectionPointToPdfArea(HWND hwnd, POINT pt) {
    RECT avail = ViewerAvailableRect(hwnd);
    if (avail.right <= avail.left || avail.bottom <= avail.top) return pt;
    pt.y = std::clamp(pt.y, avail.top, avail.bottom);
    return pt;
}

bool UpdateTextSelection(HWND hwnd, POINT pt) {
    if (!g_state.selectingRect) return false;
    g_state.selectionCurrent = ClampSelectionPointToPdfArea(hwnd, pt);
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

void UpdateTextSelectionAutoScroll(HWND hwnd) {
    if (!g_state.selectingRect) return;
    RECT avail = ViewerAvailableRect(hwnd);
    if (avail.right <= avail.left || avail.bottom <= avail.top) return;

    if (g_state.lastMouse.y <= avail.bottom) return;

    // Reaching the PDF pane's bottom edge reaches the maximum acceleration;
    // moving farther outside the pane must not increase it any further.
    ScrollContinuousBy(hwnd, kTextSelectionAutoScrollMaxSpeedPx);
    UpdateTextSelection(hwnd, g_state.lastMouse);
}

bool FinishTextSelection(HWND hwnd) {
    if (!g_state.selectingRect) return false;
    KillTimer(hwnd, kTextSelectionAutoScrollTimerId);
    g_state.selectingRect = false;
    if (GetCapture() == hwnd) ReleaseCapture();

    RECT clientRect = NormalizeClientRect(g_state.selectionAnchor, g_state.selectionCurrent);
    if (!RectHasArea(clientRect, 4)) {
        ClearTextSelection();
        return false;
    }

    PageLayout layout{};
    if (!PageLayoutForIndex(hwnd, g_state.selectionPage, layout)) {
        ClearTextSelection();
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    RECT clipped{};
    if (!IntersectRect(&clipped, &clientRect, &layout.rect) || !RectHasArea(clipped, 4)) {
        ClearTextSelection();
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }

    double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
    POINT a{clipped.left, clipped.top};
    POINT b{clipped.right, clipped.bottom};
    if (!ClientPointToPdfPoint(layout, a, x1, y1) ||
        !ClientPointToPdfPoint(layout, b, x2, y2)) {
        ClearTextSelection();
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    g_state.selectionLeftPt = std::min(x1, x2);
    g_state.selectionRightPt = std::max(x1, x2);
    g_state.selectionTopPt = std::max(y1, y2);
    g_state.selectionBottomPt = std::min(y1, y2);
    g_state.hasRectSelection = true;
    g_state.selectedPage = -1;
    g_state.selectedItem = -1;
    g_state.selectedText = BuildSelectedText(g_state.selectionPage,
                                             g_state.selectionLeftPt,
                                             g_state.selectionTopPt,
                                             g_state.selectionRightPt,
                                             g_state.selectionBottomPt);
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

bool HitTestClropAnnotations(int pageIndex,
                             POINT clientPt,
                             const RECT& pageRect,
                             double pageWidthPt,
                             double pageHeightPt) {
    g_state.selectedPage = -1;
    g_state.selectedItem = -1;
    if (!g_state.showAnnotations || !g_state.clrop) return false;
    for (const auto& page : g_state.clrop->document.pages) {
        if (page.page != pageIndex) continue;
        for (int i = static_cast<int>(page.items.size()) - 1; i >= 0; --i) {
            RECT bounds = ItemBoundsClient(page.items[static_cast<size_t>(i)], pageRect, pageWidthPt, pageHeightPt);
            if (PtInRect(&bounds, clientPt)) {
                g_state.selectedPage = page.page;
                g_state.selectedItem = i;
                return true;
            }
        }
    }
    return false;
}

COLORREF TempStrokeColor(TempDrawTool tool) {
    return tool == TempDrawTool::Marker ? RGB(255, 220, 0) : RGB(220, 40, 40);
}

BYTE TempStrokeAlpha(TempDrawTool tool) {
    return tool == TempDrawTool::Marker ? static_cast<BYTE>(96) : static_cast<BYTE>(255);
}

double TempStrokeWidthPt(TempDrawTool tool) {
    return tool == TempDrawTool::Marker ? 14.0 : 2.5;
}

void DrawTempStroke(HDC hdc, const TempStroke& stroke, const PageLayout& layout) {
    if (stroke.pageIndex != layout.pageIndex || stroke.path.size() < 4) return;
    std::vector<POINT> pts;
    pts.reserve(stroke.path.size() / 2);
    for (size_t i = 0; i + 1 < stroke.path.size(); i += 2) {
        pts.push_back(PdfPointToClient(layout.rect,
                                       layout.pageWidthPt,
                                       layout.pageHeightPt,
                                       stroke.path[i],
                                       stroke.path[i + 1]));
    }
    if (pts.size() < 2) return;
    const double pageScale = static_cast<double>(std::max<LONG>(1, layout.rect.right - layout.rect.left)) /
                             std::max(1.0, layout.pageWidthPt);
    const int penWidth = std::max(1, static_cast<int>(std::lround(TempStrokeWidthPt(stroke.tool) * pageScale)));
    DrawPolylineAlpha(hdc, pts, penWidth, TempStrokeColor(stroke.tool), TempStrokeAlpha(stroke.tool));
}

void DrawTemporaryStrokes(HDC hdc, const PageLayout& layout) {
    for (const TempStroke& stroke : g_state.tempStrokes) {
        DrawTempStroke(hdc, stroke, layout);
    }
    if (g_state.drawingTempStroke) {
        DrawTempStroke(hdc, g_state.activeTempStroke, layout);
    }
}

void PaintPlaceholderPage(HDC hdc, const RECT& client) {
    RECT page = client;
    InflateRect(&page, -48, -58);
    if (page.right <= page.left || page.bottom <= page.top) return;

    HBRUSH pageBg = CreateSolidBrush(g_state.theme.pdfPageBg);
    FillRect(hdc, &page, pageBg);
    DeleteObject(pageBg);

    HPEN border = CreatePen(PS_SOLID, 1, g_state.theme.splitterLine);
    HGDIOBJ oldPen = SelectObject(hdc, border);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, page.left, page.top, page.right, page.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(border);
}

void DrawPdfInfoOverlay(HDC hdc, const RECT& client) {
    if (!g_state.showPdfInfo || !g_state.pdf) return;

    RECT panel{client.left + 24, client.top + 52, client.left + 420, client.top + 210};
    HBRUSH bg = CreateSolidBrush(g_state.theme.panelBg);
    FillRect(hdc, &panel, bg);
    DeleteObject(bg);
    HPEN pen = CreatePen(PS_SOLID, 1, g_state.theme.splitterLine);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, panel.left, panel.top, panel.right, panel.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    std::wstring text = L"PDF information\n";
    text += L"File: " + FileNameFromPath(g_state.pdf->path) + L"\n";
    text += L"Pages: " + std::to_wstring(g_state.pdf->pageCount) + L"\n";
    text += L"Current page: " + std::to_wstring(g_state.currentPage + 1) + L"\n";
    if (g_state.hasPdfWatchedMetadata) {
        text += L"Size: " + std::to_wstring(g_state.pdfWatchedMetadata.size) + L" bytes\n";
    }
    if (g_state.clrop) {
        text += L"CLROP: " + FileNameFromPath(g_state.clrop->path) + L"\n";
    } else {
        text += L"CLROP: none\n";
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_state.theme.panelText);
    RECT textRc = panel;
    InflateRect(&textRc, -12, -10);
    DrawTextW(hdc, text.c_str(), -1, &textRc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
}

void DrawMagnifierLens(HDC hdc, const RECT& client) {
    if (!g_state.showMagnifier) return;
    POINT center = g_state.lastMouse;
    if (center.x < client.left || center.x > client.right ||
        center.y < client.top || center.y > client.bottom) {
        return;
    }

    const int lensW = kMagnifierRadiusPx * 2;
    const int lensH = kMagnifierRadiusPx * 2;
    const int srcW = std::max(1, static_cast<int>(std::lround(lensW / kMagnifierZoom)));
    const int srcH = std::max(1, static_cast<int>(std::lround(lensH / kMagnifierZoom)));
    const int srcX = center.x - srcW / 2;
    const int srcY = center.y - srcH / 2;
    RECT lens{center.x - kMagnifierRadiusPx,
              center.y - kMagnifierRadiusPx,
              center.x + kMagnifierRadiusPx,
              center.y + kMagnifierRadiusPx};

    HDC srcDC = CreateCompatibleDC(hdc);
    if (!srcDC) return;
    HBITMAP srcBmp = CreateCompatibleBitmap(hdc, srcW, srcH);
    if (!srcBmp) {
        DeleteDC(srcDC);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(srcDC, srcBmp);
    BitBlt(srcDC, 0, 0, srcW, srcH, hdc, srcX, srcY, SRCCOPY);

    const int saved = SaveDC(hdc);
    HRGN clip = CreateEllipticRgn(lens.left, lens.top, lens.right, lens.bottom);
    if (clip) SelectClipRgn(hdc, clip);
    SetStretchBltMode(hdc, HALFTONE);
    StretchBlt(hdc, lens.left, lens.top, lensW, lensH, srcDC, 0, 0, srcW, srcH, SRCCOPY);
    RestoreDC(hdc, saved);

    HPEN pen = CreatePen(PS_SOLID, 2, g_state.theme.accent);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Ellipse(hdc, lens.left, lens.top, lens.right, lens.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    if (clip) DeleteObject(clip);

    SelectObject(srcDC, oldBmp);
    DeleteObject(srcBmp);
    DeleteDC(srcDC);
}

void PaintViewer(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC paintHdc = BeginPaint(hwnd, &ps);
    if (!paintHdc) return;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int clientW = static_cast<int>(std::max<LONG>(0, rc.right - rc.left));
    const int clientH = static_cast<int>(std::max<LONG>(0, rc.bottom - rc.top));

    HDC memDc = nullptr;
    HBITMAP backBuffer = nullptr;
    HGDIOBJ oldBackBuffer = nullptr;
    HDC hdc = paintHdc;
    if (clientW > 0 && clientH > 0) {
        memDc = CreateCompatibleDC(paintHdc);
        if (memDc) {
            backBuffer = CreateCompatibleBitmap(paintHdc, clientW, clientH);
            if (backBuffer) {
                oldBackBuffer = SelectObject(memDc, backBuffer);
                hdc = memDc;
            }
        }
    }

    HBRUSH bg = CreateSolidBrush(g_state.theme.pdfBg);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    if (g_state.pdf && g_state.pdf->document) {
        ClampContinuousScroll(hwnd);
        SyncCurrentPageFromScroll(hwnd);
        std::vector<PageLayout> layouts = BuildVisiblePageLayouts(hwnd);
        g_state.renderedPages.reserve(g_state.renderedPages.size() + layouts.size());
        for (const auto& layout : layouts) {
            RenderedPage* rendered = EnsureRenderedPage(layout.pageIndex, layout.width, layout.height);
            if (!rendered) continue;
            DrawRenderedPage(hdc, *rendered, layout.rect);
            DrawClropAnnotations(hdc,
                                 layout.pageIndex,
                                 layout.rect,
                                 layout.pageWidthPt,
                                 layout.pageHeightPt,
                                 rendered);
            DrawTemporaryStrokes(hdc, layout);
            DrawTextSelection(hdc, layout);

            HPEN border = CreatePen(PS_SOLID, 1, g_state.theme.splitterLine);
            HGDIOBJ oldPen = SelectObject(hdc, border);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(hdc, layout.rect.left, layout.rect.top, layout.rect.right, layout.rect.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(border);
        }
        DrawMagnifierLens(hdc, rc);
        PruneRenderedPageCache(layouts);
    } else {
        PaintPlaceholderPage(hdc, rc);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_state.theme.panelText);
    RECT textRc = rc;
    textRc.left += 24;
    textRc.top += 18;
    textRc.right -= 24;
    DrawTextW(hdc, g_state.status.c_str(), -1, &textRc, DT_SINGLELINE | DT_LEFT | DT_TOP | DT_END_ELLIPSIS);
    DrawPdfInfoOverlay(hdc, rc);

    if (hdc == memDc && backBuffer) {
        BitBlt(paintHdc, 0, 0, clientW, clientH, memDc, 0, 0, SRCCOPY);
    }
    if (oldBackBuffer) SelectObject(memDc, oldBackBuffer);
    if (backBuffer) DeleteObject(backBuffer);
    if (memDc) DeleteDC(memDc);

    EndPaint(hwnd, &ps);
}

bool PageLayoutAtPoint(HWND hwnd, POINT pt, PageLayout& out) {
    std::vector<PageLayout> layouts = BuildVisiblePageLayouts(hwnd);
    for (const auto& layout : layouts) {
        if (PtInRect(&layout.rect, pt)) {
            out = layout;
            return true;
        }
    }
    return false;
}

void SetWindowTitle(HWND hwnd) {
    std::wstring title = L"PDF Read-Only Viewer";
    if (g_state.pdf && !g_state.pdf->path.empty()) {
        title += L" - " + FileNameFromPath(g_state.pdf->path);
    }
    SetWindowTextW(hwnd, title.c_str());
}

void OpenPdfInViewer(HWND hwnd, const std::wstring& path, const std::wstring& clropPath = {}) {
    std::unique_ptr<LoadedPdf> next;
    std::wstring status;
    if (!LoadPdfReadOnly(path, next, status)) {
        g_state.status = status;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    FileMetadata metadata{};
    bool hasMetadata = ReadFileMetadataShared(path, metadata);
    g_state.pdf = std::move(next);
    g_state.currentPage = 0;
    g_state.pageRangeFirst = 0;
    g_state.pageRangeLast = g_state.pdf->pageCount > 500
        ? std::min(19, g_state.pdf->pageCount - 1)
        : -1;
    g_state.panX = 0.0;
    g_state.panY = 0.0;
    g_state.scrollY = 0.0;
    g_state.panning = false;
    g_state.drawingTempStroke = false;
    g_state.activeTempStroke = TempStroke{};
    g_state.tempStrokes.clear();
    ClearRenderCaches();
    g_state.selectedPage = -1;
    g_state.selectedItem = -1;
    g_state.pdfWatchedMetadata = metadata;
    g_state.hasPdfWatchedMetadata = hasMetadata;
    g_state.pdfReloadPending = false;
    UpdateStatusFromPdf();
    LoadAdjacentClropForPdf(hwnd, clropPath);
    SetWindowTitle(hwnd);
    SetTimer(hwnd, kFileWatchTimerId, kFileWatchIntervalMs, nullptr);
    InvalidateRect(hwnd, nullptr, FALSE);
    // Start at the beginning for unusually large documents, then let the user
    // refine it before scrolling makes PDFium render pages beyond that range.
    if (g_state.pdf->pageCount > 500) {
        ChoosePageRange(hwnd);
    }
}

void OpenInputPath(HWND hwnd, const std::wstring& path) {
    const std::wstring abs = ToAbsolutePath(path);
    if (HasExtensionCaseInsensitive(abs, L".clrop")) {
        std::unique_ptr<LoadedClrop> loaded;
        std::wstring err;
        if (!LoadClropReadOnly(abs, loaded, err)) {
            g_state.status = L"Could not read CLROP";
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        const std::wstring pdfPath = ResolvePdfPathForClrop(abs, loaded->document);
        if (pdfPath.empty()) {
            g_state.clrop = std::move(loaded);
            g_state.pdf.reset();
            g_state.panX = 0.0;
            g_state.panY = 0.0;
            g_state.scrollY = 0.0;
            g_state.panning = false;
            g_state.drawingTempStroke = false;
            g_state.activeTempStroke = TempStroke{};
            g_state.tempStrokes.clear();
            ClearRenderCaches();
            g_state.status = L"Could not resolve PDF for CLROP: " + FileNameFromPath(abs);
            SetWindowTitle(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        OpenPdfInViewer(hwnd, pdfPath, abs);
        return;
    }
    OpenPdfInViewer(hwnd, abs);
}

std::optional<std::wstring> PromptOpenPath(HWND owner) {
    if (g_useNativeFileDialogs) {
        return PromptNativeOpenPath(owner);
    }
    return PromptLocalOpenPath(owner);
}

void OpenFileDialog(HWND hwnd) {
    auto path = PromptOpenPath(hwnd);
    if (path && !path->empty()) {
        OpenInputPath(hwnd, *path);
    }
}

void OpenDroppedFile(HWND hwnd, HDROP drop) {
    if (!drop) return;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (count == 0) {
        DragFinish(drop);
        return;
    }

    const UINT len = DragQueryFileW(drop, 0, nullptr, 0);
    if (len == 0) {
        DragFinish(drop);
        return;
    }
    std::wstring path(len + 1, L'\0');
    const UINT copied = DragQueryFileW(drop, 0, path.data(), static_cast<UINT>(path.size()));
    if (copied > 0) {
        path.resize(copied);
        OpenInputPath(hwnd, path);
    }
    DragFinish(drop);
}

std::wstring CurrentExePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0) return {};
    if (len >= buffer.size()) {
        buffer.resize(32768, L'\0');
        len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0 || len >= buffer.size()) return {};
    }
    buffer.resize(len);
    return buffer;
}

std::filesystem::path FindMainSoftwareExecutable() {
    const std::filesystem::path exePath(CurrentExePath());
    const std::filesystem::path exeDir = exePath.parent_path();
    if (exeDir.empty()) return {};

    const std::filesystem::path candidates[] = {
        exeDir / L"pdf_note_workspace.exe",
    };
    for (const auto& candidate : candidates) {
        if (FileExistsRegular(candidate.wstring())) {
            return candidate;
        }
    }
    return {};
}

void LaunchNewViewerWindow() {
    const std::wstring exe = CurrentExePath();
    if (exe.empty()) return;
    std::wstring params;
    auto appendParam = [&](const std::wstring& value) {
        if (!params.empty()) params += L" ";
        params += value;
    };
    if (g_state.pdf) {
        appendParam(L"--pdf " + QuoteArg(g_state.pdf->path));
        appendParam(L"--page " + std::to_wstring(g_state.currentPage + 1));
        appendParam(L"--zoom " + FormatZoomArg(g_state.zoomFactor));
        appendParam(L"--pan-x " + FormatPixelArg(g_state.panX));
        appendParam(L"--pan-y " + FormatPixelArg(g_state.panY));
    }
    appendParam(std::wstring(L"--annotations ") + (g_state.showAnnotations ? L"on" : L"off"));
    appendParam(std::wstring(L"--magnifier ") + (g_state.showMagnifier ? L"on" : L"off"));
    if (g_state.clrop && !g_state.clrop->path.empty()) {
        appendParam(L"--clrop " + QuoteArg(g_state.clrop->path));
    }
    if (g_state.theme.name == L"inline") {
        appendParam(L"--theme-inline " + QuoteArg(BuildInlineThemeSpec(g_state.theme)));
    } else {
        appendParam(L"--theme " + QuoteArg(g_state.theme.name));
    }
    ShellExecuteW(nullptr, L"open", exe.c_str(), params.empty() ? nullptr : params.c_str(), nullptr, SW_SHOWNORMAL);
}

void LaunchMainSoftware(HWND hwnd) {
    if (!g_state.pdf) return;
    const std::filesystem::path mainExe = FindMainSoftwareExecutable();
    if (mainExe.empty()) {
        g_state.status = L"Could not find main software: pdf_note_workspace.exe";
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    std::wstring params = L"--pdf ";
    params += QuoteArg(g_state.pdf->path);
    HINSTANCE result = ShellExecuteW(nullptr, L"open", mainExe.wstring().c_str(), params.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        g_state.status = L"Could not launch main software";
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

struct ViewerCloseContext {
    int requested = 0;
};

BOOL CALLBACK CloseViewerWindowEnumProc(HWND hwnd, LPARAM lParam) {
    auto* context = reinterpret_cast<ViewerCloseContext*>(lParam);
    if (!context || !hwnd) return TRUE;
    wchar_t className[128]{};
    if (GetClassNameW(hwnd, className, static_cast<int>(sizeof(className) / sizeof(className[0]))) <= 0) {
        return TRUE;
    }
    if (wcscmp(className, kWindowClassName) != 0) return TRUE;
    if (PostMessageW(hwnd, WM_CLOSE, 0, 0)) {
        ++context->requested;
    }
    return TRUE;
}

void CloseAllViewerWindows() {
    ViewerCloseContext context{};
    EnumWindows(CloseViewerWindowEnumProc, reinterpret_cast<LPARAM>(&context));
}

HMENU BuildViewerMenu() {
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, kCmdNewWindow, L"読み取り新ウィンドウ");
    AppendMenuW(file, MF_STRING, kCmdOpen, L"読み込み");
    AppendMenuW(file, MF_STRING, kCmdLaunchMain, L"メインソフト起動");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    UINT exportFlags = MF_STRING;
    if (!g_state.pdf || !g_state.clrop) exportFlags |= MF_GRAYED;
    AppendMenuW(file, exportFlags, kCmdExportAnnotatedPdf, L"注釈合成PDFを書き出し...");
    AppendMenuW(file, MF_STRING, kCmdPageRange, L"表示ページ範囲...");
    AppendMenuW(file, MF_STRING, kCmdPdfInfo, L"PDF情報");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, kCmdCloseAllViewers, L"すべての閲覧専用ビューアを閉じる");
    AppendMenuW(file, MF_STRING, kCmdExit, L"終了");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"ファイル");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING | (g_state.showAnnotations ? MF_CHECKED : 0), kCmdToggleAnnots, L"注釈表示");
    AppendMenuW(view, MF_STRING | (g_state.showMagnifier ? MF_CHECKED : 0), kCmdToggleMagnifier, L"拡大鏡");
    AppendMenuW(view, MF_STRING | (g_state.showGrayscale ? MF_CHECKED : 0), kCmdToggleGrayscale, L"白黒表示");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view,
                MF_STRING | (g_state.tempTool == TempDrawTool::Marker ? MF_CHECKED : 0),
                kCmdTempMarker,
                L"一時マーカー");
    AppendMenuW(view,
                MF_STRING | (g_state.tempTool == TempDrawTool::Pen ? MF_CHECKED : 0),
                kCmdTempPen,
                L"一時線");
    AppendMenuW(view, MF_STRING, kCmdClearTempDrawing, L"一時描画を消去");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, kCmdZoomIn, L"拡大\tCtrl++");
    AppendMenuW(view, MF_STRING, kCmdZoomOut, L"縮小\tCtrl+-");
    AppendMenuW(view, MF_STRING, kCmdZoomReset, L"倍率リセット\tCtrl+0");
    HMENU themeMenu = CreatePopupMenu();
    for (UINT i = 0; i < g_state.themeCatalog.size(); ++i) {
        const auto& theme = g_state.themeCatalog[static_cast<size_t>(i)];
        UINT flags = MF_STRING;
        if (theme.name == g_state.theme.name) flags |= MF_CHECKED;
        AppendMenuW(themeMenu, flags, kCmdThemeBase + i, theme.name.c_str());
    }
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(themeMenu), L"カラーテーマ");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"表示");
    return menu;
}

void RefreshViewerMenu(HWND hwnd) {
    HMENU oldMenu = GetMenu(hwnd);
    HMENU newMenu = BuildViewerMenu();
    SetMenu(hwnd, newMenu);
    if (oldMenu) DestroyMenu(oldMenu);
    DrawMenuBar(hwnd);
}

bool AppendTempStrokePoint(const PageLayout& layout, POINT pt) {
    if (!g_state.drawingTempStroke) return false;
    double xPt = 0.0;
    double yPt = 0.0;
    if (!ClientPointToPdfPoint(layout, pt, xPt, yPt)) return false;
    auto& path = g_state.activeTempStroke.path;
    if (path.size() >= 2) {
        const double dx = xPt - path[path.size() - 2];
        const double dy = yPt - path[path.size() - 1];
        if ((dx * dx + dy * dy) < 0.04) return false;
    }
    path.push_back(xPt);
    path.push_back(yPt);
    return true;
}

bool BeginTempStroke(HWND hwnd, POINT pt) {
    if (g_state.tempTool == TempDrawTool::None) return false;
    PageLayout layout{};
    if (!PageLayoutAtPoint(hwnd, pt, layout)) return false;
    g_state.drawingTempStroke = true;
    g_state.activeTempStroke = TempStroke{};
    g_state.activeTempStroke.tool = g_state.tempTool;
    g_state.activeTempStroke.pageIndex = layout.pageIndex;
    AppendTempStrokePoint(layout, pt);
    SetCapture(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

bool UpdateTempStroke(HWND hwnd, POINT pt) {
    if (!g_state.drawingTempStroke) return false;
    PageLayout layout{};
    if (!PageLayoutForIndex(hwnd, g_state.activeTempStroke.pageIndex, layout)) return true;
    if (AppendTempStrokePoint(layout, pt)) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    return true;
}

bool EndTempStroke(HWND hwnd) {
    if (!g_state.drawingTempStroke) return false;
    if (g_state.activeTempStroke.path.size() >= 4) {
        g_state.tempStrokes.push_back(std::move(g_state.activeTempStroke));
    }
    g_state.activeTempStroke = TempStroke{};
    g_state.drawingTempStroke = false;
    if (GetCapture() == hwnd) ReleaseCapture();
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

void ClearTemporaryDrawing(HWND hwnd) {
    g_state.tempStrokes.clear();
    g_state.activeTempStroke = TempStroke{};
    g_state.drawingTempStroke = false;
    if (GetCapture() == hwnd) ReleaseCapture();
    InvalidateRect(hwnd, nullptr, FALSE);
}

void BeginPagePan(HWND hwnd, POINT pt) {
    if (!g_state.pdf || !g_state.pdf->document) return;
    g_state.panning = true;
    g_state.panStart = pt;
    g_state.panStartX = g_state.panX;
    g_state.panStartY = g_state.scrollY;
    SetCapture(hwnd);
}

void UpdatePagePan(HWND hwnd, POINT pt) {
    if (!g_state.panning) return;
    g_state.panX = g_state.panStartX + static_cast<double>(pt.x - g_state.panStart.x);
    g_state.scrollY = g_state.panStartY - static_cast<double>(pt.y - g_state.panStart.y);
    ClampContinuousScroll(hwnd);
    SyncCurrentPageFromScroll(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void EndPagePan(HWND hwnd) {
    if (!g_state.panning) return;
    g_state.panning = false;
    if (GetCapture() == hwnd) ReleaseCapture();
}

LRESULT CALLBACK ViewerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        ClearRenderCaches();
        ClampContinuousScroll(hwnd);
        SyncCurrentPageFromScroll(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        if (id == kCmdOpen) {
            OpenFileDialog(hwnd);
            return 0;
        }
        if (id == kCmdNewWindow) {
            LaunchNewViewerWindow();
            return 0;
        }
        if (id == kCmdLaunchMain) {
            LaunchMainSoftware(hwnd);
            return 0;
        }
        if (id == kCmdPdfInfo) {
            g_state.showPdfInfo = !g_state.showPdfInfo;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (id == kCmdPageRange) {
            ChoosePageRange(hwnd);
            return 0;
        }
        if (id == kCmdExportAnnotatedPdf) {
            ExportAnnotatedPdf(hwnd);
            return 0;
        }
        if (id == kCmdToggleAnnots) {
            g_state.showAnnotations = !g_state.showAnnotations;
            RefreshViewerMenu(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (id == kCmdToggleMagnifier) {
            g_state.showMagnifier = !g_state.showMagnifier;
            RefreshViewerMenu(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (id == kCmdToggleGrayscale) {
            g_state.showGrayscale = !g_state.showGrayscale;
            SaveReadonlyViewerSettings();
            ClearRenderCaches();
            RefreshViewerMenu(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (id == kCmdTempMarker) {
            EndTempStroke(hwnd);
            g_state.tempTool = (g_state.tempTool == TempDrawTool::Marker) ? TempDrawTool::None : TempDrawTool::Marker;
            RefreshViewerMenu(hwnd);
            return 0;
        }
        if (id == kCmdTempPen) {
            EndTempStroke(hwnd);
            g_state.tempTool = (g_state.tempTool == TempDrawTool::Pen) ? TempDrawTool::None : TempDrawTool::Pen;
            RefreshViewerMenu(hwnd);
            return 0;
        }
        if (id == kCmdClearTempDrawing) {
            ClearTemporaryDrawing(hwnd);
            return 0;
        }
        if (id == kCmdZoomIn) {
            AdjustZoom(hwnd, 1.15);
            return 0;
        }
        if (id == kCmdZoomOut) {
            AdjustZoom(hwnd, 1.0 / 1.15);
            return 0;
        }
        if (id == kCmdZoomReset) {
            ResetZoom(hwnd);
            return 0;
        }
        if (id == kCmdCloseAllViewers) {
            CloseAllViewerWindows();
            return 0;
        }
        if (id == kCmdExit) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (id >= kCmdThemeBase && id < kCmdThemeBase + g_state.themeCatalog.size()) {
            g_state.theme = g_state.themeCatalog[static_cast<size_t>(id - kCmdThemeBase)];
            ClearRenderCaches();
            RefreshViewerMenu(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_KEYDOWN:
    {
        const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool altDown = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (ctrlDown && !altDown && wParam == 'C') {
            CopyCurrentSelection(hwnd);
            return 0;
        }
        WPARAM navKey = wParam;
        if (!ctrlDown && !altDown) {
            switch (wParam) {
            case 'H':
                navKey = VK_LEFT;
                break;
            case 'J':
                navKey = VK_DOWN;
                break;
            case 'K':
                navKey = VK_UP;
                break;
            case 'L':
                navKey = VK_RIGHT;
                break;
            default:
                break;
            }
        }
        switch (navKey) {
        case VK_NEXT:
            {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                ScrollContinuousBy(hwnd, std::max<double>(kKeyScrollStepPx, (rc.bottom - rc.top) * 0.85));
            }
            return 0;
        case VK_DOWN:
            ScrollContinuousBy(hwnd, kKeyScrollStepPx);
            return 0;
        case VK_PRIOR:
            {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                ScrollContinuousBy(hwnd, -std::max<double>(kKeyScrollStepPx, (rc.bottom - rc.top) * 0.85));
            }
            return 0;
        case VK_UP:
            ScrollContinuousBy(hwnd, -kKeyScrollStepPx);
            return 0;
        case VK_RIGHT:
            SetCurrentPage(hwnd, g_state.currentPage + 1);
            return 0;
        case VK_LEFT:
            SetCurrentPage(hwnd, g_state.currentPage - 1);
            return 0;
        case VK_HOME:
            ScrollContinuousTo(hwnd, 0.0);
            return 0;
        case VK_END:
            if (g_state.pdf) ScrollContinuousTo(hwnd, MaxContinuousScrollY(hwnd));
            return 0;
        case VK_ADD:
        case VK_OEM_PLUS:
            AdjustZoom(hwnd, 1.15);
            return 0;
        case VK_SUBTRACT:
        case VK_OEM_MINUS:
            AdjustZoom(hwnd, 1.0 / 1.15);
            return 0;
        case L'0':
            if (ctrlDown) {
                ResetZoom(hwnd);
                return 0;
            }
            break;
        default:
            break;
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        bool ctrlDown = (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0;
        if (ctrlDown) {
            double steps = static_cast<double>(delta) / WHEEL_DELTA;
            double factor = std::pow(1.15, steps);
            AdjustZoom(hwnd, factor);
        } else {
            ScrollContinuousBy(hwnd,
                               (static_cast<double>(-delta) / WHEEL_DELTA) *
                                   kWheelScrollStepPx);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        g_state.lastMouse = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (g_state.selectingRect) {
            UpdateTextSelection(hwnd, g_state.lastMouse);
            return 0;
        }
        if (g_state.drawingTempStroke) {
            UpdateTempStroke(hwnd, g_state.lastMouse);
            return 0;
        }
        if (g_state.panning) {
            UpdatePagePan(hwnd, g_state.lastMouse);
            return 0;
        }
        if (g_state.showMagnifier) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        g_state.lastMouse = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (BeginTempStroke(hwnd, g_state.lastMouse)) {
            return 0;
        }
        if (BeginTextSelection(hwnd, g_state.lastMouse)) {
            return 0;
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (EndTempStroke(hwnd)) {
            return 0;
        }
        g_state.lastMouse = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (FinishTextSelection(hwnd)) {
            return 0;
        }
        {
            PageLayout layout{};
            if (PageLayoutAtPoint(hwnd, g_state.lastMouse, layout)) {
                g_state.currentPage = layout.pageIndex;
                UpdateStatusFromPdf();
                ClearTextSelection();
                HitTestClropAnnotations(layout.pageIndex,
                                        g_state.lastMouse,
                                        layout.rect,
                                        layout.pageWidthPt,
                                        layout.pageHeightPt);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        break;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        g_state.lastMouse = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        BeginPagePan(hwnd, g_state.lastMouse);
        return 0;
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        EndPagePan(hwnd);
        return 0;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) != hwnd) {
            KillTimer(hwnd, kTextSelectionAutoScrollTimerId);
            g_state.selectingRect = false;
            g_state.panning = false;
            g_state.drawingTempStroke = false;
            g_state.activeTempStroke = TempStroke{};
        }
        return 0;
    case WM_TIMER:
        if (wParam == kFileWatchTimerId) {
            CheckWatchedFiles(hwnd);
            return 0;
        }
        if (wParam == kTextSelectionAutoScrollTimerId) {
            UpdateTextSelectionAutoScroll(hwnd);
            return 0;
        }
        break;
    case WM_DROPFILES:
        OpenDroppedFile(hwnd, reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_PAINT:
        PaintViewer(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        KillTimer(hwnd, kFileWatchTimerId);
        KillTimer(hwnd, kTextSelectionAutoScrollTimerId);
        DragAcceptFiles(hwnd, FALSE);
        ClearRenderCaches();
        g_state.pdf.reset();
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterViewerClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ViewerWndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadReadonlyViewerIcon(instance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hIconSm = LoadReadonlyViewerIcon(instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClassName;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace


} // namespace pdf_panel

bool RegisterPdfPreviewPanelClass(HINSTANCE hInstance) {
    return pdf_panel::RegisterViewerClass(hInstance);
}

HWND CreatePdfPreviewPanel(HWND hParent, HINSTANCE hInstance, int id) {
    if (!hParent || !hInstance || !RegisterPdfPreviewPanelClass(hInstance)) {
        return nullptr;
    }
    HWND hwnd = CreateWindowExW(
        0,
        L"PdfPreviewPanelClass",
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 100, 100, // Initial size
        hParent,
        (HMENU)(UINT_PTR)id,
        hInstance,
        nullptr
    );
    return hwnd;
}

ThemeColors PdfPreviewPanel_GetTheme() {
    return pdf_panel::g_state.theme;
}

void PdfPreviewPanel_OpenPdf(HWND hwnd, const std::wstring& pdfPath, const std::wstring& clropPath) {
    pdf_panel::OpenPdfInViewer(hwnd, pdfPath, clropPath);
}

void PdfPreviewPanel_ChoosePageRange(HWND hwnd) {
    pdf_panel::ChoosePageRange(hwnd);
}

std::wstring PdfPreviewPanel_GetPdfPath(HWND hwnd) {
    if (pdf_panel::g_state.pdf) {
        return pdf_panel::g_state.pdf->path;
    }
    return L"";
}

void PdfPreviewPanel_ClosePdf(HWND hwnd) {
    pdf_panel::g_state.pdf.reset();
    pdf_panel::g_state.clrop.reset();
    pdf_panel::g_state.renderedPages.clear();
    pdf_panel::g_state.status.clear();
    InvalidateRect(hwnd, nullptr, TRUE);
}

} // namespace readonly_viewer
