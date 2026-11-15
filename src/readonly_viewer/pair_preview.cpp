#include "pair_preview.h"
#include "pdf_preview_panel.h"
#include "core/identity_path_scope.h"
#include "core/localization.h"
#include "ui/window_corners.h"
#include "ui/window_dpi.h"
#include <shellapi.h>

namespace readonly_viewer {
namespace {
struct Context { HWND panel{}, banner{}; std::wstring pdf, clrop; };
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<Context*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        ctx = static_cast<Context*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
    }
    if (!ctx) return DefWindowProcW(window, message, wp, lp);
    switch (message) {
    case WM_CREATE:
        (void)ui::ApplyAuxiliaryWindowCorners(window);
        ctx->banner = CreateWindowExW(0, L"STATIC", localization::Text(L"pair_preview.banner").c_str(),
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(1001), GetModuleHandleW(nullptr), nullptr);
        ctx->panel = CreatePdfPreviewPanel(window, GetModuleHandleW(nullptr), 1002);
        if (!ctx->panel || !ctx->banner) return -1;
        SendMessageW(ctx->banner, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return 0;
    case WM_SIZE: {
        const int bannerHeight = MulDiv(50, ui::WindowDpi(window), 96);
        RECT r{}; GetClientRect(window, &r);
        MoveWindow(ctx->banner, 12, 8, std::max(1L, r.right - 24), bannerHeight - 8, TRUE);
        MoveWindow(ctx->panel, 0, bannerHeight, r.right, std::max(1L, r.bottom - bannerHeight), TRUE);
        return 0;
    }
    case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {640, 480}; return 0;
    case WM_DPICHANGED: {
        const auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(window, nullptr, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY: PdfPreviewPanel_ClosePdf(ctx->panel); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}
std::optional<int> RunPdfPairPreviewCommand(HINSTANCE instance, int show) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    struct Free { LPWSTR* argv; ~Free() { LocalFree(argv); } } free{argv};
    if (argc < 2 || std::wstring(argv[1]) != L"--pair-preview") return std::nullopt;
    if (argc != 4) return 1;
    const auto pdf = identity_path_scope::LocalPath(argv[2]), clrop = identity_path_scope::LocalPath(argv[3]);
    if (!pdf || !clrop || !identity_path_scope::NoReparseAncestors(*pdf) || !identity_path_scope::NoReparseAncestors(*clrop)) return 1;
    auto pdfExtension = pdf->extension().wstring(), clropExtension = clrop->extension().wstring();
    for (auto& c : pdfExtension) c = static_cast<wchar_t>(std::towlower(c));
    for (auto& c : clropExtension) c = static_cast<wchar_t>(std::towlower(c));
    if (pdfExtension != L".pdf" || clropExtension != L".clrop") return 1;
    Context ctx; ctx.pdf = pdf->wstring(); ctx.clrop = clrop->wstring();
    WNDCLASSW cls{}; cls.lpfnWndProc = Procedure; cls.hInstance = instance;
    cls.lpszClassName = L"PdfReadonlyPairPreviewWindow"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;
    const HWND window = CreateWindowExW(0, cls.lpszClassName, localization::Text(L"pair_preview.title").c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1050, 850, nullptr, nullptr, instance, &ctx);
    if (!window) return 1;
    ShowWindow(window, show); UpdateWindow(window);
    PdfPreviewPanel_PreviewPair(ctx.panel, ctx.pdf, ctx.clrop);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return static_cast<int>(message.wParam);
}
} // namespace readonly_viewer
