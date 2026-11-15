#include "pdf_pair_finder.h"
#include "core/localization.h"
#include "ui/core/main_window_api.h"
#include "ui/dialogs/dialogs.h"
#include "ui/window_corners.h"
#include "ui/window_dpi.h"
#include <commctrl.h>
#include <atomic>
#include <thread>

namespace {
using namespace clrop::discovery;
namespace fs = std::filesystem;
constexpr wchar_t kClass[] = L"PdfNotePairFinderWindow";
// This window's timer only publishes worker results; it never starts I/O.
constexpr UINT_PTR kTimer = 4610;
enum { kDirectoryMode = 4601, kClropMode, kDirectory, kPickDirectory, kClrop,
       kPickClrop, kRun, kCancel, kPreview, kList = 4611, kDetail, kStatus, kClose };
struct Context {
    HWND owner{}, window{}, list{}, status{}, detail{};
    Mode mode;
    fs::path initial;
    PdfPairPreview preview;
    std::thread worker;
    std::atomic_bool cancel{false}, done{false};
    bool running = false, closing = false;
    Result completed;
    std::vector<Row> rows;
    ~Context() { cancel.store(true); if (worker.joinable()) worker.join(); }
};
std::wstring T(const wchar_t* id) { return localization::Text(id); }
int Px(HWND hwnd, int n) { return MulDiv(n, ui::WindowDpi(hwnd), 96); }
std::wstring Read(HWND window, int id) {
    const HWND edit = GetDlgItem(window, id);
    const int length = GetWindowTextLengthW(edit);
    if (length > 32760) return {};
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    value.resize(GetWindowTextW(edit, value.data(), length + 1)); return value;
}
void Place(HWND window, int id, int x, int y, int width, int height) {
    MoveWindow(GetDlgItem(window, id), Px(window, x), Px(window, y), Px(window, width), Px(window, height), TRUE);
}
void Layout(Context& ctx) {
    RECT r{}; GetClientRect(ctx.window, &r);
    const int w = MulDiv(r.right, 96, ui::WindowDpi(ctx.window));
    const int h = MulDiv(r.bottom, 96, ui::WindowDpi(ctx.window));
    Place(ctx.window, 4599, 14, 12, w - 28, 44);
    Place(ctx.window, kDirectoryMode, 14, 58, 260, 26); Place(ctx.window, kClropMode, 290, 58, w - 304, 26);
    Place(ctx.window, 4597, 14, 96, 110, 22); Place(ctx.window, kDirectory, 130, 92, w - 250, 28);
    Place(ctx.window, kPickDirectory, w - 110, 92, 96, 28);
    Place(ctx.window, 4598, 14, 132, 110, 22); Place(ctx.window, kClrop, 130, 128, w - 250, 28);
    Place(ctx.window, kPickClrop, w - 110, 128, 96, 28);
    Place(ctx.window, kRun, 14, 166, 126, 30); Place(ctx.window, kCancel, 150, 166, 120, 30);
    Place(ctx.window, kStatus, 282, 170, w - 296, 25);
    Place(ctx.window, kList, 14, 206, w - 28, std::max(80, h - 410));
    Place(ctx.window, kDetail, 14, h - 194, w - 28, 132);
    Place(ctx.window, kPreview, 14, h - 48, 260, 32); Place(ctx.window, kClose, w - 124, h - 48, 110, 32);
}
const wchar_t* EvidenceId(Evidence value) {
    switch (value) {
    case Evidence::Match: return L"pair_finder.match";
    case Evidence::Different: return L"pair_finder.different";
    case Evidence::Missing: return L"pair_finder.missing";
    default: return L"pair_finder.unknown";
    }
}
const wchar_t* GradeId(Grade grade) {
    switch (grade) {
    case Grade::Hash: return L"pair_finder.hash_candidate";
    case Grade::StructureAndSize: return L"pair_finder.structure_size";
    case Grade::Structure: return L"pair_finder.structure";
    case Grade::Size: return L"pair_finder.size_only";
    case Grade::Pages: return L"pair_finder.pages_only";
    case Grade::Name: return L"pair_finder.name_only";
    case Grade::NoCandidate: return L"pair_finder.no_candidate";
    default: return L"pair_finder.unreadable";
    }
}
int Selected(Context& ctx) { return ListView_GetNextItem(ctx.list, -1, LVNI_SELECTED); }
void Selection(Context& ctx) {
    const int index = Selected(ctx);
    const bool selected = index >= 0 && static_cast<size_t>(index) < ctx.rows.size();
    EnableWindow(GetDlgItem(ctx.window, kPreview), selected && ctx.rows[index].preview && !ctx.running);
    if (!selected) { SetWindowTextW(ctx.detail, L""); return; }
    const auto& row = ctx.rows[index];
    const auto hash = [](const std::string& s) { return s.empty() ? std::wstring(L"—") : std::wstring(s.begin(), s.end()); };
    // Paths are editable above; the result table/details intentionally use
    // complete filenames only. No path shortening changes operation targets.
    const std::wstring detail = T(GradeId(row.grade)) + L"\r\nCLROP: " + row.clrop.filename().wstring() +
        L"    PDF: " + row.pdf.filename().wstring() + L"\r\n" + T(L"pair_finder.size") + L": " +
        std::to_wstring(row.expectedSize) + L" / " + std::to_wstring(row.actualSize) + L"    " + T(L"pair_finder.pages") +
        L": " + std::to_wstring(row.expectedPages) + L" / " + std::to_wstring(row.actualPages) +
        L"\r\nSHA-256 (CLROP): " + hash(row.expectedHash) + L"\r\nSHA-256 (PDF): " + hash(row.actualHash);
    SetWindowTextW(ctx.detail, detail.c_str());
}
void Controls(Context& ctx) {
    for (int id : {kDirectoryMode, kClropMode, kDirectory, kPickDirectory, kRun})
        EnableWindow(GetDlgItem(ctx.window, id), !ctx.running);
    for (int id : {kClrop, kPickClrop}) EnableWindow(GetDlgItem(ctx.window, id), !ctx.running && ctx.mode == Mode::ForClrop);
    EnableWindow(GetDlgItem(ctx.window, kCancel), ctx.running);
    CheckRadioButton(ctx.window, kDirectoryMode, kClropMode, ctx.mode == Mode::Directory ? kDirectoryMode : kClropMode);
    Selection(ctx);
}
void Fill(Context& ctx) {
    SendMessageW(ctx.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(ctx.list);
    for (size_t i = 0; i < ctx.rows.size(); ++i) {
        const auto& row = ctx.rows[i];
        std::wstring columns[] = {row.clrop.filename().wstring(), row.pdf.filename().wstring(), T(GradeId(row.grade)),
            row.clrop.empty() ? L"—" : row.grade == Grade::Unreadable ? T(L"pair_finder.unknown") :
                T(row.orphan ? L"pair_finder.orphan" : L"pair_finder.adjacent"),
            T(EvidenceId(row.hash)), T(EvidenceId(row.size)), T(EvidenceId(row.pages)), T(EvidenceId(row.dimensions))};
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i); item.pszText = columns[0].data();
        ListView_InsertItem(ctx.list, &item);
        for (int col = 1; col < 8; ++col) ListView_SetItemText(ctx.list, static_cast<int>(i), col, columns[col].data());
    }
    SendMessageW(ctx.list, WM_SETREDRAW, TRUE, 0); InvalidateRect(ctx.list, nullptr, TRUE);
}
void Complete(Context& ctx) {
    if (!ctx.running || !ctx.done.load(std::memory_order_acquire)) return;
    if (ctx.worker.joinable()) ctx.worker.join();
    ctx.running = false;
    ctx.rows = std::move(ctx.completed.rows); Fill(ctx);
    const wchar_t* state = ctx.completed.completion == Completion::Complete ? L"pair_finder.complete" :
        ctx.completed.completion == Completion::Canceled ? L"pair_finder.canceled" :
        ctx.completed.completion == Completion::Limited ? L"pair_finder.limited" : L"pair_finder.failed";
    const auto status = T(state) + L"  PDF " + std::to_wstring(ctx.completed.pdfs) + L" / CLROP " +
        std::to_wstring(ctx.completed.clrops) + L" / " + std::to_wstring(ctx.rows.size()) + L" " + T(L"pair_finder.rows") +
        L" / " + T(L"pair_finder.skipped") + L" " + std::to_wstring(ctx.completed.skipped);
    SetWindowTextW(ctx.status, status.c_str()); Controls(ctx);
    if (ctx.closing) DestroyWindow(ctx.window);
}
void Start(Context& ctx) {
    if (ctx.running) return;
    Request request; request.mode = ctx.mode;
    request.directory = Read(ctx.window, kDirectory); request.clrop = Read(ctx.window, kClrop);
    ctx.cancel.store(false); ctx.done.store(false); ctx.running = true; ctx.completed = {};
    ctx.rows.clear(); Fill(ctx); Controls(ctx); SetWindowTextW(ctx.status, T(L"pair_finder.running").c_str());
    try {
        ctx.worker = std::thread([&ctx, request] {
            try { ctx.completed = Scan(request, ctx.cancel); }
            catch (...) { ctx.completed.completion = Completion::Failed; }
            ctx.done.store(true, std::memory_order_release);
        });
    } catch (...) {
        ctx.running = false; Controls(ctx); SetWindowTextW(ctx.status, T(L"pair_finder.failed").c_str());
    }
}
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<Context*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        ctx = static_cast<Context*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        ctx->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
    }
    if (!ctx) return DefWindowProcW(window, message, wp, lp);
    switch (message) {
    case WM_CREATE: {
        auto make = [&](const wchar_t* cls, const wchar_t* label, int id, DWORD style) {
            HWND child = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
            if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            return child;
        };
        bool ok = make(L"STATIC", T(L"pair_finder.explanation").c_str(), 4599, 0) != nullptr;
        ok &= make(L"BUTTON", T(L"pair_finder.directory_mode").c_str(), kDirectoryMode, BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP) != nullptr;
        ok &= make(L"BUTTON", T(L"pair_finder.clrop_mode").c_str(), kClropMode, BS_AUTORADIOBUTTON | WS_TABSTOP) != nullptr;
        ok &= make(L"STATIC", T(L"pair_finder.directory").c_str(), 4597, 0) != nullptr;
        ok &= make(L"STATIC", L"CLROP", 4598, 0) != nullptr;
        ok &= make(L"EDIT", ctx->initial.c_str(), kDirectory, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL) != nullptr;
        ok &= make(L"EDIT", L"", kClrop, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL) != nullptr;
        for (int id : {kPickDirectory, kPickClrop}) ok &= make(L"BUTTON", T(L"pair_finder.browse").c_str(), id, WS_TABSTOP) != nullptr;
        ok &= make(L"BUTTON", T(L"pair_finder.run").c_str(), kRun, WS_TABSTOP) != nullptr;
        ok &= make(L"BUTTON", T(L"pair_finder.cancel").c_str(), kCancel, WS_TABSTOP) != nullptr;
        ok &= make(L"BUTTON", T(L"pair_finder.preview").c_str(), kPreview, WS_TABSTOP) != nullptr;
        ok &= make(L"BUTTON", T(L"common.close").c_str(), kClose, WS_TABSTOP) != nullptr;
        ctx->status = make(L"STATIC", T(L"pair_finder.ready").c_str(), kStatus, 0); ok &= ctx->status != nullptr;
        ctx->detail = make(L"EDIT", L"", kDetail, WS_BORDER | ES_READONLY | ES_MULTILINE | WS_VSCROLL | WS_TABSTOP); ok &= ctx->detail != nullptr;
        ctx->list = make(WC_LISTVIEWW, L"", kList, LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP); ok &= ctx->list != nullptr;
        if (!ok) return -1;
        SendDlgItemMessageW(window, kDirectory, EM_SETLIMITTEXT, 32760, 0); SendDlgItemMessageW(window, kClrop, EM_SETLIMITTEXT, 32760, 0);
        ListView_SetExtendedListViewStyle(ctx->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* labels[] = {L"CLROP", L"PDF", L"pair_finder.grade", L"pair_finder.same_name_pdf", L"SHA-256", L"pair_finder.size", L"pair_finder.pages", L"pair_finder.dimensions"};
        for (int i = 0; i < 8; ++i) {
            auto label = i == 0 || i == 1 || i == 4 ? std::wstring(labels[i]) : T(labels[i]);
            LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.pszText = label.data(); column.cx = Px(window, i < 3 ? 170 : i == 3 ? 150 : 100);
            ListView_InsertColumn(ctx->list, i, &column);
        }
        Controls(*ctx); Layout(*ctx); (void)ui::ApplyAuxiliaryWindowCorners(window);
        if (!SetTimer(window, kTimer, 150, nullptr)) return -1;
        return 0;
    }
    case WM_SIZE: Layout(*ctx); return 0;
    case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {Px(window, 900), Px(window, 660)}; return 0;
    case WM_DPICHANGED: { const auto* r = reinterpret_cast<RECT*>(lp); SetWindowPos(window, nullptr, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER | SWP_NOACTIVATE); Layout(*ctx); return 0; }
    case WM_TIMER: if (wp == kTimer) Complete(*ctx); return 0;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lp)->idFrom == kList) {
            Selection(*ctx);
            if (reinterpret_cast<NMHDR*>(lp)->code == NM_DBLCLK) SendMessageW(window, WM_COMMAND, kPreview, 0);
        }
        return 0;
    case WM_COMMAND:
        if (ctx->list && !ctx->running && HIWORD(wp) == EN_CHANGE &&
            (LOWORD(wp) == kDirectory || LOWORD(wp) == kClrop)) {
            ctx->rows.clear(); Fill(*ctx); Controls(*ctx); SetWindowTextW(ctx->status, T(L"pair_finder.ready").c_str());
            return 0;
        }
        switch (LOWORD(wp)) {
        case kDirectoryMode: case kClropMode:
            if (!ctx->running) {
                ctx->mode = LOWORD(wp) == kDirectoryMode ? Mode::Directory : Mode::ForClrop;
                ctx->rows.clear(); Fill(*ctx); Controls(*ctx); SetWindowTextW(ctx->status, T(L"pair_finder.ready").c_str());
            }
            break;
        case kPickDirectory: case kPickClrop: {
            if (ctx->running) break;
            const bool directory = LOWORD(wp) == kPickDirectory;
            const auto selected = PromptExistingLocalPath(window, ctx->initial, T(directory ? L"pair_finder.directory" : L"pair_finder.pick_clrop"), directory);
            if (selected) SetDlgItemTextW(window, directory ? kDirectory : kClrop, selected->c_str());
            break;
        }
        case kRun: Start(*ctx); break;
        case kCancel: ctx->cancel.store(true); SetWindowTextW(ctx->status, T(L"pair_finder.stopping").c_str()); break;
        case kPreview: {
            const int i = Selected(*ctx);
            if (!ctx->running && i >= 0 && static_cast<size_t>(i) < ctx->rows.size() && ctx->rows[i].preview) {
                const auto row = ctx->rows[i];
                if (!ctx->preview || !ctx->preview(window, row.pdf, row.clrop)) SetWindowTextW(ctx->status, T(L"pair_finder.preview_failed").c_str());
            }
            break;
        }
        case kClose: SendMessageW(window, WM_CLOSE, 0, 0); break;
        }
        return 0;
    case WM_CLOSE:
        if (ctx->running) { ctx->closing = true; ctx->cancel.store(true); SetWindowTextW(ctx->status, T(L"pair_finder.stopping").c_str()); }
        else DestroyWindow(window);
        return 0;
    case WM_DESTROY: ctx->cancel.store(true); KillTimer(window, kTimer); return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}
void ShowPdfPairFinder(HWND owner, Mode mode, const fs::path& initialDirectory, PdfPairPreview preview) {
    Context ctx; ctx.owner = owner; ctx.mode = mode; ctx.initial = initialDirectory; ctx.preview = std::move(preview);
    WNDCLASSW cls{}; cls.lpfnWndProc = Procedure; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = kClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    const HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, T(L"pair_finder.title").c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, Px(owner, 1150), Px(owner, 740), owner, nullptr, cls.hInstance, &ctx);
    if (!window) return;
    const bool enabled = IsWindowEnabled(owner) != FALSE;
    if (enabled) EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW); UpdateWindow(window);
    MSG message{};
    while (IsWindow(window)) {
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) { if (!got) PostQuitMessage(static_cast<int>(message.wParam)); DestroyWindow(window); break; }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { SendMessageW(window, WM_CLOSE, 0, 0); continue; }
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (enabled && IsWindow(owner)) { EnableWindow(owner, TRUE); SetActiveWindow(owner); }
}
