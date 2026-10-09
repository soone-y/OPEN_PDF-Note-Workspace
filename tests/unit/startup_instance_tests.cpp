#include "app/startup_instance.h"
#include "app/main_window_liveness_policy.h"
#include "app/main_window_recovery_notice.h"
#include "core/constants.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static std::wstring SelfPath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    Check(size != 0 && size < buffer.size(), "test executable path");
    return std::wstring(buffer.data(), size);
}
static std::wstring ReadyName(DWORD pid) { return L"PdfNoteStartupTestReady_" + std::to_wstring(pid); }
static HWND TestWindow(bool main, bool visible) {
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = main ? kMainClass : L"PdfNoteStartupTestHelper";
    RegisterClassW(&wc);
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Startup fixture",
        WS_OVERLAPPED | (visible ? WS_VISIBLE : 0), 0, 0, 80, 40,
        nullptr, nullptr, wc.hInstance, nullptr);
}
static int RunChild(const std::wstring& mode, DWORD parentId) {
    const bool legacyMode = mode == L"legacy";
    HANDLE endpoint = CreateEventW(nullptr, FALSE, FALSE,
        (legacyMode ? SingleInstanceShutdownRequestEventName() : ProcessShutdownRequestEventName()).c_str());
    HANDLE legacyMutex = legacyMode ? CreateMutexW(nullptr, TRUE, SingleInstanceMutexName().c_str()) : nullptr;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ReadyName(GetCurrentProcessId()).c_str());
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, (ReadyName(GetCurrentProcessId()) + L"_stop").c_str());
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentId);
    if (!endpoint || !ready || !stop || !parent) return 4;
    HWND window = nullptr;
    if (mode != L"none") {
        window = TestWindow(mode.find(L"main") != std::wstring::npos,
                            mode.find(L"visible") == 0);
        if (!window) return 5;
        if (mode.find(L"visible") == 0) {
            ShowWindow(window, SW_SHOWNOACTIVATE);
            ShowWindow(window, SW_SHOWNOACTIVATE);
        }
    }
    SetEvent(ready);
    const ULONGLONG deadline = GetTickCount64() + 45000;
    while (GetTickCount64() < deadline) {
        HANDLE events[] = {endpoint, stop, parent};
        const DWORD wait = MsgWaitForMultipleObjects(3, events, FALSE, 250, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0 && mode == L"hidden_main_hold") continue;
        if (wait >= WAIT_OBJECT_0 && wait < WAIT_OBJECT_0 + 3) break;
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    }
    if (window) DestroyWindow(window);
    CloseHandle(ready);
    CloseHandle(endpoint);
    CloseHandle(stop);
    CloseHandle(parent);
    if (legacyMutex) CloseHandle(legacyMutex);
    return 0;
}
struct Child {
    HANDLE process = nullptr;
    DWORD pid = 0;
    HANDLE endpoint = nullptr;
    std::wstring mode;
    Child(const std::filesystem::path& executable, const std::wstring& value) : mode(value) {
        std::wstring command = L"\"" + executable.wstring() + L"\" --child " + mode + L" " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION info{};
        Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &info) != FALSE, "fixture start");
        process = info.hProcess; pid = info.dwProcessId; CloseHandle(info.hThread);
    }
    Child(Child&& other) noexcept : process(other.process), pid(other.pid), endpoint(other.endpoint), mode(std::move(other.mode)) {
        other.process = nullptr; other.endpoint = nullptr;
    }
    Child(const Child&) = delete;
    ~Child() {
        if (endpoint) { SetEvent(endpoint); CloseHandle(endpoint); }
        if (process) {
            // This handle owns only this disposable child, never a user app.
            if (WaitForSingleObject(process, 5000) == WAIT_TIMEOUT) {
                TerminateProcess(process, 9);
                WaitForSingleObject(process, 5000);
            }
            CloseHandle(process);
        }
    }
};

static HWND OwnNoticeWindow(DWORD thread) {
    HWND found = nullptr;
    EnumThreadWindows(thread, [](HWND window, LPARAM data) -> BOOL {
        wchar_t name[128]{};
        if (GetClassNameW(window, name, 128) &&
            wcscmp(name, main_window_liveness::kRecoveryNoticeClass) == 0)
            *reinterpret_cast<HWND*>(data) = window;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}
struct NoticeFixture {
    HANDLE ready = nullptr, stop = nullptr, commandDone = nullptr;
    std::atomic<HWND> window{nullptr};
    std::atomic<unsigned> commands{0};
    std::atomic<bool> failed{false};
};
static DWORD WINAPI NoticeWorker(LPVOID parameter) {
    auto& test = *static_cast<NoticeFixture*>(parameter);
    try {
        main_window_liveness::NoticeText text{L"Isolated recovery notice", L"No main UI is pumping messages.\n\nUnsaved data protection remains mandatory.",
            L"Retry display", L"Try normal exit", L"Keep waiting", L"Request pending", L"Close pending", L"Request failed"};
        main_window_liveness::RecoveryNotice notice(std::move(text), test.stop);
        Check(notice.Show(), "independent notice create");
        test.window = OwnNoticeWindow(GetCurrentThreadId());
        Check(test.window && IsWindowVisible(test.window) && !GetWindow(test.window, GW_OWNER),
            "visible notice is not owned by unavailable main UI");
        RECT client{}; GetClientRect(test.window, &client);
        for (int id = 1; id <= 5; ++id) {
            RECT child{}; Check(GetWindowRect(GetDlgItem(test.window, id), &child), "notice control rectangle");
            MapWindowPoints(nullptr, test.window, reinterpret_cast<POINT*>(&child), 2);
            Check(child.left >= 0 && child.top >= 0 && child.right <= client.right && child.bottom <= client.bottom,
                "notice text and actions fit client area");
        }
        SetEvent(test.ready);
        for (;;) {
            const DWORD wait = MsgWaitForMultipleObjects(1, &test.stop, FALSE, 200, QS_ALLINPUT);
            if (wait == WAIT_OBJECT_0) {
                // Dispatch the private stop timer as a nested native modal loop
                // would. It must tear down the window without the main UI.
                SendMessageW(test.window, WM_TIMER, 1, 0);
                Check(!OwnNoticeWindow(GetCurrentThreadId()), "notice stop during nested dispatch");
                return 0;
            }
            Check(wait != WAIT_FAILED, "notice wait");
            const auto command = notice.PumpCommand();
            if (command != main_window_liveness::NoticeCommand::None) {
                notice.ReportRequest(command, command == main_window_liveness::NoticeCommand::RetryDisplay);
                test.commands.fetch_or(command == main_window_liveness::NoticeCommand::RetryDisplay ? 1 : 2);
                SetEvent(test.commandDone);
            }
            if (!IsWindow(test.window)) {
                Check(notice.Show() && !OwnNoticeWindow(GetCurrentThreadId()), "dismissed notice not repeated");
                notice.Reset();
                Check(notice.Show(), "new absence episode may notify again");
                test.window = OwnNoticeWindow(GetCurrentThreadId());
                test.commands.fetch_or(4);
                SetEvent(test.commandDone);
            }
        }
    } catch (...) {
        test.failed = true; SetEvent(test.ready); SetEvent(test.commandDone); return 1;
    }
}
static void TestIndependentNotice() {
    NoticeFixture test;
    test.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    test.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    test.commandDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE worker = nullptr;
    try {
        Check(test.ready && test.stop && test.commandDone, "notice fixture events");
        worker = CreateThread(nullptr, 0, NoticeWorker, &test, 0, nullptr);
        Check(worker && WaitForSingleObject(test.ready, 5000) == WAIT_OBJECT_0 && !test.failed,
            "notice shown while main UI does not pump");
        Check(PostMessageW(test.window, WM_COMMAND, MAKEWPARAM(3, BN_CLICKED), 0) &&
            WaitForSingleObject(test.commandDone, 5000) == WAIT_OBJECT_0 && (test.commands.load() & 1),
            "retry available while main UI blocked");
        Check(PostMessageW(test.window, WM_COMMAND, MAKEWPARAM(4, BN_CLICKED), 0) &&
            WaitForSingleObject(test.commandDone, 5000) == WAIT_OBJECT_0 && (test.commands.load() & 2),
            "normal exit request remains a request, not forced exit");
        Check(PostMessageW(test.window, WM_CLOSE, 0, 0) &&
            WaitForSingleObject(test.commandDone, 5000) == WAIT_OBJECT_0 && (test.commands.load() & 4),
            "dismissal suppression and recurrence");
        SetEvent(test.stop);
        Check(WaitForSingleObject(worker, 5000) == WAIT_OBJECT_0 && !test.failed, "notice cleanup joined");
    } catch (...) {
        if (test.stop) SetEvent(test.stop);
        // The outer fixture process has a 60-second watchdog. Do not destroy
        // borrowed handles/stack context while its worker may still use them.
        if (worker) { WaitForSingleObject(worker, INFINITE); CloseHandle(worker); }
        if (test.ready) CloseHandle(test.ready);
        if (test.stop) CloseHandle(test.stop);
        if (test.commandDone) CloseHandle(test.commandDone);
        throw;
    }
    CloseHandle(worker); CloseHandle(test.ready); CloseHandle(test.stop); CloseHandle(test.commandDone);
}
static void TestNoticeCancellation() {
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(stop != nullptr, "notice cancellation stop event");
    try {
        const main_window_liveness::NoticeText text{L"Cancellation fixture", L"Cancellation must not post a pending close.",
            L"Retry", L"Close", L"Wait", L"Retry pending", L"Close pending", L"Failed"};
        {
            main_window_liveness::RecoveryNotice unavailable(text, nullptr);
            Check(!unavailable.Show() && !OwnNoticeWindow(GetCurrentThreadId()), "invalid stop fails without notice");
            main_window_liveness::RecoveryNotice notice(text, stop);
            for (const unsigned dismissal : {WM_CLOSE, WM_COMMAND}) {
                notice.Reset(); Check(notice.Show(), "cancellation notice create");
                HWND window = OwnNoticeWindow(GetCurrentThreadId());
                Check(window && PostMessageW(window, WM_COMMAND, MAKEWPARAM(4, BN_CLICKED), 0) &&
                    PostMessageW(window, dismissal, dismissal == WM_COMMAND ? MAKEWPARAM(5, BN_CLICKED) : 0, 0),
                    "queued close and cancellation");
                Check(notice.PumpCommand() == main_window_liveness::NoticeCommand::None &&
                    !OwnNoticeWindow(GetCurrentThreadId()), "wait/close cancels an unsent command");
            }
            notice.Reset(); SetEvent(stop);
            Check(!notice.Show() && !OwnNoticeWindow(GetCurrentThreadId()), "stop before show creates no window");
        }
    } catch (...) { CloseHandle(stop); throw; }
    CloseHandle(stop);
}
static void TestAutomationExecutionPolicy() {
    const wchar_t* names[] = {
        L"PDF_NOTE_SMALL_UI_AUTOMATION",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_LOG_CONTRACT_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_RECOVERY_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_CONFIG_UNKNOWN_FIELD_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_PDFIUM_RAW_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_SETTINGS_BUNDLE_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_HELP_VISIBILITY_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_DIALOG_OWNER_VISIBILITY_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_OUTPUT_EXPORT_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_TARGET_SESSION_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_PDF_ONLY",
        L"PDF_NOTE_SMALL_UI_AUTOMATION_MATH_RENDER_ONLY",
    };
    struct SavedEnv {
        const wchar_t* name;
        bool present;
        std::wstring value;
    };
    std::vector<SavedEnv> saved;
    struct Restore {
        std::vector<SavedEnv>& saved;
        ~Restore() {
            for (const auto& env : saved)
                SetEnvironmentVariableW(env.name, env.present ? env.value.c_str() : nullptr);
        }
    } restore{saved};
    for (auto name : names) {
        std::wstring value;
        const bool present = ReadMainEnvVar(name, &value);
        saved.push_back({name, present, value});
        Check(SetEnvironmentVariableW(name, nullptr), "clear automation mode for policy test");
    }
    Check(!IsBackgroundUiAutomationEnabled(), "ordinary launch is foreground-capable");
    Check(SetEnvironmentVariableW(names[0], L"1"), "enable automation for policy test");
    Check(!IsBackgroundUiAutomationEnabled(), "full automation requires foreground capability");
    for (size_t i = 1; i < std::size(names); ++i) {
        for (auto value : {L"1", L"true"}) {
            Check(SetEnvironmentVariableW(names[i], value), "select policy mode");
            Check(IsBackgroundUiAutomationEnabled() == (i <= 5), "mode selects correct display contract");
        }
        Check(SetEnvironmentVariableW(names[0], nullptr), "disable automation");
        Check(!IsBackgroundUiAutomationEnabled(), "mode alone cannot hide a normal launch");
        Check(SetEnvironmentVariableW(names[0], L"1"), "restore automation");
        if (i != 1) {
            Check(SetEnvironmentVariableW(names[1], L"1"), "select conflicting mode");
            Check(!IsBackgroundUiAutomationEnabled(), "mixed modes cannot opt out of UI checks");
            Check(SetEnvironmentVariableW(names[1], nullptr), "clear conflicting mode");
        }
        Check(SetEnvironmentVariableW(names[i], L"0"), "disable selected mode");
        Check(!IsBackgroundUiAutomationEnabled(), "false mode does not enable background execution");
        Check(SetEnvironmentVariableW(names[i], nullptr), "clear selected mode");
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring(argv[1]) == L"--child")
        return RunChild(argv[2], static_cast<DWORD>(std::stoul(argv[3])));
    try {
        TestAutomationExecutionPolicy();
        main_window_liveness::Policy policy(100);
        Check(!policy.Observe(false, 15099) && policy.Observe(false, 15100), "startup grace");
        Check(!policy.TakeNotification() && !policy.Observe(false, 20099) &&
            policy.Observe(false, 20100), "recovery retry spaced");
        Check(!policy.TakeNotification() && policy.Observe(false, 25100) && policy.TakeNotification(),
            "third attempt notifies once");
        Check(!policy.Observe(false, 99999) && !policy.TakeNotification(), "bounded attempts and notification");
        Check(!policy.Observe(true, 100000), "recovery reset");
        Check(!policy.Observe(false, 100100) && !policy.Observe(false, 115099) &&
            policy.Observe(false, 115100), "runtime grace and recurrence");
        Check(!policy.Observe(true, 115101) && !policy.Observe(false, 115102) &&
            !policy.Observe(true, 115103), "transient hide protected");
        main_window_liveness::Policy delayed(0);
        Check(delayed.Observe(false, 1000000) && !delayed.Observe(false, 1000000) &&
            !delayed.Observe(false, 999999) && !delayed.TakeNotification(), "no catch-up burst or backward clock");
        TestIndependentNotice();
        TestNoticeCancellation();
        HWND hidden = TestWindow(true, false);
        Check(hidden != nullptr, "own window create");
        PublishSelfMainWindow(hidden);
        Check(!IsSelfMainWindowVisible(), "hidden self main");
        ShowWindow(hidden, SW_SHOWNOACTIVATE);
        ShowWindow(hidden, SW_SHOWNOACTIVATE);
        Check(IsSelfMainWindowVisible(), "visible self main");
        ShowWindow(hidden, SW_MINIMIZE);
        Check(IsSelfMainWindowVisible(), "minimization is not absence");
        Check(RestoreSelfMainWindowOnUiThread() && IsIconic(hidden), "recovery preserves minimization");
        ShowWindow(hidden, SW_HIDE);
        Check(RestoreSelfMainWindowOnUiThread() && IsSelfMainWindowVisible(), "hidden UI recovery");
        constexpr UINT recoveryMessage = WM_APP + 200;
        Check(PostSelfMainWindowRecovery(recoveryMessage), "recovery addressed to self HWND");
        MSG recovery{};
        Check(PeekMessageW(&recovery, hidden, recoveryMessage, recoveryMessage, PM_REMOVE) &&
            recovery.hwnd == hidden, "nested loop can dispatch addressed recovery");
        PublishSelfMainWindow(nullptr); DestroyWindow(hidden);
        Check(!IsSelfMainWindowVisible(), "cleared self main");
        Check(!RestoreSelfMainWindowOnUiThread(), "missing main never recovered via another process");
        Check(!PostSelfMainWindowRecovery(recoveryMessage), "missing main never sends to another process");
        HWND helper = TestWindow(false, false);
        Check(helper != nullptr, "unrelated helper window create");
        PublishSelfMainWindow(helper);
        Check(!RestoreSelfMainWindowOnUiThread() && !PostSelfMainWindowRecovery(WM_CLOSE) && IsWindow(helper),
            "a stale or wrong-class publication never closes a helper window");
        PublishSelfMainWindow(nullptr); DestroyWindow(helper);

        const auto source = std::filesystem::path(SelfPath());
        const auto fixtureDir = source.parent_path() / L"startup_instance_fixture";
        std::error_code fileError;
        std::filesystem::create_directories(fixtureDir, fileError);
        Check(!fileError, "fixture directory");
        const auto otherExecutable = fixtureDir / source.filename();
        std::filesystem::copy_file(source, otherExecutable, std::filesystem::copy_options::overwrite_existing, fileError);
        Check(!fileError, "fixture executable copy");
        std::vector<Child> children;
        for (const auto* mode : {L"none", L"hidden_helper", L"hidden_main", L"visible_main", L"visible_aux", L"hidden_main_hold", L"legacy"})
            children.emplace_back(otherExecutable, mode);
        const ULONGLONG readyDeadline = GetTickCount64() + 5000;
        for (auto& child : children) {
            HANDLE ready = nullptr;
            while (!ready && GetTickCount64() < readyDeadline) {
                ready = OpenEventW(SYNCHRONIZE, FALSE, ReadyName(child.pid).c_str());
                if (!ready) Sleep(20);
            }
            Check(ready != nullptr, "fixture ready endpoint");
            const DWORD wait = WaitForSingleObject(ready, 5000); CloseHandle(ready);
            Check(wait == WAIT_OBJECT_0, "fixture ready");
            child.endpoint = OpenEventW(EVENT_MODIFY_STATE, FALSE, (ReadyName(child.pid) + L"_stop").c_str());
            Check(child.endpoint != nullptr, "owned cleanup endpoint");
        }
        auto find = [&](DWORD pid) -> std::optional<OtherPackageHeadlessMainProcess> {
            const auto candidates = FindOtherPackageHeadlessMainProcesses();
            const auto it = std::find_if(candidates.begin(), candidates.end(),
                [&](const auto& candidate) { return candidate.processId == pid; });
            return it == candidates.end() ? std::nullopt : std::optional<OtherPackageHeadlessMainProcess>(*it);
        };
        Check(!find(children[0].pid), "concurrent startup protected");
        // Real creation-time grace, not an injected production bypass.
        Sleep(15500);
        for (std::size_t i = 0; i < children.size(); ++i) {
            const auto target = find(children[i].pid);
            if (target.has_value() != (i < 3 || i >= 5)) {
                std::wcerr << L"classification mode=" << children[i].mode << L" target=" << target.has_value() << L'\n';
                Check(false, "main/auxiliary visibility classification");
            }
            // Capture the PID-scoped cooperative endpoint solely for cleanup
            // of a rejected/visible fixture. Never enumerate user targets.
            if (target) {
                if (children[i].mode == L"legacy") {
                    Check(!target->canRequestShutdown && !RequestOtherPackageHeadlessMainProcessShutdown(*target) &&
                        WaitForSingleObject(children[i].process, 0) == WAIT_TIMEOUT,
                        "legacy diagnostic never sends a package-wide request");
                    continue;
                }
                auto stale = *target; ++stale.creationTime.dwLowDateTime;
                Check(!RequestOtherPackageHeadlessMainProcessShutdown(stale), "creation identity mismatch rejected");
                auto observation = RequestOtherPackageHeadlessMainProcessShutdown(*target);
                Check(observation.has_value(), "cooperative request");
                if (children[i].mode == L"hidden_main_hold") {
                    Check(observation->Poll() == ProcessShutdownStatus::Pending &&
                        observation->Poll(0) == ProcessShutdownStatus::TimedOut &&
                        WaitForSingleObject(children[i].process, 0) == WAIT_TIMEOUT,
                        "request without exit remains unconfirmed, never terminated");
                    continue;
                }
                const ULONGLONG end = GetTickCount64() + 5000;
                while (observation->Poll() == ProcessShutdownStatus::Pending && GetTickCount64() < end) Sleep(20);
                Check(observation->Poll() == ProcessShutdownStatus::Exited, "exit confirmed on original process handle");
                Check(!find(children[i].pid), "exited process no longer candidate");
            }
        }
        HANDLE current = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
        Check(current != nullptr, "self synchronize handle");
        ProcessShutdownObservation pending(current, GetTickCount64());
        Check(pending.Poll() == ProcessShutdownStatus::Pending &&
            pending.Poll(0) == ProcessShutdownStatus::TimedOut, "nonblocking timeout never reports exit");
        ProcessShutdownObservation invalid(nullptr, GetTickCount64());
        Check(invalid.Poll() == ProcessShutdownStatus::Failed, "invalid observation fails closed");
        std::cout << "startup instance tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
