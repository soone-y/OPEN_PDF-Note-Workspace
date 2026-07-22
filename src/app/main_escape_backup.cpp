#include "app/main_escape_backup.h"

#include "core/app_core.h"
#include "core/preview_trace.h"

#include <filesystem>

namespace {

constexpr wchar_t kLectureLastOpenFileName[] = L"lecture_last_open.txt";

std::filesystem::path CanonicalOrSelf(const std::filesystem::path& p) {
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(p, ec);
    return ec ? p : c;
}

std::wstring ToLowerAscii(std::wstring s) {
    for (wchar_t& ch : s) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return s;
}

bool IsPathUnderRoot(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto p = CanonicalOrSelf(path);
    auto r = CanonicalOrSelf(root);
    if (p.empty() || r.empty()) return false;
    auto pIt = p.begin();
    for (auto rIt = r.begin(); rIt != r.end(); ++rIt, ++pIt) {
        if (pIt == p.end()) return false;
        if (ToLowerAscii(rIt->wstring()) != ToLowerAscii(pIt->wstring())) return false;
    }
    return true;
}

} // namespace

std::filesystem::path EscapeRootPath() {
    if (g_workspaceRoot.empty()) return {};
    return std::filesystem::path(g_workspaceRoot) / L"__resource__" / L"__escape__";
}

EscapeBackupPresence ScanEscapeBackupPresence() {
    const ULONGLONG startTick = preview_trace::TickNow();
    EscapeBackupPresence state;
    auto escapeRoot = EscapeRootPath();
    if (escapeRoot.empty()) {
        preview_trace::Append(
            L"ScanEscapeBackupPresence",
            L"skip=no_escape_root elapsed_ms=" + preview_trace::ElapsedMs(startTick));
        return state;
    }
    std::error_code ec;
    if (!std::filesystem::exists(escapeRoot, ec) || ec || !std::filesystem::is_directory(escapeRoot, ec)) {
        preview_trace::Append(
            L"ScanEscapeBackupPresence",
            L"skip=missing_or_invalid elapsed_ms=" + preview_trace::ElapsedMs(startTick));
        return state;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(escapeRoot, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
        std::error_code stEc;
        if (!it->is_regular_file(stEc) || stEc) continue;
        const std::wstring name = it->path().filename().wstring();
        if (!state.hasPdfPositionBackup && name.rfind(L"pdf_view_positions.txt", 0) == 0) {
            state.hasPdfPositionBackup = true;
        } else if (!state.hasLectureLastOpenBackup && name.rfind(kLectureLastOpenFileName, 0) == 0) {
            state.hasLectureLastOpenBackup = true;
        } else if (!state.hasSessionLastOpenBackup && name.rfind(L"session_last_open.txt", 0) == 0) {
            state.hasSessionLastOpenBackup = true;
        }
        if (!state.hasSavedFileBackup &&
            name.size() >= 9 &&
            name.rfind(L".meta.txt") == name.size() - 9 &&
            IsPathUnderRoot(it->path(), escapeRoot / L"backup")) {
            state.hasSavedFileBackup = true;
        }
        if (state.hasPdfPositionBackup &&
            state.hasLectureLastOpenBackup &&
            state.hasSessionLastOpenBackup &&
            state.hasSavedFileBackup) {
            break;
        }
    }
    preview_trace::Append(
        L"ScanEscapeBackupPresence",
        L"end pdfBackup=" + preview_trace::Bool(state.hasPdfPositionBackup) +
        L" lectureBackup=" + preview_trace::Bool(state.hasLectureLastOpenBackup) +
        L" sessionBackup=" + preview_trace::Bool(state.hasSessionLastOpenBackup) +
        L" savedFileBackup=" + preview_trace::Bool(state.hasSavedFileBackup) +
        L" elapsed_ms=" + preview_trace::ElapsedMs(startTick));
    return state;
}
