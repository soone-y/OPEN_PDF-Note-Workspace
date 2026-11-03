#include <iostream>
#include <limits>
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
    for (const bool rendering : {false, true}) {
        for (const bool vim : {false, true}) {
            const auto available = note::ResolveNoteInteractionControlAvailability(rendering, vim);
            Expect(available.raw_wrap == !rendering &&
                       available.normal_caret_raw == (rendering && vim) &&
                       available.click_enters_insert == vim,
                   "settings and status assist share applicability without erasing inactive preferences");
        }
    }

    const note::NoteImeClientRect imeCaret{120, 30, 121, 50};
    const note::NoteImeClientRect imeViewport{0, 0, 320, 100};
    const auto imePlacement = note::ResolveNoteImeCandidatePlacement(imeCaret, imeCaret, imeViewport, 6);
    Expect(imePlacement && imePlacement->exclusion.left == 0 && imePlacement->exclusion.right == 320 &&
               imePlacement->exclusion.top == 24 && imePlacement->exclusion.bottom == 56 &&
               imePlacement->anchor_x == 120 && imePlacement->anchor_y == 30,
           "IME candidates avoid the input row plus a gap, not only the caret column");
    const auto imeHighDpi = note::ResolveNoteImeCandidatePlacement(
        {240, 60, 242, 100}, {240, 60, 242, 100}, {0, 0, 640, 200}, 12);
    Expect(imeHighDpi && imeHighDpi->exclusion.top == 48 && imeHighDpi->exclusion.bottom == 112,
           "IME candidate clearance scales with supplied client pixel geometry and DPI gap");
    const auto imeWrapped = note::ResolveNoteImeCandidatePlacement(
        imeCaret, {0, 5, 320, 90}, imeViewport, 6);
    Expect(imeWrapped && imeWrapped->exclusion.top == -1 && imeWrapped->exclusion.bottom == 96,
           "wrapped preedit protects all visible input bands above and below the active caret");
    for (const int allocatedHeight : {20, 36, 64, 120}) {
        for (const int scalePercent : {100, 150, 200}) {
            const int top = 30 * scalePercent / 100;
            const int bottom = top + allocatedHeight * scalePercent / 100;
            const int gap = 6 * scalePercent / 100;
            const auto paddedRow = note::ResolveNoteImeCandidatePlacement(
                {120, top, 121, top + 20 * scalePercent / 100},
                {0, top, 320, bottom}, {0, 0, 320, 400}, gap);
            Expect(paddedRow && paddedRow->exclusion.bottom == bottom + gap &&
                       paddedRow->exclusion.bottom - bottom == gap && paddedRow->anchor_y == top,
                   "IME clearance stays constant below allocated rows while its point of interest remains at the caret");
        }
    }
    const auto imeLastRow = note::ResolveNoteImeCandidatePlacement(
        {310, 80, 311, 100}, {310, 80, 311, 100}, imeViewport, 6);
    Expect(imeLastRow && imeLastRow->anchor_y == 80 && imeLastRow->exclusion.bottom == 106,
           "last-row IME exclusion retains its gap but never moves the point of interest beyond the client edge");
    const auto imePartlyClipped = note::ResolveNoteImeCandidatePlacement(
        {-10, -5, -9, 15}, {-10, -5, -9, 15}, imeViewport, 6);
    Expect(imePartlyClipped && imePartlyClipped->anchor_x == 0 && imePartlyClipped->anchor_y == 0 &&
               imePartlyClipped->exclusion.top == -6 && imePartlyClipped->exclusion.bottom == 21,
           "IME placement uses visible input pixels and a bounded horizontal anchor after scrolling");
    Expect(!note::ResolveNoteImeCandidatePlacement(imeCaret, imeCaret, {0, 0, 0, 100}, 6) &&
               !note::ResolveNoteImeCandidatePlacement(imeCaret, imeCaret, {0, 0, 320, 0}, 6) &&
               !note::ResolveNoteImeCandidatePlacement({0, 100, 1, 120}, {0, 100, 1, 120}, imeViewport, 6) &&
               !note::ResolveNoteImeCandidatePlacement({0, -20, 1, 0}, {0, -20, 1, 0}, imeViewport, 6) &&
               !note::ResolveNoteImeCandidatePlacement(imeCaret, {0, 35, 320, 50}, imeViewport, 6) &&
               !note::ResolveNoteImeCandidatePlacement(imeCaret, imeCaret, imeViewport, -1),
           "unavailable, off-screen, inconsistent and invalid IME geometry never invents an anchor");
    const int pixelMax = std::numeric_limits<int>::max();
    const auto imeExtreme = note::ResolveNoteImeCandidatePlacement(
        {pixelMax - 2, pixelMax - 20, pixelMax - 1, pixelMax},
        {0, pixelMax - 20, pixelMax, pixelMax},
        {0, 0, pixelMax, pixelMax}, pixelMax);
    Expect(imeExtreme && imeExtreme->exclusion.bottom == pixelMax &&
               imeExtreme->exclusion.top == -20 && imeExtreme->anchor_x == pixelMax - 2,
           "IME pixel arithmetic saturates instead of overflowing at extreme dimensions");

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
    Expect(note::NoteEditingSelectionOwnsRawLine(false, 0, 0, 7, 7) &&
               !note::NoteEditingSelectionOwnsRawLine(false, 0, 0, 7, 6) &&
               note::NoteEditingSelectionOwnsRawLine(true, 4, 7, 0, 4) &&
               note::NoteEditingSelectionOwnsRawLine(true, 4, 7, 0, 7) &&
               !note::NoteEditingSelectionOwnsRawLine(true, 4, 7, 0, 8) &&
               note::NoteEditingSelectionOwnsRawLine(true, 7, 4, 0, 5),
           "an Insert caret or selection owns exactly its raw rows");
    Expect(note::NoteNormalCaretPreferenceOwnsRawLine(true, false, true, 7, 7) &&
               !note::NoteNormalCaretPreferenceOwnsRawLine(false, false, true, 7, 7) &&
               !note::NoteNormalCaretPreferenceOwnsRawLine(true, true, true, 7, 7) &&
               !note::NoteNormalCaretPreferenceOwnsRawLine(true, false, false, 7, 7) &&
               !note::NoteNormalCaretPreferenceOwnsRawLine(true, false, true, 6, 7),
           "a Vim normal-mode preference owns only its current non-visual raw line");
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

    {
        using ImeEvent = note::NoteImePresentationEvent;
        using ImePhase = note::NoteImePresentationPhase;
        ImePhase phase = ImePhase::Idle;
        phase = note::AdvanceNoteImePresentationPhase(phase, ImeEvent::StartComposition);
        Expect(note::NoteImePresentationPhaseHasLivePreedit(phase),
               "IME start enters the live-preedit presentation phase");
        phase = note::AdvanceNoteImePresentationPhase(phase, ImeEvent::ObservedNoCompositionText);
        Expect(phase == ImePhase::Preedit,
               "an empty provisional replacement cannot become a canonical deletion before IME commit or cancellation");
        phase = note::AdvanceNoteImePresentationPhase(phase, ImeEvent::ResultCommitted);
        Expect(phase == ImePhase::ResultCommittedAwaitingEnd &&
                   !note::NoteImePresentationPhaseHasLivePreedit(phase),
               "an acknowledged IME result ends provisional presentation before WM_IME_END");
        phase = note::AdvanceNoteImePresentationPhase(
            phase, ImeEvent::ObservedCompositionText);
        Expect(phase == ImePhase::ResultCommittedAwaitingEnd &&
                   !note::NoteImePresentationPhaseHasLivePreedit(phase),
               "residual GCS_COMPSTR cannot revive a committed IME preedit");
        phase = note::AdvanceNoteImePresentationPhase(phase, ImeEvent::EndComposition);
        Expect(phase == ImePhase::Idle,
               "IME end clears the committed-result presentation latch");
        for (const ImeEvent terminalEvent : {ImeEvent::ResultCommitted, ImeEvent::CancelComposition}) {
            const ImePhase expected = terminalEvent == ImeEvent::ResultCommitted
                ? ImePhase::ResultCommittedAwaitingEnd : ImePhase::CancelledAwaitingEnd;
            phase = note::AdvanceNoteImePresentationPhase(ImePhase::Preedit, terminalEvent);
            bool pollingKeepsTerminalPhase = phase == expected;
            for (const ImeEvent observation : {ImeEvent::ObservedNoCompositionText,
                                               ImeEvent::ObservedCompositionText,
                                               ImeEvent::ObservedNoCompositionText,
                                               ImeEvent::ObservedCompositionText}) {
                phase = note::AdvanceNoteImePresentationPhase(phase, observation);
                pollingKeepsTerminalPhase = pollingKeepsTerminalPhase && phase == expected &&
                    !note::NoteImePresentationPhaseHasLivePreedit(phase);
            }
            Expect(pollingKeepsTerminalPhase,
                   "empty and residual IMM observations cannot revive an acknowledged result or cancellation");
            Expect(note::AdvanceNoteImePresentationPhase(phase, ImeEvent::StartComposition) ==
                       ImePhase::Preedit &&
                   note::AdvanceNoteImePresentationPhase(phase, ImeEvent::EndComposition) ==
                       ImePhase::Idle &&
                   note::AdvanceNoteImePresentationPhase(phase, ImeEvent::FocusLost) ==
                       ImePhase::Idle,
                   "only the next start, end or focus loss releases an IME terminal presentation latch");
        }
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

    Expect(note::RichEditWindowTextLengthForIndexedText(L"# title\nbody") == 13,
           "RichEdit length accounts for CRLF expansion");

    Expect(note::ResolveNoteDerivedRefreshPlan({false, false, false}).lightweight(),
           "inactive derived presentation takes the lightweight path");
    const note::NoteDerivedRefreshPlan renderPlan =
        note::ResolveNoteDerivedRefreshPlan({true, false, false});
    Expect(renderPlan.refresh_syntax && renderPlan.refresh_render_plan && renderPlan.refresh_assist,
           "active rendering refreshes syntax, render plan, and assist state");

    std::cout << "Summary: failed=" << g_failed << "\n";
    return g_failed == 0 ? 0 : 1;
}
