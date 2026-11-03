#include "workspace/temp_path_worker.h"
#include "core/path_safety.h"
#include "core/localization.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <unordered_set>

namespace temp_paths {
namespace {
std::wstring Key(std::wstring path) {
    for (auto& ch : path) ch = ch == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(ch));
    return path;
}
std::wstring Canonical(const std::filesystem::path& path) {
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error);
    return (error ? path.lexically_normal() : canonical).wstring();
}
}

Worker::Worker(Input input, AccessCheck prepareWorkspace, AccessCheck checkDirectory)
    : input_(std::move(input)), prepare_(std::move(prepareWorkspace)), check_(std::move(checkDirectory)),
      thread_(&Worker::Run, this) {}
Worker::~Worker() { Cancel(); if (thread_.joinable()) thread_.join(); }

Result Worker::Take() {
    if (!ready()) throw std::logic_error("temporary-path worker is not complete");
    if (thread_.joinable()) thread_.join();
    return std::move(result_);
}

void Worker::Run() noexcept {
    (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    try {
        result_.desired = input_.before;
        std::unordered_set<std::wstring> before, listed, seen;
        for (const auto& path : input_.before) before.insert(Key(path));
        for (const auto& path : input_.listed) listed.insert(Key(path));
        seen = before;
        const auto workspace = Canonical(input_.workspace), classes = Canonical(input_.classes);
        for (const auto& requested : input_.requested) {
            if (cancel_) break;
            if (input_.retry && before.count(Key(requested))) continue;
            const std::filesystem::path path(requested);
            // Registration never authorizes relative paths or UNC access.
            if (path.empty() || !path.is_absolute() || IsUncPath(path)) {
                result_.failed = true;
                result_.error = localization::Text(L"workspace.temp_paths.invalid");
                break;
            }
            const auto canonical = Canonical(path);
            const auto key = Key(canonical);
            if (seen.count(key)) continue;
            if (!input_.retry && (listed.count(key) ||
                    _wcsicmp(workspace.c_str(), canonical.c_str()) == 0 ||
                    _wcsicmp(classes.c_str(), canonical.c_str()) == 0)) continue;
            // Keep all candidates before access checks: a denial retains the
            // complete batch for retry without publishing partial registration.
            result_.desired.push_back(canonical);
            seen.insert(key);
        }
        if (!cancel_ && !result_.failed && result_.desired != input_.before) {
            if (!prepare_(std::filesystem::path(input_.workspace), result_.error)) result_.failed = true;
            for (const auto& path : result_.desired) {
                if (cancel_ || result_.failed) break;
                if (before.count(Key(path))) continue;
                if (!check_(std::filesystem::path(path), result_.error)) result_.failed = true;
            }
        }
    } catch (...) {
        result_.failed = true; // Preserve the immutable input at the UI owner.
        result_.error.clear();
    }
    result_.canceled = cancel_.load();
    ready_.store(true, std::memory_order_release);
}
} // namespace temp_paths
