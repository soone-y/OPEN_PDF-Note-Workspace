// Real button -> actual bundled viewer, including missing-asset rejection.
// Own a disposable main app and all its descendants; never use user processes.
#include "app_test_driver.h"
#include "core/command_ids.h"
#include "core/json_string.h"
#include <tlhelp32.h>

struct OwnedJob {
    HANDLE handle = CreateJobObjectW(nullptr, nullptr);
    OwnedJob() {
        Require(handle != nullptr, "create owned guide test job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(handle); handle = nullptr;
            throw std::runtime_error("configure guide test watchdog job");
        }
    }
    ~OwnedJob() { if (handle) CloseHandle(handle); }
};
struct OwnedProcess {
    PROCESS_INFORMATION info{};
    ~OwnedProcess() {
        if (info.hProcess && WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) {
            TerminateProcess(info.hProcess, 79);
            WaitForSingleObject(info.hProcess, 5000);
        }
        if (info.hThread) CloseHandle(info.hThread);
        if (info.hProcess) CloseHandle(info.hProcess);
    }
};
static void Write(const fs::path& path, const std::string& text) {
    std::wstring error;
    Require(atomic_write::AtomicWriteUtf8(path, text, path.parent_path(), &error), "write isolated guide fixture");
}
static std::string Read(const fs::path& path) {
    std::string text;
    Require(ReadFileBytesWin32(path, text), "read guide fixture evidence");
    return text;
}
static std::vector<DWORD> ViewerChildren() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    Require(snapshot != INVALID_HANDLE_VALUE, "observe exact main child descendants");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    std::vector<DWORD> children;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (entry.th32ParentProcessID == childPid && _wcsicmp(entry.szExeFile, L"readonly_viewer.exe") == 0)
            children.push_back(entry.th32ProcessID);
    }
    const DWORD error = GetLastError();
    CloseHandle(snapshot);
    Require(error == ERROR_NO_MORE_FILES, "process ownership enumeration completed");
    return children;
}
static HWND ViewerWindow(DWORD pid) {
    HWND window = nullptr;
    while ((window = FindWindowExW(nullptr, window, L"PdfReadonlyViewerWindow", nullptr))) {
        DWORD actual = 0; GetWindowThreadProcessId(window, &actual);
        if (actual == pid) return window;
    }
    return nullptr;
}
static HWND InputField(HWND main, HWND guide) {
    RECT guideBounds{};
    Require(GetWindowRect(guide, &guideBounds), "guide bounds observable");
    HWND field = nullptr;
    while ((field = FindWindowExW(main, field, L"EDIT", nullptr))) {
        const auto style = GetWindowLongPtrW(field, GWL_STYLE);
        RECT bounds{};
        if ((style & ES_AUTOHSCROLL) && !(style & (ES_NUMBER | ES_CENTER | ES_MULTILINE)) &&
            GetWindowRect(field, &bounds) && bounds.top > guideBounds.bottom &&
            bounds.top - guideBounds.bottom <= 16 && bounds.left >= guideBounds.left && bounds.right <= guideBounds.right)
            return field;
    }
    return nullptr;
}
static HWND OpenFixtureNote(HWND main) {
    HWND lists[4]{};
    for (int i = 0; i < 4; ++i) {
        lists[i] = FindWindowExW(main, i ? lists[i - 1] : nullptr, L"LISTBOX", nullptr);
        Require(lists[i] != nullptr, "owned workspace navigation controls exist");
    }
    for (const int index : {0, 1, 3}) {
        Require(Until([&] { return Send(lists[index], LB_GETCOUNT) == 1; }), "single fixture navigation row");
        Require(Send(lists[index], LB_SETCURSEL, 0) != static_cast<DWORD_PTR>(LB_ERR), "select fixture row");
        Send(main, WM_COMMAND, MAKEWPARAM(0, LBN_SELCHANGE), reinterpret_cast<LPARAM>(lists[index]));
    }
    HWND editor = nullptr;
    Require(Until([&] {
        editor = Descendant(main, L"RICHEDIT50W");
        return editor && WindowText(editor).find(L"Guide regression fixture") != std::wstring::npos;
    }), "fixture note is actually open");
    return editor;
}
int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 3, "explicit owned root and locale required");
        const fs::path root(argv[1]);
        const bool english = wcscmp(argv[2], L"en") == 0;
        Require(english || wcscmp(argv[2], L"ja") == 0, "explicit supported locale");
        Require(root.is_absolute() && fs::is_directory(root / L"app"), "owned runner fixture exists");
        const fs::path exe = root / L"app" / L"pdf_note_workspace.exe";
        Require(Read(root / L"owned_fixture.txt") == "note_writing_guide_fixture_v1\n", "explicit runner-owned fixture marker");
        Require(!fs::exists(root / L"workspace") &&
            !fs::exists(exe.parent_path() / L"pdf_note_workspace_setup.json") &&
            !fs::exists(exe.parent_path() / L"readonly_setup.json"), "refuse existing workspace or user preferences");
        const fs::path guide = exe.parent_path() / L"docs" / L"Note_Writing_Guide.md";
        const fs::path viewer = exe.parent_path() / L"readonly_viewer.exe";
        const auto guideBytes = Read(guide);
        const fs::path workspace = root / L"workspace";
        const fs::path note = workspace / L"lecture" / L"session" / L"fixture.clro";
        fs::create_directories(note.parent_path());
        const std::string noteBytes = "Guide regression fixture\r\nUnchanged note body.\r\n";
        Write(note, noteBytes);
        Write(workspace / L"workspace.json", "{\"classesDir\":\".\",\"startupSelectFirstSession\":false,\"noteRenderEnabled\":false,\"noteRawOnly\":true}");
        Write(exe.parent_path() / L"pdf_note_workspace_setup.json",
            "{\"workspaceRootMode\":\"absolute\",\"workspaceRoot\":\"" + json_string::Escape(workspace.u8string()) + "\"}");
        // The test never enables the application's internal automation mode.
        // A suspended owned process joins the kill-on-close job before running.
        Require(SetEnvironmentVariableW(L"PDF_NOTE_SMALL_UI_AUTOMATION", nullptr) &&
            SetEnvironmentVariableW(L"PDF_NOTE_SMALL_AUTOMATION_WORKSPACE_ROOT", nullptr) &&
            SetEnvironmentVariableW(L"PDF_NOTE_SMALL_ENABLE_FAULT_INJECTION", nullptr), "normal owned app startup");
        const std::wstring suffix = L"_guide_" + std::to_wstring(GetCurrentProcessId());
        Require(SetEnvironmentVariableW(L"PDF_NOTE_SMALL_INSTANCE_SUFFIX", suffix.c_str()), "isolated main instance");
        OwnedJob job;
        OwnedProcess mainProcess;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        std::wstring command = L"\"" + exe.wstring() + L"\"";
        Require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
            nullptr, exe.parent_path().c_str(), &startup, &mainProcess.info), "start exact owned app copy");
        Require(AssignProcessToJobObject(job.handle, mainProcess.info.hProcess) &&
            ResumeThread(mainProcess.info.hThread) != static_cast<DWORD>(-1), "assign owned process before startup");
        childPid = mainProcess.info.dwProcessId;
        const HWND main = WaitWindow(L"PdfWorkspaceMainWnd");
        Require(Until([&] { return IsWindowEnabled(main) && IsWindowVisible(main); }), "app ready");
        const HWND button = GetDlgItem(main, ID_NOTE_ASSIST_GUIDE);
        Require(button && IsWindowVisible(button) && IsWindowEnabled(button), "guide accessible without open note");
        const HWND field = InputField(main, button);
        Require(field != nullptr, "observe the actual assist input field immediately below its guide");
        Require(WindowText(button) == (english ? L"Writing guide" : L"書き方ガイド"), "guide button uses requested locale");
        const HWND editor = OpenFixtureNote(main);
        Type(field, L"guide input retained");
        Send(editor, EM_SETSEL, 2, 8);
        const auto originalText = WindowText(editor);
        const auto originalSelection = Send(editor, EM_GETSEL);
        const auto originalUndo = Send(editor, EM_CANUNDO);
        const auto preserved = [&] {
            return WindowText(field) == L"guide input retained" && WindowText(editor) == originalText &&
                Send(editor, EM_GETSEL) == originalSelection && Send(editor, EM_CANUNDO) == originalUndo &&
                Read(note) == noteBytes && Read(guide) == guideBytes;
        };
        Send(button, BM_CLICK);
        Require(Until([&] { return ViewerChildren().size() == 1; }), "one actual viewer child launched");
        const DWORD viewerPid = ViewerChildren().front();
        OwnedProcess viewerProcess;
        viewerProcess.info.hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE, FALSE, viewerPid);
        Require(viewerProcess.info.hProcess != nullptr, "owned viewer observation handle");
        HWND reader = nullptr;
        Require(Until([&] {
            reader = ViewerWindow(viewerPid);
            return reader && IsWindowVisible(reader) &&
                WindowText(GetDlgItem(reader, 104)).find(english ? L"Note writing guide" : L"ノートの書き方ガイド") != std::wstring::npos;
        }), "actual locale guide opened and displayed");
        Require(WindowText(reader).find(L"Note_Writing_Guide.md") != std::wstring::npos,
            "viewer opened the exact Markdown document instead of an empty folder");
        Require(preserved(), "successful guide launch retains input note selection undo and original bytes");
        Require(PostMessageW(reader, WM_CLOSE, 0, 0) &&
            WaitForSingleObject(viewerProcess.info.hProcess, 10000) == WAIT_OBJECT_0, "viewer joined after normal close");
        DWORD status = 0;
        Require(GetExitCodeProcess(viewerProcess.info.hProcess, &status) && status == 0, "viewer normal exit");
        std::cout << "[PASS] guide button opens actual locale Markdown and preserves input/note/selection/history\n";

        const auto reject = [&](const fs::path& path) {
            if (const HWND prior = Window(L"PdfWorkspaceSoftNotice"); prior && IsWindowVisible(prior)) {
                Send(prior, WM_LBUTTONDOWN, 0, 0); Send(prior, WM_LBUTTONUP, 0, 0);
                Require(!IsWindowVisible(prior), "prior notice dismissed before failure observation");
            }
            const fs::path hidden = path.wstring() + L".owned_missing_fixture";
            fs::rename(path, hidden);
            Send(button, BM_CLICK);
            HWND notice = nullptr;
            Require(Until([&] {
                notice = Window(L"PdfWorkspaceSoftNotice");
                return notice && IsWindowVisible(notice);
            }), "missing asset produces a fresh owned visual notice");
            Require(ViewerChildren().empty(), "missing asset never launches an empty viewer");
            fs::rename(hidden, path);
            Require(preserved(), "missing asset preserves input selection note history and original bytes");
        };
        reject(guide);
        reject(viewer);
        std::cout << "[PASS] missing guide and missing viewer reject without data loss\n";
        Require(PostMessageW(main, WM_CLOSE, 0, 0) &&
            WaitForSingleObject(mainProcess.info.hProcess, 10000) == WAIT_OBJECT_0, "main app joined after normal close");
        Require(GetExitCodeProcess(mainProcess.info.hProcess, &status) && status == 0, "main normal exit");
        Require(Read(note) == noteBytes && Read(guide) == guideBytes, "original files unchanged after shutdown");
        std::cout << "[PASS] owned shutdown and original bytes unchanged; checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << " checks=" << checks << '\n';
        return 1;
    }
}
