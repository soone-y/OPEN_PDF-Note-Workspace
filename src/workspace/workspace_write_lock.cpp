#include "workspace/workspace_write_lock.h"

#include <windows.h>

#include <cstdint>
#include <cwctype>

namespace {

HANDLE g_workspaceWriteLock = nullptr;
std::wstring g_workspaceWriteLockKey;

std::wstring NormalizeWorkspaceWriteLockKey(const std::filesystem::path& root) {
    if (root.empty()) return {};

    std::error_code ec;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(root, ec);
    if (ec || normalized.empty()) {
        ec.clear();
        normalized = std::filesystem::absolute(root, ec);
    }
    if (ec || normalized.empty()) return {};

    std::wstring key = normalized.lexically_normal().wstring();
    for (wchar_t& ch : key) {
        ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
    }
    return key;
}

std::uint64_t WorkspaceWriteLockHash(const std::wstring& value) {
    std::uint64_t hash = 14695981039346656037ull;
    for (wchar_t ch : value) {
        const std::uint16_t codeUnit = static_cast<std::uint16_t>(ch);
        hash ^= static_cast<std::uint8_t>(codeUnit & 0xffu);
        hash *= 1099511628211ull;
        hash ^= static_cast<std::uint8_t>(codeUnit >> 8u);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::wstring WorkspaceWriteLockName(const std::wstring& key) {
    wchar_t hash[17]{};
    swprintf_s(hash, L"%016llx", static_cast<unsigned long long>(WorkspaceWriteLockHash(key)));
    return L"Local\\PdfNoteWorkspaceWriteLock_" + std::wstring(hash);
}

}  // namespace

bool AcquireWorkspaceWriteLock(const std::filesystem::path& workspaceRoot,
                               std::wstring* outError) {
    if (outError) outError->clear();

    const std::wstring key = NormalizeWorkspaceWriteLockKey(workspaceRoot);
    if (key.empty()) {
        if (outError) *outError = L"ワークスペースのパスを正規化できません。";
        return false;
    }
    if (g_workspaceWriteLock && key == g_workspaceWriteLockKey) return true;

    const std::wstring name = WorkspaceWriteLockName(key);
    HANDLE candidate = CreateMutexW(nullptr, TRUE, name.c_str());
    if (!candidate) {
        if (outError) *outError = L"ワークスペース排他を作成できません。";
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(candidate);
        if (outError) {
            *outError = L"このワークスペースは別の PDF Note Workspace プロセスが使用中です。";
        }
        return false;
    }

    ReleaseWorkspaceWriteLock();
    g_workspaceWriteLock = candidate;
    g_workspaceWriteLockKey = key;
    return true;
}

void ReleaseWorkspaceWriteLock() {
    if (!g_workspaceWriteLock) return;
    ReleaseMutex(g_workspaceWriteLock);
    CloseHandle(g_workspaceWriteLock);
    g_workspaceWriteLock = nullptr;
    g_workspaceWriteLockKey.clear();
}
