#include <array>
#include <iostream>
#include <string_view>

#include "note/note_presentation.h"

namespace {

int g_failed = 0;

void Expect(bool condition, std::string_view message) {
    if (condition) {
        std::cout << "[PASS] " << message << "\n";
        return;
    }
    std::cout << "[FAIL] " << message << "\n";
    ++g_failed;
}

} // namespace

int main() {
    using Action = note::NotePresentationFrameAction;
    using FrameKind = note::NotePresentationFrameKind;
    using Caret = note::NoteCaretPresenter;
    using Reason = note::NotePresentationReason;

    const note::Span displayMath{{10}, {20}};
    Expect(note::NoteSelectionTouchesSpan(displayMath, 10, 10),
           "caret at a display-math start owns its raw editing surface");
    Expect(note::NoteSelectionTouchesSpan(displayMath, 19, 19),
           "caret inside a display-math span owns its raw editing surface");
    Expect(!note::NoteSelectionTouchesSpan(displayMath, 20, 20),
           "caret immediately after display math leaves its structured surface visible");
    Expect(!note::NoteSelectionTouchesSpan(displayMath, 9, 10),
           "selection ending at display-math start leaves it structured");
    Expect(!note::NoteSelectionTouchesSpan(displayMath, 20, 21),
           "selection starting at display-math end leaves it structured");
    Expect(note::NoteSelectionTouchesSpan(displayMath, 9, 11),
           "selection overlapping display math owns its raw editing surface");
    Expect(note::NotePointerMovedCaretOwnsRawLine(true, true, false, false, 7, 7) &&
               !note::NotePointerMovedCaretOwnsRawLine(false, true, false, false, 7, 7) &&
               !note::NotePointerMovedCaretOwnsRawLine(true, true, true, false, 7, 7) &&
               !note::NotePointerMovedCaretOwnsRawLine(true, true, false, true, 7, 7) &&
               !note::NotePointerMovedCaretOwnsRawLine(true, false, false, false, 7, 7) &&
               !note::NotePointerMovedCaretOwnsRawLine(true, true, false, false, 6, 7),
           "a pointer-moved editing caret owns exactly its raw line");
    Expect(note::NoteMathRenderEnabledForRoute(true, true, false) &&
               note::NoteMathRenderEnabledForRoute(true, false, true) &&
               !note::NoteMathRenderEnabledForRoute(true, false, false) &&
               !note::NoteMathRenderEnabledForRoute(false, true, true),
           "math rendering follows the explicit Markdown setting and the TeX-only route");

    {
        note::NoteTableLayoutInput input;
        input.column_count = 3;
        input.minimum_content_width_px = 12;
        input.cell_horizontal_padding_px = 3;
        input.border_width_px = 1;
        input.cell_measures = {
            {0, 18}, {1, 8}, {2, 25}, {0, 14}, {1, 20},
        };
        const note::NoteTableLayout table = note::ResolveNoteTableLayout(input);
        Expect(table.valid && table.columns.size() == 3 &&
                   table.columns[0].content_width_px == 18 &&
                   table.columns[1].content_width_px == 20 &&
                   table.columns[2].content_width_px == 25 &&
                   table.columns[0].left_border_x_px == 0 &&
                   table.columns[1].left_border_x_px == 25 &&
                   table.columns[2].left_border_x_px == 52 &&
                   table.total_width_px == 85,
               "table layout derives one stable grid from all cell measurements");
        Expect(note::ResolveNoteTableCellContentOffset(
                   table.columns[1], 8, note::NoteTableCellAlignment::Left) == 0 &&
                   note::ResolveNoteTableCellContentOffset(
                       table.columns[1], 8, note::NoteTableCellAlignment::Center) == 6 &&
                   note::ResolveNoteTableCellContentOffset(
                       table.columns[1], 8, note::NoteTableCellAlignment::Right) == 12,
               "table layout aligns left, center, and right cells inside one measured column");
        Expect(note::ResolveNoteTableDividerHeightPx(1) == 1 &&
                   note::ResolveNoteTableDividerHeightPx(0) == 0 &&
                   note::ResolveNoteTableDividerHeightPx(-1) == 0,
               "a Markdown table delimiter occupies exactly one valid grid border");
        input.cell_measures.push_back({3, 1});
        Expect(!note::ResolveNoteTableLayout(input).valid,
               "table layout rejects a cell whose column is outside the table");
    }

    struct FrameCase {
        note::NotePresentationFrameState state;
        Action expected;
        FrameKind expectedFrameKind;
        Caret expectedCaret;
        Reason expectedReason;
        bool requiresFullRepaint;
        const char* message;
    };
    const std::array<FrameCase, 11> frameCases{{
        {{false, true, true, false, false, false}, Action::RawFallback,
         FrameKind::NativeEditorFallback, Caret::NativeEditor, Reason::RenderingDisabled, false,
         "render-disabled frames use native raw fallback"},
        {{true, true, true, false, false, false}, Action::RenderCurrent,
         FrameKind::DrawCommitted, Caret::CommittedLayout, Reason::CurrentSnapshot, false,
         "current committed snapshots render directly"},
        {{true, true, false, true, false, false}, Action::ReuseCommittedLayout,
         FrameKind::DrawCommitted, Caret::NativeEditor, Reason::SameLinePendingEdit, false,
         "same-line pending edits reuse committed layout"},
        {{true, true, false, true, true, false}, Action::CommitBeforePaint,
         FrameKind::CommitThenDraw, Caret::NativeEditor, Reason::GeometryMayChange, true,
         "line-count changes form a paint barrier"},
        {{true, true, false, false, true, false}, Action::CommitBeforePaint,
         FrameKind::CommitThenDraw, Caret::NativeEditor, Reason::GeometryMayChange, true,
         "derived structural repair forms a paint barrier"},
        {{true, true, false, true, false, true, true, false}, Action::RawFallback,
         FrameKind::NativeEditorFallback, Caret::NativeEditor, Reason::StaleImeComposition, false,
         "unproven stale IME composition uses native raw fallback"},
        {{true, true, false, true, false, true, true, true}, Action::ReuseCommittedLayout,
         FrameKind::DrawCommitted, Caret::NativeEditor, Reason::SameLinePendingEdit, false,
         "isolated IME composition keeps unrelated lines committed"},
        {{true, true, false, true, true, true, true, true}, Action::RawFallback,
         FrameKind::NativeEditorFallback, Caret::NativeEditor, Reason::StaleImeComposition, false,
         "geometry-changing IME composition uses native raw fallback"},
        {{true, true, true, true, false, true}, Action::RenderCurrent,
         FrameKind::DrawCommitted, Caret::CommittedLayout, Reason::CurrentSnapshot, false,
         "synchronized IME composition keeps current rendering"},
        {{true, false, false, true, false, false}, Action::RawFallback,
         FrameKind::NativeEditorFallback, Caret::NativeEditor, Reason::SnapshotUnavailable, false,
         "a missing cache cannot be reused"},
        {{true, true, true, false, false, false, false}, Action::RawFallback,
         FrameKind::NativeEditorFallback, Caret::NativeEditor, Reason::EditorBindingUnavailable, false,
         "an unbound editor document cannot reuse canonical line geometry"},
    }};
    for (const FrameCase& test : frameCases) {
        const note::NotePresentationPlan plan = note::ResolveNotePresentationPlan(test.state);
        Expect(note::ResolveNotePresentationFrameAction(plan) == test.expected,
               test.message);
        Expect(note::ResolveNotePresentationFrameAction(test.state) ==
                   note::ResolveNotePresentationFrameAction(plan),
               "legacy frame action agrees with the presentation plan");
        Expect(plan.frame_kind == test.expectedFrameKind &&
                   plan.caret_presenter == test.expectedCaret &&
                   plan.reason == test.expectedReason &&
                   plan.requires_full_repaint_after_commit == test.requiresFullRepaint,
               "presentation plan fixes frame, caret, reason, and repaint ownership");
        const note::NoteNativePaintScope expectedNativeScope =
            (test.expected == Action::RawFallback || test.expected == Action::CommitBeforePaint)
                ? note::NoteNativePaintScope::AllLines
                : note::NoteNativePaintScope::NativeRawLinesOnly;
        Expect(note::ResolveNoteNativePaintScope(test.expected) == expectedNativeScope,
               "native paint scope follows the resolved frame action");
    }

    using Surface = note::NotePresentationLineSurface;
    for (const bool nativeRawRequested : {false, true}) {
        for (const bool rawSurfaceSuppressed : {false, true}) {
            for (const bool staleEditingLine : {false, true}) {
                const Surface actual = note::ResolveNotePresentationLineSurface(
                    nativeRawRequested, rawSurfaceSuppressed, staleEditingLine);
                const Surface expected = (nativeRawRequested && !rawSurfaceSuppressed)
                    ? Surface::NativeRaw
                    : (staleEditingLine ? Surface::OverlayRaw : Surface::Structured);
                Expect(actual == expected,
                       "each logical line resolves to exactly one presentation surface");
                Expect(note::NotePresentationSurfaceUsesNativeLineMetrics(actual) ==
                           (actual == Surface::NativeRaw),
                       "only the native-raw surface delegates line metrics to RichEdit");
            }
        }
    }

    struct LineCountCase {
        std::wstring_view text;
        size_t expected;
    };
    const std::array<LineCountCase, 7> lineCountCases{{
        {L"", 1},
        {L"one", 1},
        {L"one\n", 2},
        {L"one\ntwo", 2},
        {L"one\r\ntwo", 2},
        {L"one\rtwo\n", 3},
        {L"\r\n\r\n", 3},
    }};
    for (const LineCountCase& test : lineCountCases) {
        Expect(note::CountNoteLogicalLines(test.text) == test.expected,
               "logical line count matches RichEdit and canonical hard breaks");
    }

    Expect(note::RichEditWindowTextLengthForIndexedText(L"# title\nbody") == 13,
           "RichEdit length accounts for CRLF expansion");

    const std::array<note::EditorLineBoundary, 4> boundaries{{
        {0, 12}, {3, 30}, {7, 70}, {10, 55},
    }};
    const note::EditorLineLayout lineLayout(
        note::EditorLineRect{0, 20, 120, 44}, boundaries.data(), boundaries.size());
    Expect(lineLayout.valid(), "editor line layout accepts nondecreasing source boundaries");
    const auto caret = lineLayout.CaretAt(8);
    Expect(caret.has_value() && caret->x == 70 && caret->top == 20 && caret->bottom == 44,
           "editor line layout maps a source offset to the preceding boundary");
    const auto textRange = lineLayout.TextRangeRect(3, 10);
    Expect(textRange.has_value() && textRange->left == 30 && textRange->right == 70,
           "editor line layout spans anchored nonmonotonic X geometry safely");
    const std::array<note::EditorLineBoundary, 3> anchoredRunBoundaries{{
        {6, 200}, {7, 214}, {8, 230},
    }};
    const note::EditorLineLayout anchoredRunLayout(
        note::EditorLineRect{0, 20, 260, 44}, anchoredRunBoundaries.data(), anchoredRunBoundaries.size());
    const auto anchoredRunRange = anchoredRunLayout.TextRangeRect(6, 8);
    Expect(anchoredRunRange.has_value() && anchoredRunRange->left == 200 && anchoredRunRange->right == 230,
           "editor line layout keeps a locally anchored run at its own start X");
    const auto hit = lineLayout.HitTest(54, 30);
    Expect(hit.has_value() && *hit == 10,
           "editor line layout selects the nearest source boundary for hit testing");
    Expect(!lineLayout.HitTest(54, 44).has_value(),
           "editor line layout rejects points outside its vertical band");
    Expect(lineLayout.MaxInlineExtent() == 70,
           "editor line layout reports the maximum inline extent");
    const std::array<note::EditorLineBoundary, 3> duplicateXBoundaries{{
        {0, 12}, {3, 30}, {10, 30},
    }};
    const note::EditorLineLayout duplicateXLineLayout(
        note::EditorLineRect{0, 20, 120, 44}, duplicateXBoundaries.data(), duplicateXBoundaries.size());
    const auto duplicateXHit = duplicateXLineLayout.HitTest(30, 30);
    Expect(duplicateXHit.has_value() && *duplicateXHit == 3,
           "editor line layout preserves the earlier source boundary for equal X positions");
    const std::array<note::EditorLineBoundary, 2> unsortedBoundaries{{
        {3, 0}, {2, 10},
    }};
    const note::EditorLineLayout invalidLineLayout(
        note::EditorLineRect{0, 0, 10, 10}, unsortedBoundaries.data(), unsortedBoundaries.size());
    Expect(!invalidLineLayout.valid(), "editor line layout rejects nonmonotonic source boundaries");
    const std::array<note::EditorLineBoundary, 2> scrolledBoundaries{{
        {0, -24}, {2, -8},
    }};
    const note::EditorLineLayout scrolledLineLayout(
        note::EditorLineRect{0, 0, 40, 20}, scrolledBoundaries.data(), scrolledBoundaries.size());
    Expect(scrolledLineLayout.MaxInlineExtent() == -8,
           "editor line layout preserves a scrolled negative inline extent");
    const note::EditorLineLayout invalidRectLineLayout(
        note::EditorLineRect{4, 0, 3, 20}, scrolledBoundaries.data(), scrolledBoundaries.size());
    Expect(!invalidRectLineLayout.valid(), "editor line layout rejects an inverted line rectangle");

    Expect(note::ResolveNoteDerivedRefreshPlan({false, false, false}).lightweight(),
           "inactive derived presentation takes the lightweight path");
    const note::NoteDerivedRefreshPlan renderPlan =
        note::ResolveNoteDerivedRefreshPlan({true, false, false});
    Expect(renderPlan.refresh_syntax && renderPlan.refresh_render_plan && renderPlan.refresh_assist,
           "active rendering refreshes syntax, render plan, and assist state");

    std::cout << "Summary: failed=" << g_failed << "\n";
    return g_failed == 0 ? 0 : 1;
}
