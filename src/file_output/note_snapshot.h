// Note text snapshot resolution shared by note loading, search, link lookup, and export.
// The resolver returns UTF-8 bytes; callers decide how to decode or parse them.
#pragma once

#include "core/path_safety.h"
#include "file_output/file_output.h"
#include "note/note_identity_store.h"
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace note_snapshot {

enum class TextSnapshotReadPolicy {
    DiskAndStage,
    LatestOnly
};

enum class TextSnapshotSource {
    None,
    CurrentEdit,
    Disk,
    Stage
};

struct CurrentEditTextSnapshot {
    bool available = false;
    std::wstring targetPath;
    note::SnapshotIdentity identity{};
    std::string bytes;
};

struct TextStorageSnapshot {
    note::NoteIdentity noteIdentity{};
    bool diskOk = false;
    uint64_t diskPersistenceRevision = 0;
    std::string diskBytes;
    std::filesystem::path stagePath;
    bool hasStage = false;
    bool stageResolved = false;
    uint64_t stageContentRevision = 0;
    uint64_t stagePersistenceRevision = 0;
    std::optional<file_output::StagedNoteRestoreViewState> stageRestoreView;
    std::string stageBytes;
    std::wstring stageError;
};

struct LatestTextSnapshot {
    bool ok = false;
    TextSnapshotSource source = TextSnapshotSource::None;
    std::wstring targetPath;
    note::SnapshotIdentity identity{};
    std::string bytes;
    std::filesystem::path stagePath;
    std::wstring error;
};

inline bool IsStageSource(TextSnapshotSource source) {
    return source == TextSnapshotSource::Stage;
}

inline const wchar_t* TextSnapshotSourceLabel(TextSnapshotSource source) {
    switch (source) {
    case TextSnapshotSource::CurrentEdit:
        return L"current";
    case TextSnapshotSource::Disk:
        return L"disk";
    case TextSnapshotSource::Stage:
        return L"stage";
    case TextSnapshotSource::None:
    default:
        return L"";
    }
}

inline const wchar_t* LatestTextSnapshotMetaLabel(const LatestTextSnapshot& snapshot) {
    return (snapshot.source == TextSnapshotSource::CurrentEdit ||
            snapshot.source == TextSnapshotSource::Stage)
        ? TextSnapshotSourceLabel(snapshot.source)
        : L"";
}

inline TextStorageSnapshot LoadTextStorageSnapshot(const std::wstring& path,
                                                   TextSnapshotReadPolicy policy) {
    TextStorageSnapshot snapshot;
    snapshot.noteIdentity = note::ResolveRuntimeNoteIdentityPath(path);
    const auto observeDisk = [&snapshot]() {
        if (!snapshot.diskOk || !snapshot.noteIdentity.note_id.valid()) return;
        snapshot.diskPersistenceRevision = note::ObserveRuntimeNoteDiskSnapshot(
            snapshot.noteIdentity.note_id,
            note::FingerprintSnapshotBytes(snapshot.diskBytes));
    };
    if (policy == TextSnapshotReadPolicy::DiskAndStage) {
        snapshot.diskOk = ReadFileBytesWin32(std::filesystem::path(path), snapshot.diskBytes);
        observeDisk();
    }

    const auto stageRef = file_output::FindLatestStagedNoteSnapshotFor(path);
    if (stageRef.has_value()) {
        snapshot.stagePath = stageRef->stagePath;
        snapshot.stageContentRevision = stageRef->contentRevision;
        snapshot.stagePersistenceRevision = stageRef->persistenceRevision;
        snapshot.stageRestoreView = stageRef->restoreView;
    }
    snapshot.hasStage = stageRef.has_value() && stageRef->valid();
    if (snapshot.hasStage) {
        if (stageRef->noteId != 0 &&
            stageRef->noteId != snapshot.noteIdentity.note_id.value) {
            snapshot.stageError = L"note stage の永続IDが対象ノートと一致しません。";
        } else {
            snapshot.stageResolved =
                file_output::LoadResolvedStagedNoteBytes(path,
                                                         snapshot.stagePath,
                                                         &snapshot.stageBytes,
                                                         &snapshot.stageError);
        }
    }
    if (policy == TextSnapshotReadPolicy::LatestOnly && !snapshot.hasStage) {
        snapshot.diskOk = ReadFileBytesWin32(std::filesystem::path(path), snapshot.diskBytes);
        observeDisk();
    }
    return snapshot;
}

inline LatestTextSnapshot ResolveLatestTextSnapshot(const std::wstring& path,
                                                    TextStorageSnapshot snapshot) {
    LatestTextSnapshot result;
    result.targetPath = path;
    result.identity.note_id = snapshot.noteIdentity.note_id;
    if (snapshot.hasStage) {
        if (!snapshot.stageResolved) {
            result.error = snapshot.stageError;
            result.stagePath = std::move(snapshot.stagePath);
            return result;
        }
        result.ok = true;
        result.source = TextSnapshotSource::Stage;
        result.bytes = std::move(snapshot.stageBytes);
        result.stagePath = std::move(snapshot.stagePath);
        result.identity = note::BuildSnapshotIdentity(
            snapshot.noteIdentity.note_id,
            snapshot.stageContentRevision,
            snapshot.stagePersistenceRevision,
            result.bytes);
        return result;
    }
    if (!snapshot.diskOk) {
        result.error = L"note text の読み込みに失敗しました。";
        return result;
    }
    result.ok = true;
    result.source = TextSnapshotSource::Disk;
    result.bytes = std::move(snapshot.diskBytes);
    result.identity = note::BuildSnapshotIdentity(
        snapshot.noteIdentity.note_id, 0, snapshot.diskPersistenceRevision, result.bytes);
    return result;
}

inline LatestTextSnapshot LoadLatestTextSnapshot(const std::wstring& path) {
    return ResolveLatestTextSnapshot(
        path, LoadTextStorageSnapshot(path, TextSnapshotReadPolicy::LatestOnly));
}

inline std::optional<LatestTextSnapshot> ResolveCurrentEditTextSnapshot(
    const std::wstring& path,
    const note::NoteIdentity& targetIdentity,
    const CurrentEditTextSnapshot& currentEdit) {
    if (!currentEdit.available || currentEdit.targetPath.empty() ||
        NormalizePathKey(currentEdit.targetPath) != NormalizePathKey(path) ||
        (currentEdit.identity.note_id.valid() &&
         currentEdit.identity.note_id != targetIdentity.note_id)) {
        return std::nullopt;
    }
    LatestTextSnapshot result;
    result.ok = true;
    result.source = TextSnapshotSource::CurrentEdit;
    result.targetPath = currentEdit.targetPath;
    result.bytes = currentEdit.bytes;
    result.identity = note::BuildSnapshotIdentity(
        targetIdentity.note_id,
        currentEdit.identity.content_revision,
        currentEdit.identity.persistence_revision,
        result.bytes);
    return result;
}

inline LatestTextSnapshot LoadLatestTextSnapshot(const std::wstring& path,
                                                 const CurrentEditTextSnapshot& currentEdit) {
    const note::NoteIdentity targetIdentity =
        note::ResolveRuntimeNoteIdentityPath(path);
    if (auto result = ResolveCurrentEditTextSnapshot(path, targetIdentity, currentEdit)) {
        return std::move(*result);
    }
    return LoadLatestTextSnapshot(path);
}

inline bool LoadLatestTextBytes(const std::wstring& path,
                                std::string* outBytes,
                                std::wstring* outErr = nullptr) {
    if (outBytes) outBytes->clear();
    if (outErr) outErr->clear();

    LatestTextSnapshot snapshot = LoadLatestTextSnapshot(path);
    if (!snapshot.ok) {
        if (outErr) *outErr = std::move(snapshot.error);
        return false;
    }
    if (outBytes) *outBytes = std::move(snapshot.bytes);
    return true;
}

inline bool LoadStagedTextBytes(const std::wstring& path,
                                const std::filesystem::path& stagePath,
                                std::string* outBytes,
                                std::wstring* outErr = nullptr) {
    if (outBytes) outBytes->clear();
    if (outErr) outErr->clear();
    if (path.empty() || stagePath.empty()) {
        if (outErr) *outErr = L"note stage が見つかりません。";
        return false;
    }
    return file_output::LoadResolvedStagedNoteBytes(path, stagePath, outBytes, outErr);
}

} // namespace note_snapshot
