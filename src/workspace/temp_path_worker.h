#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace temp_paths {

// Immutable, UI-captured request. The worker owns no HWND or application state.
struct Input {
    std::wstring workspace, classes;
    std::vector<std::wstring> before, listed, requested;
    bool retry = false; // requested is the complete retained desired list
};
struct Result {
    std::vector<std::wstring> desired;
    std::wstring error;
    bool failed = false, canceled = false;
};
using AccessCheck = std::function<bool(const std::filesystem::path&, std::wstring&)>;

// One UI owner admits one job. No detached thread, unbounded queue, or UI call.
// Cancel is cooperative between I/O calls. The owner polls ready(), keeping the
// UI alive during cancellation, and destroys/joins only a completed worker.
class Worker {
public:
    Worker(Input input, AccessCheck prepareWorkspace, AccessCheck checkDirectory);
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    void Cancel() noexcept { cancel_.store(true); }
    [[nodiscard]] bool ready() const noexcept { return ready_.load(std::memory_order_acquire); }
    [[nodiscard]] Result Take(); // ready() is required; never waits for active I/O
private:
    void Run() noexcept;
    Input input_;
    AccessCheck prepare_, check_;
    Result result_;
    std::atomic_bool cancel_{false}, ready_{false};
    std::thread thread_;
};

} // namespace temp_paths
