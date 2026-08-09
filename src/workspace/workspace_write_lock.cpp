#include "workspace/workspace_write_lock.h"

#include <windows.h>

#include <cstdint>
#include <cwctype>
#include <unordered_map>

namespace {

std::wstring g_workspaceWriteLockKey;
std::unordered_map<std::wstring, HANDLE> g_documentOpenLocks;
int g_documentOpenLockTransitionDepth = 0;

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

std::wstring NormalizeDocumentOpenLockKey(const std::filesystem::path& path) {
    return NormalizeWorkspaceWriteLockKey(path);
}

std::wstring DocumentOpenLockName(const std::wstring& key) {
    wchar_t hash[17]{};
    swprintf_s(hash, L"%016llx", static_cast<unsigned long long>(WorkspaceWriteLockHash(key)));
    return L"Local\\PdfNoteDocumentOpenLock_" + std::wstring(hash);
}

void ReleaseDocumentOpenLockByKey(const std::wstring& key) {
    const auto found = g_documentOpenLocks.find(key);
    if (found == g_documentOpenLocks.end()) return;
    ReleaseMutex(found->second);
    CloseHandle(found->second);
    g_documentOpenLocks.erase(found);
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
    g_workspaceWriteLockKey = key;
    return true;
}

void ReleaseWorkspaceWriteLock() {
    g_workspaceWriteLockKey.clear();
}

WorkspaceOperationLock::WorkspaceOperationLock(const std::filesystem::path& workspaceRoot,
                                               std::wstring* outError) {
    if (outError) outError->clear();
    const std::wstring key = NormalizeWorkspaceWriteLockKey(workspaceRoot);
    if (key.empty()) {
        if (outError) *outError = L"ワークスペースのパスを正規化できません。";
        return;
    }

    const std::wstring name = WorkspaceWriteLockName(key);
    HANDLE candidate = CreateMutexW(nullptr, TRUE, name.c_str());
    if (!candidate) {
        if (outError) *outError = L"ワークスペース操作の排他を作成できません。";
        return;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(candidate);
        if (outError) *outError = L"このワークスペースでは別の共有操作が進行中です。";
        return;
    }
    handle_ = candidate;
}

WorkspaceOperationLock::~WorkspaceOperationLock() {
    if (!handle_) return;
    ReleaseMutex(handle_);
    CloseHandle(handle_);
    handle_ = nullptr;
}

DocumentOpenLockCandidate::DocumentOpenLockCandidate(const std::filesystem::path& path,
                                                     std::wstring* outError) {
    if (outError) outError->clear();
    key_ = NormalizeDocumentOpenLockKey(path);
    if (key_.empty()) {
        if (outError) *outError = L"開くファイルのパスを正規化できません。";
        return;
    }

    ++g_documentOpenLockTransitionDepth;
    if (g_documentOpenLocks.find(key_) != g_documentOpenLocks.end()) {
        acquired_ = true;
        return;
    }

    const std::wstring name = DocumentOpenLockName(key_);
    HANDLE candidate = CreateMutexW(nullptr, TRUE, name.c_str());
    if (!candidate) {
        if (outError) *outError = L"ファイル排他を作成できません。";
        --g_documentOpenLockTransitionDepth;
        return;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(candidate);
        if (outError) *outError = L"このファイルは別の PDF Note Workspace ウィンドウで開かれています。";
        --g_documentOpenLockTransitionDepth;
        return;
    }

    g_documentOpenLocks.emplace(key_, candidate);
    acquired_ = true;
    newlyAcquired_ = true;
}

DocumentOpenLockCandidate::~DocumentOpenLockCandidate() {
    if (g_documentOpenLockTransitionDepth > 0) --g_documentOpenLockTransitionDepth;
    if (newlyAcquired_) ReleaseDocumentOpenLockByKey(key_);
}

void DocumentOpenLockCandidate::CommitReplacing(const std::filesystem::path& previousPath) {
    if (!acquired_) return;
    const std::wstring previousKey = NormalizeDocumentOpenLockKey(previousPath);
    if (!previousKey.empty() && previousKey != key_) {
        ReleaseDocumentOpenLockByKey(previousKey);
    }
    newlyAcquired_ = false;
}

bool IsDocumentOpenLockTransitionActive() {
    return g_documentOpenLockTransitionDepth > 0;
}

void ReleaseDocumentOpenLock(const std::filesystem::path& path) {
    ReleaseDocumentOpenLockByKey(NormalizeDocumentOpenLockKey(path));
}

void ReleaseAllDocumentOpenLocks() {
    for (const auto& [key, handle] : g_documentOpenLocks) {
        ReleaseMutex(handle);
        CloseHandle(handle);
    }
    g_documentOpenLocks.clear();
}
