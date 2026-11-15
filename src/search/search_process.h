#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace search {
// UI owns the process and its kill-on-close job. Poll never waits. One bounded
// shared slot applies backpressure; the worker cannot queue unbounded results.
class Process {
public:
    Process() = default;
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    [[nodiscard]] bool Start(std::string_view request);
    enum class PollResult { Pending, Message, Exited, Failed };
    [[nodiscard]] PollResult Poll(std::string& message);
    [[nodiscard]] bool Continue();
    [[nodiscard]] DWORD processId() const { return process_ ? GetProcessId(process_) : 0; }
    void Stop() noexcept;
private:
    HANDLE mapping_ = nullptr, ready_ = nullptr, ack_ = nullptr;
    HANDLE cancel_ = nullptr, resume_ = nullptr, process_ = nullptr, job_ = nullptr;
    void* view_ = nullptr;
    size_t requestBytes_ = 0;
};
class Channel {
public:
    Channel(void* view, HANDLE ready, HANDLE ack, HANDLE cancel, HANDLE resume);
    [[nodiscard]] bool Send(std::string_view message);
    [[nodiscard]] bool Canceled() const;
    [[nodiscard]] bool WaitForContinue();
    [[nodiscard]] bool Rest(DWORD milliseconds) const;
private:
    void* view_;
    HANDLE ready_, ack_, cancel_, resume_;
};
// Recognizes only the private worker command, before ordinary app startup.
[[nodiscard]] bool TryRunProcess(int* exitCode);
// Implemented beside the existing search engine; no normal UI is created.
[[nodiscard]] bool RunWorker(std::string_view request, Channel& channel);
}
