#include "ui/dialogs/write_checks_dialog.h"
#include "diagnostics/write_checks.h"
#include "diagnostics/normal_operations.h"
#include "core/app_core.h"
#include "core/atomic_write.h"
#include "core/localization.h"
#include "core/ui_notify.h"
#include "workspace/workspace_actions.h"
#include "office/work_paths.h"
#include <commctrl.h>
#include <atomic>
#include <memory>
#include <thread>
#include <algorithm>
#include <cstring>

namespace {
using namespace write_checks;
// IDs are local to WriteChecksWndProc. WM_TIMER never dispatches a probe.
constexpr UINT_PTR kWriteChecksDisplayTimerId = 4701;
constexpr int kListId = 4702, kRunId = 4703, kDetailId = 4704, kRefreshId = 4705, kCloseId = 4706, kSaveId = 4707;
static_assert(kWriteChecksDisplayTimerId != kListId && kWriteChecksDisplayTimerId != kRunId &&
              kWriteChecksDisplayTimerId != kDetailId && kWriteChecksDisplayTimerId != kRefreshId && kWriteChecksDisplayTimerId != kCloseId && kWriteChecksDisplayTimerId != kSaveId);
constexpr wchar_t kClass[] = L"PdfNoteWriteChecksWnd";
HWND window = nullptr;
ManualLimit manualLimit;
// The same check kind can have distinct destinations. Keep persisted kind IDs
// stable and share normal-use evidence with each matching destination.
constexpr std::array<Kind, kCount + 2> kItemKinds = {Kind::SetupRead, Kind::SetupWrite,
    Kind::WorkspaceRead, Kind::WorkspaceWrite, Kind::SettingsWrite, Kind::TempWrite,
    Kind::RecoveryWrite, Kind::LogsWrite, Kind::PdfWrite, Kind::NoteWrite,
    Kind::PdfWrite, Kind::NoteWrite, Kind::OfficeTempWrite, Kind::PdfRead, Kind::NoteReadWrite, Kind::OfficeConversion};
const wchar_t* ItemLabelId(size_t index) {
    if (index == 10) return L"checks.session_pdf_write";
    if (index == 11) return L"checks.session_note_write";
    return LabelId(kItemKinds[index]);
}
struct Context {
    HWND owner{}, list{}, header{}, status{}, run{}, detail{}, refresh{}, close{}, save{};
    std::array<std::filesystem::path, kItemKinds.size()> targets;
    std::filesystem::path store;
    std::vector<Record> records;
    std::string baseline;
    DWORD storageError = 0;
    DWORD loadError = 0; // Keep the ledger-read cause separate from pending-save errors.
    bool loadOk = true, unsaved = false, closing = false;
    bool normalUnsaved = false;
    std::uint64_t normalRevision = 0;
    std::thread worker;
    std::atomic_bool cancel{false}, done{false};
    Record completed;
    std::vector<Record> completedRecords;
    std::string completedBaseline;
    DWORD completedStorageError = 0;
    bool completedUnsaved = false, pending = false, callerOwns = true, storageOnly = false;
    std::wstring snapshotRoot, snapshotSession, snapshotPdf, snapshotNote;
    bool previousComparable = false;
    Outcome previousOutcome{};
    std::wstring lastStatus;
    ~Context() { cancel.store(true); if (worker.joinable()) worker.join(); }
};
void Layout(HWND hwnd, Context& ctx);
std::wstring T(const wchar_t* id) { return localization::Text(id); }
std::filesystem::path ExecutableDirectory() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!size) return {};
        if (size < buffer.size()) return std::filesystem::path(std::wstring(buffer.data(), size)).parent_path();
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
}
void Snapshot(Context& ctx) {
    const auto exe = ExecutableDirectory();
    const std::filesystem::path root(g_workspaceRoot);
    ctx.snapshotRoot = g_workspaceRoot; ctx.snapshotSession = g_currentSessionPath; ctx.snapshotPdf = CurrentLogicalPdfPath();
    ctx.snapshotNote = g_currentNotePath;
    ctx.targets = {exe.empty() ? std::filesystem::path{} : exe / L"pdf_note_workspace_setup.json", exe,
        root, root, root.empty() ? std::filesystem::path{} : root / L"__pdf_note_workspace__" / L"__settings__",
        root.empty() ? std::filesystem::path{} : root / L"__pdf_note_workspace__" / L"__tmp__",
        root.empty() ? std::filesystem::path{} : root / L"__pdf_note_workspace__" / L"__escape__",
        root.empty() ? std::filesystem::path{} : root / L"__pdf_note_workspace__" / L"__log__",
        CurrentPdfDirectory(), CurrentNoteDirectory(), std::filesystem::path(g_currentSessionPath), std::filesystem::path(g_currentSessionPath),
        root.empty() ? std::filesystem::path{} : office::work_paths::Root(root).parent_path(),
        std::filesystem::path(CurrentLogicalPdfPath()), std::filesystem::path(g_currentNotePath), office::work_paths::Root(root)};
    for (auto& path : ctx.targets) if (!path.empty()) path = path.lexically_normal();
    ctx.store = root.empty() ? std::filesystem::path{} : root / L"__pdf_note_workspace__" / L"__log__" / L"write_checks.log";
}
void LoadRecords(Context& ctx) {
    ctx.records.clear(); ctx.baseline.clear(); ctx.unsaved = false;
    try { ctx.loadOk = Load(ctx.store, ctx.records, ctx.baseline, ctx.storageError); }
    catch (...) { ctx.loadOk = false; ctx.storageError = ERROR_INVALID_DATA; }
    ctx.loadError = ctx.loadOk ? 0 : ctx.storageError;
    ctx.normalUnsaved = false;
    ctx.normalRevision = MergeNormalObservations(ctx.store, ctx.records, ctx.normalUnsaved, ctx.storageError);
}
const wchar_t* OutcomeId(Outcome outcome) {
    switch (outcome) {
    case Outcome::Passed: return L"checks.success";
    case Outcome::Failed: return L"checks.failure";
    case Outcome::Unavailable: return L"checks.check_failure";
    case Outcome::Canceled: return L"checks.canceled";
    }
    return L"checks.check_failure";
}
std::wstring Timestamp(std::uint64_t value) {
    if (!value) return L"—";
    FILETIME utc{static_cast<DWORD>(value), static_cast<DWORD>(value >> 32)}, local{};
    SYSTEMTIME time{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &time)) return L"—";
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return buffer;
}
int Selected(Context& ctx) { return ListView_GetNextItem(ctx.list, -1, LVNI_SELECTED); }
std::wstring Key(const std::filesystem::path& path) { return path.empty() ? L"(no target)" : path.wstring(); }
std::wstring TargetName(std::filesystem::path path) {
    if (path.empty()) return T(L"checks.no_target");
    // Strip trailing separators only from the display copy. Keep a root intact.
    while (path.has_relative_path() && path.filename().empty()) path = path.parent_path();
    return path.filename().empty() ? path.root_path().wstring() : path.filename().wstring();
}
void Fill(Context& ctx) {
    const int selected = Selected(ctx);
    ListView_DeleteAllItems(ctx.list);
    for (size_t i = 0; i < kItemKinds.size(); ++i) {
        const auto kind = kItemKinds[i];
        auto label = T(ItemLabelId(i)); LVITEMW item{};
        item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i); item.pszText = label.data();
        ListView_InsertItem(ctx.list, &item);
        const Record* record = Find(ctx.records, kind, Key(ctx.targets[i]));
        auto status = T(record ? OutcomeId(record->result.outcome) : (ctx.loadOk ? L"checks.no_record" : L"checks.check_failure"));
        ListView_SetItemText(ctx.list, static_cast<int>(i), 1, status.data());
        auto time = record ? Timestamp(record->result.time) : std::wstring(L"—");
        ListView_SetItemText(ctx.list, static_cast<int>(i), 2, time.data());
        // Filename only; exact paths are confined to the shared detail dialog.
        auto name = TargetName(ctx.targets[i]);
        ListView_SetItemText(ctx.list, static_cast<int>(i), 3, name.data());
    }
    ListView_SetItemState(ctx.list, selected < 0 ? 0 : selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}
void UpdateStatus(Context& ctx) {
    const auto now = GetTickCount64();
    const auto remaining = manualLimit.Remaining(now);
    std::wstring status;
    const bool busy = ctx.pending || manualLimit.busy();
    const bool stale = ctx.snapshotRoot != g_workspaceRoot || ctx.snapshotSession != g_currentSessionPath ||
        ctx.snapshotPdf != CurrentLogicalPdfPath() || ctx.snapshotNote != g_currentNotePath;
    if (busy) status = T(ctx.closing ? L"checks.canceling" : L"checks.running");
    else if (remaining) status = localization::Format(L"checks.cooldown", {{L"SECONDS", std::to_wstring((remaining + 999) / 1000)}}) + L" " +
        (manualLimit.RecoveryAvailable(now) ? localization::Format(L"checks.exception_available", {{L"COUNT", std::to_wstring(manualLimit.RemainingRetries(now))}}) : T(L"checks.exception_used"));
    else status = T(L"checks.ready");
    if (!ctx.loadOk) status += L"\r\n" + T(L"checks.record_load_failed") + L" (" + std::to_wstring(ctx.loadError) + L")";
    if (ctx.unsaved || ctx.normalUnsaved) status += L"\r\n" +
        T(!ctx.unsaved && !ctx.storageError ? L"checks.save_pending" : L"checks.not_saved") +
        (ctx.storageError ? L" (" + std::to_wstring(ctx.storageError) + L")" : L"");
    if (stale) status += L"\r\n" + T(L"checks.target_changed");
    if (status != ctx.lastStatus) {
        SetWindowTextW(ctx.status, status.c_str()); ctx.lastStatus = status;
        Layout(GetParent(ctx.status), ctx);
    }
    const int selected = Selected(ctx);
    const Record* record = selected >= 0 ? Find(ctx.records, kItemKinds[selected], Key(ctx.targets[selected])) : nullptr;
    const bool retry = remaining && manualLimit.RecoveryAvailable(now) && record && record->result.outcome == Outcome::Failed;
    const bool first = remaining && manualLimit.RecoveryAvailable(now) && !record && ctx.loadOk;
    SetWindowTextW(ctx.run, T(retry ? L"checks.improved_retry" : L"checks.run").c_str());
    EnableWindow(ctx.run, !ctx.closing && !busy && !stale && selected >= 0 && (!remaining || retry || first));
    EnableWindow(ctx.refresh, !busy && !ctx.unsaved); // Normal pending evidence stays in the process registry.
    EnableWindow(ctx.save, !busy && (ctx.unsaved || ctx.normalUnsaved));
    EnableWindow(ctx.detail, !busy && selected >= 0);
}
void Detail(HWND hwnd, Context& ctx, size_t index) {
    const auto kind = kItemKinds[index];
    const auto* record = Find(ctx.records, kind, Key(ctx.targets[index]));
    SilentDialogOptions options;
    options.title = T(ItemLabelId(index));
    options.message = T(L"dialog.details.status") + L"\n" +
        T(record ? OutcomeId(record->result.outcome) : (ctx.loadOk ? L"checks.no_record" : L"checks.check_failure"));
    options.additionalInformation = T(L"dialog.details.additional_information") + L"\n" +
        T(record && record->result.source == Source::Normal ?
        (kind == Kind::SetupRead || kind == Kind::WorkspaceRead || kind == Kind::PdfRead ? L"checks.description.normal_read" : L"checks.description.normal") :
        kind == Kind::OfficeConversion ? L"checks.description.conversion" :
        kind == Kind::PdfRead ? L"checks.description.pdf" :
        kind == Kind::NoteReadWrite ? L"checks.description.note" :
        kind == Kind::SetupRead ? L"checks.description.file" :
        kind == Kind::WorkspaceRead ? L"checks.description.read" : L"checks.description.write");
    if (kind == Kind::OfficeTempWrite) options.additionalInformation += L"\n" + T(L"checks.description.office_location");
    if (kind == Kind::PdfWrite || kind == Kind::NoteWrite)
        options.additionalInformation += L"\n" + T(L"checks.description.destination");
    if (ctx.targets[index].empty()) options.additionalInformation += L"\n" + T(L"checks.reason.no_target");
    if (!ctx.loadOk) options.additionalInformation += L"\n" + T(L"checks.record_load_failed") +
        L"\n" + std::to_wstring(ctx.loadError) + L": " + atomic_write::Win32ErrorMessage(ctx.loadError);
    if (ctx.unsaved || ctx.normalUnsaved) {
        options.additionalInformation += L"\n" + T(!ctx.unsaved && !ctx.storageError ? L"checks.save_pending" : L"checks.not_saved");
        if (ctx.storageError) options.additionalInformation += L"\n" + std::to_wstring(ctx.storageError) + L": " + atomic_write::Win32ErrorMessage(ctx.storageError);
    }
    if (!ctx.targets[index].empty()) options.paths.push_back({T(L"checks.target"), ctx.targets[index].wstring()});
    if (record) {
        options.additionalInformation += L"\n" + T(record->result.source == Source::Normal ? L"checks.source.normal" : L"checks.source.manual");
        options.additionalInformation += L"\n\n" + T(L"checks.time") + L": " + Timestamp(record->result.time) + L"\n" + T(StepId(record->result.step));
        if (record->result.error) options.additionalInformation += L"\n" + std::to_wstring(record->result.error) + L": " + atomic_write::Win32ErrorMessage(record->result.error);
        if (record->result.outcome == Outcome::Unavailable && record->result.step == Step::Open) {
            if (record->result.error == ERROR_FILE_NOT_FOUND || record->result.error == ERROR_PATH_NOT_FOUND)
                options.additionalInformation += L"\n" + T(kind == Kind::PdfRead || kind == Kind::NoteReadWrite || kind == Kind::SetupRead
                    ? L"checks.reason.missing_file" : L"checks.reason.missing_directory");
            else if (record->result.error == ERROR_DIRECTORY) options.additionalInformation += L"\n" + T(L"checks.reason.not_directory");
        }
        if (!record->result.remaining.empty()) options.paths.push_back({T(L"checks.remaining_path"), record->result.remaining});
        if (!record->result.operationPath.empty()) options.paths.push_back({T(L"checks.operation_path"), record->result.operationPath});
    }
    if (!ctx.store.empty()) options.paths.push_back({T(L"checks.record_path"), ctx.store.wstring()});
    options.message += L"\n\n" + T(L"dialog.details.paths");
    if (options.paths.empty()) options.message += L"\n" + T(L"checks.no_target");
    const auto ignored = ShowSilentDialog(hwnd, options); (void)ignored;
}
Result Execute(Kind kind, const std::filesystem::path& target, std::atomic_bool& cancel) {
    IgnoreNormalOperations ignore;
    Result result; result.time = UtcNow();
    if (cancel.load()) { result.outcome = Outcome::Canceled; result.error = ERROR_CANCELLED; return result; }
    if (target.empty()) { result.outcome = Outcome::Unavailable; result.error = ERROR_NOT_READY; return result; }
    if (kind == Kind::OfficeConversion) {
        result.step = Step::Convert;
        if (!HasOfficeConversionFeature()) { result.outcome = Outcome::Unavailable; result.error = ERROR_NOT_SUPPORTED; return result; }
        std::filesystem::path retained;
        try {
            if (RunOfficeConversionCheck(target, cancel, retained, result.error, result.step)) { result.outcome = Outcome::Passed; result.step = Step::Complete; }
        } catch (...) { result.outcome = Outcome::Unavailable; result.error = ERROR_UNHANDLED_EXCEPTION; }
        if (result.error == ERROR_NOT_SUPPORTED || result.error == ERROR_INVALID_NAME) result.outcome = Outcome::Unavailable;
        result.remaining = retained.wstring();
        if (cancel.load()) result.outcome = Outcome::Canceled;
        return result;
    }
    if (kind == Kind::PdfRead) {
        std::wstring detail;
        if (!IsSafeLocalPath(target)) { result.outcome = Outcome::Unavailable; result.error = ERROR_INVALID_NAME; }
        else {
            const DWORD attributes = GetFileAttributesW(ToExtendedWin32PathIfAbsoluteLocal(target).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                result.error = GetLastError();
                if (result.error == ERROR_FILE_NOT_FOUND || result.error == ERROR_PATH_NOT_FOUND) result.outcome = Outcome::Unavailable;
            } else if (attributes & FILE_ATTRIBUTE_DIRECTORY) { result.outcome = Outcome::Unavailable; result.error = ERROR_INVALID_NAME; }
            else {
                result.step = Step::Pdf;
                if (ValidateImportPdfFile(target, &detail)) { result.outcome = Outcome::Passed; result.step = Step::Complete; }
                else result.error = ERROR_INVALID_DATA;
            }
        }
        return result;
    }
    if (kind == Kind::NoteReadWrite) return ProbeNoteFile(target, cancel);
    return kind == Kind::SetupRead ? ProbeFile(target) : ProbeDirectory(target, kind != Kind::WorkspaceRead);
}
void Run(HWND hwnd, Context& ctx) {
    const int selected = Selected(ctx);
    if (selected < 0 || manualLimit.busy() || ctx.pending || ctx.snapshotRoot != g_workspaceRoot || ctx.snapshotSession != g_currentSessionPath ||
        ctx.snapshotPdf != CurrentLogicalPdfPath() || ctx.snapshotNote != g_currentNotePath) return;
    const auto kind = kItemKinds[selected];
    const auto* previous = Find(ctx.records, kind, Key(ctx.targets[selected]));
    const bool recovery = manualLimit.Remaining(GetTickCount64()) && previous && previous->result.outcome == Outcome::Failed;
    const bool first = manualLimit.Remaining(GetTickCount64()) && !previous && ctx.loadOk;
    if (recovery) {
        if (!manualLimit.RecoveryAvailable(GetTickCount64())) return;
        SilentDialogOptions confirm; confirm.title = T(L"checks.title"); confirm.message = T(L"checks.retry_confirmation");
        confirm.buttons = SilentDialogButtons::YesNo; confirm.defaultResult = SilentDialogResult::No; confirm.escapeResult = SilentDialogResult::No;
        if (ShowSilentDialog(hwnd, confirm) != SilentDialogResult::Yes) return;
    }
    if (!manualLimit.Start(GetTickCount64(), recovery || first)) return;
    ctx.previousComparable = previous && previous->result.outcome != Outcome::Canceled;
    if (previous) ctx.previousOutcome = previous->result.outcome;
    ctx.cancel.store(false); ctx.done.store(false);
    ctx.pending = true;
    ctx.storageOnly = false;
    const auto target = ctx.targets[selected];
    try {
        Record initial; initial.kind = kind; initial.target = Key(target);
        ctx.worker = std::thread([&ctx, kind, target, completed = std::move(initial), records = ctx.records, baseline = ctx.baseline,
                                  store = ctx.store, loadOk = ctx.loadOk, storageError = ctx.storageError]() mutable {
            try { completed.result = Execute(kind, target, ctx.cancel); }
            catch (...) { completed.result.outcome = Outcome::Unavailable; completed.result.error = ERROR_UNHANDLED_EXCEPTION; completed.result.time = UtcNow(); }
            completed.result.time = UtcNow(); // Timestamp confirmation, not probe start.
            try {
                Upsert(records, completed);
                ctx.completedUnsaved = !loadOk || !SaveLatest(store, records, baseline, storageError);
                if (!ctx.completedUnsaved) AcknowledgeNormalObservations(store, records);
                ctx.completedRecords = std::move(records);
                ctx.completedBaseline = std::move(baseline);
                ctx.completedStorageError = storageError;
            } catch (...) { ctx.completedUnsaved = true; ctx.completedStorageError = ERROR_UNHANDLED_EXCEPTION; }
            ctx.completed = std::move(completed);
            ctx.done.store(true, std::memory_order_release);
        });
    } catch (...) {
        ctx.pending = false;
        manualLimit.Finish(GetTickCount64(), false);
        ctx.storageError = ERROR_NOT_ENOUGH_MEMORY;
        ShowSoftNotice(hwnd, T(L"checks.worker_failed"), SoftNoticeKind::Error);
    }
    UpdateStatus(ctx);
}
void Complete(Context& ctx) {
    if (!ctx.pending || !ctx.done.load(std::memory_order_acquire)) return;
    if (ctx.worker.joinable()) ctx.worker.join();
    ctx.pending = false;
    const auto outcome = ctx.completed.result.outcome;
    if (!ctx.storageOnly) manualLimit.Finish(GetTickCount64(), ctx.previousComparable && ctx.previousOutcome == outcome &&
        outcome != Outcome::Canceled);
    if (!ctx.completedRecords.empty()) ctx.records = std::move(ctx.completedRecords);
    else if (!ctx.storageOnly) Upsert(ctx.records, ctx.completed);
    ctx.baseline = std::move(ctx.completedBaseline);
    ctx.storageError = ctx.completedStorageError;
    ctx.unsaved = ctx.completedUnsaved;
    if (ctx.storageOnly && !ctx.unsaved) { ctx.loadOk = true; ctx.loadError = 0; }
    Fill(ctx); UpdateStatus(ctx);
}
void SaveAgain(Context& ctx) {
    if (ctx.pending || manualLimit.busy() || (!ctx.unsaved && !ctx.normalUnsaved)) return;
    ctx.done.store(false); ctx.pending = true; ctx.storageOnly = true;
    try {
        ctx.worker = std::thread([&ctx, records = ctx.records, store = ctx.store]() mutable {
            DWORD error = 0; std::string baseline;
            bool saved = false;
            try {
                saved = SaveLatest(store, records, baseline, error);
                if (saved) AcknowledgeNormalObservations(store, records);
            } catch (...) { error = ERROR_UNHANDLED_EXCEPTION; }
            ctx.completedRecords = std::move(records); ctx.completedBaseline = std::move(baseline);
            ctx.completedStorageError = error; ctx.completedUnsaved = !saved;
            ctx.done.store(true, std::memory_order_release);
        });
    } catch (...) { ctx.pending = false; ctx.storageError = ERROR_NOT_ENOUGH_MEMORY; }
    UpdateStatus(ctx);
}
int Scale(HWND hwnd, int value) {
    using GetWindowDpi = UINT (WINAPI*)(HWND);
    static const auto getDpi = [] {
        const FARPROC address = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        GetWindowDpi result = nullptr;
        static_assert(sizeof(result) == sizeof(address));
        std::memcpy(&result, &address, sizeof(result));
        return result;
    }();
    int dpi = getDpi ? static_cast<int>(getDpi(hwnd)) : 96;
    if (!getDpi) { HDC dc = GetDC(hwnd); if (dc) { dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(hwnd, dc); } }
    return MulDiv(value, dpi > 0 ? dpi : 96, 96);
}
int TextHeight(HWND hwnd, HWND control, const std::wstring& text, int width) {
    HDC dc = GetDC(hwnd);
    if (!dc) return Scale(hwnd, 80);
    const auto font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    HGDIOBJ previous = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    RECT bounds{0, 0, width, 0};
    DrawTextW(dc, text.c_str(), -1, &bounds, DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, previous); ReleaseDC(hwnd, dc);
    return std::max(1L, bounds.bottom);
}
void Layout(HWND hwnd, Context& ctx) {
    RECT rc{}; GetClientRect(hwnd, &rc);
    const int p = Scale(hwnd, 12), gap = Scale(hwnd, 8), buttonHeight = Scale(hwnd, 30);
    const int width = std::max(1L, rc.right - 2 * p);
    const int headerHeight = std::max(Scale(hwnd, 80), TextHeight(hwnd, ctx.header, T(L"checks.description"), width));
    const int statusHeight = std::max(Scale(hwnd, 62), TextHeight(hwnd, ctx.status, ctx.lastStatus, width));
    const int buttonsY = rc.bottom - buttonHeight - p;
    const int footer = buttonsY - gap - statusHeight;
    const int listY = p + headerHeight + gap;
    const int buttonWidth = std::max(1, (width - gap * 4) / 5);
    MoveWindow(ctx.header, p, p, width, headerHeight, TRUE);
    MoveWindow(ctx.list, p, listY, width, std::max(1, footer - gap - listY), TRUE);
    MoveWindow(ctx.status, p, footer, width, statusHeight, TRUE);
    const HWND buttons[] = {ctx.detail, ctx.refresh, ctx.run, ctx.save, ctx.close};
    for (size_t i = 0; i < 5; ++i) MoveWindow(buttons[i], p + static_cast<int>(i) * (buttonWidth + gap), buttonsY, buttonWidth, buttonHeight, TRUE);
    ListView_SetColumnWidth(ctx.list, 0, Scale(hwnd, 235));
    ListView_SetColumnWidth(ctx.list, 1, Scale(hwnd, 130));
    ListView_SetColumnWidth(ctx.list, 2, Scale(hwnd, 175));
    ListView_SetColumnWidth(ctx.list, 3, std::max(Scale(hwnd, 90), width - Scale(hwnd, 566)));
}
LRESULT CALLBACK WriteChecksWndProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<Context*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        ctx = static_cast<Context*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
    }
    if (!ctx) return DefWindowProcW(hwnd, message, wp, lp);
    switch (message) {
    case WM_CREATE: {
        auto create = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
            HWND child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE); return child;
        };
        ctx->header = create(L"STATIC", T(L"checks.description").c_str(), 0, 0);
        ctx->list = create(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, kListId);
        ListView_SetExtendedListViewStyle(ctx->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* columns[] = {L"checks.item", L"checks.result", L"checks.time", L"checks.target"};
        for (int i = 0; i < 4; ++i) { auto text = T(columns[i]); LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.pszText = text.data(); column.cx = 140; ListView_InsertColumn(ctx->list, i, &column); }
        ctx->status = create(L"STATIC", L"", 0, 0);
        ctx->detail = create(L"BUTTON", T(L"checks.details").c_str(), WS_TABSTOP, kDetailId);
        ctx->refresh = create(L"BUTTON", T(L"checks.refresh").c_str(), WS_TABSTOP, kRefreshId);
        ctx->run = create(L"BUTTON", T(L"checks.run").c_str(), WS_TABSTOP, kRunId);
        ctx->save = create(L"BUTTON", T(L"checks.save_again").c_str(), WS_TABSTOP, kSaveId);
        ctx->close = create(L"BUTTON", T(L"checks.close").c_str(), WS_TABSTOP, kCloseId);
        if (!ctx->list || !ctx->header || !ctx->status || !ctx->run || !ctx->detail || !ctx->refresh || !ctx->close || !ctx->save) return -1;
        if (!SetTimer(hwnd, kWriteChecksDisplayTimerId, 250, nullptr)) return -1;
        Fill(*ctx); UpdateStatus(*ctx); Layout(hwnd, *ctx); return 0;
    }
    case WM_SIZE: Layout(hwnd, *ctx); return 0;
    case WM_DPICHANGED: { const auto* rc = reinterpret_cast<RECT*>(lp); SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE); Layout(hwnd, *ctx); return 0; }
    case WM_GETMINMAXINFO: { auto* info = reinterpret_cast<MINMAXINFO*>(lp); info->ptMinTrackSize = {Scale(hwnd, 840), Scale(hwnd, 570)}; return 0; }
    case WM_NOTIFY: if (reinterpret_cast<NMHDR*>(lp)->idFrom == kListId) { UpdateStatus(*ctx); if (reinterpret_cast<NMHDR*>(lp)->code == NM_DBLCLK) { const int i = Selected(*ctx); if (i >= 0 && !ctx->pending && !manualLimit.busy()) Detail(hwnd, *ctx, static_cast<size_t>(i)); } } return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == kRunId) Run(hwnd, *ctx);
        if (LOWORD(wp) == kDetailId) { const int i = Selected(*ctx); if (i >= 0 && !ctx->pending && !manualLimit.busy()) Detail(hwnd, *ctx, static_cast<size_t>(i)); }
        if (LOWORD(wp) == kRefreshId && !ctx->pending && !manualLimit.busy() && !ctx->unsaved) { Snapshot(*ctx); LoadRecords(*ctx); Fill(*ctx); UpdateStatus(*ctx); }
        if (LOWORD(wp) == kCloseId) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        if (LOWORD(wp) == kSaveId) SaveAgain(*ctx);
        return 0;
    case WM_TIMER:
        if (wp == kWriteChecksDisplayTimerId) {
            Complete(*ctx);
            if (ctx->closing && !ctx->pending && !manualLimit.busy()) { DestroyWindow(hwnd); return 0; }
            bool unsaved = false;
            const auto revision = MergeNormalObservations(ctx->store, ctx->records, unsaved, ctx->storageError);
            if (revision) {
                ctx->normalUnsaved = unsaved;
                if (revision != ctx->normalRevision) { ctx->normalRevision = revision; Fill(*ctx); }
            }
            UpdateStatus(*ctx);
        }
        return 0;
    case WM_CLOSE:
        if (ctx->pending || manualLimit.busy()) { ctx->closing = true; ctx->cancel.store(true); UpdateStatus(*ctx); return 0; }
        DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kWriteChecksDisplayTimerId);
        // Context joins only its own worker. Closing the main owner may bypass
        // WM_CLOSE, so cancellation and result persistence also happen here.
        ctx->cancel.store(true); if (ctx->worker.joinable()) { ctx->worker.join(); Complete(*ctx); manualLimit.Finish(GetTickCount64(), false); }
        window = nullptr; return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); if (!ctx->callerOwns) delete ctx; return DefWindowProcW(hwnd, message, wp, lp);
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
}
void ShowWriteChecksDialog(HWND owner) {
    if (window && IsWindow(window)) { ShowWindow(window, SW_SHOW); SetForegroundWindow(window); return; }
    auto ctx = std::make_unique<Context>(); ctx->owner = owner; Snapshot(*ctx); LoadRecords(*ctx);
    WNDCLASSW cls{}; cls.lpfnWndProc = WriteChecksWndProc; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = kClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { ShowSoftNotice(owner, T(L"checks.worker_failed"), SoftNoticeKind::Error); return; }
    window = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, T(L"checks.title").c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, Scale(owner, 880), Scale(owner, 620), owner, nullptr, cls.hInstance, ctx.get());
    if (!window) { ShowSoftNotice(owner, T(L"checks.worker_failed"), SoftNoticeKind::Error); return; }
    ctx->callerOwns = false; (void)ctx.release();
    ShowWindow(window, SW_SHOW); UpdateWindow(window);
}
bool HandleWriteChecksMessage(const MSG& message) {
    if (!window || !IsWindow(window) || !message.hwnd || GetAncestor(message.hwnd, GA_ROOT) != window) return false;
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { PostMessageW(window, WM_CLOSE, 0, 0); return true; }
    MSG copy = message;
    return IsDialogMessageW(window, &copy) != 0;
}
