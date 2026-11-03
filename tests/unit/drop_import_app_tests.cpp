// Link the actual application objects and exercise WM_DROPFILES on its UI thread.
#include "core/app_core.h"
#include "core/localization.h"
#include "workspace/workspace_actions.h"
#include "workspace/import_size.h"
#include <winioctl.h>
#include <array>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <shellapi.h>

extern "C" int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int);
void CollectOfficeFilesMissingPdfForOpen(const std::filesystem::path&, std::vector<std::filesystem::path>&);
// Thread timer ledger: readiness / expected failure / unexpected dialog / size decision.
static constexpr UINT_PTR kOperationTimer = 4917;
static constexpr UINT_PTR kFailureDialogTimer = 4918;
static constexpr UINT_PTR kUnexpectedDialogTimer = 4919;
static constexpr UINT_PTR kSizeDecisionTimer = 4920;
static UINT_PTR ownedTimer = 0;
static bool completed = false;
static bool verifyRestart = false;
static bool separateDirectories = false;
static std::filesystem::path root;
static int rejectedCases = 0;
static int failureDialogs = 0;
static std::wstring expectedFailureTitle;
enum class SizeDecision { StopButton, DefaultEnter, Escape, Close, Continue };
static bool expectingSize = false;
static int sizeDecisions = 0;
static std::uint64_t expectedSize = 0;
static SizeDecision sizeDecision = SizeDecision::StopButton;
static void Trace(const std::string& message) {
    std::ofstream log(root / L"probe.log", std::ios::binary | std::ios::app);
    log << message << '\n';
}
static void Write(const std::filesystem::path& path, std::string_view bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec); assert(!ec);
    std::ofstream file(path, std::ios::binary); file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); file.close(); assert(file);
}
static std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary); assert(file);
    return {std::istreambuf_iterator<char>(file), {}};
}
static std::string PdfData(const char* label) { return Read(root / L"fixture.pdf") + "\n% " + label + "\n"; }
static HWND OwnedWindow(const wchar_t* windowClass, const wchar_t* title) {
    HWND found = nullptr;
    while ((found = FindWindowExW(nullptr, found, windowClass, title)) != nullptr)
        if (GetWindowThreadProcessId(found, nullptr) == GetCurrentThreadId()) return found;
    return nullptr;
}
static void CALLBACK GuardUnexpectedDialog(HWND, UINT, UINT_PTR, DWORD) {
    if (!expectedFailureTitle.empty() || expectingSize) return;
    if (const HWND dialog = OwnedWindow(L"SilentDialogClass", nullptr)) {
        wchar_t title[256]{}; GetWindowTextW(dialog, title, 256);
        const HWND edit = FindWindowExW(dialog, nullptr, L"EDIT", nullptr);
        const int size = edit ? GetWindowTextLengthW(edit) : 0;
        std::wstring message(static_cast<size_t>(size) + 1, L'\0');
        if (edit) message.resize(static_cast<size_t>(GetWindowTextW(edit, message.data(), size + 1)));
        Trace("unexpected modal: " + WideToUTF8(title) + " " + WideToUTF8(message));
        std::cerr << "Unexpected modal: " << WideToUTF8(title) << " " << WideToUTF8(message) << '\n';
        assert(false);
    }
}
static HWND Notice() { return OwnedWindow(L"PdfWorkspaceSoftNotice", nullptr); }
static void Drop(HWND target, const std::vector<std::filesystem::path>& paths) {
    if (const HWND notice = Notice()) {
        SendMessageW(notice, WM_LBUTTONDOWN, 0, 0);
        SendMessageW(notice, WM_LBUTTONUP, 0, 0);
    }
    std::wstring names;
    for (const auto& path : paths) { names += path.wstring(); names += L'\0'; }
    names += L'\0';
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + names.size() * sizeof(wchar_t));
    assert(memory);
    auto* header = static_cast<DROPFILES*>(GlobalLock(memory)); assert(header);
    header->pFiles = sizeof(DROPFILES); header->fWide = TRUE;
    std::memcpy(reinterpret_cast<char*>(header) + sizeof(DROPFILES), names.data(), names.size() * sizeof(wchar_t));
    GlobalUnlock(memory);
    SendMessageW(target, WM_DROPFILES, reinterpret_cast<WPARAM>(memory), 0); // Receiver owns DragFinish.
    assert(IsWindowEnabled(g_hMainWnd));
}
static void Reject(HWND target, const std::vector<std::filesystem::path>& paths,
                   const std::filesystem::path& mustNotExist) {
    const auto lecture = g_currentLecturePath;
    const auto session = g_currentSessionPath;
    const auto note = g_currentNotePath;
    assert(!PathExistsWin32(mustNotExist));
    Drop(target, paths);
    assert(Notice() && IsWindowVisible(Notice()));
    assert(!PathExistsWin32(mustNotExist));
    assert(g_currentLecturePath == lecture && g_currentSessionPath == session && g_currentNotePath == note);
    ++rejectedCases;
}
static void CALLBACK CloseExpectedFailure(HWND, UINT, UINT_PTR, DWORD) {
    const HWND dialog = OwnedWindow(L"SilentDialogClass", expectedFailureTitle.c_str());
    if (!dialog) {
        if (const HWND other = OwnedWindow(L"SilentDialogClass", nullptr)) {
            wchar_t title[256]{}; GetWindowTextW(other, title, 256);
            Trace("unexpected dialog: " + WideToUTF8(title));
        }
    }
    if (!dialog || !IsWindowVisible(dialog) || GetWindow(dialog, GW_OWNER) != g_hMainWnd ||
        GetWindowThreadProcessId(dialog, nullptr) != GetCurrentThreadId()) return;
    ++failureDialogs;
    SendMessageW(dialog, WM_CLOSE, 0, 0);
}
static void DropWithExpectedFailure(const std::vector<std::filesystem::path>& paths) {
    Trace("before expected failure: " + WideToUTF8(paths.back().filename().wstring()));
    expectedFailureTitle = GetUiText().menuImportDirAsSession;
    const int before = failureDialogs;
    const UINT_PTR timer = SetTimer(nullptr, kFailureDialogTimer, 20, CloseExpectedFailure);
    assert(timer);
    Drop(g_hSessionList, paths);
    KillTimer(nullptr, timer);
    assert(failureDialogs == before + 1);
    Trace("after expected failure");
    expectedFailureTitle.clear();
}
static std::wstring WindowText(HWND window) {
    const int count = GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(count) + 1, L'\0');
    text.resize(static_cast<size_t>(GetWindowTextW(window, text.data(), count + 1)));
    return text;
}
static void CALLBACK ChooseSize(HWND, UINT, UINT_PTR, DWORD) {
    const auto title = localization::Text(L"workspace.import_size.title");
    const HWND dialog = OwnedWindow(L"SilentDialogClass", title.c_str());
    if (!dialog || !IsWindowVisible(dialog)) return;
    assert(GetWindow(dialog, GW_OWNER) == g_hMainWnd);
    const auto message = WindowText(FindWindowExW(dialog, nullptr, L"EDIT", nullptr));
    const wchar_t* tier = expectedSize >= 1000000000 ? L"workspace.import_size.over_1gb"
        : expectedSize >= 500000000 ? L"workspace.import_size.over_500mb" : L"workspace.import_size.over_100mb";
    assert(message.find(localization::Text(tier)) != std::wstring::npos);
    assert(message.find(std::to_wstring(expectedSize)) != std::wstring::npos);
    HWND stop = nullptr, proceed = nullptr, button = nullptr;
    while ((button = FindWindowExW(dialog, button, L"BUTTON", nullptr)) != nullptr) {
        if (WindowText(button) == localization::Text(L"workspace.import_size.stop")) stop = button;
        if (WindowText(button) == localization::Text(L"workspace.import_size.continue")) proceed = button;
    }
    assert(stop && proceed);
    assert((GetWindowLongPtrW(stop, GWL_STYLE) & BS_TYPEMASK) == BS_DEFPUSHBUTTON);
    assert((GetWindowLongPtrW(proceed, GWL_STYLE) & BS_TYPEMASK) != BS_DEFPUSHBUTTON);
    ++sizeDecisions;
    if (sizeDecision == SizeDecision::Close) SendMessageW(dialog, WM_CLOSE, 0, 0);
    else if (sizeDecision == SizeDecision::DefaultEnter) SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    else if (sizeDecision == SizeDecision::Escape) SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
    else {
        const HWND chosen = sizeDecision == SizeDecision::Continue ? proceed : stop;
        SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(chosen), BN_CLICKED), reinterpret_cast<LPARAM>(chosen));
    }
}
static void LargeDrop(HWND target, const std::vector<std::filesystem::path>& paths,
                      std::uint64_t bytes, SizeDecision choice) {
    expectedSize = bytes;
    sizeDecision = choice;
    expectingSize = true;
    const int before = sizeDecisions;
    const auto lecture = g_currentLecturePath, session = g_currentSessionPath, note = g_currentNotePath;
    const UINT_PTR timer = SetTimer(nullptr, kSizeDecisionTimer, 20, ChooseSize); assert(timer);
    Drop(target, paths);
    KillTimer(nullptr, timer);
    expectingSize = false;
    assert(sizeDecisions == before + 1); // One decision for the whole batch, never one per threshold/file.
    if (choice != SizeDecision::Continue)
        assert(g_currentLecturePath == lecture && g_currentSessionPath == session && g_currentNotePath == note);
}
static void SparseSource(const std::filesystem::path& path, std::uint64_t bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error); assert(!error);
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(handle != INVALID_HANDLE_VALUE);
    DWORD returned = 0;
    assert(DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr));
    LARGE_INTEGER length{}; length.QuadPart = static_cast<LONGLONG>(bytes);
    assert(SetFilePointerEx(handle, length, nullptr, FILE_BEGIN) && SetEndOfFile(handle));
    LARGE_INTEGER zero{};
    assert(SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN));
    assert(WriteFile(handle, "A", 1, &returned, nullptr) && returned == 1);
    length.QuadPart -= 1;
    assert(SetFilePointerEx(handle, length, nullptr, FILE_BEGIN));
    assert(WriteFile(handle, "Z", 1, &returned, nullptr) && returned == 1);
    assert(CloseHandle(handle));
}
static void CompareLargeCopy(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::ifstream left(source, std::ios::binary), right(target, std::ios::binary); assert(left && right);
    std::array<char, 65536> a{}, b{};
    do {
        left.read(a.data(), a.size()); right.read(b.data(), b.size());
        assert(left.gcount() == right.gcount());
        assert(std::memcmp(a.data(), b.data(), static_cast<size_t>(left.gcount())) == 0);
    } while (left.gcount());
    assert(left.eof() && right.eof());
}
static void CheckLargeImports(const std::filesystem::path& session) {
    const auto source = root / L"large_sources";
    const auto hundred = source / L"hundred.bin", fiveHundred = source / L"five-hundred.bin", gigabyte = source / L"gigabyte.bin";
    const auto folder = source / L"large-folder";
    SparseSource(hundred, 100000000); SparseSource(fiveHundred, 500000000); SparseSource(gigabyte, 1000000000);
    SparseSource(folder / L"first.bin", 60000000); SparseSource(folder / L"second.bin", 60000000);
    LargeDrop(g_hPdfList, {hundred}, 100000000, SizeDecision::DefaultEnter);
    assert(!PathExistsWin32(session / hundred.filename()));
    LargeDrop(g_hNoteList, {fiveHundred}, 500000000, SizeDecision::StopButton);
    assert(!PathExistsWin32(session / fiveHundred.filename()));
    LargeDrop(g_hPdfList, {gigabyte}, 1000000000, SizeDecision::Escape);
    assert(!PathExistsWin32(session / gigabyte.filename()));
    LargeDrop(g_hNoteList, {folder / L"first.bin", folder / L"second.bin"}, 120000000, SizeDecision::StopButton);
    assert(!PathExistsWin32(session / L"first.bin") && !PathExistsWin32(session / L"second.bin"));
    LargeDrop(g_hLectureList, {folder}, 120000000, SizeDecision::Close);
    assert(!PathExistsWin32(root / L"workspace/large-folder"));
    LargeDrop(g_hSessionList, {folder}, 120000000, SizeDecision::StopButton);
    assert(!PathExistsWin32(session.parent_path() / L"large-folder"));
    LargeDrop(g_hPdfList, {hundred}, 100000000, SizeDecision::Continue);
    workspace_import::ImportSize measured;
    assert(measured.AddFile(session / hundred.filename()) && measured.bytes == 100000000);
    CompareLargeCopy(hundred, session / hundred.filename());
    const int before = sizeDecisions;
    Drop(g_hNoteList, {hundred}); // Existing-name refusal precedes the size prompt.
    assert(sizeDecisions == before && Notice() && IsWindowVisible(Notice()));
    assert(!PathExistsWin32(session / L"hundred (2).bin"));
    assert(sizeDecisions == 7);
    Trace("large import: 7 decisions; all tiers, aggregate files/folders, cancellation and full 100 MB copy matched");
    std::cout << "Large import PASS: 7 decisions, 3 tiers, aggregate files/folders, default Stop, Enter/Esc/Close cancellation, complete 100 MB copy.\n";
}
static void SelectOriginalSession() {
    const auto lecture = root / L"workspace/lecture1";
    int lectureIndex = -1;
    for (size_t index = 0; index < g_lectures.size(); ++index)
        if (std::filesystem::path(g_lectures[index]) == lecture) lectureIndex = static_cast<int>(index);
    assert(lectureIndex >= 0);
    SendMessageW(g_hLectureList, LB_SETCURSEL, lectureIndex, 0);
    SendMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(0, LBN_SELCHANGE), reinterpret_cast<LPARAM>(g_hLectureList));
    int sessionIndex = -1;
    for (size_t index = 0; index < g_sessions.size(); ++index)
        if (std::filesystem::path(g_sessions[index].path) == lecture / L"session1") sessionIndex = static_cast<int>(index);
    assert(sessionIndex >= 0);
    SendMessageW(g_hSessionList, LB_SETCURSEL, sessionIndex, 0);
    SendMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(0, LBN_SELCHANGE), reinterpret_cast<LPARAM>(g_hSessionList));
    assert(std::filesystem::path(g_currentSessionPath) == lecture / L"session1");
}
static void CALLBACK Operate(HWND, UINT, UINT_PTR, DWORD) {
    if (completed || !g_hMainWnd || !g_hLectureList || !g_hSessionList || !g_hPdfList || !g_hNoteList) return;
    if (SendMessageW(g_hLectureList, LB_GETCOUNT, 0, 0) <= 0) return;
    KillTimer(nullptr, ownedTimer); ownedTimer = 0;
    const UINT_PTR guardTimer = SetTimer(nullptr, kUnexpectedDialogTimer, 50, GuardUnexpectedDialog); assert(guardTimer);
    SelectOriginalSession();
    const auto session = root / L"workspace/lecture1/session1";
    const auto pdfDir = separateDirectories ? session / L"pdf" : session;
    const auto noteDir = separateDirectories ? session / L"note" : session;
    const auto source = root / L"drop_sources";
    const auto incoming = source / L"new.txt";
    const auto office = source / L"pending.docx";
    const auto folder = source / L"new-session";
    const auto unicodeFolder = source / L"資料 第1回";
    const auto deepRelative = std::filesystem::path(std::wstring(90, L'a')) / std::wstring(90, L'b') / L"資料.txt";
    if (!verifyRestart) {
        for (HWND target : {g_hLectureList, g_hSessionList}) {
            Reject(target, {incoming}, root / L"workspace/new.txt");
            Reject(target, {folder, incoming}, root / L"workspace/lecture1/new-session");
        }
        for (HWND target : {g_hPdfList, g_hNoteList}) {
            Reject(target, {folder}, session / L"new-session");
            Reject(target, {incoming, folder}, noteDir / L"new.txt");
        }
        Reject(g_hMainWnd, {incoming}, noteDir / L"new.txt");
        Reject(g_hMainWnd, {folder}, session / L"new-session");
        CheckLargeImports(session);
        // A batch collision must not import even the first otherwise-valid file.
        Reject(g_hPdfList, {incoming, source / L"original.txt"}, noteDir / L"new.txt");
        Reject(g_hNoteList, {incoming, source / L"same-name/new.txt"}, noteDir / L"new.txt");
        Reject(g_hPdfList, {incoming, incoming}, noteDir / L"new.txt");
        assert(Read(noteDir / L"original.txt") == "original note");
        const auto savedSession = g_currentSessionPath;
        g_currentSessionPath.clear();
        Reject(g_hPdfList, {incoming}, noteDir / L"new.txt");
        g_currentSessionPath = savedSession;
        const auto savedLecture = g_currentLecturePath;
        g_currentLecturePath.clear();
        Reject(g_hSessionList, {folder}, root / L"workspace/new-session");
        g_currentLecturePath = savedLecture;

        if (separateDirectories) {
            Write(pdfDir, "destination parent is a file");
            Reject(g_hNoteList, {incoming, source / L"fresh.pdf"}, noteDir / L"new.txt");
            assert(Read(pdfDir) == "destination parent is a file");
            std::error_code cleanupError;
            assert(std::filesystem::remove(pdfDir, cleanupError) && !cleanupError);
        }
        Drop(g_hPdfList, {incoming, office});
        assert(Read(noteDir / L"new.txt") == "drop text");
        assert(Read(pdfDir / L"pending.docx") == "unconverted Office source");
        assert(!PathExistsWin32(pdfDir / L"pending.pdf"));
        Reject(g_hNoteList, {incoming, office}, noteDir / L"new (2).txt");
        Reject(g_hPdfList, {noteDir / L"new.txt"}, noteDir / L"new (3).txt");
        Reject(g_hPdfList, {source / L"NEW.TXT"}, noteDir / L"NEW (2).TXT");
        // Pair conflicts cannot attach new annotations to an existing PDF, or vice versa.
        Write(pdfDir / L"paired.pdf", PdfData("existing PDF"));
        Reject(g_hPdfList, {source / L"paired.clrop", source / L"paired.pdf"}, pdfDir / L"paired.clrop");
        Write(pdfDir / L"orphan.clrop", "existing annotations");
        Reject(g_hNoteList, {source / L"orphan.pdf"}, pdfDir / L"orphan.pdf");
        Drop(g_hNoteList, {source / L"fresh.clrop", source / L"fresh.pdf", source / L"second.txt"});
        assert(Read(pdfDir / L"fresh.pdf") == PdfData("new PDF"));
        assert(Read(pdfDir / L"fresh.clrop") == "new annotations");
        assert(Read(noteDir / L"second.txt") == "second note");
        assert(Read(pdfDir / L"paired.pdf") == PdfData("existing PDF"));
        assert(Read(pdfDir / L"orphan.clrop") == "existing annotations");

        // Folder conflicts, repeated inputs, tree overlap and wrong targets all preflight.
        Reject(g_hSessionList, {source / L"session1"}, session / L"keep.txt");
        Reject(g_hSessionList, {folder, source / L"same-name/new-session"}, session.parent_path() / L"new-session");
        Reject(g_hSessionList, {folder, folder}, session.parent_path() / L"new-session");
        Reject(g_hSessionList, {source / L"overlap", source / L"overlap/child"}, session.parent_path() / L"overlap");
        Reject(g_hLectureList, {root / L"workspace"}, root / L"workspace/workspace");
        Reject(g_hLectureList, {source / L"lecture1"}, root / L"workspace/lecture1/unwanted.txt");
        Reject(g_hLectureList, {source / L"z-lecture", source / L"same-name/z-lecture"}, root / L"workspace/z-lecture");
        Reject(g_hSessionList, {source / L"__pdf_note_workspace__"}, session.parent_path() / L"__pdf_note_workspace__");
        Reject(g_hPdfList, {source / L"missing.txt"}, noteDir / L"missing.txt");
        Reject(g_hNoteList, {std::filesystem::path(L"\\\\server\\share\\remote.pdf")}, pdfDir / L"remote.pdf");
        Reject(g_hNoteList, {std::filesystem::path(L"relative.txt")}, noteDir / L"relative.txt");
        Reject(g_hSessionList, {folder, source / L"ΔΟΚΙΜΗ", source / L"same-name/δοκιμη"}, session.parent_path() / L"new-session");
        // Preflight a later recursive plan before creating the first destination.
        DropWithExpectedFailure({source / L"preflight-good", source / L"preflight-bad"});
        assert(!PathExistsWin32(session.parent_path() / L"preflight-good"));
        assert(!PathExistsWin32(session.parent_path() / L"preflight-bad"));
        // A copy-time read refusal rolls back only the failed folder, reports
        // earlier completed imports and preserves source bytes for retry.
        const auto locked = source / L"read-failure/locked.txt";
        HANDLE readLock = CreateFileW(locked.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        assert(readLock != INVALID_HANDLE_VALUE);
        DropWithExpectedFailure({source / L"completed-before-failure", source / L"read-failure", source / L"after-failure"});
        assert(CloseHandle(readLock));
        assert(Read(session.parent_path() / L"completed-before-failure/retained.txt") == "completed");
        assert(!PathExistsWin32(session.parent_path() / L"read-failure"));
        assert(!PathExistsWin32(session.parent_path() / L"after-failure"));
        assert(Read(locked) == "locked original");
        Trace("before successful directories");
        SelectOriginalSession();
        Drop(g_hSessionList, {folder, unicodeFolder, source / L"new-session-next"});
        assert(Read(session.parent_path() / L"資料 第1回" / deepRelative) == "long Unicode folder note");
        assert(Read(session.parent_path() / L"new-session/inside.txt") == "folder note");
        assert(Read(session.parent_path() / L"new-session/subfolder/data.bin") == "nested data");
        assert(PathExistsWin32(session.parent_path() / L"new-session/empty"));
        assert(Read(session.parent_path() / L"new-session-next/next.txt") == "next note");
        assert(std::filesystem::path(g_currentSessionPath) == session.parent_path() / L"new-session-next");
        Drop(g_hLectureList, {source / L"z-lecture", source / L"z-other-lecture"});
        assert(Read(root / L"workspace/z-lecture/session-a/document.txt") == "lecture note");
        assert(Read(root / L"workspace/z-other-lecture/session-b/document.txt") == "other lecture note");
        assert(std::filesystem::path(g_currentLecturePath) == root / L"workspace/z-other-lecture");
        SelectOriginalSession();
    }
    workspace_import::ImportSize retainedSize;
    assert(retainedSize.AddFile(session / L"hundred.bin") && retainedSize.bytes == 100000000);
    assert(Read(noteDir / L"new.txt") == "drop text");
    assert(Read(noteDir / L"second.txt") == "second note");
    assert(Read(noteDir / L"original.txt") == "original note");
    assert(Read(pdfDir / L"pending.docx") == "unconverted Office source");
    assert(Read(root / L"workspace/lecture1/new-session/subfolder/data.bin") == "nested data");
    assert(Read(root / L"workspace/z-lecture/session-a/document.txt") == "lecture note");
    std::vector<std::filesystem::path> pending;
    CollectOfficeFilesMissingPdfForOpen(session, pending);
    assert(pending.size() == 1 && pending.front() == pdfDir / L"pending.docx");
    assert(Read(incoming) == "drop text" && Read(office) == "unconverted Office source");
    assert(Read(folder / L"inside.txt") == "folder note");
    assert(Read(root / L"workspace/lecture1/資料 第1回" / deepRelative) == "long Unicode folder note");
    KillTimer(nullptr, guardTimer);
    completed = true;
    PostMessageW(g_hMainWnd, WM_CLOSE, 0, 0);
}
int wmain(int argc, wchar_t** argv) {
    std::error_code ec;
    root = std::filesystem::current_path(ec); assert(!ec);
    for (int index = 1; index < argc; ++index) {
        if (std::wstring(argv[index]) == L"--verify-restart") verifyRestart = true;
        else if (std::wstring(argv[index]) == L"--separate-directories") separateDirectories = true;
        else assert(false);
    }
    if (!verifyRestart) {
        assert(!PathExistsWin32(root / L"workspace"));
        assert(std::filesystem::create_directories(root / L"workspace/lecture1/session1", ec) && !ec);
        const auto originalDir = separateDirectories ? root / L"workspace/lecture1/session1/note"
            : root / L"workspace/lecture1/session1";
        Write(originalDir / L"original.txt", "original note");
        Write(root / L"workspace/workspace.json", separateDirectories
            ? "{\"classesDir\":\".\",\"sessionFileLayout\":\"separate_directories\",\"sessionAutoOpenMode\":\"edit\"}"
            : "{\"classesDir\":\".\",\"sessionFileLayout\":\"session_root\",\"sessionAutoOpenMode\":\"edit\"}");
        Write(root / L"pdf_note_workspace_setup.json", "{\"workspaceRootMode\":\"relative\",\"workspaceRoot\":\"workspace\"}");
        const auto source = root / L"drop_sources";
        for (const auto& name : {L"new.txt", L"same-name/new.txt", L"NEW.TXT"}) Write(source / name, "drop text");
        Write(source / L"original.txt", "must not overwrite");
        Write(source / L"pending.docx", "unconverted Office source");
        Write(source / L"second.txt", "second note");
        for (const auto& name : {L"paired.pdf", L"fresh.pdf", L"orphan.pdf"}) Write(source / name, PdfData("new PDF"));
        for (const auto& name : {L"paired.clrop", L"fresh.clrop"}) Write(source / name, "new annotations");
        Write(source / L"new-session/inside.txt", "folder note");
        Write(source / L"new-session/subfolder/data.bin", "nested data");
        assert(std::filesystem::create_directory(source / L"new-session/empty", ec) && !ec);
        Write(source / L"new-session-next/next.txt", "next note");
        const auto deepRelative = std::filesystem::path(std::wstring(90, L'a')) / std::wstring(90, L'b') / L"資料.txt";
        Write(source / L"資料 第1回" / deepRelative, "long Unicode folder note");
        for (const auto& name : {L"same-name/new-session/duplicate.txt", L"session1/keep.txt", L"overlap/child/keep.txt",
                                L"lecture1/unwanted.txt", L"same-name/z-lecture/duplicate.txt", L"__pdf_note_workspace__/keep.txt",
                                L"preflight-good/good.txt", L"preflight-bad/__pdf_note_workspace__/invalid.txt",
                                L"ΔΟΚΙΜΗ/source.txt", L"same-name/δοκιμη/source.txt"}) Write(source / name, "must not import");
        Write(source / L"z-lecture/session-a/document.txt", "lecture note");
        Write(source / L"z-other-lecture/session-b/document.txt", "other lecture note");
        Write(source / L"completed-before-failure/retained.txt", "completed");
        Write(source / L"read-failure/locked.txt", "locked original");
        Write(source / L"after-failure/never.txt", "must not import");
    }
    ownedTimer = SetTimer(nullptr, kOperationTimer, 50, Operate); assert(ownedTimer);
    wchar_t command[] = L"";
    const int result = wWinMain(GetModuleHandleW(nullptr), nullptr, command, SW_HIDE);
    assert(completed && result == 0);
    std::cout << (verifyRestart ? "D&D restart PASS: files, directory hierarchy and pending Office originals retained.\n"
        : "D&D app PASS: 4 list targets, " + std::to_string(rejectedCases) + " notified rejections, whole-drop preflight, PDF pairing, directory copies, 2 observed failure dialogs and owned rollback.\n");
    return 0;
}
