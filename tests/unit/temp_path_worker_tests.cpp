#include "workspace/temp_path_worker.h"
#include "lecture_order/priority_policy.h"
#include <windows.h>
#include <atomic>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void Wait(temp_paths::Worker& worker) {
    const auto deadline = GetTickCount64() + 5000;
    while (!worker.ready() && GetTickCount64() < deadline) Sleep(1);
    Require(worker.ready(), "worker deadline");
}
temp_paths::Input Input() {
    temp_paths::Input input;
    input.workspace = L"C:\\temp_path_worker_test";
    input.classes = input.workspace + L"\\classes";
    for (int i = 0; i < 256; ++i)
        input.requested.push_back(input.workspace + L"\\external_" + std::to_wstring(i));
    return input;
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_APP + 1) {
        ++*reinterpret_cast<unsigned*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
}
int main() {
    try {
        lecture_priority::Priorities managed, external;
        Require(lecture_priority::Next(managed, external) == 5000, "initial path priority");
        managed[L"managed"] = 5000;
        external[L"external"] = 5001;
        external[L"manual tie"] = 5001;
        const auto beforePriorities = external;
        Require(lecture_priority::Next(managed, external) == 5002 && external == beforePriorities,
            "skip collisions without changing existing or tied priorities");
        external.clear();
        for (int priority = 5000; priority <= 9999; ++priority) external[std::to_wstring(priority)] = priority;
        Require(!lecture_priority::Next(managed, external), "full lower-priority range must reject registration");
        external.erase(L"9999");
        Require(lecture_priority::Next(managed, external) == 9999, "last available priority");
        Require(lecture_priority::PathKey(L"C:/External/Notes/") == lecture_priority::PathKey(L"c:\\external\\notes"),
            "path priority identity ignores Windows case and separators");
        const auto uiThread = GetCurrentThreadId();
        std::atomic<unsigned> checks{0};
        auto prepare = [](const auto&, auto&) { return true; };
        {
            auto input = Input();
            input.requested.push_back(input.requested.front());
            temp_paths::Worker worker(std::move(input), prepare, [&](const auto&, auto&) {
                Require(GetCurrentThreadId() != uiThread, "probe ran on UI thread");
                ++checks; return true;
            });
            Wait(worker);
            const auto result = worker.Take();
            Require(!result.failed && !result.canceled && result.desired.size() == 256 && checks == 256,
                "batch/deduplication result");
        }
        {
            checks = 0;
            temp_paths::Worker worker(Input(), prepare, [&](const auto&, auto& error) {
                ++checks; error = L"denied"; return false;
            });
            Wait(worker);
            const auto result = worker.Take();
            Require(result.failed && result.error == L"denied" && result.desired.size() == 256 && checks == 1,
                "denial must retain the complete batch");
        }
        {
            temp_paths::Worker worker(Input(), prepare, [](const auto&, auto&) -> bool {
                throw std::runtime_error("injected worker failure");
            });
            Wait(worker);
            Require(worker.Take().failed, "worker exception must become failure");
        }
        {
            HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            Require(entered && release, "test events");
            checks = 0;
            temp_paths::Worker worker(Input(), prepare, [&](const auto&, auto&) {
                ++checks; SetEvent(entered);
                return WaitForSingleObject(release, 3000) == WAIT_OBJECT_0;
            });
            Require(WaitForSingleObject(entered, 3000) == WAIT_OBJECT_0, "blocked probe was not entered");
            unsigned heartbeat = 0;
            WNDCLASSW cls{}; cls.lpfnWndProc = WindowProc; cls.hInstance = GetModuleHandleW(nullptr);
            cls.lpszClassName = L"TempPathWorkerTest";
            Require(RegisterClassW(&cls) != 0, "test window class");
            HWND hwnd = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                nullptr, cls.hInstance, nullptr);
            Require(hwnd != nullptr, "message-only test window");
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&heartbeat));
            Require(PostMessageW(hwnd, WM_APP + 1, 0, 0), "UI heartbeat post");
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            Require(heartbeat == 1 && !worker.ready(), "UI must dispatch during blocked probe");
            worker.Cancel();
            Require(!worker.ready(), "Cancel must not wait for active I/O");
            SetEvent(release);
            Wait(worker);
            const auto result = worker.Take();
            Require(result.canceled && checks == 1, "cancel must stop subsequent probes");
            DestroyWindow(hwnd); UnregisterClassW(cls.lpszClassName, cls.hInstance);
            CloseHandle(entered); CloseHandle(release);
        }
        std::cout << "PASS temp_path_worker batch=256 denial=1 exception=1 cancel=1 ui_heartbeat=1\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
