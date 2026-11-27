#pragma once

// Location-independent paths in workspace management records.
//
// Management records (operation-transaction manifests, backup and stage
// metadata, view positions, last-open maps) live inside the workspace and
// must stay readable after the folder that holds the workspace (for example
// the exe folder with a relative workspace) is renamed or moved while the app
// is closed.
//
// Record path form (shared by every record that adopts it):
//   "講義\\note\\a.clro"       inside the workspace: relative, case kept
//   "@external:C:\\授業\\a.pdf" inside a registered external folder
//   "C:\\..."                  anything else, and every legacy record
// This follows identity_path_scope (the note-identity catalog form), but keeps
// the original letter case because these paths are also shown to the user.
//
// Records that hold paths, and how each survives a renamed workspace folder:
//   __operations__/*/manifest.tsv
//       V7 record form. V1-V6: own backups resolved from the record's current
//       directory; members mapped only after the user agrees (recovery runs
//       automatically when a session opens). Committed records never block.
//   __backup__/{note,clrop}/**/*.meta.txt
//       dest_record/backup_name keys (record form) next to the absolute
//       dest/backup keys older versions read. Legacy: derived from `backup=`;
//       restoring to a mapped destination asks first.
//   __unsaved__/**/*.meta.txt
//       target_record/dest_record keys next to the absolute keys. Legacy:
//       derived from `stage=`; applying a stage stays a user action.
//   __recent__/pdf_view_positions.txt, __recent__/session_last_open.txt
//       record form; legacy absolute entries are only lost display state.
//   __settings__/note_identity_registry.bin
//       identity_path_scope keys (already workspace-relative).
//   __recent__/lecture_last_open.txt   folder names only.
//   __recent__/startup_last_open.txt   absolute; a stale entry only skips the
//       startup restore (not mapped).
// Reading the record's own files is automatic. Mapping a path that names the
// user's files is used for an automatic write only after the user agrees, and
// a user-started write shows the mapped path first.
//
// Legacy records hold absolute paths only. DetectFromSelfPath derives the
// previous workspace root from where such a record says it was and where it
// is now. Everything here is lexical: nothing touches the file system, and the
// caller decides whether a mapped path may be read, written, or must first be
// confirmed by the user. Only local drive paths are supported, as in
// identity_path_scope; UNC and device paths are never mapped.

#include "core/identity_path_scope.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace workspace_relocation {

inline constexpr wchar_t kManagedRootName[] = L"__pdf_note_workspace__";

namespace detail {

// Lexically normalized local drive path with the original case, or nullopt
// for anything identity_path_scope::LocalPath rejects.
inline std::optional<std::filesystem::path> CaseKeptLocal(const std::filesystem::path& path) {
    if (!identity_path_scope::LocalPath(path.wstring())) return std::nullopt;
    std::wstring raw = path.wstring();
    for (auto& ch : raw) {
        if (ch == L'/') ch = L'\\';
    }
    if (raw.rfind(L"\\\\?\\", 0) == 0) raw.erase(0, 4);
    raw = std::filesystem::path(raw).lexically_normal().wstring();
    while (raw.size() > 3 && raw.back() == L'\\') raw.pop_back();
    return std::filesystem::path(raw);
}

inline std::vector<std::filesystem::path> Parts(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> out;
    for (const auto& part : path) {
        if (!part.empty()) out.push_back(part);
    }
    return out;
}

inline std::filesystem::path Join(const std::vector<std::filesystem::path>& parts,
                                  size_t begin, size_t end) {
    std::filesystem::path out;
    for (size_t i = begin; i < end && i < parts.size(); ++i) out /= parts[i];
    return out;
}

}  // namespace detail

inline bool SameLocation(const std::filesystem::path& a, const std::filesystem::path& b) {
    const auto la = identity_path_scope::LocalPath(a.wstring());
    const auto lb = identity_path_scope::LocalPath(b.wstring());
    return la && lb && *la == *lb;
}

// The part of `path` below `root` with the original case of `path`, an empty
// path when both name the same place, or nullopt when `path` is not inside.
inline std::optional<std::filesystem::path> RelativeUnder(const std::filesystem::path& path,
                                                          const std::filesystem::path& root) {
    const auto lp = identity_path_scope::LocalPath(path.wstring());
    const auto lr = identity_path_scope::LocalPath(root.wstring());
    if (!lp || !lr) return std::nullopt;
    if (*lp == *lr) return std::filesystem::path{};
    if (!identity_path_scope::Below(*lp, *lr)) return std::nullopt;
    const auto kept = detail::CaseKeptLocal(path);
    if (!kept) return std::nullopt;
    const auto keptParts = detail::Parts(*kept);
    const size_t rootCount = detail::Parts(*lr).size();
    if (keptParts.size() <= rootCount) return std::nullopt;
    return detail::Join(keptParts, rootCount, keptParts.size());
}

// Maps `path` from below `fromRoot` to the same place below `toRoot`.
inline std::optional<std::filesystem::path> Rebase(const std::filesystem::path& path,
                                                   const std::filesystem::path& fromRoot,
                                                   const std::filesystem::path& toRoot) {
    if (toRoot.empty()) return std::nullopt;
    const auto rel = RelativeUnder(path, fromRoot);
    if (!rel) return std::nullopt;
    return rel->empty() ? toRoot : toRoot / *rel;
}

struct Relocation {
    std::filesystem::path recordedRoot;  // workspace root when the record was written
    std::filesystem::path currentRoot;   // workspace root that holds the record now
    bool moved = false;
};

// Compares where a legacy record says one of its own files is with where that
// file actually is. Both must name the same file below "__pdf_note_workspace__"
// (same tail, compared case-insensitively). nullopt means the recorded path
// does not have that shape, so no mapping may be inferred from it.
inline std::optional<Relocation> DetectFromSelfPath(const std::filesystem::path& recordedSelf,
                                                    const std::filesystem::path& actualSelf) {
    const auto recordedKey = identity_path_scope::LocalPath(recordedSelf.wstring());
    const auto actualKey = identity_path_scope::LocalPath(actualSelf.wstring());
    const auto recordedKept = detail::CaseKeptLocal(recordedSelf);
    const auto actualKept = detail::CaseKeptLocal(actualSelf);
    if (!recordedKey || !actualKey || !recordedKept || !actualKept) return std::nullopt;
    const auto recorded = detail::Parts(*recordedKey);
    const auto actual = detail::Parts(*actualKey);
    const std::filesystem::path managed(kManagedRootName);  // already lowercase, like LocalPath keys
    size_t managedIndex = actual.size();
    for (size_t i = actual.size(); i > 0; --i) {
        if (actual[i - 1] == managed) {
            managedIndex = i - 1;
            break;
        }
    }
    // Index 0 and 1 are the drive and its root directory; the workspace root
    // needs at least one folder, and the managed root needs a tail below it.
    if (managedIndex == actual.size() || managedIndex < 3 || managedIndex + 1 >= actual.size()) {
        return std::nullopt;
    }
    const size_t tailCount = actual.size() - managedIndex;
    if (recorded.size() < tailCount + 3) return std::nullopt;
    const size_t recordedManagedIndex = recorded.size() - tailCount;
    for (size_t i = 0; i < tailCount; ++i) {
        if (recorded[recordedManagedIndex + i] != actual[managedIndex + i]) return std::nullopt;
    }
    Relocation out;
    out.recordedRoot = detail::Join(detail::Parts(*recordedKept), 0, recordedManagedIndex);
    out.currentRoot = detail::Join(detail::Parts(*actualKept), 0, managedIndex);
    out.moved = !SameLocation(out.recordedRoot, out.currentRoot);
    return out;
}

// Serializes `path` in record path form.
inline std::wstring ToRecordPath(const std::filesystem::path& path,
                                 const std::filesystem::path& workspaceRoot,
                                 const std::vector<std::filesystem::path>& externalRoots = {}) {
    if (!workspaceRoot.empty()) {
        const auto rel = RelativeUnder(path, workspaceRoot);
        if (rel && !rel->empty() && identity_path_scope::SafeRelative(*rel)) return rel->wstring();
    }
    const auto kept = detail::CaseKeptLocal(path);
    if (kept) {
        for (const auto& root : externalRoots) {
            const auto rel = RelativeUnder(*kept, root);
            if (rel && !rel->empty()) {
                return std::wstring(identity_path_scope::kExternalPrefix) + kept->wstring();
            }
        }
    }
    return path.wstring();
}

// Resolves a record path written by ToRecordPath, or a legacy absolute path.
// Relative values must stay inside the workspace; nullopt rejects the value.
// An external value is only parsed here: whether its folder is still
// registered is for the caller's write-permission check to decide.
inline std::optional<std::filesystem::path> FromRecordPath(const std::wstring& value,
                                                           const std::filesystem::path& workspaceRoot) {
    if (value.empty()) return std::nullopt;
    if (identity_path_scope::IsExternal(value)) {
        const std::filesystem::path raw(value.substr(identity_path_scope::kExternalPrefix.size()));
        const auto kept = detail::CaseKeptLocal(raw);
        if (!kept || kept->relative_path().empty()) return std::nullopt;
        return kept;
    }
    const std::filesystem::path recorded(value);
    if (recorded.is_absolute()) return recorded;
    if (!identity_path_scope::SafeRelative(recorded)) return std::nullopt;
    const auto root = detail::CaseKeptLocal(workspaceRoot);
    if (!root) return std::nullopt;
    return *root / recorded;
}

// A record's own file (a backup next to its manifest) is stored by name only.
inline std::optional<std::filesystem::path> FromOwnFileName(const std::wstring& value,
                                                            const std::filesystem::path& recordDir) {
    const std::filesystem::path name(value);
    if (recordDir.empty() || !identity_path_scope::SafeRelative(name) ||
        std::distance(name.begin(), name.end()) != 1) {
        return std::nullopt;
    }
    return recordDir / name;
}

}  // namespace workspace_relocation
