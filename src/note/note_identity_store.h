#pragma once

#include "note/note_identity.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace note {

struct PersistentNoteIdentityRecord {
    NoteId note_id{};
    uint64_t persistence_revision = 0;
    uint64_t disk_fingerprint = 0;
};

class PersistentNoteIdentityCatalog {
public:
    void Reset(uint64_t workspaceNonce);
    bool Parse(std::string_view bytes, std::wstring* outError = nullptr);
    std::string Serialize() const;

    std::optional<NoteId> FindPath(std::string_view relativePathKeyUtf8) const;
    std::optional<std::string> FindNote(NoteId noteId) const;
    std::optional<PersistentNoteIdentityRecord> FindRecord(NoteId noteId) const;
    NoteId AllocateNoteId();
    bool BindPath(NoteId noteId, std::string relativePathKeyUtf8);
    bool RebindPath(NoteId noteId, std::string relativePathKeyUtf8);
    std::optional<uint64_t> ObserveDisk(NoteId noteId, uint64_t contentFingerprint);
    bool CommitPersistence(NoteId noteId,
                           uint64_t expectedBaseRevision,
                           uint64_t committedRevision,
                           uint64_t contentFingerprint);

    uint64_t workspace_nonce() const { return workspace_nonce_; }
    uint64_t next_sequence() const { return next_sequence_; }
    size_t size() const { return records_by_path_.size(); }
    const std::map<std::string, PersistentNoteIdentityRecord>& records() const {
        return records_by_path_;
    }

private:
    uint64_t workspace_nonce_ = 0;
    uint64_t next_sequence_ = 1;
    std::map<std::string, PersistentNoteIdentityRecord> records_by_path_;
    std::map<uint64_t, std::string> path_by_note_id_;
};

bool ConfigureRuntimeNoteIdentityStore(const std::filesystem::path& workspaceRoot,
                                       std::wstring* outError = nullptr);
NoteIdentity ResolveRuntimeNoteIdentityPath(const std::wstring& absolutePath,
                                            std::wstring* outError = nullptr);
bool RebindRuntimeNoteIdentityPath(NoteId noteId,
                                   const std::wstring& oldAbsolutePath,
                                   const std::wstring& newAbsolutePath,
                                   std::wstring* outError = nullptr);
uint64_t ObserveRuntimeNoteDiskSnapshot(NoteId noteId,
                                        uint64_t contentFingerprint,
                                        std::wstring* outError = nullptr);
std::optional<PersistentNoteIdentityRecord> FindRuntimeNotePersistenceRecord(
    NoteId noteId,
    std::wstring* outError = nullptr);
bool CommitRuntimeNotePersistence(NoteId noteId,
                                  uint64_t expectedBaseRevision,
                                  uint64_t committedRevision,
                                  uint64_t contentFingerprint,
                                  std::wstring* outError = nullptr);
std::filesystem::path RuntimeNoteIdentityStorePath();

} // namespace note
