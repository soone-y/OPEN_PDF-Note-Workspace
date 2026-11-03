#include "search/note_search_worker.h"
#include <windows.h>

namespace search {

NoteSearchWorker::NoteSearchWorker(note::NoteWorkspaceService& service)
    : service_(service), thread_(&NoteSearchWorker::Run, this) {}

NoteSearchWorker::~NoteSearchWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        if (pending_) pending_->completion->canceled = true;
        if (running_) running_->canceled = true;
        pending_.reset();
    }
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
}

std::shared_ptr<NoteSearchCompletion> NoteSearchWorker::Submit(NoteSearchInput input) {
    auto completion = std::make_shared<NoteSearchCompletion>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_) pending_->completion->canceled = true;
        if (running_) running_->canceled = true;
        pending_ = Task{std::move(input), completion};
    }
    wake_.notify_one();
    return completion;
}

void NoteSearchWorker::Run() noexcept {
    // One worker, below interactive work; do not change process priority.
    (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        std::optional<Task> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            if (stopping_) return;
            task = std::move(pending_);
            pending_.reset();
            running_ = task->completion;
        }
        auto& result = *task->completion;
        std::shared_ptr<const note::WorkspaceNoteIndexSnapshot> index;
        std::vector<note::WorkspaceNoteLineMatch> matches;
        bool failed = false;
        try {
            if (!result.canceled) {
                auto& input = task->input;
                index = service_.ResolveIndex(input.identity, std::move(input.path),
                    std::move(input.metadata), std::move(input.text), input.kind);
                if (index && !result.canceled) {
                    matches = note::SearchWorkspaceNoteIndex(
                        *index, input.terms, input.options, &result.canceled);
                }
            }
        } catch (...) {
            failed = true; // Never unwind out of a worker thread.
        }
        {
            std::lock_guard<std::mutex> lock(result.mutex);
            if (!result.canceled) {
                result.index = std::move(index);
                result.matches = std::move(matches);
            }
            result.failed = failed;
            result.ready = true;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_.reset();
        }
    }
}

NoteSearchWorker& BackgroundNoteSearch() {
    auto& service = note::RuntimeNoteWorkspaceService();
    static NoteSearchWorker worker(service);
    return worker;
}

} // namespace search
