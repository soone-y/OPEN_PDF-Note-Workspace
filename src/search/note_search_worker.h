#pragma once

#include "note/note_workspace_service.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace search {

// Immutable UI-captured input. The worker never reads editors, paths from UI
// globals, stage registries or HWNDs, and never writes user files.
struct NoteSearchInput {
    note::SnapshotIdentity identity;
    std::wstring path;
    note::NoteMetadata metadata;
    std::wstring text;
    note::NoteContentKind kind = note::NoteContentKind::Markdown;
    note::NoteQueryGroups terms;
    note::SemanticSearchOptions options;
};

struct NoteSearchCompletion {
    std::atomic<bool> canceled{false};
    std::mutex mutex;
    bool ready = false;
    bool failed = false;
    std::shared_ptr<const note::WorkspaceNoteIndexSnapshot> index;
    std::vector<note::WorkspaceNoteLineMatch> matches;
};

class NoteSearchWorker {
public:
    explicit NoteSearchWorker(note::NoteWorkspaceService& service);
    ~NoteSearchWorker();
    NoteSearchWorker(const NoteSearchWorker&) = delete;
    NoteSearchWorker& operator=(const NoteSearchWorker&) = delete;
    [[nodiscard]] std::shared_ptr<NoteSearchCompletion> Submit(NoteSearchInput input);
private:
    void Run() noexcept;
    struct Task {
        NoteSearchInput input;
        std::shared_ptr<NoteSearchCompletion> completion;
    };
    note::NoteWorkspaceService& service_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::optional<Task> pending_;
    std::shared_ptr<NoteSearchCompletion> running_;
    std::thread thread_;
};

// The service is constructed first, so it outlives the joined worker at exit.
NoteSearchWorker& BackgroundNoteSearch();

} // namespace search
