#include "workspace/operation_record_store.h"

#include "core/path_safety.h"

#include <string>

namespace operation_record_store {

namespace {

bool IsPlainDirectory(const std::filesystem::path& path) {
    bool isReparse = false;
    if (!TryIsReparsePointNoFollow(path, isReparse) || isReparse) return false;
    return DirectoryExistsWin32(path);
}

}  // namespace

SetAsideResult MoveRecordAside(const std::filesystem::path& recordDir,
                               const std::filesystem::path& asideRoot) {
    SetAsideResult result;
    if (recordDir.empty() || asideRoot.empty() || !recordDir.has_filename() ||
        !IsPlainDirectory(recordDir)) {
        return result;
    }
    std::error_code ec;
    std::filesystem::create_directories(asideRoot, ec);
    if (ec || !IsPlainDirectory(asideRoot)) return result;

    const std::wstring name = recordDir.filename().wstring();
    std::filesystem::path destination = asideRoot / name;
    bool found = false;
    for (int suffix = 1; suffix <= 1000; ++suffix) {
        if (!PathExistsWin32(destination)) {
            found = true;
            break;
        }
        destination = asideRoot / (name + L"_" + std::to_wstring(suffix));
    }
    if (!found) return result;
    // No MOVEFILE_REPLACE_EXISTING: a destination that appears meanwhile is
    // never overwritten; the move then fails and the record stays in place.
    if (!MoveFileExW(ToExtendedWin32PathIfAbsoluteLocal(recordDir).c_str(),
                     ToExtendedWin32PathIfAbsoluteLocal(destination).c_str(), 0)) {
        result.win32Error = GetLastError();
        return result;
    }
    result.ok = true;
    result.destination = destination;
    return result;
}

}  // namespace operation_record_store
