// Real Win32 controls/worker/store; conversion and host UI are inert stubs.
#include "ui/dialogs/write_checks_dialog.h"
#include "core/app_core.h"
#include "core/localization.h"
#include "diagnostics/write_checks.h"
#include "diagnostics/normal_operations.h"
#include <commctrl.h>
#include <atomic>
#include <cassert>
#include <fstream>
#include <iostream>

std::wstring g_workspaceRoot;
std::wstring g_currentSessionPath;
std::wstring g_currentNotePath;
static std::wstring currentPdfPath;
const std::wstring& CurrentLogicalPdfPath() { return currentPdfPath; }
static bool flatLayout = false;
std::filesystem::path CurrentPdfDirectory() { return flatLayout ? std::filesystem::path(g_currentSessionPath) : std::filesystem::path(g_currentSessionPath) / L"pdf"; }
std::filesystem::path CurrentNoteDirectory() { return flatLayout ? std::filesystem::path(g_currentSessionPath) : std::filesystem::path(g_currentSessionPath) / L"note"; }
bool HasOfficeConversionFeature() { return false; }
bool RunOfficeConversionCheck(const std::filesystem::path&, std::atomic_bool&, std::filesystem::path&, DWORD&, write_checks::Step&) { return false; }
bool ValidateImportPdfFile(const std::filesystem::path&, std::wstring*) { return false; }
void ShowSoftNotice(HWND, const std::wstring&, SoftNoticeKind) { assert(false); }
static bool confirm = true;
static SilentDialogOptions lastDetails;
SilentDialogResult ShowSilentDialog(HWND, const SilentDialogOptions& options) {
    if (options.buttons == SilentDialogButtons::Ok) lastDetails = options;
    return options.buttons == SilentDialogButtons::YesNo && confirm ? SilentDialogResult::Yes : SilentDialogResult::No;
}
namespace localization {
std::wstring Text(std::wstring_view id) { return std::wstring(id); }
std::wstring Format(std::wstring_view id, const std::vector<std::pair<std::wstring_view, std::wstring>>& values) { std::wstring text(id); for (const auto& value : values) text += L" " + value.second; return text; }
}
static HWND Window() {
    HWND hwnd = nullptr;
    while ((hwnd = FindWindowExW(nullptr, hwnd, L"PdfNoteWriteChecksWnd", nullptr))) {
        DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) return hwnd;
    }
    return nullptr;
}
static void Pump() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (!HandleWriteChecksMessage(msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
}
static std::wstring Text(HWND hwnd) {
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(hwnd)) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size())); text.resize(text.size() - 1); return text;
}
static std::wstring Cell(HWND hwnd, int row, int column = 1) {
    wchar_t buffer[128]{}; ListView_GetItemText(GetDlgItem(hwnd, 4702), row, column, buffer, 128); return buffer;
}
static void Select(HWND hwnd, int row) {
    ListView_SetItemState(GetDlgItem(hwnd, 4702), -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(GetDlgItem(hwnd, 4702), row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    Pump();
}
static void Trial(HWND hwnd) {
    SendMessageW(hwnd, WM_COMMAND, 4703, 0);
    const auto start = GetTickCount64();
    // Run immediately disables Details; completion reenables it.
    while (!IsWindowEnabled(GetDlgItem(hwnd, 4704))) {
        assert(GetTickCount64() - start < 10000); Pump(); Sleep(5);
    }
}
int wmain() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES}; assert(InitCommonControlsEx(&controls));
    std::error_code ec;
    const auto root = std::filesystem::current_path(ec) / (L"ui_fixture_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    assert(!ec); assert(std::filesystem::create_directories(root / L"__pdf_note_workspace__" / L"__log__", ec) && !ec);
    const auto temp = root / L"conversion_temp";
    assert(std::filesystem::create_directory(temp, ec) && !ec);
    // Poison the host temporary path in this fixture process. The dialog must
    // select its workspace management folder without probing the host path.
    assert(SetEnvironmentVariableW(L"TMP", temp.c_str()));
    assert(SetEnvironmentVariableW(L"TEMP", temp.c_str()));
    g_workspaceRoot = root.wstring() + L"\\";
    const auto session = root / L"session";
    assert(std::filesystem::create_directory(session, ec) && !ec);
    g_currentSessionPath = session.wstring();
    const auto currentNote = session / L"current.txt";
    { std::ofstream out(currentNote, std::ios::binary); out << "unsaved edits must not be written by a check"; }
    g_currentNotePath = currentNote.wstring();
    ShowWriteChecksDialog(nullptr); HWND hwnd = Window(); assert(hwnd);
    assert(ListView_GetItemCount(GetDlgItem(hwnd, 4702)) == write_checks::kCount + 2);
    assert(Cell(hwnd, 12, 3) == L"__tmp__");
    assert(Cell(hwnd, 3, 3) == root.filename().wstring()); // trailing slash display regression
    assert(std::filesystem::is_empty(temp, ec) && !ec);
    for (int i = 0; i < static_cast<int>(write_checks::kCount + 2); ++i) assert(Cell(hwnd, i) == L"checks.no_record");
    // Visual order and keyboard traversal agree with the assets window:
    // Details, Refresh, task-specific actions, Close.
    const int buttons[] = {4704, 4705, 4703, 4707, 4706};
    for (size_t i = 1; i < 5; ++i) {
        RECT previous{}, current{};
        assert(GetWindowRect(GetDlgItem(hwnd, buttons[i - 1]), &previous));
        assert(GetWindowRect(GetDlgItem(hwnd, buttons[i]), &current));
        assert(previous.right < current.left);
        assert(GetWindow(GetDlgItem(hwnd, buttons[i - 1]), GW_HWNDNEXT) == GetDlgItem(hwnd, buttons[i]));
    }
    assert(!std::filesystem::exists(root / L"__pdf_note_workspace__" / L"__log__" / L"write_checks.log", ec));
    SendMessageW(hwnd, WM_COMMAND, 4704, 0);
    assert(lastDetails.message == L"dialog.details.status\nchecks.no_record\n\ndialog.details.paths");
    assert(lastDetails.additionalInformation == L"dialog.details.additional_information\nchecks.description.file");
    assert(lastDetails.paths.size() == 2 && lastDetails.paths.front().label == L"checks.target" &&
           lastDetails.paths.back().label == L"checks.record_path");
    // Direct session destinations are checked even when pdf/note children do
    // not exist. The folder is not created by the probe or layout switched.
    Select(hwnd, 10); Trial(hwnd); assert(Cell(hwnd, 10) == L"checks.success");
    SendMessageW(hwnd, WM_COMMAND, 4704, 0);
    assert(lastDetails.title == L"checks.session_pdf_write" && lastDetails.paths.front().value == session.wstring());
    Select(hwnd, 11); Trial(hwnd); assert(Cell(hwnd, 11) == L"checks.success");
    assert(Cell(hwnd, 8) == L"checks.no_record" && Cell(hwnd, 9) == L"checks.no_record");
    assert(!std::filesystem::exists(session / L"pdf", ec) && !ec);
    assert(!std::filesystem::exists(session / L"note", ec) && !ec);
    Select(hwnd, 8); Trial(hwnd); assert(Cell(hwnd, 8) == L"checks.check_failure");
    SendMessageW(hwnd, WM_COMMAND, 4704, 0);
    assert(lastDetails.additionalInformation.find(L"checks.reason.missing_directory") != std::wstring::npos);
    assert(lastDetails.additionalInformation.find(L"checks.description.destination") != std::wstring::npos);
    Select(hwnd, 9); Trial(hwnd); assert(Cell(hwnd, 9) == L"checks.check_failure");
    Select(hwnd, 14); Trial(hwnd); assert(Cell(hwnd, 14) == L"checks.success");
    SendMessageW(hwnd, WM_COMMAND, 4704, 0);
    assert(lastDetails.title == L"checks.note_read_write" && lastDetails.paths.front().value == currentNote.wstring());
    assert(lastDetails.additionalInformation.find(L"checks.description.note") != std::wstring::npos);
    assert(SetFileAttributesW(currentNote.c_str(), FILE_ATTRIBUTE_READONLY));
    Trial(hwnd); assert(Cell(hwnd, 14) == L"checks.failure");
    { std::ifstream in(currentNote, std::ios::binary); std::string actual;
      std::getline(in, actual); assert(actual == "unsaved edits must not be written by a check"); }
    g_currentNotePath.clear(); SendMessageW(hwnd, WM_TIMER, 4701, 0);
    assert(!IsWindowEnabled(GetDlgItem(hwnd, 4703))); // changed note target must be reloaded
    SendMessageW(hwnd, WM_COMMAND, 4705, 0);
    SendMessageW(hwnd, WM_COMMAND, 4704, 0);
    assert(lastDetails.additionalInformation.find(L"checks.reason.no_target") != std::wstring::npos);
    g_currentNotePath = currentNote.wstring(); SendMessageW(hwnd, WM_COMMAND, 4705, 0);
    currentPdfPath = (session / L"missing.pdf").wstring(); SendMessageW(hwnd, WM_COMMAND, 4705, 0);
    Select(hwnd, 13); Trial(hwnd); assert(Cell(hwnd, 13) == L"checks.check_failure");
    SendMessageW(hwnd, WM_COMMAND, 4704, 0);
    assert(lastDetails.additionalInformation.find(L"checks.reason.missing_file") != std::wstring::npos);
    // Missing folders are unavailable. The read-only note above supplies a
    // recoverable permission failure; first and changed results do not count.
    const auto settings = root / L"__pdf_note_workspace__" / L"__settings__";
    Select(hwnd, 4); Trial(hwnd); assert(Cell(hwnd, 4) == L"checks.check_failure");
    SendMessageW(hwnd, WM_COMMAND, 4704, 0);
    assert(lastDetails.message.find(L"checks.check_failure") != std::wstring::npos &&
           lastDetails.message.find(L"checks.description.write") == std::wstring::npos);
    assert(lastDetails.additionalInformation.find(L"checks.description.write") != std::wstring::npos &&
           lastDetails.additionalInformation.find(L"checks.time") != std::wstring::npos);
    Select(hwnd, 15); Trial(hwnd); assert(Cell(hwnd, 15) == L"checks.check_failure");
    Trial(hwnd); // unchanged unavailable checks also consume the shared limit
    Select(hwnd, 3); Trial(hwnd); assert(Cell(hwnd, 3) == L"checks.success");
    HANDLE locked = CreateFileW((root / L"__pdf_note_workspace__" / L"__log__" / L"write_checks.log").c_str(), GENERIC_READ, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    Trial(hwnd); assert(Cell(hwnd, 3) == L"checks.success");
    assert(IsWindowEnabled(GetDlgItem(hwnd, 4707))); // result remains visible and unsaved
    SendMessageW(hwnd, WM_COMMAND, 4704, 0);
    assert(lastDetails.additionalInformation.find(L"checks.not_saved") != std::wstring::npos &&
           lastDetails.additionalInformation.find(L"32:") != std::wstring::npos);
    assert(lastDetails.additionalInformation.find(L"checks.record_load_failed") == std::wstring::npos);
    CloseHandle(locked);
    SendMessageW(hwnd, WM_COMMAND, 4707, 0);
    const auto saveStart = GetTickCount64();
    while (!IsWindowEnabled(GetDlgItem(hwnd, 4704))) { assert(GetTickCount64() - saveStart < 10000); Pump(); Sleep(5); }
    assert(!IsWindowEnabled(GetDlgItem(hwnd, 4707))); // saving is retried independently of the trial limit
    for (int i = 0; i < 3; ++i) { assert(IsWindowEnabled(GetDlgItem(hwnd, 4703))); Trial(hwnd); }
    assert(!IsWindowEnabled(GetDlgItem(hwnd, 4703)));
    SendMessageW(hwnd, WM_CLOSE, 0, 0); Pump(); assert(!Window());
    ShowWriteChecksDialog(nullptr); hwnd = Window(); assert(hwnd);
    Select(hwnd, 3); assert(Cell(hwnd, 3) == L"checks.success"); assert(!IsWindowEnabled(GetDlgItem(hwnd, 4703))); // reopen cannot reset cooldown
    assert(std::filesystem::create_directory(settings, ec) && !ec);
    assert(SetFileAttributesW(currentNote.c_str(), FILE_ATTRIBUTE_NORMAL));
    Select(hwnd, 14); assert(IsWindowEnabled(GetDlgItem(hwnd, 4703)));
    assert(Text(GetDlgItem(hwnd, 4703)) == L"checks.improved_retry");
    confirm = false; SendMessageW(hwnd, WM_COMMAND, 4703, 0); assert(IsWindowEnabled(GetDlgItem(hwnd, 4703))); // declining does not consume exception
    confirm = true; Trial(hwnd); assert(Cell(hwnd, 14) == L"checks.success");
    assert(!IsWindowEnabled(GetDlgItem(hwnd, 4703)));
    Select(hwnd, 15); assert(!IsWindowEnabled(GetDlgItem(hwnd, 4703))); // unavailable is not eligible
    Select(hwnd, 2); assert(Cell(hwnd, 2) == L"checks.no_record" && IsWindowEnabled(GetDlgItem(hwnd, 4703)));
    Trial(hwnd); assert(Cell(hwnd, 2) == L"checks.success"); // second shared slot admits an untested target
    Select(hwnd, 5); assert(Cell(hwnd, 5) == L"checks.no_record" && !IsWindowEnabled(GetDlgItem(hwnd, 4703)));
    SendMessageW(hwnd, WM_COMMAND, 4703, 0); Pump(); assert(Cell(hwnd, 5) == L"checks.no_record"); // cannot bypass disabled button
    // Actual normal use remains allowed during cooldown; timer only merges
    // memory, and the source/actual file are visible in the existing details UI.
    write_checks::EnableNormalObservations();
    const auto config = root / L"workspace.json";
    std::wstring writeError;
    assert(write_checks::ObservedWriteUtf8(g_workspaceRoot, config, "normal", root, root, &writeError));
    SendMessageW(hwnd, WM_TIMER, 4701, 0);
    Select(hwnd, 3); assert(Cell(hwnd, 3) == L"checks.success" && !IsWindowEnabled(GetDlgItem(hwnd, 4703)));
    SendMessageW(hwnd, WM_COMMAND, 4704, 0);
    assert(lastDetails.additionalInformation.find(L"checks.source.normal") != std::wstring::npos);
    assert(lastDetails.additionalInformation.find(L"checks.description.write") == std::wstring::npos);
    assert(lastDetails.paths.size() == 3 && lastDetails.paths[1].value == config.wstring());
    HANDLE ledgerLock = CreateFileW((root / L"__pdf_note_workspace__" / L"__log__" / L"write_checks.log").c_str(), GENERIC_READ, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(ledgerLock != INVALID_HANDLE_VALUE);
    assert(SetFileAttributesW(config.c_str(), FILE_ATTRIBUTE_READONLY));
    assert(!write_checks::ObservedWriteUtf8(g_workspaceRoot, config, "rejected", root, root, &writeError));
    SendMessageW(hwnd, WM_TIMER, 4701, 0);
    assert(Cell(hwnd, 3) == L"checks.failure" && IsWindowEnabled(GetDlgItem(hwnd, 4707)));
    assert(!IsWindowEnabled(GetDlgItem(hwnd, 4703))); // Normal failure grants no extra retry slot.
    CloseHandle(ledgerLock);
    SendMessageW(hwnd, WM_COMMAND, 4707, 0);
    const auto normalSaveStart = GetTickCount64();
    while (!IsWindowEnabled(GetDlgItem(hwnd, 4704))) { assert(GetTickCount64() - normalSaveStart < 10000); Pump(); Sleep(5); }
    SendMessageW(hwnd, WM_TIMER, 4701, 0);
    assert(!IsWindowEnabled(GetDlgItem(hwnd, 4707)));
    assert(SetFileAttributesW(config.c_str(), FILE_ATTRIBUTE_NORMAL));
    assert(write_checks::ObservedWriteUtf8(g_workspaceRoot, config, "recovered", root, root, &writeError));
    SendMessageW(hwnd, WM_TIMER, 4701, 0);
    assert(Cell(hwnd, 3) == L"checks.success" && !IsWindowEnabled(GetDlgItem(hwnd, 4703)));
    const auto directNote = session / L"direct.txt";
    assert(write_checks::ObservedWriteUtf8(g_workspaceRoot, directNote, "original", session, session, &writeError));
    assert(SetFileAttributesW(directNote.c_str(), FILE_ATTRIBUTE_READONLY));
    assert(!write_checks::ObservedWriteUtf8(g_workspaceRoot, directNote, "rejected", session, session, &writeError));
    { std::ifstream original(directNote, std::ios::binary); std::string contents;
      std::getline(original, contents); assert(contents == "original"); }
    SendMessageW(hwnd, WM_TIMER, 4701, 0);
    assert(Cell(hwnd, 11) == L"checks.failure" && Cell(hwnd, 9) == L"checks.check_failure");
    Select(hwnd, 11); SendMessageW(hwnd, WM_COMMAND, 4704, 0);
    assert(lastDetails.additionalInformation.find(L"checks.source.normal") != std::wstring::npos);
    assert(lastDetails.paths[1].value == directNote.wstring());
    assert(SetFileAttributesW(directNote.c_str(), FILE_ATTRIBUTE_NORMAL));
    assert(write_checks::ObservedWriteUtf8(g_workspaceRoot, directNote, "recovered", session, session, &writeError));
    SendMessageW(hwnd, WM_TIMER, 4701, 0);
    assert(Cell(hwnd, 11) == L"checks.success");
    flatLayout = true; SendMessageW(hwnd, WM_COMMAND, 4705, 0);
    assert(Cell(hwnd, 8) == Cell(hwnd, 10) && Cell(hwnd, 9) == Cell(hwnd, 11));
    assert(Cell(hwnd, 8) == L"checks.success" && Cell(hwnd, 9) == L"checks.success");
    SendMessageW(hwnd, WM_CLOSE, 0, 0); Pump();
    std::vector<write_checks::Record> records; std::string baseline; DWORD error = 0;
    assert(write_checks::Load(root / L"__pdf_note_workspace__" / L"__log__" / L"write_checks.log", records, baseline, error));
    assert(records.size() == 10 && write_checks::Find(records, write_checks::Kind::NoteReadWrite, currentNote.wstring())->result.outcome == write_checks::Outcome::Passed);
    assert(write_checks::Find(records, write_checks::Kind::PdfWrite, session.wstring()));
    assert(write_checks::Find(records, write_checks::Kind::NoteWrite, session.wstring())->result.source == write_checks::Source::Normal);
    const auto ledger = root / L"__pdf_note_workspace__" / L"__log__" / L"write_checks.log";
    // Corrupt only the disposable ledger; inspection must not overwrite it or
    // treat a failed read as no previous check. The real operation result stays separate.
    { std::ofstream out(ledger, std::ios::binary | std::ios::trunc); out << "broken fixture ledger"; }
    ShowWriteChecksDialog(nullptr); hwnd = Window(); assert(hwnd);
    Select(hwnd, 0); assert(Cell(hwnd, 0) == L"checks.check_failure");
    SendMessageW(hwnd, WM_COMMAND, 4704, 0);
    assert(lastDetails.additionalInformation.find(L"checks.record_load_failed") != std::wstring::npos &&
           lastDetails.additionalInformation.find(L"13:") != std::wstring::npos);
    assert(lastDetails.additionalInformation.find(L"checks.not_saved") == std::wstring::npos);
    assert(lastDetails.paths.back().value == ledger.wstring());
    { std::ifstream in(ledger, std::ios::binary); std::string bytes; std::getline(in, bytes); assert(bytes == "broken fixture ledger"); }
    SendMessageW(hwnd, WM_CLOSE, 0, 0); Pump(); assert(!Window());
    assert(DeleteFileW((root / L"__pdf_note_workspace__" / L"__log__" / L"write_checks.log").c_str()));
    assert(DeleteFileW(config.c_str()));
    assert(DeleteFileW(currentNote.c_str()));
    assert(DeleteFileW(directNote.c_str()));
    std::filesystem::directory_iterator sessionEntries(session, ec), end;
    assert(!ec);
    while (sessionEntries != end) {
        assert(sessionEntries->path().filename().wstring().rfind(L"direct.txt.__atomic__.", 0) == 0);
        assert(DeleteFileW(sessionEntries->path().c_str()));
        sessionEntries.increment(ec); assert(!ec);
    }
    assert(RemoveDirectoryW(session.c_str()));
    assert(RemoveDirectoryW((root / L"__pdf_note_workspace__" / L"__settings__").c_str()));
    assert(RemoveDirectoryW((root / L"__pdf_note_workspace__" / L"__log__").c_str()));
    assert(RemoveDirectoryW((root / L"__pdf_note_workspace__").c_str())); assert(RemoveDirectoryW(temp.c_str()));
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        assert(entry.path().filename().wstring().rfind(L"workspace.json.__atomic__.", 0) == 0);
        assert(DeleteFileW(entry.path().c_str()));
    }
    assert(RemoveDirectoryW(root.c_str()));
    std::cout << "write checks real controls, stored results and recovery retry passed\n";
    return 0;
}
