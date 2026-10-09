#pragma once
#include "diagnostics/write_checks.h"
#include "core/atomic_write.h"
#include <atomic>

namespace write_checks {
// Optional sink keeps standalone persistence helpers independent of application
// initialization. Callers capture the owning workspace before starting IO.
using NormalSink = void (*)(const std::filesystem::path&, const std::filesystem::path&,
                           Kind, const std::filesystem::path&, bool, DWORD) noexcept;
inline std::atomic<NormalSink> normalSink{nullptr};
inline thread_local unsigned normalSuppression = 0;
class IgnoreNormalOperations {
public:
    IgnoreNormalOperations() noexcept { ++normalSuppression; }
    ~IgnoreNormalOperations() { --normalSuppression; }
    IgnoreNormalOperations(const IgnoreNormalOperations&) = delete;
    IgnoreNormalOperations& operator=(const IgnoreNormalOperations&) = delete;
};
class NormalOperation {
public:
    template<class Root>
    NormalOperation(const Root& root, const std::filesystem::path& file,
                    Kind kind = Kind::Count, const std::filesystem::path& target = {}) noexcept {
        Initialize(root, file, kind, target);
    }
    NormalOperation(const std::filesystem::path& root, const std::filesystem::path& file,
                    Kind kind = Kind::Count, const std::filesystem::path& target = {}) noexcept {
        Initialize(root, file, kind, target);
    }
private:
    template<class Root>
    void Initialize(const Root& root, const std::filesystem::path& file,
                    Kind kind, const std::filesystem::path& target) noexcept {
        const DWORD saved = GetLastError();
        try {
            if (!normalSuppression && normalSink.load()) {
                root_ = root; file_ = file; kind_ = kind; target_ = target; active_ = true;
            }
        } catch (...) { active_ = false; }
        SetLastError(saved);
    }
public:
    ~NormalOperation() noexcept {
        const DWORD saved = GetLastError();
        if (active_) if (auto sink = normalSink.load()) sink(root_, file_, kind_, target_, passed_, error_);
        SetLastError(saved);
    }
    void Success() noexcept { passed_ = true; error_ = 0; }
    void Failure(DWORD error) noexcept { passed_ = false; error_ = error; }
    // Startup binds the owner after parsing setup; diagnostics never change
    // the real load result if capturing that owner fails.
    void SetWorkspaceRoot(const std::filesystem::path& root) noexcept {
        const DWORD saved = GetLastError();
        try { root_ = root; } catch (...) { active_ = false; }
        SetLastError(saved);
    }
    void Cancel() noexcept { active_ = false; }
    NormalOperation(const NormalOperation&) = delete;
    NormalOperation& operator=(const NormalOperation&) = delete;
private:
    std::filesystem::path root_, file_, target_;
    Kind kind_ = Kind::Count;
    bool active_ = false, passed_ = false;
    DWORD error_ = 0; // 0: the original operation supplied no Win32 error.
};
template<class Root>
inline bool ObservedWriteBytes(const Root& root,
    const std::filesystem::path& dest, const void* bytes, size_t size,
    const std::filesystem::path& temp, const std::filesystem::path& quarantine,
    std::wstring* error) {
    NormalOperation observation(root, dest);
    const bool ok = atomic_write::AtomicWriteBytes(dest, bytes, size, temp, quarantine, error);
    if (ok) observation.Success();
    return ok;
}
template<class Root>
inline bool ObservedWriteUtf8(const Root& root,
    const std::filesystem::path& dest, std::string_view bytes,
    const std::filesystem::path& temp, const std::filesystem::path& quarantine,
    std::wstring* error) {
    return ObservedWriteBytes(root, dest, bytes.data(), bytes.size(), temp, quarantine, error);
}
}
