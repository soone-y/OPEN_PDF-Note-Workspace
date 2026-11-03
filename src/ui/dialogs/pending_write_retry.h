#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace pending_write_retry {

enum class Result { Saved, Failed, Conflict };
enum class Category { General, TemporaryPaths };

// UI-thread-owned. The callback must own its request data, recheck the target,
// and complete the entire operation (including success-side state changes).
struct Attempt {
    std::wstring key;
    std::wstring operation;
    std::wstring targetPath;
    std::wstring content;
    std::wstring preview;
    std::wstring error;
    bool retryable = true;
    // Auto-save may replace only its own still-pending draft with a newer draft.
    bool coalesce = false;
    Category category = Category::General;
    std::function<Result(std::wstring&)> retry;
    // Started and completed on the UI thread; the async owner must retain the
    // completion until success/failure/cancellation. No worker calls this UI.
    std::function<void(std::function<void(Result, std::wstring)>)> retryAsync;
    bool inFlight = false;
};

void Remember(HWND owner, Attempt attempt);
void Forget(const std::wstring& key);
void Show(HWND owner);
bool HasPending();
bool HasPendingTemporaryPaths();
void RetryTemporaryPaths(HWND owner);
bool ConfirmExit(HWND owner);

} // namespace pending_write_retry
