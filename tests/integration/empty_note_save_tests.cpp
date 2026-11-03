// Real application regression: only new, owned fixtures under out/tests.
#include <windows.h>
#include <commctrl.h>
#include "core/atomic_write.h"
#include "core/command_ids.h"
#include <array>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
static int checks = 0;
static DWORD childPid = 0;
static void Require(bool ok, const char* label) { ++checks; if (!ok) throw std::runtime_error(label); }
template<class Predicate> static bool Until(Predicate predicate, DWORD timeout = 15000) {
    const auto end = GetTickCount64() + timeout;
    do { if (predicate()) return true; Sleep(20); } while (GetTickCount64() < end);
    return false;
}
static HWND Window(const wchar_t* name) {
    HWND window = nullptr;
    while ((window = FindWindowExW(nullptr, window, name, nullptr))) {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        if (pid == childPid) return window;
    }
    return nullptr;
}
static bool DialogReady() {
    const HWND dialog = Window(L"SilentDialogClass");
    // CreateWindow publishes its top-level HWND before creating the message
    // control. Observe the completed dialog, not that intermediate lifetime.
    const HWND message = dialog ? GetDlgItem(dialog, 5101) : nullptr;
    const HWND action = dialog ? GetDlgItem(dialog, 5102) : nullptr;
    return dialog && IsWindowVisible(dialog) && message && IsWindowVisible(message) &&
        action && IsWindowEnabled(action);
}
static DWORD_PTR Send(HWND window, UINT message, WPARAM wp = 0, LPARAM lp = 0) {
    DWORD_PTR result = 0;
    const bool ok = SendMessageTimeoutW(window, message, wp, lp, SMTO_ABORTIFHUNG, 5000, &result) != 0;
    if (!ok) {
        std::cerr << "message timeout: message=" << message << " wp=" << wp << " checks=" << checks << '\n';
        if (HWND dialog = Window(L"SilentDialogClass")) {
            wchar_t body[8192]{}; DWORD_PTR copied = 0;
            SendMessageTimeoutW(GetDlgItem(dialog, 5101), WM_GETTEXT, 8192, reinterpret_cast<LPARAM>(body),
                SMTO_ABORTIFHUNG, 1000, &copied);
            const std::wstring wide(body);
            const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            std::string bytes(length, '\0');
            if (length) WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), bytes.data(), length, nullptr, nullptr);
            std::cerr << bytes << '\n';
        }
    }
    Require(ok, "bounded fixture message");
    return result;
}
static bool Equals(const fs::path& path, const std::string& expected) {
    std::string bytes; return ReadFileBytesWin32(path, bytes) && bytes == expected;
}
static void Write(const fs::path& path, const std::string& bytes) {
    std::wstring error;
    Require(atomic_write::AtomicWriteUtf8(path, bytes, path.parent_path(), &error), "write owned fixture");
}
static HWND OpenNote(HWND main, bool restoreStage = false, bool deferred = false) {
    HWND lists[4]{};
    for (int i = 0; i < 4; ++i) lists[i] = FindWindowExW(main, i ? lists[i - 1] : nullptr, L"LISTBOX", nullptr);
    for (int i : {0, 1, 3}) {
        if (deferred && i == 3) {
            Require(lists[i] && Send(lists[i], LB_GETCOUNT) == 0, "no note has been created");
            continue;
        }
        Require(lists[i] && Send(lists[i], LB_GETCOUNT) == 1, "one owned navigation item");
        if (restoreStage && i == 3) continue; // Session auto-open already loaded the recovered note.
        Send(lists[i], LB_SETCURSEL, 0);
        if (restoreStage && i == 1) {
            // The real recovery confirmation runs a modal loop. Post native
            // selection, inspect that specific question, and choose restore.
            Require(PostMessageW(main, WM_COMMAND, MAKEWPARAM(0, LBN_SELCHANGE), reinterpret_cast<LPARAM>(lists[i])),
                "select session containing owned recovery data");
            Require(Until(DialogReady), "recovery confirmation offered");
            const HWND dialog = Window(L"SilentDialogClass");
            wchar_t body[8192]{};
            Send(GetDlgItem(dialog, 5101), WM_GETTEXT, 8192, reinterpret_cast<LPARAM>(body));
            Require(std::wstring(body).find(L"未統合の差分") != std::wstring::npos &&
                std::wstring(body).find(L"復元") != std::wstring::npos, "confirm only the owned stage restoration");
            Require(PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(5102, BN_CLICKED), 0), "approve owned empty-stage restoration");
            Require(Until([&] { return !Window(L"SilentDialogClass") && IsWindowEnabled(main); }), "stage recovery confirmation completed");
        } else {
            Send(main, WM_COMMAND, MAKEWPARAM(0, LBN_SELCHANGE), reinterpret_cast<LPARAM>(lists[i]));
        }
    }
    HWND editor = FindWindowExW(main, nullptr, L"RICHEDIT50W", nullptr);
    if (!editor) editor = FindWindowExW(main, nullptr, L"RichEdit20W", nullptr);
    if (!editor) editor = FindWindowExW(main, nullptr, L"EDIT", nullptr);
    Require(editor != nullptr, "real note editor");
    return editor;
}
int main() {
    std::cout << std::unitbuf;
    PROCESS_INFORMATION process{};
    fs::path protectedNote;
    try {
        wchar_t buffer[32768]{};
        const DWORD size = GetEnvironmentVariableW(L"EMPTY_NOTE_APP_TEST_ROOT", buffer, 32768);
        Require(size && size < 32768, "owned test root provided");
        const fs::path root(std::wstring(buffer, size));
        const auto prefix = (fs::current_path() / L"out" / L"tests").wstring() + L"\\";
        Require(root.wstring().rfind(prefix, 0) == 0, "fixture restricted to out/tests");
        const auto executable = root / L"app" / (L"pdf_note_empty_" + root.filename().wstring() + L".exe");
        Require(SetEnvironmentVariableW(L"PDF_NOTE_SMALL_UI_AUTOMATION", nullptr), "normal app UI mode");
        Require(SetEnvironmentVariableW(L"PDF_NOTE_SMALL_AUTOMATION_WORKSPACE_ROOT", nullptr), "use owned setup");
        const std::array<const wchar_t*, 4> extensions{L".clro", L".md", L".txt", L".csv"};
        for (int scenario = 0; scenario < 9; ++scenario) {
            const auto workspace = root / (L"case_" + std::to_wstring(scenario));
            const auto note = workspace / L"01_lecture" / L"01_session" / L"empty.clro";
            const std::string bom = scenario == 2 ? "\xEF\xBB\xBF" : "";
            const std::string original = bom + (scenario ? "original note" : "");
            if (scenario >= 5) fs::create_directories(note.parent_path());
            else Write(note, original);
            const std::wstring extension = scenario >= 5 ? extensions[scenario - 5] : L".clro";
            Write(workspace / L"workspace.json", "{\"classesDir\":\".\",\"startupSelectFirstSession\":false,\"newNoteExtension\":\"" +
                fs::path(extension).u8string() + "\"}");
            Write(executable.parent_path() / L"pdf_note_workspace_setup.json",
                "{\"workspaceRootMode\":\"absolute\",\"workspaceRoot\":\"" + workspace.generic_u8string() + "\"}");
            const auto suffix = L"_empty_note_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(scenario);
            Require(SetEnvironmentVariableW(L"PDF_NOTE_SMALL_INSTANCE_SUFFIX", suffix.c_str()), "isolated app identity");
            const auto launch = [&]() {
                std::wstring command = L"\"" + executable.wstring() + L"\"";
                STARTUPINFOW startup{}; startup.cb = sizeof(startup);
                Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                    executable.parent_path().c_str(), &startup, &process), "launch owned actual application");
                childPid = process.dwProcessId;
                Require(Until([] { const HWND main = Window(L"PdfWorkspaceMainWnd");
                    return main && IsWindowVisible(main) && IsWindowEnabled(main); }), "actual app ready");
                return Window(L"PdfWorkspaceMainWnd");
            };
            HWND main = launch(), editor = OpenNote(main, false, scenario >= 5);
            if (scenario >= 5) {
                Require(Send(editor, WM_GETTEXTLENGTH) == 0, "blank uncreated note");
                for (wchar_t ch : std::wstring(L"new note")) Send(editor, WM_CHAR, ch);
                Require(Send(editor, WM_GETTEXTLENGTH) == 8, "first input creates a usable draft binding");
                // Focus loss stages the draft. A second edit must update that
                // stage with the same ID used by its eventual destination.
                Send(editor, WM_KILLFOCUS, reinterpret_cast<WPARAM>(main));
                Send(editor, WM_CHAR, L'!');
                Require(PostMessageW(main, WM_COMMAND, ID_FILE_SAVE_ALL, 0), "Ctrl+S route creates and saves draft");
                fs::path saved;
                Require(Until([&] {
                    for (const auto& file : fs::recursive_directory_iterator(note.parent_path())) {
                        if (file.path().extension() == extension && Equals(file.path(), "new note!")) {
                            saved = file.path(); return true;
                        }
                    }
                    return false;
                }), "draft stage integrates without an identity mismatch");
                Require(Until([&] {
                    if (Send(editor, WM_GETTEXTLENGTH) == 9) Send(editor, WM_CHAR, L'?');
                    return Send(editor, WM_GETTEXTLENGTH) == 10;
                }), "editing resumes after background save completion");
                Send(editor, WM_KILLFOCUS, reinterpret_cast<WPARAM>(main));
                Send(editor, WM_CHAR, L'#');
                Require(PostMessageW(main, WM_COMMAND, ID_FILE_SAVE_ALL, 0), "repeat draft-origin save");
                Require(Until([&] { return Equals(saved, "new note!?#"); }), "subsequent stage update retains its durable ID");
                Require(PostMessageW(main, WM_CLOSE, 0, 0), "normal exit after draft creation");
                Require(WaitForSingleObject(process.hProcess, 15000) == WAIT_OBJECT_0, "draft save process exits");
                CloseHandle(process.hThread); CloseHandle(process.hProcess); process = {};
                // Reopen through normal navigation, using the saved catalog.
                main = launch(); editor = OpenNote(main);
                Require(Send(editor, WM_GETTEXTLENGTH) == 11 && Equals(saved, "new note!?#"), "created draft reloads after restart");
                Require(PostMessageW(main, WM_CLOSE, 0, 0), "close reloaded draft");
                Require(WaitForSingleObject(process.hProcess, 15000) == WAIT_OBJECT_0, "reloaded draft exits");
                CloseHandle(process.hThread); CloseHandle(process.hProcess); process = {};
                std::cout << "PASS: uncreated note input / stage / Ctrl+S / repeated edit / restart\n";
                continue;
            }
            Require(Send(editor, WM_GETTEXTLENGTH) == (scenario ? 13u : 0u), "original note loaded");
            if (!scenario) Send(editor, WM_CHAR, L'x');
            Send(editor, EM_SETSEL, 0, -1); Send(editor, WM_CLEAR);
            Require(Send(editor, WM_GETTEXTLENGTH) == 0, "clear note through native editor");
            if (scenario == 4) {
                protectedNote = note;
                Require(SetFileAttributesW(note.c_str(), FILE_ATTRIBUTE_READONLY), "deny replacement of owned original");
                Require(PostMessageW(main, WM_COMMAND, ID_FILE_SAVE_NOTE, 0), "save empty note with replacement denied");
                Require(Until(DialogReady), "real save error dialog");
                HWND dialog = Window(L"SilentDialogClass");
                const HWND exitButton = GetDlgItem(dialog, 5103);
                wchar_t label[64]{};
                Send(exitButton, WM_GETTEXT, 64, reinterpret_cast<LPARAM>(label));
                Require(std::wstring(label).find(L"非破壊") != std::wstring::npos, "actual non-destructive exit button");
                Send(dialog, WM_COMMAND, MAKEWPARAM(5103, BN_CLICKED));
            } else {
                if (scenario < 3) {
                    Require(PostMessageW(main, WM_COMMAND, ID_FILE_SAVE_NOTE, 0), "explicit empty note save");
                    Require(Until([&] { return Equals(note, bom) && IsWindowEnabled(main) && !Window(L"SilentDialogClass"); }),
                        "empty note is integrated with its selected BOM");
                }
                Require(PostMessageW(main, WM_CLOSE, 0, 0), "normal exit with empty note");
            }
            // Managed exit shows its final report before the process ends.
            bool reportClosePosted = false;
            Require(Until([&] {
                if (scenario == 4 && !reportClosePosted) if (HWND report = Window(L"SilentDialogClass")) {
                    // Post like native input: a synchronous Send can destroy
                    // the final window while its GetMessage still awaits a
                    // queued message, leaving the test itself holding exit.
                    Require(PostMessageW(report, WM_COMMAND, IDOK, 0), "dismiss owned final report");
                    reportClosePosted = true;
                }
                return WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0;
            }), "empty note does not trap normal or non-destructive exit");
            DWORD code = 99; Require(GetExitCodeProcess(process.hProcess, &code) && code == (scenario == 4 ? 3u : 0u), "correct exit completion code");
            CloseHandle(process.hThread); CloseHandle(process.hProcess); process = {};
            Require(Equals(note, scenario == 4 ? original : bom), "normal save or protected original bytes");
            if (scenario == 4) {
                Require(SetFileAttributesW(note.c_str(), FILE_ATTRIBUTE_NORMAL), "restore owned original attributes");
                protectedNote.clear();
                std::cout << "Restarting managed-exit fixture\n";
                main = launch(); editor = OpenNote(main, true);
                Require(Send(editor, WM_GETTEXTLENGTH) == 0 && Equals(note, original), "restart restores the staged empty text without losing the original");
                Require(PostMessageW(main, WM_CLOSE, 0, 0), "exit after recovering empty text");
                Require(WaitForSingleObject(process.hProcess, 15000) == WAIT_OBJECT_0 && Equals(note, ""), "recovered empty note integrates on normal exit");
                Require(GetExitCodeProcess(process.hProcess, &code) && code == 0, "recovery normal exit code");
                CloseHandle(process.hThread); CloseHandle(process.hProcess); process = {};
            }
            std::cout << "PASS: empty note scenario " << scenario << '\n';
        }
        std::cout << "Empty note actual app: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        if (process.hProcess) {
            TerminateProcess(process.hProcess, 79); WaitForSingleObject(process.hProcess, 1000);
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
        }
        if (!protectedNote.empty()) SetFileAttributesW(protectedNote.c_str(), FILE_ATTRIBUTE_NORMAL);
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
