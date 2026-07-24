#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "note/note_export.h"
#include "note/note_dirty_graph.h"
#include "note/note_identity.h"
#include "note/note_identity_store.h"
#include "note/note_layout.h"
#include "note/note_kernel.h"
#include "note/note_math.h"
#include "note/note_parser.h"
#include "note/note_persistence.h"
#include "note/note_presentation.h"
#include "note/note_revision_gate.h"
#include "note/note_semantic_index.h"
#include "note/note_transaction.h"
#include "note/note_text_core.h"
#include "note/note_text_boundaries.h"
#include "note/note_workspace_index.h"
#include "note/note_workspace_service.h"
#include "file_output/note_snapshot.h"
#include "math/math_render.h"
#include "app/main_close_policy.h"
#include "core/setup_json_policy.h"
#include "core/cache_dir_policy.h"

std::wstring UTF8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string TestWideToUTF8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        out.data(), length, nullptr, nullptr);
    return out;
}

namespace note {

} // namespace note

namespace {

int g_failed = 0;

void Expect(bool condition, const char* message) {
    if (condition) {
        std::cout << "[PASS] " << message << "\n";
        return;
    }
    std::cout << "[FAIL] " << message << "\n";
    ++g_failed;
}

const note::Diagnostic* FindDiagnostic(const note::NoteDocument& doc, std::wstring_view code) {
    for (const auto& diag : doc.diagnostics) {
        if (diag.code == code) {
            return &diag;
        }
    }
    return nullptr;
}

const note::StyleSpan* FindStyleSpan(const note::NoteDocument& doc,
                                     note::StyleKind kind,
                                     std::wstring_view value = {}) {
    for (const auto& span : doc.style_spans) {
        if (span.kind != kind) continue;
        if (!value.empty() && span.value != value) continue;
        return &span;
    }
    return nullptr;
}

const note::BlockNode* FindBlock(const note::NoteDocument& doc,
                                 note::BlockKind kind,
                                 int ordinal = 0) {
    for (const auto& block : doc.blocks) {
        if (block.kind != kind) continue;
        if (ordinal == 0) {
            return &block;
        }
        --ordinal;
    }
    return nullptr;
}

const note::InlineNode* FindInline(const note::NoteDocument& doc,
                                   note::InlineKind kind,
                                   int ordinal = 0) {
    for (const auto& inlineNode : doc.inlines) {
        if (inlineNode.kind != kind) continue;
        if (ordinal == 0) {
            return &inlineNode;
        }
        --ordinal;
    }
    return nullptr;
}

bool HasError(const note::MathInputAnalysis& analysis, std::wstring_view code) {
    for (const auto& diag : analysis.diagnostics) {
        if (diag.severity == note::DiagnosticSeverity::Error && diag.code == code) {
            return true;
        }
    }
    return false;
}

bool ContainsMathNodeType(const mathrender::Node* node, mathrender::Node::Type type) {
    if (!node) return false;
    if (node->type == type) return true;
    if (ContainsMathNodeType(node->a.get(), type)) return true;
    if (ContainsMathNodeType(node->b.get(), type)) return true;
    if (ContainsMathNodeType(node->super.get(), type)) return true;
    if (ContainsMathNodeType(node->sub.get(), type)) return true;
    for (const auto& row : node->rows) {
        for (const auto& cell : row) {
            if (ContainsMathNodeType(cell.get(), type)) return true;
        }
    }
    return false;
}

note::NoteDocument ParseMd4c(std::wstring text) {
    note::NoteMetadata meta;
    meta.file_name = L"note.md";
    note::NoteTextModel model = note::MakeNoteTextModel(std::move(meta), std::move(text), 1);
    return note::ParseNoteDocument(model);
}

std::pair<note::NoteTextModel, note::NoteDocument> BuildMd4cModelAndDoc(std::wstring text) {
    note::NoteMetadata meta;
    meta.file_name = L"note.md";
    note::NoteTextModel model = note::MakeNoteTextModel(std::move(meta), std::move(text), 1);
    note::NoteDocument doc = note::ParseNoteDocument(model);
    return {std::move(model), std::move(doc)};
}

bool ApplyTextEditKeepsLineStartsInSync(std::wstring text, note::TextEdit edit) {
    note::NoteMetadata meta;
    meta.file_name = L"note.md";
    note::NoteTextModel model = note::MakeNoteTextModel(std::move(meta), std::move(text), 1);
    note::ApplyTextEdit(&model, edit);
    return model.line_starts == note::BuildLineStarts(model.raw);
}

} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    {
        const std::wstring text = L"abc\r\nかな、e\x0301\xD83D\xDE00";
        const auto units = note::BuildTextUnits(text);
        Expect(units.size() == 9, "text boundaries split character-type runs into grapheme-safe units");
        Expect(units[3].klass == note::TextUnitClass::LineBreak &&
                   units[3].span.start.value == 3 && units[3].span.end.value == 5,
               "text boundaries keep CRLF as one deletion unit");
        Expect(units[7].span.end.value - units[7].span.start.value == 2,
               "text boundaries keep combining marks with their base character");
        Expect(units[8].span.end.value - units[8].span.start.value == 2,
               "text boundaries keep surrogate pairs intact");
        const note::Span backward = note::PreviousTextUnitRun(text, {7});
        Expect(backward.start.value == 5 && backward.end.value == 7,
               "backward character-type deletion restores the exact preceding run");
        const note::Span forward = note::NextTextUnitRun(text, {7});
        Expect(forward.start.value == 7 && forward.end.value == 8,
               "forward character-type deletion stops at punctuation boundaries");
    }

    {
        note::NoteIdentityRegistry registry;
        const note::NoteIdentity first = registry.ResolvePath(L"c:\\notes\\first.md");
        const note::NoteIdentity firstAgain = registry.ResolvePath(L"c:\\notes\\first.md");
        const note::NoteIdentity second = registry.ResolvePath(L"c:\\notes\\second.md");
        Expect(first.valid() && first.note_id == firstAgain.note_id,
               "note identity registry resolves one runtime id per normalized path");
        Expect(second.valid() && second.note_id != first.note_id,
               "note identity registry separates different notes");

        const note::NoteIdentity transient = registry.CreateTransient();
        Expect(transient.valid() && transient.transient,
               "note identity registry creates pathless transient notes");
        Expect(registry.BindPath(transient.note_id, L"c:\\notes\\created.md"),
               "transient note can acquire a path without changing note id");
        const auto created = registry.FindPath(L"c:\\notes\\created.md");
        Expect(created.has_value() && created->note_id == transient.note_id &&
                   !created->transient,
               "bound path resolves to the original transient note identity");
        Expect(!registry.BindPath(second.note_id, L"c:\\notes\\first.md"),
               "path alias cannot be stolen from a different note identity");
        Expect(registry.BindPath(first.note_id, L"c:\\notes\\renamed.md") &&
                   !registry.FindPath(L"c:\\notes\\first.md").has_value(),
               "renaming a note rebinds its path alias without retaining the old alias");
        const auto renamed = registry.FindPath(L"c:\\notes\\renamed.md");
        Expect(renamed.has_value() && renamed->note_id == first.note_id,
               "renamed path preserves the runtime note identity");

        const note::ViewIdentity view = registry.CreateView(first.note_id);
        const auto rebound = registry.RebindView(view.view_id, second.note_id);
        Expect(view.valid() && rebound.has_value() &&
                   rebound->view_id == view.view_id &&
                   rebound->note_id == second.note_id &&
                   rebound->binding_revision == view.binding_revision + 1,
               "view identity survives note rebinding and advances its binding revision");
        registry.ForgetView(view.view_id);
        Expect(!registry.FindView(view.view_id).has_value(),
               "forgotten view identity cannot retain stale presentation state");

        const note::SnapshotIdentity firstRevision =
            note::BuildSnapshotIdentity(first.note_id, 7, 2, "line1\r\nline2");
        const note::SnapshotIdentity sameRevision =
            note::BuildSnapshotIdentity(first.note_id, 7, 2, "\xEF\xBB\xBFline1\nline2");
        const note::SnapshotIdentity newerRevision =
            note::BuildSnapshotIdentity(first.note_id, 8, 2, "line1\nline2");
        const note::SnapshotIdentity changedContent =
            note::BuildSnapshotIdentity(first.note_id, 8, 2, "changed");
        Expect(note::SameSnapshotIdentity(firstRevision, sameRevision),
               "snapshot fingerprint canonicalizes UTF-8 BOM and line endings");
        Expect(!note::SameSnapshotIdentity(firstRevision, newerRevision) &&
                   note::SameSnapshotOwner(firstRevision, newerRevision) &&
                   note::SameSnapshotContent(firstRevision, newerRevision),
               "snapshot owner remains stable while content revision advances");
        Expect(!note::SameSnapshotContent(firstRevision, changedContent),
               "snapshot content comparison rejects changed text for the same note");

        note::SemanticLinkTargetResolution linkTarget;
        linkTarget.target_path = L"c:\\notes\\renamed.md";
        linkTarget.snapshot_identity = firstRevision;
        linkTarget.anchor_position = 12;
        Expect(note::SemanticLinkTargetMatchesSnapshot(linkTarget, newerRevision),
               "resolved link target accepts the same canonical note content");
        Expect(!note::SemanticLinkTargetMatchesSnapshot(linkTarget, changedContent),
               "resolved link target rejects a changed target snapshot");

        note_snapshot::TextStorageSnapshot diskStorage;
        diskStorage.noteIdentity = first;
        diskStorage.diskOk = true;
        diskStorage.diskBytes = "disk text";
        const note_snapshot::LatestTextSnapshot diskSnapshot =
            note_snapshot::ResolveLatestTextSnapshot(
                L"c:\\notes\\renamed.md", std::move(diskStorage));
        Expect(diskSnapshot.ok &&
                   diskSnapshot.source == note_snapshot::TextSnapshotSource::Disk &&
                   diskSnapshot.identity.note_id == first.note_id &&
                   diskSnapshot.identity.content_fingerprint != 0,
               "snapshot resolver labels disk source with note id and content fingerprint");

        note_snapshot::TextStorageSnapshot stageStorage;
        stageStorage.noteIdentity = first;
        stageStorage.diskOk = true;
        stageStorage.diskBytes = "old disk";
        stageStorage.hasStage = true;
        stageStorage.stageResolved = true;
        stageStorage.stageContentRevision = 17;
        stageStorage.stagePersistenceRevision = 42;
        stageStorage.stageBytes.clear();
        const note_snapshot::LatestTextSnapshot stageSnapshot =
            note_snapshot::ResolveLatestTextSnapshot(
                L"c:\\notes\\renamed.md", std::move(stageStorage));
        Expect(stageSnapshot.ok &&
                   stageSnapshot.source == note_snapshot::TextSnapshotSource::Stage &&
                   stageSnapshot.bytes.empty() &&
                   stageSnapshot.identity.content_revision == 17 &&
                   stageSnapshot.identity.persistence_revision == 42 &&
                   stageSnapshot.identity.content_fingerprint != 0,
               "snapshot resolver preserves empty staged text and persistence revision");

        note_snapshot::CurrentEditTextSnapshot wrongOwner;
        wrongOwner.available = true;
        wrongOwner.targetPath = L"c:\\notes\\renamed.md";
        wrongOwner.identity = note::BuildSnapshotIdentity(second.note_id, 9, 0, "edit");
        wrongOwner.bytes = "edit";
        Expect(!note_snapshot::ResolveCurrentEditTextSnapshot(
                    L"c:\\notes\\renamed.md", *renamed, wrongOwner).has_value(),
               "snapshot resolver rejects current edit owned by another note id");

        wrongOwner.identity = note::BuildSnapshotIdentity(first.note_id, 9, 0, "edit");
        const auto currentSnapshot = note_snapshot::ResolveCurrentEditTextSnapshot(
            L"C:\\NOTES\\RENAMED.MD", *renamed, wrongOwner);
        Expect(currentSnapshot.has_value() &&
                   currentSnapshot->source == note_snapshot::TextSnapshotSource::CurrentEdit &&
                   currentSnapshot->identity.content_revision == 9 &&
                   currentSnapshot->identity.note_id == first.note_id,
               "snapshot resolver accepts normalized-path current edit for the same note id");
    }

    {
        note::PersistentNoteIdentityCatalog catalog;
        catalog.Reset(0x123456789ABCDEF0ULL);
        const note::NoteId firstId = catalog.AllocateNoteId();
        const note::NoteId secondId = catalog.AllocateNoteId();
        Expect(firstId.valid() && secondId.valid() && firstId != secondId,
               "persistent identity catalog allocates distinct nonzero note ids");
        Expect(catalog.BindPath(firstId, "class/session/note-a.md") &&
                   catalog.BindPath(secondId, "class/session/note-b.md"),
               "persistent identity catalog binds workspace-relative aliases");
        Expect(catalog.FindRecord(firstId).has_value() &&
                   catalog.FindRecord(firstId)->persistence_revision == 0 &&
                   catalog.ObserveDisk(firstId, 0xA1ULL) == std::optional<uint64_t>(1) &&
                   catalog.ObserveDisk(firstId, 0xA1ULL) == std::optional<uint64_t>(1) &&
                   catalog.ObserveDisk(firstId, 0xB2ULL) == std::optional<uint64_t>(2),
               "persistent identity catalog advances only when disk content changes");
        Expect(catalog.CommitPersistence(firstId, 2, 3, 0xC3ULL) &&
                   !catalog.CommitPersistence(firstId, 1, 2, 0xD4ULL),
               "persistent identity catalog commits the next generation and rejects stale bases");

        const std::string serialized = catalog.Serialize();
        note::PersistentNoteIdentityCatalog restored;
        std::wstring parseError;
        Expect(restored.Parse(serialized, &parseError) &&
                   restored.workspace_nonce() == catalog.workspace_nonce() &&
                   restored.FindPath("class/session/note-a.md") == firstId &&
                   restored.FindNote(secondId) ==
                       std::optional<std::string>("class/session/note-b.md") &&
                   restored.FindRecord(firstId).has_value() &&
                   restored.FindRecord(firstId)->persistence_revision == 3 &&
                   restored.FindRecord(firstId)->disk_fingerprint == 0xC3ULL,
               "persistent identity catalog round-trips binary records and allocator state");
        Expect(restored.RebindPath(firstId, "class/session/renamed.md") &&
                   !restored.FindPath("class/session/note-a.md").has_value() &&
                   restored.FindPath("class/session/renamed.md") == firstId &&
                   restored.FindRecord(firstId)->persistence_revision == 3,
               "persistent identity catalog preserves note id across path rebind");
        Expect(!restored.BindPath(secondId, "class/session/renamed.md"),
               "persistent identity catalog rejects alias ownership collisions");
        Expect(!restored.RebindPath(firstId, "../outside.md") &&
                   !restored.BindPath(secondId, "class/session/bad:name.md"),
               "persistent identity catalog rejects traversal and invalid Windows paths");

        std::string corrupt = serialized;
        corrupt[corrupt.size() / 2] ^= 0x01;
        note::PersistentNoteIdentityCatalog rejected;
        Expect(!rejected.Parse(corrupt, &parseError),
               "persistent identity catalog rejects checksum-corrupted data");
    }

    {
        using Decision = note::NotePersistenceCommitDecision;
        const note::NoteId owner{0x9901ULL};
        const note::NotePersistenceCommitIntent intent{
            owner,
            7,
            8,
            0xC3ULL,
        };
        Expect(note::ResolveNotePersistenceCommit(
                   note::NotePersistenceObservation{owner, true, 7, 0xB2ULL},
                   intent) == Decision::ReadyToWrite,
               "persistence core allows a write only from the expected base generation");
        Expect(note::ResolveNotePersistenceCommit(
                   note::NotePersistenceObservation{owner, false, 0, 0},
                   note::NotePersistenceCommitIntent{owner, 0, 1, 0xA1ULL}) ==
                   Decision::ReadyToWrite,
               "persistence core permits creation from an empty generation");
        Expect(note::ResolveNotePersistenceCommit(
                   note::NotePersistenceObservation{owner, true, 8, 0xC3ULL},
                   intent) == Decision::AlreadyWritten,
               "persistence core recognizes an idempotent retry after the destination write");
        Expect(note::ResolveNotePersistenceCommit(
                   note::NotePersistenceObservation{owner, true, 9, 0xD4ULL},
                   intent) == Decision::Conflict &&
                   note::ResolveNotePersistenceCommit(
                       note::NotePersistenceObservation{owner, true, 8, 0xD4ULL},
                       intent) == Decision::Conflict &&
                   note::ResolveNotePersistenceCommit(
                       note::NotePersistenceObservation{owner, false, 8, 0},
                       intent) == Decision::Conflict,
               "persistence core rejects newer, divergent, and missing written destinations");
        Expect(note::ResolveNotePersistenceCommit(
                   note::NotePersistenceObservation{note::NoteId{2}, true, 7, 0xB2ULL},
                   intent) == Decision::InvalidInput &&
                   note::ResolveNotePersistenceCommit(
                       note::NotePersistenceObservation{owner, true, 7, 0xB2ULL},
                       note::NotePersistenceCommitIntent{owner, 7, 9, 0xC3ULL}) ==
                       Decision::InvalidInput &&
                   note::ResolveNotePersistenceCommit(
                       note::NotePersistenceObservation{owner, true, 7, 0xB2ULL},
                       note::NotePersistenceCommitIntent{owner, 7, 8, 0}) ==
                       Decision::InvalidInput &&
                   note::ResolveNotePersistenceCommit(
                       note::NotePersistenceObservation{owner, false, 7, 0xB2ULL},
                       intent) == Decision::InvalidInput,
               "persistence core fails closed for invalid ownership, generations, and observations");
        Expect(note::ResolveNotePersistenceCommit(
                   note::NotePersistenceObservation{
                       owner, true, std::numeric_limits<uint64_t>::max(), 0xB2ULL},
                   note::NotePersistenceCommitIntent{
                       owner,
                       std::numeric_limits<uint64_t>::max(),
                       1,
                       0xC3ULL}) == Decision::InvalidInput,
               "persistence core rejects generation overflow without mutation");
    }

    {
        note::NoteTransactionCore transactions;
        const note::NoteId owner{0x1234ULL};
        const auto load = transactions.Begin(
            note::NoteChangeOrigin::ProgramLoad, owner, true);
        const auto active = transactions.Snapshot();
        Expect(load.valid() && active.suppress_change &&
                   active.program_mutation_active && active.depth == 1 &&
                   active.origin == note::NoteChangeOrigin::ProgramLoad &&
                   active.owner_note_id == owner &&
                   !transactions.ObserveUserInput(),
               "note transaction keeps program load active despite input-like notifications");

        const auto presentation = transactions.Begin(
            note::NoteChangeOrigin::PresentationUpdate, owner, false);
        Expect(!transactions.Commit(load) &&
                   transactions.Snapshot().depth == 2 &&
                   transactions.Commit(presentation) &&
                   transactions.Snapshot().depth == 1,
               "note transaction enforces LIFO nesting for presentation updates");
        Expect(transactions.Commit(load) &&
                   transactions.Snapshot().waiting_for_user_input &&
                   transactions.ShouldSuppressChange(),
               "note transaction converts committed load into a user-input latch");
        Expect(transactions.ObserveUserInput() &&
                   !transactions.ShouldSuppressChange() &&
                   !transactions.ObserveUserInput(),
               "note transaction releases suppression only on explicit user input");

        const auto clear = transactions.Begin(
            note::NoteChangeOrigin::ProgramClear, owner, true);
        Expect(transactions.Commit(clear),
               "note transaction can arm a clear-operation latch");
        const auto nested = transactions.Begin(
            note::NoteChangeOrigin::PresentationUpdate, owner, false);
        Expect(transactions.Cancel(nested) &&
                   transactions.Snapshot().waiting_for_user_input,
               "cancelled nested transaction preserves the prior input latch");
    }

    {
        using Action = note::NotePresentationFrameAction;
        Expect(note::ResolveNotePresentationFrameAction({false, true, true, false, false}) ==
                   Action::RawFallback,
               "presentation policy uses raw fallback when render mode is disabled");
        Expect(note::ResolveNotePresentationFrameAction({true, true, true, false, false}) ==
                   Action::RenderCurrent,
               "presentation policy renders a current committed cache");
        Expect(note::ResolveNotePresentationFrameAction({true, true, false, true, false}) ==
                   Action::ReuseCommittedLayout,
               "presentation policy keeps committed layout for a same-line pending edit");
        Expect(note::ResolveNotePresentationFrameAction({true, true, false, true, true}) ==
                   Action::CommitBeforePaint,
               "presentation policy commits line-count changes before paint");
        Expect(note::ResolveNotePresentationFrameAction({true, true, false, false, true}) ==
                   Action::CommitBeforePaint,
               "presentation policy treats pending line-count repair as a paint barrier");
        Expect(note::ResolveNotePresentationFrameAction({true, true, false, true, false, true}) ==
                   Action::RawFallback,
               "presentation policy keeps IME composition on the raw edit surface when cache is stale");
        Expect(note::ResolveNotePresentationFrameAction({true, true, true, true, false, true}) ==
                   Action::RenderCurrent,
               "presentation policy can render the current cache while IME state is already synchronized");
        Expect(note::ResolveNotePresentationFrameAction({true, false, false, true, false}) ==
                   Action::RawFallback,
               "presentation policy refuses reuse without a committed cache");

        const note::NoteDerivedRefreshPlan lightweight =
            note::ResolveNoteDerivedRefreshPlan({false, false, false});
        const note::NoteDerivedRefreshPlan headings =
            note::ResolveNoteDerivedRefreshPlan({false, false, true});
        const note::NoteDerivedRefreshPlan render =
            note::ResolveNoteDerivedRefreshPlan({true, false, false});
        const note::NoteDerivedRefreshPlan mathPane =
            note::ResolveNoteDerivedRefreshPlan({false, true, false});
        Expect(lightweight.lightweight() && lightweight.refresh_assist &&
                   headings.refresh_syntax && !headings.refresh_render_plan &&
                   render.refresh_syntax && render.refresh_render_plan &&
                   mathPane.refresh_syntax && mathPane.refresh_render_plan,
               "derived refresh policy separates semantic panes from render-plan work");
    }

    {
        using Action = main_close_policy::CloseRequestAction;
        Expect(main_close_policy::ResolveCloseRequestAction({true, false, false, false}) ==
                   Action::IgnoreAlreadyExiting,
               "main close policy ignores duplicate close while exit is already in progress");
        Expect(main_close_policy::ResolveCloseRequestAction({false, true, false, false}) ==
                   Action::WaitForActiveSave,
               "main close policy waits for an active save before exiting");
        Expect(main_close_policy::ResolveCloseRequestAction({false, false, true, true}) ==
                   Action::BypassFinishedAutomation,
               "main close policy bypasses unattended staged-diff prompts after automation finishes");
        Expect(main_close_policy::ResolveCloseRequestAction({false, false, false, false}) ==
                   Action::RunExitFlow,
               "main close policy routes normal close requests through the exit flow");

        const auto failedExit = main_close_policy::ResolveExitFlowCompletion(false);
        Expect(!failedExit.destroy_window && !failedExit.exit_in_progress,
               "main close policy keeps the window open when exit save/cancel flow fails");
        const auto completedExit = main_close_policy::ResolveExitFlowCompletion(true);
        Expect(completedExit.destroy_window && completedExit.exit_in_progress,
               "main close policy destroys the window only after the exit flow succeeds");
    }

    {
        using Decision = setup_json_policy::AutoUpdateDecision;
        Expect(setup_json_policy::IsKnownTopLevelField("workspaceRoot") &&
                   setup_json_policy::IsKnownTopLevelField("annotToolModeOrder") &&
                   setup_json_policy::IsKnownTopLevelField("annotToolOrder"),
               "setup json policy recognizes current and legacy setup fields");
        Expect(!setup_json_policy::IsKnownTopLevelField("futureWorkspaceRootSchema"),
               "setup json policy treats unknown setup fields as preservation barriers");
        Expect(setup_json_policy::ResolveAutoUpdateDecision(true, true, true, false) ==
                   Decision::Allow,
               "setup json policy allows auto-update for known valid setup json");
        Expect(setup_json_policy::ResolveAutoUpdateDecision(false, true, true, false) ==
                   Decision::BlockReadFailure,
               "setup json policy blocks auto-update when setup json cannot be read");
        Expect(setup_json_policy::ResolveAutoUpdateDecision(true, false, true, false) ==
                   Decision::BlockInvalidJson,
               "setup json policy blocks auto-update for malformed setup json");
        Expect(setup_json_policy::ResolveAutoUpdateDecision(true, true, false, false) ==
                   Decision::BlockMissingWorkspaceRoot,
               "setup json policy blocks default overwrite when workspaceRoot is missing");
        Expect(setup_json_policy::ResolveAutoUpdateDecision(true, true, true, true) ==
                   Decision::BlockUnknownTopLevelField,
               "setup json policy blocks auto-update when unknown setup fields are present");
    }

    {
        using Decision = cache_dir_policy::CacheDirDecision;
        Expect(cache_dir_policy::ResolveCacheDirDecision(L"__resource__/__tmp__") ==
                   Decision::ManagedDefault &&
                   cache_dir_policy::ResolveCacheDirDecision(L"__resource__\\__tmp__") ==
                       Decision::ManagedDefault &&
                   cache_dir_policy::ResolveCacheDirDecision(L"") == Decision::ManagedDefault,
               "cache dir policy accepts only the reserved resource tmp cache as the default");
        Expect(cache_dir_policy::ResolveCacheDirDecision(L"bin") ==
                   Decision::UnsafeCustom &&
                   cache_dir_policy::ResolveCacheDirDecision(L"__cache__") ==
                       Decision::UnsafeCustom &&
                   cache_dir_policy::ResolveCacheDirDecision(L"somewhere/__cache__") ==
                       Decision::UnsafeCustom &&
                   cache_dir_policy::ResolveCacheDirDecision(L"__resource__/user-cache") ==
                       Decision::UnsafeCustom,
               "cache dir policy rejects user-like or arbitrary cache directories");
        Expect(cache_dir_policy::EffectiveCacheDir(L"bin") ==
                   std::wstring(cache_dir_policy::kManagedDefault) &&
                   cache_dir_policy::EffectiveCacheDir(L"__cache__") ==
                       std::wstring(cache_dir_policy::kManagedDefault),
               "cache dir policy always falls back to the reserved tmp root for runtime use");
    }


    {
        using Decision = note::NoteDerivedReadDecision;
        const note::NoteId owner{0x4401ULL};
        const note::NoteDerivedSnapshotState derived{
            note::NoteDerivedSnapshotIdentity{owner, 12}, true};
        const note::NoteDerivedReadContext current{owner, 12, false, false};
        Expect(note::ResolveNoteDerivedRead(derived, current) == Decision::Current &&
                   note::CanReadCurrentNoteDerivedSnapshot(derived, current),
               "revision gate accepts a present derived snapshot from the current owner and revision");
        Expect(note::ResolveNoteDerivedRead(
                   note::NoteDerivedSnapshotState{
                       note::NoteDerivedSnapshotIdentity{owner, 12}, false},
                   current) ==
                   Decision::Missing,
               "revision gate distinguishes an absent derived snapshot");
        Expect(note::ResolveNoteDerivedRead(
                   note::NoteDerivedSnapshotState{
                       note::NoteDerivedSnapshotIdentity{note::NoteId{0x4402ULL}, 12},
                       true},
                   current) == Decision::OwnerMismatch,
               "revision gate rejects another note even when revisions coincide");
        Expect(note::ResolveNoteDerivedRead(
                   note::NoteDerivedSnapshotState{
                       note::NoteDerivedSnapshotIdentity{owner, 11}, true},
                   current) ==
                   Decision::RevisionMismatch,
               "revision gate rejects an older derived revision");
        Expect(note::ResolveNoteDerivedRead(
                   derived,
                   note::NoteDerivedReadContext{owner, 12, true, false}) ==
                   Decision::BlockedByPendingEdit,
               "revision gate blocks stable reads while a content edit is pending");
        Expect(note::ResolveNoteDerivedRead(
                   derived,
                   note::NoteDerivedReadContext{owner, 12, false, true}) ==
                   Decision::BlockedByDeferredRefresh,
               "revision gate blocks stable reads while full derived refresh is deferred");
    }

    {
        const note::NoteId owner{0x4501ULL};
        const note::NoteDerivedSnapshotIdentity revision12{owner, 12};
        const note::NoteDerivedSnapshotIdentity revision13{owner, 13};
        std::vector<note::NoteLayoutLineMetrics> lines(3);
        lines[1].line_height_permille = 1450;
        lines[1].max_font_px = 28;
        lines[1].visual_ascent_px = 20;

        note::NoteLayoutMetricsSnapshot snapshot;
        snapshot.Commit(revision12, lines);
        Expect(snapshot.Matches(revision12) && !snapshot.Matches(revision13),
               "layout metrics snapshot requires exact owner and source revision for reads");
        Expect(note::CanAdvanceLayoutSnapshot(snapshot, revision13) &&
                   !note::CanAdvanceLayoutSnapshot(
                       snapshot,
                       note::NoteDerivedSnapshotIdentity{note::NoteId{0x4502ULL}, 13}) &&
                   !note::CanAdvanceLayoutSnapshot(
                       snapshot, note::NoteDerivedSnapshotIdentity{owner, 11}),
               "layout metrics can advance only to a newer revision of the same note");

        std::vector<note::NoteLayoutLineMetrics> visualOnly = lines;
        visualOnly[1].visual_descent_px = 9;
        Expect(note::SameParagraphSpacing(lines, visualOnly) &&
                   !note::SameVisualMetrics(lines, visualOnly),
               "layout metrics distinguish paragraph formatting from paint geometry");
        Expect(note::LayoutMetricsUnchangedOutsideRange(lines, visualOnly, 1, 1),
               "layout dirty-range check accepts changes confined to the requested line");
        visualOnly[2].max_font_px = 18;
        Expect(!note::LayoutMetricsUnchangedOutsideRange(lines, visualOnly, 1, 1),
               "layout dirty-range check rejects changes outside the requested line");

        snapshot.Reset();
        Expect(!snapshot.source_identity.valid() && snapshot.lines.empty(),
               "layout metrics reset clears identity and line records together");
    }

    {
        const note::NoteId owner{0x4601ULL};
        note::LocalNoteKernel kernel;
        kernel.Reset(owner,
                     note::NoteMetadata{L"kernel.md", L"kernel"},
                     L"# Head\n\nBody",
                     10,
                     3,
                     note::NoteContentKind::Markdown);
        Expect(kernel.valid() && !kernel.CanReadSyntax(),
               "local note kernel owns text before derived state is built");

        const note::NoteKernelRefreshResult full = kernel.RefreshDerived();
        Expect(full.kind == note::NoteKernelRefreshKind::Full &&
                   full.current && kernel.CanReadSyntax() &&
                   kernel.CanReadSemantic() &&
                   kernel.semantic_index().headings.size() == 1,
               "local note kernel atomically builds syntax and semantic snapshots");

        const size_t bodyEnd = kernel.text_core().model().raw.size();
        const note::NoteKernelApplyResult ordinary =
            kernel.Apply(note::TextEdit{bodyEnd, 0, L" text"}, true);
        Expect(ordinary.text_result == note::NoteTextApplyResult::Applied &&
                   ordinary.dirty_graph.has_edit &&
                   kernel.has_pending_edit() && !kernel.CanReadSyntax(),
               "local note kernel advances raw text and blocks stale derived reads");
        const note::NoteKernelRefreshResult incremental = kernel.RefreshDerived();
        Expect(incremental.kind == note::NoteKernelRefreshKind::Incremental &&
                   incremental.current &&
                   incremental.consumed_dirty_graph.has_value() &&
                   kernel.CanReadSemantic(),
               "local note kernel incrementally commits ordinary text edits");

        const size_t lineBreakAt = kernel.text_core().model().raw.size();
        kernel.Apply(note::TextEdit{lineBreakAt, 0, L"\n"}, true);
        const note::NoteKernelRefreshResult deferred = kernel.RefreshDerived();
        Expect(deferred.kind == note::NoteKernelRefreshKind::Deferred &&
                   kernel.has_deferred_full_refresh() &&
                   !kernel.CanReadSyntax() && !kernel.CanReadSemantic(),
               "local note kernel fails closed until a line-break repair parse commits");
        kernel.RequestFullRefresh();
        const note::NoteKernelRefreshResult repaired = kernel.RefreshDerived();
        Expect(repaired.kind == note::NoteKernelRefreshKind::Full &&
                   repaired.current && !kernel.has_deferred_full_refresh(),
               "local note kernel repairs deferred structure with one full snapshot commit");

        kernel.Apply(note::TextEdit{kernel.text_core().model().raw.size(), 0, L"a"}, true);
        kernel.Apply(note::TextEdit{kernel.text_core().model().raw.size(), 0, L"b"}, true);
        Expect(kernel.requires_full_refresh(),
               "local note kernel coalesces multiple pending edits into a full refresh barrier");
        const note::NoteKernelRefreshResult coalesced = kernel.RefreshDerived();
        const std::wstring& coalescedRaw = kernel.text_core().model().raw;
        Expect(coalesced.kind == note::NoteKernelRefreshKind::Full &&
                   coalesced.current &&
                   coalescedRaw.size() >= 2 &&
                   coalescedRaw.compare(coalescedRaw.size() - 2, 2, L"ab") == 0,
               "local note kernel rebuilds one coherent snapshot after pending edit coalescing");

        kernel.Reset(owner,
                     note::NoteMetadata{L"kernel.txt", L"kernel"},
                     L"plain",
                     30,
                     4,
                     note::NoteContentKind::PlainText);
        const note::NoteKernelRefreshResult plain = kernel.RefreshDerived();
        Expect(plain.kind == note::NoteKernelRefreshKind::Cleared &&
                   kernel.text_core().MatchesRaw(L"plain") &&
                   !kernel.CanReadSyntax(),
               "local note kernel keeps plain text canonical without fabricating syntax state");
    }

    {
        note::LocalNoteKernelRegistry registry;
        const note::NoteId first{0x4701ULL};
        const note::NoteId second{0x4702ULL};
        registry.Reset(first, note::NoteMetadata{}, L"first", 1, 0,
                       note::NoteContentKind::Markdown);
        registry.Reset(second, note::NoteMetadata{}, L"second", 1, 0,
                       note::NoteContentKind::Markdown);
        const note::ViewIdentity firstView{note::ViewId{0x91ULL}, first, 1};
        Expect(registry.FindForView(firstView) == registry.Find(first) &&
                   registry.Find(first) != registry.Find(second) &&
                   registry.size() == 2,
               "local note kernel registry isolates notes while sharing one kernel per view owner");
    }

    {
        const std::wstring before = L"alpha\nbeta";
        const std::optional<note::TextEdit> computed =
            note::ComputeNoteTextEdit(before, L"alpha\nXbeta");
        Expect(computed.has_value() && computed->start.value == 6 &&
                   computed->deleted_len == 0 && computed->inserted_text == L"X",
               "text model computes one canonical edit between raw snapshots");
        const std::vector<size_t> starts{0, 6};
        const note::TextEdit plain{2, 0, L"x"};
        const note::NoteDirtyGraph plainGraph =
            note::BuildNoteDirtyGraph(before, starts, plain, true);
        Expect(plainGraph.has_edit &&
                   plainGraph.edit_kind == note::NoteEditKind::PlainText &&
                   plainGraph.content_dirty && plainGraph.syntax_dirty &&
                   plainGraph.render_dirty && plainGraph.layout_dirty &&
                   !plainGraph.line_count_may_change &&
                   plainGraph.stale_lines.valid &&
                   plainGraph.stale_lines.first == 0 &&
                   plainGraph.stale_lines.last == 0,
               "dirty graph classifies plain insertion and limits stale lines");
        Expect(note::NoteDirtyGraphAllowsRenderEarlyStop(plainGraph, false) &&
                   note::NoteDirtyGraphAllowsLineSpacingFastPath(plainGraph),
               "dirty graph enables bounded render and spacing fast paths for short plain edits");

        const note::NoteDirtyGraph structuralGraph = note::BuildNoteDirtyGraph(
            before, starts, note::TextEdit{2, 0, L":"}, true);
        Expect(structuralGraph.edit_kind == note::NoteEditKind::StructuralText &&
                   structuralGraph.structure_dirty &&
                   note::HasNoteDirtySyntaxFeature(
                       structuralGraph.syntax_features,
                       note::NoteDirtySyntaxFeature::BlockStructure) &&
                   !structuralGraph.line_count_may_change &&
                   structuralGraph.stale_lines.first == 0 &&
                   structuralGraph.stale_lines.last == 0 &&
                   !note::NoteDirtyGraphAllowsRenderEarlyStop(structuralGraph, false),
               "dirty graph keeps same-line structural edits local without early stop");

        const note::NoteDirtyGraph newlineGraph = note::BuildNoteDirtyGraph(
            before, starts, note::TextEdit{2, 0, L"\r\n"}, true);
        Expect(newlineGraph.edit_kind == note::NoteEditKind::LineBreakOnly &&
                   newlineGraph.line_count_may_change &&
                   newlineGraph.stale_lines.first == 0 &&
                   newlineGraph.stale_lines.last == 1,
               "dirty graph expands line-count changes to a full committed mapping");

        const std::wstring crlfText = L"one\r\ntwo\r\nthree";
        const note::NoteDirtyGraph deleteCrlf = note::BuildNoteDirtyGraph(
            crlfText, {0, 5, 10}, note::TextEdit{3, 2, L""}, true);
        Expect(deleteCrlf.edit_kind == note::NoteEditKind::LineBreakOnly &&
                   deleteCrlf.line_count_may_change &&
                   deleteCrlf.stale_lines.last == 2,
               "dirty graph treats CRLF deletion as one logical line break");

        const note::TextEdit invalid{99, 1, L"x"};
        const note::NoteDirtyGraph invalidGraph =
            note::BuildNoteDirtyGraph(before, starts, invalid, true);
        Expect(invalidGraph.edit_kind == note::NoteEditKind::StructuralText &&
                   invalidGraph.line_count_may_change &&
                   invalidGraph.stale_lines.first == 0 &&
                   invalidGraph.stale_lines.last == 1,
               "dirty graph fails closed for an invalid source edit range");
        Expect(note::TextEditsEqual(plain, note::TextEdit{2, 0, L"x"}) &&
                   !note::TextEditsEqual(plain, structuralGraph.edit),
               "dirty graph can carry one exact edit across derived layers");

        const std::wstring fenced = L"```md\nalpha\n```\ntail";
        const note::NoteDirtyGraph fencedGraph = note::BuildNoteDirtyGraph(
            fenced, note::BuildLineStarts(fenced),
            note::TextEdit{8, 0, L"x"}, true);
        Expect(note::HasNoteDirtySyntaxFeature(
                   fencedGraph.syntax_features,
                   note::NoteDirtySyntaxFeature::CodeFence) &&
                   fencedGraph.propagation ==
                       note::NoteDirtyPropagation::DelimiterRegion &&
                   fencedGraph.stale_lines.first == 0 &&
                   fencedGraph.stale_lines.last == 2 &&
                   !note::NoteDirtyGraphAllowsRenderEarlyStop(fencedGraph, false),
               "dirty graph expands edits inside a code fence to its closing boundary");

        const std::wstring blockMath = L"$$\nx\n$$\ntail";
        const note::NoteDirtyGraph blockMathGraph = note::BuildNoteDirtyGraph(
            blockMath, note::BuildLineStarts(blockMath),
            note::TextEdit{4, 0, L"y"}, true);
        Expect(note::HasNoteDirtySyntaxFeature(
                   blockMathGraph.syntax_features,
                   note::NoteDirtySyntaxFeature::Math) &&
                   blockMathGraph.propagation ==
                       note::NoteDirtyPropagation::DelimiterRegion &&
                   blockMathGraph.stale_lines.first == 0 &&
                   blockMathGraph.stale_lines.last == 2,
               "dirty graph expands edits inside block math to its closing boundary");

        const std::wstring inlineMath = L"before $x$ after\n";
        const note::NoteDirtyGraph inlineMathGraph = note::BuildNoteDirtyGraph(
            inlineMath, note::BuildLineStarts(inlineMath),
            note::TextEdit{9, 0, L"y"}, true);
        Expect(note::HasNoteDirtySyntaxFeature(
                   inlineMathGraph.syntax_features,
                   note::NoteDirtySyntaxFeature::Math) &&
                   inlineMathGraph.propagation == note::NoteDirtyPropagation::LocalLine &&
                   inlineMathGraph.stale_lines.first == 0 &&
                   inlineMathGraph.stale_lines.last == 0,
               "dirty graph keeps balanced inline math changes on the affected line");

        const std::wstring linkText = L"[label](target)\nnext";
        const note::NoteDirtyGraph linkGraph = note::BuildNoteDirtyGraph(
            linkText, note::BuildLineStarts(linkText),
            note::TextEdit{2, 0, L"x"}, true);
        Expect(note::HasNoteDirtySyntaxFeature(
                   linkGraph.syntax_features,
                   note::NoteDirtySyntaxFeature::Link) &&
                   linkGraph.propagation == note::NoteDirtyPropagation::LocalLine,
               "dirty graph marks link semantics without invalidating unrelated lines");

        const std::wstring styleText = L"<color=red>text</>\n";
        const note::NoteDirtyGraph styleGraph = note::BuildNoteDirtyGraph(
            styleText, note::BuildLineStarts(styleText),
            note::TextEdit{12, 0, L"x"}, true);
        Expect(note::HasNoteDirtySyntaxFeature(
                   styleGraph.syntax_features,
                   note::NoteDirtySyntaxFeature::LegacyStyle) &&
                   styleGraph.propagation == note::NoteDirtyPropagation::LocalLine,
               "dirty graph marks balanced legacy style changes as line-local");
    }

    {
        note::NoteTextCore textCore;
        Expect(textCore.Apply(note::TextEdit{0, 0, L"x"}) ==
                   note::NoteTextApplyResult::InvalidOwner,
               "text core rejects edits without an owning note");

        note::NoteMetadata metadata;
        metadata.file_name = L"core.md";
        textCore.Reset(note::NoteId{77}, metadata, L"a\r\nb", 7, 3);
        Expect(textCore.valid() && textCore.note_id() == note::NoteId{77} &&
                   textCore.content_revision() == 7 &&
                   textCore.persistence_revision() == 3 &&
                   textCore.model().line_starts == std::vector<size_t>({0, 3}),
               "text core owns raw text, revision, persistence generation, and line index");
        Expect(textCore.Apply(note::TextEdit{1, 0, L"x"}) ==
                   note::NoteTextApplyResult::Applied &&
                   textCore.MatchesRaw(L"ax\r\nb") &&
                   textCore.content_revision() == 8 &&
                   textCore.model().line_starts == std::vector<size_t>({0, 4}) &&
                   textCore.BuildStorageTextCrlf() == L"ax\r\nb",
               "text core applies an edit atomically and advances its line index");

        textCore.Reset(note::NoteId{77}, metadata, L"a\rb\nc\r\nd", 9, 4);
        Expect(textCore.BuildStorageTextCrlf() == L"a\r\nb\r\nc\r\nd",
               "text core emits one CRLF for every logical line-break representation");

        const uint64_t revisionBeforeInvalid = textCore.content_revision();
        Expect(textCore.Apply(note::TextEdit{99, 1, L"bad"}) ==
                   note::NoteTextApplyResult::InvalidRange &&
                   textCore.content_revision() == revisionBeforeInvalid &&
                   textCore.MatchesRaw(L"a\rb\nc\r\nd"),
               "text core rejects an invalid range without clamping or mutation");
        textCore.SetPersistenceRevision(4);
        Expect(textCore.persistence_revision() == 4,
               "text core advances persistence identity independently from content revision");

        textCore.Reset(note::NoteId{77}, metadata, L"z",
                       std::numeric_limits<uint64_t>::max(), 4);
        Expect(textCore.Apply(note::TextEdit{1, 0, L"x"}) ==
                   note::NoteTextApplyResult::RevisionExhausted &&
                   textCore.MatchesRaw(L"z"),
               "text core rejects mutation when its monotonic revision is exhausted");

        note::LocalNoteKernel historyKernel;
        historyKernel.Reset(note::NoteId{78}, metadata, L"", 1, 0,
                            note::NoteContentKind::PlainText);
        const note::NoteTextSelection emptySelection{{0}, {0}};
        const note::NoteTextSelection afterA{{1}, {1}};
        const note::NoteTextSelection afterB{{2}, {2}};
        historyKernel.ApplyUserEdit(note::TextEdit{0, 0, L"a"}, emptySelection,
                                    afterA, note::NoteHistoryOperationKind::Typing,
                                    100, false);
        historyKernel.ApplyUserEdit(note::TextEdit{1, 0, L"b"}, afterA,
                                    afterB, note::NoteHistoryOperationKind::Typing,
                                    200, false);
        Expect(historyKernel.CanUndo() && historyKernel.text_core().MatchesRaw(L"ab"),
               "kernel history owns user edits for one note id");
        const auto undo = historyKernel.Undo(false);
        Expect(undo.has_value() && undo->selection.caret.value == 0 &&
                   historyKernel.text_core().MatchesRaw(L""),
               "merged typing undo restores both text and original caret");
        const auto redo = historyKernel.Redo(false);
        Expect(redo.has_value() && redo->selection.caret.value == 2 &&
                   historyKernel.text_core().MatchesRaw(L"ab"),
               "kernel history redo restores merged typing and final caret");

        note::LocalNoteKernel lineHistoryKernel;
        lineHistoryKernel.Reset(note::NoteId{79}, metadata, L"", 1, 0,
                               note::NoteContentKind::PlainText);
        const note::NoteTextSelection afterFirstLine{{2}, {2}};
        const note::NoteTextSelection afterBreak{{3}, {3}};
        const note::NoteTextSelection afterSecondLine{{4}, {4}};
        (void)lineHistoryKernel.ApplyUserEdit(note::TextEdit{0, 0, L"ab"}, emptySelection,
                                              afterFirstLine, note::NoteHistoryOperationKind::Typing,
                                              100, false);
        (void)lineHistoryKernel.ApplyUserEdit(note::TextEdit{2, 0, L"\n"}, afterFirstLine,
                                              afterBreak, note::NoteHistoryOperationKind::Typing,
                                              200, false);
        (void)lineHistoryKernel.ApplyUserEdit(note::TextEdit{3, 0, L"c"}, afterBreak,
                                              afterSecondLine, note::NoteHistoryOperationKind::Typing,
                                              300, false);
        const auto undoSecondLine = lineHistoryKernel.Undo(false);
        const auto undoLineBreak = lineHistoryKernel.Undo(false);
        const auto undoFirstLine = lineHistoryKernel.Undo(false);
        Expect(undoSecondLine.has_value() && undoLineBreak.has_value() && undoFirstLine.has_value() &&
                   lineHistoryKernel.text_core().MatchesRaw(L"") &&
                   undoSecondLine->selection.caret.value == 3 &&
                   undoLineBreak->selection.caret.value == 2 &&
                   undoFirstLine->selection.caret.value == 0,
               "history undo stops at a line boundary before resuming on the preceding line");

        note::NoteTextCoreRegistry textCores;
        note::NoteTextCore* firstCore = textCores.Reset(
            note::NoteId{101}, metadata, L"first", 1, 0);
        note::NoteTextCore* sameCore = textCores.Find(note::NoteId{101});
        note::NoteTextCore* secondCore = textCores.Reset(
            note::NoteId{202}, metadata, L"second", 1, 0);
        Expect(firstCore && firstCore == sameCore && secondCore &&
                   secondCore != firstCore && textCores.size() == 2,
               "text core registry shares one stable core per note id");
        const note::ViewIdentity firstView{
            note::ViewId{301}, note::NoteId{101}, 1};
        const note::ViewIdentity reboundView{
            note::ViewId{301}, note::NoteId{202}, 2};
        Expect(textCores.FindForView(firstView) == firstCore &&
                   textCores.FindForView(reboundView) == secondCore &&
                   textCores.FindForView(note::ViewIdentity{}) == nullptr,
               "text core registry resolves the model from a valid view binding");
        note::NoteTextCore* resetFirst = textCores.Reset(
            note::NoteId{101}, metadata, L"updated", 2, 1);
        Expect(resetFirst == firstCore && firstCore->MatchesRaw(L"updated") &&
                   firstCore->persistence_revision() == 1,
               "text core registry resets one note without replacing its shared instance");
        Expect(textCores.Forget(note::NoteId{202}) &&
                   !textCores.Find(note::NoteId{202}) && textCores.size() == 1,
               "text core registry forgets one note without affecting other cores");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(L"# Heading\n\nbody");
        Expect(parsed.second.source_identity.source_revision == parsed.first.revision &&
                   !parsed.second.source_identity.note_id.valid(),
               "parsed NoteDocument records its source revision");
        Expect(note::NoteDocumentMatchesTextModel(parsed.second, parsed.first),
               "parsed NoteDocument matches its source text model");

        note::NoteLayout layout;
        const note::NoteDerivedSnapshotIdentity layoutIdentity{
            note::NoteId{0x3301ULL}, parsed.first.revision};
        note::SetNoteLayoutSourceIdentity(&layout, layoutIdentity);
        Expect(note::NoteLayoutMatchesTextModel(layout, parsed.first) &&
                   note::NoteLayoutMatchesSourceIdentity(layout, layoutIdentity),
               "NoteLayout records one typed source identity");

        note::ApplyTextEdit(&parsed.first, note::TextEdit{0, 0, L"edited "});
        Expect(!note::NoteDocumentMatchesTextModel(parsed.second, parsed.first),
               "stale NoteDocument is rejected after a text revision change");
        Expect(!note::NoteLayoutMatchesTextModel(layout, parsed.first),
               "stale NoteLayout is rejected after a text revision change");
    }

    {
        Expect(note::NormalizeSemanticSearchTerm(L" ＡＢＣ, ﾊﾟﾋﾟ ") == L"abcパピ",
               "semantic search term normalizes width, case, separators, and voice marks");
        const note::SemanticNormalizedTextIndex normalized =
            note::BuildSemanticNormalizedTextIndex(L"Ａ B，ﾊﾟ");
        Expect(normalized.text == L"abパ",
               "semantic normalized text removes separators and normalizes width");
        Expect(normalized.source_start == std::vector<size_t>({0, 2, 4}) &&
                   normalized.source_end == std::vector<size_t>({1, 3, 6}),
                   "semantic normalized text maps combined voice marks to the full raw range");


        note::SemanticSearchOptions strictOptions;
        strictOptions.normalizeWidthKana = false;
        strictOptions.ignoreCase = false;
        strictOptions.ignoreSeparators = false;
        Expect(note::NormalizeSemanticSearchTerm(L" ＡＢＣ, ﾊﾟﾋﾟ ", strictOptions) == L" ＡＢＣ, ﾊﾟﾋﾟ ",
               "semantic search options can disable width, case, kana, and separator normalization");
        const note::NoteQueryGroups strictQuery{{
            note::NormalizeSemanticSearchTerm(L"ABC", strictOptions)}};
        Expect(!note::MatchNoteText(L"abc", strictQuery, strictOptions).matched &&
                   note::MatchNoteText(L"ABC", strictQuery, strictOptions).matched,
               "semantic search options can require case-sensitive matches");
    }

    {
        const note::NoteId owner{0x5401ULL};
        const std::wstring raw =
            L"# Head\r\n"
            L"Ａ B\r\n"
            L"<link=jump>target</>\r\n";
        const std::string bytes = TestWideToUTF8(raw);
        const note::SnapshotIdentity identity =
            note::BuildSnapshotIdentity(owner, 0, 7, bytes);
        const note::WorkspaceNoteIndexSnapshot index =
            note::BuildWorkspaceNoteIndex(
                identity,
                L"workspace\\indexed.md",
                note::NoteMetadata{L"indexed.md", L"indexed"},
                raw,
                note::NoteContentKind::Markdown);
        Expect(index.valid && note::SameSnapshotIdentity(index.snapshot_identity, identity) &&
                   index.lines.size() == 4 && index.semantic_index.valid &&
                   index.semantic_index.headings.size() == 1,
               "workspace note index binds raw lines and semantic data to one snapshot identity");

        const note::NoteQueryGroups query{{
            note::NormalizeSemanticSearchTerm(L"a b")}};
        const std::vector<note::WorkspaceNoteLineMatch> matches =
            note::SearchWorkspaceNoteIndex(index, query);
        const size_t expectedStart = raw.find(L'Ａ');
        Expect(matches.size() == 1 && matches[0].line_number == 2 &&
                   matches[0].match.firstPos == expectedStart &&
                   matches[0].match.firstLen == 3,
               "workspace note search maps normalized hits back to absolute raw offsets");

        const std::optional<size_t> anchor =
            note::FindWorkspaceLinkIdAnchor(index, L"jump");
        Expect(anchor.has_value() &&
                   !note::FindWorkspaceLinkIdAnchor(index, L"jump", anchor).has_value(),
               "workspace note index resolves link ids and excludes their source span");

        note::LocalNoteKernel currentKernel;
        currentKernel.Reset(owner, note::NoteMetadata{L"indexed.md", L"indexed"},
                            raw, 9, 7, note::NoteContentKind::Markdown);
        (void)currentKernel.RefreshDerived();
        const note::SnapshotIdentity currentIdentity =
            note::BuildSnapshotIdentity(owner, 9, 7, bytes);
        Expect(note::BuildWorkspaceNoteIndexFromKernel(
                   currentIdentity, L"workspace\\indexed.md", currentKernel).valid &&
                   !note::BuildWorkspaceNoteIndexFromKernel(
                       note::BuildSnapshotIdentity(note::NoteId{0x5402ULL}, 9, 7, bytes),
                       L"workspace\\indexed.md", currentKernel).valid,
               "workspace note index accepts only a matching active Kernel snapshot owner");

        const note::WorkspaceNoteIndexSnapshot plain =
            note::BuildWorkspaceNoteIndex(
                identity,
                L"workspace\\indexed.txt",
                note::NoteMetadata{L"indexed.txt", L"indexed"},
                L"Ａ B",
                note::NoteContentKind::PlainText);
        Expect(plain.valid && !plain.semantic_index.valid &&
                   note::SearchWorkspaceNoteIndex(plain, query).size() == 1,
               "workspace note index searches plain text without fabricating syntax semantics");
    }

    {
        note::NoteWorkspaceService workspace;
        const note::NoteId owner{0x5481ULL};
        note::LocalNoteKernel* kernel = workspace.ResetKernel(
            owner,
            note::NoteMetadata{L"service.md", L"service"},
            L"# Service\nbody",
            5,
            2,
            note::NoteContentKind::Markdown);
        Expect(kernel != nullptr &&
                   workspace.FindKernelForView(
                       note::ViewIdentity{note::ViewId{0x81ULL}, owner, 1}) == kernel,
               "workspace service owns one Kernel resolved by view identity");
        (void)kernel->RefreshDerived();
        const std::string bytes = TestWideToUTF8(kernel->text_core().model().raw);
        const note::SnapshotIdentity identity =
            note::BuildSnapshotIdentity(owner, 5, 2, bytes);
        const auto activeIndex = workspace.ResolveIndexFromKernel(
            identity, L"workspace\\service.md", *kernel);
        const auto reusedIndex = workspace.ResolveIndex(
            identity,
            L"workspace\\service.md",
            note::NoteMetadata{L"service.md", L"service"},
            kernel->text_core().model().raw,
            note::NoteContentKind::Markdown);
        Expect(activeIndex && activeIndex == reusedIndex &&
                   workspace.index_cache_size() == 1,
               "workspace service reuses one immutable index across active and resolved snapshots");

        note::MarkupExportConfig exportConfig{};
        exportConfig.format = note::ExportMarkupFormat::Html;
        const note::WorkspaceNoteExportResult exported = activeIndex
            ? note::ExportWorkspaceHtml(*activeIndex, exportConfig)
            : note::WorkspaceNoteExportResult{};
        Expect(exported.ok &&
                   note::SameSnapshotIdentity(exported.snapshot_identity, identity) &&
                   exported.bytes.find("<h1>Service</h1>") != std::string::npos,
               "workspace export preserves the resolved snapshot identity and derived document");

        note::WorkspaceNoteIndexSnapshot mismatchedExport = activeIndex
            ? *activeIndex
            : note::WorkspaceNoteIndexSnapshot{};
        ++mismatchedExport.snapshot_identity.content_fingerprint;
        Expect(!note::ExportWorkspaceHtml(
                    mismatchedExport, exportConfig).ok,
               "workspace export rejects a fingerprint that does not identify its raw text");

        (void)kernel->Apply(note::TextEdit{kernel->text_core().model().raw.size(),
                                     0, L" pending"}, true);
        const std::string pendingBytes =
            TestWideToUTF8(kernel->text_core().model().raw);
        const note::SnapshotIdentity pendingIdentity =
            note::BuildSnapshotIdentity(owner, 6, 2, pendingBytes);
        Expect(!workspace.ResolveIndexFromKernel(
                   pendingIdentity, L"workspace\\service.md", *kernel) &&
                   !note::BuildWorkspaceNoteIndexFromKernel(
                       pendingIdentity, L"workspace\\service.md", *kernel).valid,
               "workspace service rejects stale derived state while a Kernel edit is pending");
        (void)kernel->RefreshDerived();

        const std::wstring changedRaw = L"# Service\nchanged";
        const note::SnapshotIdentity changedIdentity = note::BuildSnapshotIdentity(
            owner, 6, 2, TestWideToUTF8(changedRaw));
        const auto changedIndex = workspace.ResolveIndex(
            changedIdentity,
            L"workspace\\service.md",
            note::NoteMetadata{L"service.md", L"service"},
            changedRaw,
            note::NoteContentKind::Markdown);
        Expect(changedIndex && changedIndex != activeIndex &&
                   workspace.index_cache_size() == 2,
               "workspace service never reuses an index across content revisions");

        workspace.ResetKernel(
            owner,
            note::NoteMetadata{L"service.md", L"service"},
            changedRaw,
            6,
            2,
            note::NoteContentKind::Markdown);
        Expect(workspace.index_cache_size() == 0,
               "workspace service invalidates every cached generation when a Kernel resets");

        for (uint64_t value = 1; value <= 130; ++value) {
            const note::NoteId cacheOwner{0x6000ULL + value};
            const std::string rawBytes = "cache" + std::to_string(value);
            workspace.ResolveIndex(
                note::BuildSnapshotIdentity(cacheOwner, 1, 0, rawBytes),
                L"cache\\" + std::to_wstring(value) + L".txt",
                note::NoteMetadata{},
                L"cache" + std::to_wstring(value),
                note::NoteContentKind::PlainText);
        }
        Expect(workspace.index_cache_size() == 128,
               "workspace service bounds immutable index retention");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(
            L"# First\r\n"
            L"\r\n"
            L"### **Second** [jump](target)\r\n"
            L"\r\n"
            L"<h2>Legacy Head</>\r\n"
            L"\r\n"
             L"<link=internal>local</> and $x+1$\r\n");
        const note::NoteId semanticOwner{0x5501ULL};
        const note::SemanticIndexSnapshot unboundIndex =
            note::BuildSemanticIndexSnapshot(semanticOwner, parsed.first, parsed.second);
        Expect(!unboundIndex.valid,
               "semantic index rejects a syntax snapshot without a bound owner");
        note::SetNoteDocumentSourceIdentity(
            &parsed.second,
            note::NoteDerivedSnapshotIdentity{semanticOwner, parsed.first.revision});
        const note::SemanticIndexSnapshot index =
            note::BuildSemanticIndexSnapshot(semanticOwner, parsed.first, parsed.second);
        Expect(index.valid &&
                   index.source_identity == note::NoteDerivedSnapshotIdentity{
                       semanticOwner, parsed.first.revision},
               "semantic index is valid for a matching owner and source revision");
        Expect(note::SemanticIndexMatchesTextModel(index, semanticOwner, parsed.first),
               "semantic index records its owner and source revision");
        Expect(!note::SemanticIndexMatchesTextModel(
                   index, note::NoteId{0x5502ULL}, parsed.first),
               "semantic index rejects another note with the same revision");
        Expect(!index.normalized_text.text.empty(),
               "semantic index contains normalized searchable text");
        Expect(index.headings.size() == 3,
               "semantic index contains Markdown and legacy headings");
        if (index.headings.size() == 3) {
            Expect(index.headings[0].level == 1 && index.headings[0].text == L"First",
                   "semantic index extracts first Markdown heading");
            Expect(index.headings[1].level == 3 && index.headings[1].text == L"Second jump",
                   "semantic index flattens styled Markdown heading text");
            Expect(index.headings[2].level == 2 && index.headings[2].text == L"Legacy Head",
                   "semantic index extracts legacy heading content");
            Expect(index.headings[0].line_span.end.value < parsed.first.raw.size() &&
                       parsed.first.raw[index.headings[0].line_span.end.value] == L'\r',
                   "semantic heading line span excludes CRLF");
        }

        bool foundMarkdownLink = false;
        bool foundLinkId = false;
        for (const auto& link : index.links) {
            if (link.kind == note::SemanticLinkKind::MarkdownTarget &&
                link.target == L"target") {
                foundMarkdownLink = true;
            }
            if (link.kind == note::SemanticLinkKind::LinkId &&
                link.target == L"internal" && link.text == L"local") {
                foundLinkId = true;
            }
        }
        Expect(foundMarkdownLink, "semantic index contains Markdown link targets");
        Expect(foundLinkId, "semantic index contains internal link ids");
        Expect(index.math.size() == 1 && index.math[0].normalized_tex == L"x+1",
               "semantic index contains normalized math spans");

        note::NoteTextModel newer = parsed.first;
        ++newer.revision;
        const note::SemanticIndexSnapshot stale =
            note::BuildSemanticIndexSnapshot(semanticOwner, newer, parsed.second);
        Expect(!stale.valid &&
                   !note::SemanticIndexMatchesTextModel(index, semanticOwner, newer),
               "semantic index rejects a stale NoteDocument revision");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"$hit$");
        Expect(doc.math_spans.size() == 1, "visible inline math is still extracted");
        if (doc.math_spans.size() == 1) {
            Expect(doc.math_spans[0].normalized_tex == L"hit",
                   "visible inline math keeps its normalized body");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"$x$");
        Expect(doc.math_spans.size() == 1, "$...$ inline math is extracted");
        if (doc.math_spans.size() == 1) {
            Expect(doc.math_spans[0].kind == note::MathKind::Inline,
                   "$...$ is classified as inline math");
            Expect(doc.math_spans[0].delimiter == note::MathDelimiter::Dollar,
                   "$...$ keeps the dollar delimiter");
            Expect(doc.math_spans[0].normalized_tex == L"x",
                   "$...$ keeps its normalized body");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"$$x + y$$");
        Expect(doc.math_spans.size() == 1, "$$...$$ block math is extracted");
        if (doc.math_spans.size() == 1) {
            Expect(doc.math_spans[0].kind == note::MathKind::Block,
                   "$$...$$ is classified as block math");
            Expect(doc.math_spans[0].delimiter == note::MathDelimiter::DoubleDollar,
                   "$$...$$ keeps the double-dollar delimiter");
            Expect(doc.math_spans[0].normalized_tex == L"x + y",
                   "$$...$$ keeps its normalized body");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"$$\n"
            L"a^2 + b^2 = c^2\n"
            L"$$");
        Expect(doc.math_spans.size() == 1, "multiline $$...$$ block math is extracted");
        if (doc.math_spans.size() == 1) {
            Expect(doc.math_spans[0].kind == note::MathKind::Block,
                   "multiline $$...$$ is classified as block math");
            Expect(doc.math_spans[0].normalized_tex == L"\na^2 + b^2 = c^2\n",
                   "multiline $$...$$ keeps its full normalized body");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"\\(a + b\\)");
        Expect(doc.math_spans.size() == 1, "\\(...\\) inline math is extracted");
        if (doc.math_spans.size() == 1) {
            Expect(doc.math_spans[0].kind == note::MathKind::Inline,
                   "\\(...\\) is classified as inline math");
            Expect(doc.math_spans[0].delimiter == note::MathDelimiter::BackslashParen,
                   "\\(...\\) keeps the backslash-paren delimiter");
            Expect(doc.math_spans[0].normalized_tex == L"a + b",
                   "\\(...\\) keeps its normalized body");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"\\[c + d\\]");
        Expect(doc.math_spans.size() == 1, "\\[...\\] block math is extracted");
        if (doc.math_spans.size() == 1) {
            Expect(doc.math_spans[0].kind == note::MathKind::Block,
                   "\\[...\\] is classified as block math");
            Expect(doc.math_spans[0].delimiter == note::MathDelimiter::BackslashBracket,
                   "\\[...\\] keeps the backslash-bracket delimiter");
            Expect(doc.math_spans[0].normalized_tex == L"c + d",
                   "\\[...\\] keeps its normalized body");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"\\[\n"
            L"\\frac{1}{1 + x}\n"
            L"= 1 - x + x^2\n"
            L"\\]");
        Expect(doc.math_spans.size() == 1, "multiline \\[...\\] block math is extracted");
        if (doc.math_spans.size() == 1) {
            Expect(doc.math_spans[0].kind == note::MathKind::Block,
                   "multiline \\[...\\] is classified as block math");
            Expect(doc.math_spans[0].normalized_tex == L"\n\\frac{1}{1 + x}\n= 1 - x + x^2\n",
                   "multiline \\[...\\] keeps its full normalized body");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"\\[\n"
            L"\\begin{pmatrix}\n"
            L"1 & 2 \\\\\n"
            L"3 & 4\n"
            L"\\end{pmatrix}\n"
            L"\\]");
        Expect(FindDiagnostic(doc, L"NOTE-W-MATH-UNSUPPORTED") == nullptr,
               "supported display math environment does not emit unsupported warning");
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"\\[\n"
            L"\\begin{array}{cc} 1 & 2 \\end{array}\n"
            L"\\]");
        const note::Diagnostic* diag = FindDiagnostic(doc, L"NOTE-W-MATH-UNSUPPORTED");
        Expect(diag != nullptr, "unsupported math environment emits warning");
        if (diag) {
            Expect(diag->severity == note::DiagnosticSeverity::Warning,
                   "unsupported math environment diagnostic is a warning");
        }
        Expect(doc.math_spans.size() == 1 && doc.math_spans[0].diagnostic_ids.size() == 1,
               "unsupported math environment warning is attached to its math span");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"$\\overbrace{x}$");
        const note::Diagnostic* diag = FindDiagnostic(doc, L"NOTE-W-MATH-UNSUPPORTED");
        Expect(diag != nullptr, "unsupported math command emits warning");
        if (diag) {
            Expect(diag->severity == note::DiagnosticSeverity::Warning,
                   "unsupported math command diagnostic is a warning");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"# Math UI Fixture\n\n"
            L"Inline dollar $x$\n"
            L"Inline paren \\(a + b\\)\n\n"
            L"$$z$$\n\n"
            L"\\[w + 1\\]\n");
        Expect(doc.math_spans.size() == 4,
               "mixed math fixture extracts every supported delimiter");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(
            L"# Math UI Fixture\r\r"
            L"Inline dollar $x$\r"
            L"Inline paren \\(a + b\\)\r\r"
            L"$$z$$\r\r"
            L"\\[w + 1\\]\r");
        Expect(parsed.first.line_starts.size() == 9,
               "RichEdit CR lines are indexed as logical lines");
        Expect(parsed.second.math_spans.size() == 4,
               "mixed math fixture with RichEdit CR lines extracts every delimiter");
    }

    {
        Expect(ApplyTextEditKeepsLineStartsInSync(
                   L"alpha\r\nbeta\r\ngamma",
                   note::TextEdit{2, 0, L"XYZ"}),
               "incremental note line starts stay in sync for same-line insertion");
        Expect(ApplyTextEditKeepsLineStartsInSync(
                   L"alpha\r\nbeta\r\ngamma",
                   note::TextEdit{7, 4, L"one\r\ntwo\r\nthree"}),
               "incremental note line starts stay in sync for multiline paste");
        Expect(ApplyTextEditKeepsLineStartsInSync(
                   L"alpha\r\nbeta\r\ngamma",
                   note::TextEdit{5, 2, L""}),
               "incremental note line starts stay in sync when deleting a CRLF boundary");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"`$skip$`");
        Expect(doc.math_spans.empty(), "math extraction skips inline code");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"```\r\n$$omit$$\r\n\\(omit\\)\r\n```");
        Expect(doc.math_spans.empty(), "math extraction skips fenced code");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"Broken \\(x + y");
        Expect(FindDiagnostic(doc, L"NOTE-E-MATH-UNCLOSED-PAREN") != nullptr,
               "unclosed \\( ... \\) math emits a diagnostic");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"Before\n$$\nx + y");
        Expect(doc.math_spans.empty(), "unclosed $$ block stays normal text");
        Expect(FindDiagnostic(doc, L"NOTE-E-MATH-UNCLOSED-DOUBLE-DOLLAR") == nullptr,
               "unclosed $$ block does not emit a diagnostic");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"Before\n\\[\nx + y");
        Expect(doc.math_spans.empty(), "unclosed \\[ block stays normal text");
        Expect(FindDiagnostic(doc, L"NOTE-E-MATH-UNCLOSED-BRACKET") == nullptr,
               "unclosed \\[ block does not emit a diagnostic");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"legacy <math>x + y</>");
        Expect(FindDiagnostic(doc, L"NOTE-E-LEGACY-MATH") != nullptr,
               "legacy <math> syntax is rejected in the new parser path");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"<link=dest><lu>jump</></>");
        Expect(FindStyleSpan(doc, note::StyleKind::LinkId, L"dest") != nullptr,
               "markdown route keeps legacy link-id style spans");
        Expect(FindStyleSpan(doc, note::StyleKind::LinkUnderline) != nullptr,
               "markdown route keeps legacy link underline style spans");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"`<link=dest>jump</>`");
        Expect(FindStyleSpan(doc, note::StyleKind::LinkId) == nullptr,
               "legacy link-id markup is ignored inside inline code");
    }

    {
        const std::wstring text =
            L"# Top\n"
            L"<u>top\n"
            L"## Mid\n"
            L"<char=#ff0000>mid\n"
            L"### Deep\n"
            L"<back=#00ff00>deep\n"
            L"</##>\n"
            L"after\n";
        const note::NoteDocument doc = ParseMd4c(text);
        const size_t closePos = text.find(L"</##>");
        const size_t afterPos = text.find(L"after");
        const note::StyleSpan* underline = FindStyleSpan(doc, note::StyleKind::Underline);
        const note::StyleSpan* textColor = FindStyleSpan(doc, note::StyleKind::TextColor, L"#ff0000");
        const note::StyleSpan* backColor = FindStyleSpan(doc, note::StyleKind::BackgroundColor, L"#00ff00");
        Expect(underline != nullptr && underline->span.end.value > afterPos,
               "hash heading close keeps higher-level tag effects active");
        Expect(textColor != nullptr && textColor->span.end.value == closePos,
               "</##> closes same-level tag effects");
        Expect(backColor != nullptr && backColor->span.end.value == closePos,
               "</##> closes deeper-level tag effects");
    }
    {
        const note::NoteDocument doc = ParseMd4c(L"<h2>Legacy Heading</>\n\nParagraph\n");
        const note::BlockNode* heading = FindBlock(doc, note::BlockKind::Heading, 0);
        Expect(heading != nullptr, "standalone legacy heading tag is promoted to heading block");
        if (heading != nullptr) {
            Expect(heading->origin == note::BlockOrigin::LegacyHeadingTag,
                   "promoted legacy heading keeps legacy origin");
            Expect(heading->level == 2, "promoted legacy heading keeps level");
        }
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"Paragraph <h2>Inline</> tail\n");
        const note::BlockNode* heading = FindBlock(doc, note::BlockKind::Heading, 0);
        Expect(heading == nullptr, "inline legacy heading tag is not promoted to heading block");
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"# Head\n\n"
            L"3. one\n"
            L"4. two\n\n"
            L"```cpp\n"
            L"code();\n"
            L"```\n");
        const note::BlockNode* heading = FindBlock(doc, note::BlockKind::Heading);
        const note::BlockNode* list = FindBlock(doc, note::BlockKind::List);
        const note::BlockNode* firstItem = FindBlock(doc, note::BlockKind::ListItem, 0);
        const note::BlockNode* secondItem = FindBlock(doc, note::BlockKind::ListItem, 1);
        const note::BlockNode* codeBlock = FindBlock(doc, note::BlockKind::CodeBlock);
        const size_t listIndex = (list != nullptr)
            ? static_cast<size_t>(list - doc.blocks.data())
            : static_cast<size_t>(-1);
        Expect(heading != nullptr && heading->level == 1,
               "md4c adapter keeps heading blocks with level");
        Expect(list != nullptr && list->ordered && list->start_number == 3,
               "md4c adapter keeps ordered-list metadata");
        Expect(firstItem != nullptr && secondItem != nullptr &&
                   listIndex != static_cast<size_t>(-1) &&
                   firstItem->parent == listIndex &&
                   secondItem->parent == listIndex,
               "md4c adapter keeps list-item parent links");
        Expect(codeBlock != nullptr && codeBlock->info_string == L"cpp",
               "md4c adapter keeps fenced code info strings");
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"Before\n\n"
            L"---\n\n"
            L"After\n");
        const note::BlockNode* rule = FindBlock(doc, note::BlockKind::HorizontalRule);
        Expect(rule != nullptr, "md4c adapter keeps thematic breaks as horizontal-rule blocks");
        Expect(FindDiagnostic(doc, L"NOTE-W-MD4C-BLOCK") == nullptr,
               "thematic breaks do not emit unsupported-block diagnostics");
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"| A | B |\n"
            L"|:--|--:|\n"
            L"| 1 | 2 |\n");
        const note::BlockNode* table = FindBlock(doc, note::BlockKind::Table);
        const note::BlockNode* headCell = FindBlock(doc, note::BlockKind::TableHeaderCell);
        const note::BlockNode* bodyCell = FindBlock(doc, note::BlockKind::TableCell);
        Expect(table != nullptr && table->table_column_count == 2,
               "md4c adapter keeps table blocks with column count");
        Expect(headCell != nullptr && headCell->table_cell_align == note::TableCellAlign::Left,
               "md4c adapter keeps table header cell alignment");
        Expect(bodyCell != nullptr,
               "md4c adapter keeps table body cells");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"- [x] done\n- [ ] todo\n");
        const note::BlockNode* firstItem = FindBlock(doc, note::BlockKind::ListItem, 0);
        const note::BlockNode* secondItem = FindBlock(doc, note::BlockKind::ListItem, 1);
        Expect(firstItem != nullptr && firstItem->task_item && firstItem->task_checked,
               "md4c adapter keeps checked task-list item metadata");
        Expect(secondItem != nullptr && secondItem->task_item && !secondItem->task_checked,
               "md4c adapter keeps unchecked task-list item metadata");
    }

    {
        const note::NoteDocument doc = ParseMd4c(
            L"Paragraph with *em* **strong** ~~strike~~ [jump](dest), ![alt](image.png), and `code`.\n");
        const note::InlineNode* em = FindInline(doc, note::InlineKind::Emphasis);
        const note::InlineNode* strong = FindInline(doc, note::InlineKind::Strong);
        const note::InlineNode* strike = FindInline(doc, note::InlineKind::Strike);
        const note::InlineNode* link = FindInline(doc, note::InlineKind::Link);
        const note::InlineNode* image = FindInline(doc, note::InlineKind::Image);
        const note::InlineNode* code = FindInline(doc, note::InlineKind::Code);
        Expect(em != nullptr, "md4c adapter keeps emphasis overlays");
        Expect(strong != nullptr, "md4c adapter keeps strong overlays");
        Expect(strike != nullptr, "md4c adapter keeps strike overlays");
        Expect(link != nullptr && link->target == L"dest",
               "md4c adapter keeps markdown link targets");
        Expect(image != nullptr && image->target == L"image.png",
               "md4c adapter keeps markdown image targets");
        Expect(code != nullptr, "md4c adapter keeps inline code overlays");
    }

    {
        const note::MathInputAnalysis analysis = note::AnalyzeMathBoxInput(L"  $$x + 1$$  ");
        Expect(analysis.has_input, "MathBox analysis accepts non-empty input");
        Expect(analysis.has_wrapping, "MathBox analysis recognizes wrapped latex");
        Expect(analysis.flavor == note::MathInputFlavor::Latex,
               "MathBox analysis classifies $$...$$ as latex");
        Expect(analysis.kind == note::MathKind::Block,
               "MathBox analysis classifies $$...$$ as block math");
        Expect(analysis.delimiter == note::MathDelimiter::DoubleDollar,
               "MathBox analysis keeps the double-dollar delimiter");
        Expect(analysis.content_text == L"x + 1",
               "MathBox analysis strips $$ wrappers");
        Expect(analysis.diagnostics.empty(),
               "valid wrapped latex has no diagnostics");
    }

    {
        const note::MathInputAnalysis analysis = note::AnalyzeMathBoxInput(L"\\[x + 1");
        Expect(HasError(analysis, L"NOTE-E-MATHBOX-UNCLOSED-BRACKET"),
               "MathBox analysis reports unclosed \\[ ... \\]");
    }

    {
        const note::MathInputAnalysis analysis = note::AnalyzeMathBoxInput(L"<math>a + b</>");
        Expect(analysis.has_wrapping, "MathBox analysis recognizes <math> wrapper");
        Expect(analysis.flavor == note::MathInputFlavor::Markup,
               "MathBox analysis classifies <math> wrapper as markup");
        Expect(analysis.content_text == L"a + b",
               "MathBox analysis strips the legacy wrapper body");
        Expect(analysis.diagnostics.empty(),
               "closed markup wrapper has no diagnostics");
    }

    {
        const note::MathInputAnalysis analysis = note::AnalyzeMathBoxInput(L"x^2 + y^2");
        Expect(analysis.has_input && !analysis.has_wrapping,
               "bare MathBox text stays valid without wrappers");
        Expect(analysis.content_text == L"x^2 + y^2",
               "bare MathBox text is preserved as content");
        Expect(analysis.diagnostics.empty(),
               "bare MathBox text does not emit delimiter diagnostics");
    }

    {
        auto node = mathrender::Parse(L"q = \\dfrac{a+b}{c}");
        Expect(ContainsMathNodeType(node.get(), mathrender::Node::Type::Fraction),
               "math renderer parses \\dfrac as a fraction node");
    }

    {
        auto node = mathrender::Parse(L"\\vec{x} + \\bar{y} + \\hat{z}");
        Expect(ContainsMathNodeType(node.get(), mathrender::Node::Type::Accent),
               "math renderer parses common accent commands as accent nodes");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(L"# Title\n\nText with <u>mark</> and $x$.\n");
        note::TextExportConfig config{};
        config.mathMode = note::ExportTextMathMode::Simplified;
        config.markupMode = note::ExportTextMarkupMode::Simplified;
        const std::string out = note::ExportPlainText(parsed.first, parsed.second, config);
        Expect(out.find("Title") != std::string::npos, "plain text export keeps heading text");
        Expect(out.find("mark") != std::string::npos, "plain text export keeps styled text content");
        Expect(out.find("<u>") == std::string::npos, "plain text export strips legacy style tags");
        Expect(out.find("$x$") == std::string::npos && out.find("x") != std::string::npos,
               "plain text export simplifies math text");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(L"# Head\n\nBody $x$.\n");
        note::MarkupExportConfig config{};
        config.format = note::ExportMarkupFormat::Markdown;
        config.mathMode = note::ExportMarkupMathMode::Placeholder;
        config.mathPlaceholder = "[math]";
        config.includeTitleHeading = true;
        config.shiftHeadingLevels = true;
        config.title = L"Doc";
        const std::string out = note::ExportMarkdown(parsed.first, parsed.second, config);
        Expect(out.find("# Doc") != std::string::npos, "markdown export can prepend title heading");
        Expect(out.find("## Head") != std::string::npos, "markdown export can shift existing headings");
        Expect(out.find("[math]") != std::string::npos, "markdown export can replace math spans");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(L"Text with **bold** and <u>mark</> and $x$.\n");
        note::MarkupExportConfig config{};
        config.format = note::ExportMarkupFormat::Html;
        config.title = L"Doc";
        const std::string out = note::ExportHtml(parsed.first, parsed.second, config);
        Expect(out.find("<strong>bold</strong>") != std::string::npos, "html export keeps strong emphasis");
        Expect(out.find("text-decoration:underline;") != std::string::npos, "html export maps underline style spans");
        Expect(out.find("class=\"math inline\"") != std::string::npos, "html export renders math spans");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(L"$$x + y$$\n\n\\[z + 1\\]\n");
        note::MarkupExportConfig config{};
        config.format = note::ExportMarkupFormat::Html;
        config.title = L"Doc";
        const std::string out = note::ExportHtml(parsed.first, parsed.second, config);
        Expect(out.find("class=\"math block\"") != std::string::npos,
               "html export renders block math spans");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(L"escaped \\* mark, escaped \\\\ slash, and yen \u00A5*.\n");
        note::TextExportConfig plainConfig{};
        const std::string plain = note::ExportPlainText(parsed.first, parsed.second, plainConfig);
        Expect(plain.find("escaped * mark, escaped \\ slash") != std::string::npos,
               "plain text export decodes U+005C Markdown escapes including a literal backslash");
        Expect(plain.find("yen \xC2\xA5*") != std::string::npos,
               "plain text export keeps U+00A5 yen sign as ordinary text");

        note::MarkupExportConfig htmlConfig{};
        htmlConfig.format = note::ExportMarkupFormat::Html;
        const std::string html = note::ExportHtml(parsed.first, parsed.second, htmlConfig);
        Expect(html.find("escaped * mark, escaped \\ slash") != std::string::npos,
               "HTML export matches rendered Markdown backslash escapes");
        Expect(html.find("yen \xC2\xA5*") != std::string::npos,
               "HTML export keeps U+00A5 yen sign as ordinary text");

        note::MarkupExportConfig markdownConfig{};
        markdownConfig.format = note::ExportMarkupFormat::Markdown;
        const std::string markdown = note::ExportMarkdown(parsed.first, parsed.second, markdownConfig);
        Expect(markdown.find("escaped \\* mark, escaped \\\\ slash") != std::string::npos,
               "Markdown export preserves raw U+005C escape source");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"PDF に<b>マーカー</b>を引いたり、<i>斜体</i>にする\n");
        const auto* boldSpan = FindStyleSpan(doc, note::StyleKind::Bold);
        const auto* italicSpan = FindStyleSpan(doc, note::StyleKind::Italic);
        Expect(boldSpan != nullptr, "<b> tag creates StyleKind::Bold span");
        Expect(italicSpan != nullptr, "<i> tag creates StyleKind::Italic span");
    }

    {
        auto [model, doc] = BuildMd4cModelAndDoc(L"<b>bold</b> <i>italic</i> <x>strike</x>\n");
        const auto* boldSpan = FindStyleSpan(doc, note::StyleKind::Bold);
        const auto* strikeSpan = FindStyleSpan(doc, note::StyleKind::Strike);
        Expect(boldSpan != nullptr, "<b>...</b> creates StyleKind::Bold span");
        Expect(strikeSpan != nullptr, "<x>...</x> creates StyleKind::Strike span");

        std::string html = note::ExportHtml(model, doc, note::MarkupExportConfig{});
        Expect(html.find("font-weight:bold;") != std::string::npos, "ExportHtml renders StyleKind::Bold as CSS font-weight:bold;");
        Expect(html.find("text-decoration:line-through;") != std::string::npos, "ExportHtml renders StyleKind::Strike as CSS text-decoration:line-through;");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"<b>Universal closing</b> </>\n");
        const auto* boldSpan = FindStyleSpan(doc, note::StyleKind::Bold);
        Expect(boldSpan != nullptr, "<b>...</> creates StyleKind::Bold span with universal closing tag");
    }

    {
        const note::NoteDocument doc = ParseMd4c(L"");
        Expect(doc.blocks.empty(), "empty note parses without blocks or crash");
        Expect(doc.math_spans.empty(), "empty note has no math spans");
    }

    {
        std::wstring longLine(1000 * 1000, L'a');
        longLine += L" $x$";
        const note::NoteDocument doc = ParseMd4c(std::move(longLine));
        Expect(!doc.blocks.empty(), "1MB single-line note parses without hang/crash");
        Expect(doc.math_spans.size() == 1, "1MB single-line note still extracts trailing math");
    }

    {
        std::wstring text = L"before ";
        text.push_back(L'\0');
        text += L" after $x$";
        const note::NoteDocument doc = ParseMd4c(std::move(text));
        Expect(doc.math_spans.size() == 1, "NUL-containing note parses and preserves later math scan");
    }

    {
        std::wstring text = L"bad surrogate ";
        text.push_back(static_cast<wchar_t>(0xD800));
        text += L" tail";
        const note::NoteDocument doc = ParseMd4c(std::move(text));
        Expect(!doc.diagnostics.empty() || doc.blocks.size() <= 1,
               "isolated UTF-16 surrogate is handled without parser crash");
    }

    {
        auto parsed = BuildMd4cModelAndDoc(L"<script>alert('x')</script> & text\n");
        note::MarkupExportConfig config{};
        config.format = note::ExportMarkupFormat::Html;
        config.title = L"<Unsafe>";
        const std::string out = note::ExportHtml(parsed.first, parsed.second, config);
        Expect(out.find("<script>alert") == std::string::npos,
               "html export escapes script-like input");
        Expect(out.find("&lt;script&gt;alert") != std::string::npos,
               "html export keeps script-like input only as escaped text");
        Expect(out.find("<title>&lt;Unsafe&gt;</title>") != std::string::npos,
               "html export escapes title text");
    }

    std::cout << "Summary: failed=" << g_failed << "\n";
    return g_failed == 0 ? 0 : 1;
}
