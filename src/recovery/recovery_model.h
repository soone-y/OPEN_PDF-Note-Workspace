#pragma once

#include "core/annot_stage_replay.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace recovery {

// Structured results belong to recovery only. Normal stage readers must still
// publish nothing unless every operation has passed its original preconditions.
enum class Failure {
    None, Metadata, Unsupported, Read, Parse, TargetMismatch,
    AnnotationConflict, NoteConflict, MissingData, DestinationChanged,
    Backup, Write, Commit, Unknown
};
enum class Choice { Keep, Apply };

struct Utf8ByteOffset { size_t value = 0; };

struct NoteEdit {
    Utf8ByteOffset byteOffset;
    std::string before;
    std::string after;
};

struct Inspection {
    Failure failure = Failure::None;
    std::filesystem::path stagePath;
    std::filesystem::path targetPath;
    std::filesystem::path destination;
    bool isNote = false;
    bool hasCandidate = false;
    bool incomplete = false;
    std::vector<Annotation> annotations;
    std::string noteBytes;
    std::vector<AnnotCommand> remainingAnnotations;
    std::vector<NoteEdit> remainingNoteEdits;
    std::vector<std::filesystem::path> artifacts;
    AnnotNumericPrecision precision = AnnotNumericPrecision::RoundTrip17;
    std::optional<annot_stage_replay::State> replayState;
    std::wstring diagnostic; // For local diagnostics, never a UI description.
};

// One candidate owns its replay and undo history independently of normal edits.
// Explicit Keep/Apply affects only the current operation; every successor is
// rechecked. No fuzzy matching, silent skipping, file I/O, or global state.
class AnnotationPlan {
public:
    AnnotationPlan(std::vector<Annotation> initial,
                   AnnotNumericPrecision precision,
                   std::vector<AnnotCommand> remaining,
                   std::optional<annot_stage_replay::State> replayState = std::nullopt);
    [[nodiscard]] bool Advance();
    [[nodiscard]] bool Resolve(Choice choice);
    [[nodiscard]] bool Replace(const std::vector<Annotation>& annotations);
    [[nodiscard]] bool Undo();
    [[nodiscard]] bool Redo();
    const std::vector<Annotation>& candidate() const { return candidate_; }
    const AnnotCommand* pending() const;
    bool complete() const { return cursor_ == commands_.size(); }
    size_t cursor() const { return cursor_; }
private:
    struct Version {
        std::vector<Annotation> annotations;
        annot_stage_replay::State replay;
        size_t cursor;
    };
    void Remember();
    std::vector<Annotation> candidate_;
    annot_stage_replay::State replay_;
    std::vector<AnnotCommand> commands_;
    size_t cursor_ = 0;
    std::vector<Version> undo_;
    std::vector<Version> redo_;
};

[[nodiscard]] bool ApplyNoteEdit(std::string* bytes, const NoteEdit& edit);
// The UI supplies an explicitly chosen byte position, never a guessed match.
[[nodiscard]] bool ResolveNoteEdit(std::string* bytes, const NoteEdit& edit,
                                 Utf8ByteOffset chosenByteOffset, Choice choice);

} // namespace recovery
