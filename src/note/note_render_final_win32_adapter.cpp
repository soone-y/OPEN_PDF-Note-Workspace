#include "note/note_render_final_win32_adapter.h"

#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace note {
namespace {

[[nodiscard]] bool IsValidEditorRange(Span range, size_t textLength) noexcept {
    return range.start <= range.end && range.end.value <= textLength;
}

[[nodiscard]] size_t EditorTextLength(
    const NoteTextCore& textCore,
    const std::optional<NoteRenderFinalImePreeditPresentation>& preedit) noexcept {
    return preedit.has_value() ? preedit->coordinate_map().new_source_length()
                               : textCore.text_length();
}

[[nodiscard]] bool IsStateInputWellFormed(
    const NoteRenderFinalWin32AdapterStateInput& input) noexcept {
    if (!input.text_core || !input.text_core->valid() || !input.layout_key.valid() ||
        !input.measurement_dc || !input.measurement_options.valid()) {
        return false;
    }
    if (input.owner_input.ime_preedit != input.ime_preedit.has_value()) return false;
    if (input.ime_preedit.has_value() &&
        (!input.ime_preedit->valid() || !input.ime_preedit->Matches(*input.text_core))) {
        return false;
    }
    return true;
}

[[nodiscard]] bool IsCurrentStructuralPublication(
    const std::shared_ptr<const NoteRenderFinalPublication>& publication,
    const NoteRenderFinalWin32AdapterStateInput& input) noexcept {
    return publication && publication->valid() && publication->syntax() &&
        publication->syntax()->content_kind() == input.content_kind &&
        publication->Matches(*input.text_core, input.layout_key);
}

[[nodiscard]] NoteRenderFinalCompleteBuildInput StructuralBuildInput(
    const NoteRenderFinalWin32AdapterStateInput& input) noexcept {
    NoteRenderFinalCompleteBuildInput complete;
    complete.content_kind = input.content_kind;
    complete.layout_key = input.layout_key;
    // The structural candidate contains no transient raw/IME ownership. The
    // completed hybrid frame below resolves those event-local facts from the
    // very same placement publication. This keeps structural reuse independent
    // of the current caret row and prevents an old raw owner range leaking
    // into a later fully structured frame.
    complete.owner_input.render_active = true;
    complete.owner_input.editor_text_core_current = true;
    complete.owner_input.visible_lines = {
        {0}, {input.text_core->logical_line_count()}};
    return complete;
}

} // namespace

NoteRenderFinalWin32AdapterBuildResult NoteRenderFinalWin32AdapterFrame::Build(
    const NoteRenderFinalWin32AdapterInput& input,
    std::shared_ptr<const NoteRenderFinalWin32AdapterFrame>* out) noexcept {
    if (!out) return NoteRenderFinalWin32AdapterBuildResult::InvalidOutput;
    if (!input.text_core || !input.text_core->valid() || !input.structural_publication ||
        !input.measurement_dc || !input.measurement_options.valid()) {
        return NoteRenderFinalWin32AdapterBuildResult::InvalidInput;
    }
    if (input.ime_preedit.has_value() &&
        (!input.ime_preedit->valid() || !input.ime_preedit->Matches(*input.text_core))) {
        return NoteRenderFinalWin32AdapterBuildResult::InvalidInput;
    }
    const size_t editorLength = EditorTextLength(*input.text_core, input.ime_preedit);
    if (!IsValidEditorRange(input.editor_selection, editorLength) ||
        input.editor_caret.value > editorLength ||
        input.editor_caret < input.editor_selection.start ||
        input.editor_caret > input.editor_selection.end) {
        return NoteRenderFinalWin32AdapterBuildResult::SelectionOutsideEditorText;
    }
    try {
        NoteRenderFinalGdiMeasurementProvider measurement(
            input.measurement_dc, input.measurement_options);
        std::shared_ptr<const NoteRenderFinalPresentationSnapshot> presentation;
        const NoteRenderFinalPresentationSnapshotBuildResult buildResult =
            input.ime_preedit.has_value()
                ? NoteRenderFinalPresentationSnapshot::BuildWithImePreedit(
                    *input.text_core, input.structural_publication, input.owner_input,
                    *input.ime_preedit, measurement, &presentation)
                : NoteRenderFinalPresentationSnapshot::Build(
                    *input.text_core, input.structural_publication, input.owner_input,
                    measurement, &presentation);
        if (buildResult != NoteRenderFinalPresentationSnapshotBuildResult::Built ||
            !presentation) {
            return NoteRenderFinalWin32AdapterBuildResult::RequiresNativeFallback;
        }
        std::shared_ptr<NoteRenderFinalWin32AdapterFrame> candidate(
            new NoteRenderFinalWin32AdapterFrame());
        candidate->presentation_ = std::move(presentation);
        if (ResolveNoteRenderFinalImePreeditDecorations(*candidate->presentation_, &candidate->ime_decorations_) !=
            NoteRenderFinalPresentationInteractionResult::Resolved) {
            return NoteRenderFinalWin32AdapterBuildResult::RequiresNativeFallback;
        }
        candidate->editor_selection_ = input.editor_selection;
        candidate->editor_caret_ = input.editor_caret;
        candidate->caret_affinity_ = input.caret_affinity;
        candidate->valid_ = true;
        *out = std::move(candidate);
        return NoteRenderFinalWin32AdapterBuildResult::Built;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalWin32AdapterBuildResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalWin32AdapterBuildResult::SnapshotBuildFailed;
    }
}

bool NoteRenderFinalWin32AdapterFrame::Paint(
    HDC paintDc,
    const NoteRenderFinalWin32PaintState& state) const noexcept {
    if (!valid_ || !presentation_ || !paintDc || state.horizontal_scroll_px < 0) return false;
    NoteRenderFinalGdiPaintOptions options;
    options.horizontal_scroll_px = state.horizontal_scroll_px;
    options.vertical_scroll_px = state.vertical_scroll_px;
    options.selection = editor_selection_;
    if (state.paint_caret) options.caret = editor_caret_;
    options.caret_affinity = caret_affinity_;
    options.theme = state.theme;
    options.ime_decorations = &ime_decorations_;
    return NoteRenderFinalGdiPainter::Paint(paintDc, *presentation_, options);
}

NoteRenderFinalPresentationInteractionResult NoteRenderFinalWin32AdapterFrame::HitTest(
    int contentX,
    uint64_t contentY,
    NoteRenderFinalHit* out) const noexcept {
    if (!valid_ || !presentation_) {
        return NoteRenderFinalPresentationInteractionResult::InvalidPresentation;
    }
    return HitTestNoteRenderFinalPresentationEditor(*presentation_, contentX, contentY, out);
}

NoteRenderFinalPresentationInteractionResult NoteRenderFinalWin32AdapterFrame::Caret(
    NoteRenderFinalCaretGeometry* out) const noexcept {
    if (!valid_ || !presentation_) {
        return NoteRenderFinalPresentationInteractionResult::InvalidPresentation;
    }
    return ResolveNoteRenderFinalPresentationEditorCaret(
        *presentation_, editor_caret_, caret_affinity_, out);
}

NoteRenderFinalPresentationInteractionResult NoteRenderFinalWin32AdapterFrame::Selection(
    std::vector<NoteRenderFinalRect>* out) const noexcept {
    if (!valid_ || !presentation_) {
        return NoteRenderFinalPresentationInteractionResult::InvalidPresentation;
    }
    return ResolveNoteRenderFinalPresentationEditorSelection(
        *presentation_, editor_selection_, out);
}

NoteRenderFinalPresentationInteractionResult NoteRenderFinalWin32AdapterFrame::HorizontalExtent(
    uint64_t* outExtent) const noexcept {
    if (!valid_ || !presentation_) {
        return NoteRenderFinalPresentationInteractionResult::InvalidPresentation;
    }
    return ResolveNoteRenderFinalPresentationHorizontalExtent(*presentation_, outExtent);
}

NoteRenderFinalWin32AdapterStateRefreshResult
NoteRenderFinalWin32AdapterState::Refresh(
    const NoteRenderFinalWin32AdapterStateInput& input,
    const std::optional<TextEdit>& acceptedLocalEdit) noexcept {
#if !defined(NDEBUG)
    ++test_work_.refresh_calls;
#endif
    // A previously published custom frame must never survive an event for
    // which the current frame cannot be proven. The retained structural
    // publication remains private and can only be used as local-patch input
    // when its identity establishes the immediately preceding revision.
    frame_.reset();
    if (!IsStateInputWellFormed(input)) {
        return NoteRenderFinalWin32AdapterStateRefreshResult::InvalidInput;
    }
    if (!input.owner_input.render_active || !input.owner_input.editor_text_core_current ||
        input.owner_input.geometry_may_change ||
        (input.owner_input.ime_preedit &&
         !input.owner_input.ime_preedit_can_reuse_committed_layout)) {
        return NoteRenderFinalWin32AdapterStateRefreshResult::RequiresNativeFallback;
    }

    try {
        std::shared_ptr<const NoteRenderFinalPublication> candidatePublication;
        NoteRenderFinalWin32AdapterStateRefreshResult success =
            NoteRenderFinalWin32AdapterStateRefreshResult::PublishedComplete;
        const bool currentStructural =
            IsCurrentStructuralPublication(structural_publication_, input);
        if (currentStructural) {
#if !defined(NDEBUG)
            ++test_work_.structural_reuse_attempts;
#endif
            // An accepted edit must advance the canonical revision. Treating
            // a mismatched ticket as a harmless selection refresh would make
            // it possible to render a stale publication after a failed edit
            // binding, so reject it at this boundary.
            if (acceptedLocalEdit.has_value()) {
                return NoteRenderFinalWin32AdapterStateRefreshResult::InvalidInput;
            }
            candidatePublication = structural_publication_;
            success = NoteRenderFinalWin32AdapterStateRefreshResult::PublishedReusedStructural;
        } else {
            NoteRenderFinalGdiMeasurementProvider measurement(
                input.measurement_dc, input.measurement_options);
            if (acceptedLocalEdit.has_value() && structural_publication_) {
#if !defined(NDEBUG)
                ++test_work_.local_transaction_attempts;
#endif
                NoteRenderFinalLocalPatchInput local;
                local.layout_key = input.layout_key;
                local.owner_input = StructuralBuildInput(input).owner_input;
                local.edit = *acceptedLocalEdit;
                const NoteRenderFinalCompleteBuildResult localResult =
                    NoteRenderFinalTransaction::BuildLocalPlainTextPatch(
                        structural_publication_, *input.text_core, local, measurement,
                        &candidatePublication);
                if (localResult == NoteRenderFinalCompleteBuildResult::Built &&
                    candidatePublication) {
#if !defined(NDEBUG)
                    ++test_work_.local_transaction_builds;
#endif
                    success = NoteRenderFinalWin32AdapterStateRefreshResult::PublishedLocalPatch;
                } else if (localResult == NoteRenderFinalCompleteBuildResult::AllocationFailure) {
                    return NoteRenderFinalWin32AdapterStateRefreshResult::AllocationFailure;
                }
            }
            if (!candidatePublication) {
#if !defined(NDEBUG)
                ++test_work_.complete_transaction_attempts;
#endif
                const NoteRenderFinalCompleteBuildInput complete = StructuralBuildInput(input);
                const NoteRenderFinalCompleteBuildResult completeResult =
                    NoteRenderFinalTransaction::BuildComplete(
                        *input.text_core, complete, measurement, &candidatePublication);
                if (completeResult == NoteRenderFinalCompleteBuildResult::AllocationFailure) {
                    return NoteRenderFinalWin32AdapterStateRefreshResult::AllocationFailure;
                }
                if (completeResult != NoteRenderFinalCompleteBuildResult::Built ||
                    !candidatePublication) {
                    return NoteRenderFinalWin32AdapterStateRefreshResult::RequiresNativeFallback;
                }
#if !defined(NDEBUG)
                ++test_work_.complete_transaction_builds;
#endif
                success = NoteRenderFinalWin32AdapterStateRefreshResult::PublishedComplete;
            }
        }

        NoteRenderFinalWin32AdapterInput adapter;
        adapter.text_core = input.text_core;
        adapter.structural_publication = candidatePublication;
        adapter.owner_input = input.owner_input;
        adapter.ime_preedit = input.ime_preedit;
        adapter.editor_selection = input.editor_selection;
        adapter.editor_caret = input.editor_caret;
        adapter.caret_affinity = input.caret_affinity;
        adapter.measurement_dc = input.measurement_dc;
        adapter.measurement_options = input.measurement_options;
        std::shared_ptr<const NoteRenderFinalWin32AdapterFrame> candidateFrame;
        const NoteRenderFinalWin32AdapterBuildResult adapterResult =
            NoteRenderFinalWin32AdapterFrame::Build(adapter, &candidateFrame);
        if (adapterResult == NoteRenderFinalWin32AdapterBuildResult::AllocationFailure) {
            return NoteRenderFinalWin32AdapterStateRefreshResult::AllocationFailure;
        }
        if (adapterResult != NoteRenderFinalWin32AdapterBuildResult::Built || !candidateFrame ||
            !candidateFrame->valid()) {
            return NoteRenderFinalWin32AdapterStateRefreshResult::RequiresNativeFallback;
        }

        // This is the sole publication point. A paint reader receives either
        // both current objects or none; there is no observable interval with
        // a new source/placement tree and an old owner/selection frame.
        structural_publication_ = std::move(candidatePublication);
        frame_ = std::move(candidateFrame);
        return success;
    } catch (const std::bad_alloc&) {
        return NoteRenderFinalWin32AdapterStateRefreshResult::AllocationFailure;
    } catch (const std::exception&) {
        return NoteRenderFinalWin32AdapterStateRefreshResult::RequiresNativeFallback;
    }
}

void NoteRenderFinalWin32AdapterState::Clear() noexcept {
    frame_.reset();
    structural_publication_.reset();
}

void NoteRenderFinalWin32AdapterState::ClearFrame() noexcept {
    frame_.reset();
}

} // namespace note
