#pragma once

#include "note/note_render_final_display_run.h"
#include "note/note_render_final_gdi_measurement_provider.h"
#include "note/note_render_final_interaction.h"
#include "note/note_render_final_transaction.h"
#include "note/note_render_final_win32_adapter.h"

#include <algorithm>
#include <iostream>

namespace note_whitespace_test {
struct Result {
    bool display = true, geometry = true, editing = true, reuse = true;
    size_t cases = 0;
};

[[nodiscard]] inline std::wstring DisplayLine(const note::NoteRenderSourcePlan& plan, size_t line) {
    note::NoteRenderSourceLinePlan source;
    if (!plan.ResolveLine({line}, &source)) return L"[unresolved]";
    std::wstring text;
    for (const auto& run : source.runs) {
        note::NoteRenderFinalDisplayRun display;
        if (note::BuildNoteRenderFinalDisplayRun(*plan.syntax(), run, &display) !=
            note::NoteRenderFinalDisplayRunBuildResult::Built) return L"[invalid]";
        text += display.display_text;
    }
    return text;
}

// Borrowed memory DC: no user windows, files or settings are modified.
[[nodiscard]] inline Result Run(HDC dc) {
    if (!dc) return {false, false, false, false, 0};
    struct Case { const wchar_t* raw; size_t line; const wchar_t* visible; bool prefix; };
    const Case cases[] = {
        {L"abcd", 0, L"abcd", false},
        {L" abcd", 0, L" abcd", true},
        {L"  abcd", 0, L"  abcd", true},
        {L"   abcd", 0, L"   abcd", true},
        {L"before\r\n  abcd\r\nafter", 1, L"  abcd", true},
        {L"   ", 0, L"   ", true},
        {L" \t ", 0, L" \t ", true},
        {L"\u3000abcd", 0, L"\u3000abcd", true},
        {L"\u00a0abcd", 0, L"\u00a0abcd", false},
        {L"  **abcd**", 0, L"  abcd", true},
        {L"  `abcd`", 0, L"  abcd", true},
        {L"  <s=32>abcd</>", 0, L"  abcd", true},
        {L"<s=32>before\n  abcd</>", 1, L"  abcd", true},
        {L"::: note\n  abcd\n:::", 1, L"  abcd", true},
        {L"```\n  abcd\n```", 1, L"  abcd", false},
        {L"    abcd", 0, L"    abcd", true},
        {L"\tabcd", 0, L"\tabcd", true},
        {L"  - abcd", 0, L"abcd", false},
        {L"- item\n  - abcd", 1, L"abcd", false},
        {L"  > abcd", 0, L"abcd", false},
        {L"  # abcd", 0, L"abcd", false},
        {L"  | H |\n  | --- |\n  | abcd |", 2, L"abcd", false},
        {L"  $x$", 0, L"  x", true},
        {L"before\n\n  ", 2, L"  ", true}
    };
    Result result;
    note::NoteRenderFinalGdiMeasurementProvider provider(dc);
    note::NoteRenderFinalCompleteBuildInput input;
    input.content_kind = note::NoteContentKind::Markdown;
    input.layout_key = {640, 96, 96, 1, 4, 8, true};
    input.owner_input.render_active = true;
    input.owner_input.editor_text_core_current = true;
    for (const auto& test : cases) {
        note::NoteTextCore core;
        core.Reset(note::NoteId{12000}, {L"spaces.clro", L"spaces"}, test.raw, 1, 1);
        input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
        std::shared_ptr<const note::NoteRenderFinalPublication> frame;
        if (note::NoteRenderFinalTransaction::BuildComplete(core, input, provider, &frame) !=
                note::NoteRenderFinalCompleteBuildResult::Built || !frame) {
            std::cerr << "whitespace: frame failed case=" << result.cases << '\n';
            return {false, false, false, false, result.cases};
        }
        if (DisplayLine(*frame->source_plan(), test.line) != test.visible) {
            std::cerr << "whitespace: display mismatch case=" << result.cases << '\n';
            note::NoteRenderSourceLinePlan debugLine;
            if (frame->source_plan()->ResolveLine({test.line}, &debugLine)) {
                std::cerr << "flags=" << debugLine.decoration.flags << " actual codes=";
                for (const auto ch : DisplayLine(*frame->source_plan(), test.line)) std::cerr << int(ch) << ',';
                std::cerr << '\n';
            }
            result.display = false;
        }
        result.editing = result.editing && core.MatchesRaw(test.raw);
        if (test.prefix) {
            note::NoteRenderSourceLinePlan source;
            note::NoteRenderLinePlacement placed;
            const auto line = note::NoteRenderLineLayoutMap::LineAt(frame->layout()->line_layouts(), {test.line});
            if (!line || !frame->source_plan()->ResolveLine({test.line}, &source) || source.runs.empty() ||
                !frame->placement()->ResolveLine({test.line}, &placed) || placed.runs.empty())
                return {false, false, false, false, result.cases};
            const auto& prefix = source.runs.front();
            const auto& measured = placed.runs.front();
            const auto offset = prefix.source_span.start + 1;
            const auto boundary = std::find_if(measured.boundaries.begin(), measured.boundaries.end(),
                [&](const auto& value) { return value.source_offset == offset; });
            result.geometry = result.geometry && prefix.kind == note::NoteRenderSourceRunKind::Text &&
                measured.width_px > 0 && boundary != measured.boundaries.end() && boundary->x_px > measured.x_px;
            if (boundary == measured.boundaries.end()) return {false, false, false, false, result.cases};
            if (prefix.styles.empty()) {
                SIZE space{};
                bool widthOk = GetTextExtentPoint32W(dc, L" ", 1, &space) && space.cx > 0;
                int expectedWidth = 0;
                const auto text = frame->source_plan()->syntax()->CopySourceRange(
                    prefix.source_span.start, prefix.source_span.end - prefix.source_span.start);
                for (const wchar_t ch : text) {
                    SIZE glyph{};
                    if (ch == L'\t' && widthOk) expectedWidth += space.cx * 4 - expectedWidth % (space.cx * 4);
                    else if (GetTextExtentPoint32W(dc, &ch, 1, &glyph)) expectedWidth += glyph.cx;
                    else widthOk = false;
                }
                result.geometry = result.geometry && widthOk && measured.width_px == expectedWidth;
            }
            std::vector<note::NoteRenderFinalRect> selected;
            result.editing = result.editing && note::ResolveNoteRenderFinalSelection(*frame,
                prefix.source_span, &selected) == note::NoteRenderFinalInteractionResult::Resolved &&
                selected.size() == 1 && selected.front().left_px == measured.x_px &&
                selected.front().right_px == measured.x_px + measured.width_px;
            note::NoteRenderFinalWin32AdapterInput rawInput;
            rawInput.text_core = &core;
            rawInput.structural_publication = frame;
            rawInput.owner_input = input.owner_input;
            rawInput.measurement_dc = dc;
            std::shared_ptr<const note::NoteRenderFinalWin32AdapterFrame> structured;
            note::NoteRenderFinalHit hit;
            result.editing = result.editing &&
                note::NoteRenderFinalWin32AdapterFrame::Build(rawInput, &structured) ==
                    note::NoteRenderFinalWin32AdapterBuildResult::Built && structured &&
                structured->HitTest(boundary->x_px, line->top_px, &hit) ==
                    note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                hit.source_offset == offset;
            rawInput.owner_input.caret_line = test.line;
            rawInput.owner_input.requested_editor_lines = {{{test.line}, {test.line + 1}}};
            rawInput.editor_selection = {offset, offset};
            rawInput.editor_caret = offset;
            std::shared_ptr<const note::NoteRenderFinalWin32AdapterFrame> raw;
            note::NoteRenderFinalCaretGeometry caret;
            result.editing = result.editing && note::NoteRenderFinalWin32AdapterFrame::Build(rawInput, &raw) ==
                note::NoteRenderFinalWin32AdapterBuildResult::Built && raw &&
                raw->Caret(&caret) == note::NoteRenderFinalPresentationInteractionResult::Resolved &&
                caret.source_offset == offset && raw->presentation()->SurfaceAt({test.line}) ==
                note::NoteRenderFinalSurfaceKind::RawPaint;
        }
        ++result.cases;
    }
    // Body edits still reuse unaffected rows after whitespace becomes visible.
    note::NoteTextCore core;
    core.Reset(note::NoteId{12001}, {L"reuse.clro", L"reuse"}, L"before\n  alpha\nsuffix", 1, 1);
    input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
    std::shared_ptr<const note::NoteRenderFinalPublication> before, patched, complete;
    const bool built = note::NoteRenderFinalTransaction::BuildComplete(core, input, provider, &before) ==
        note::NoteRenderFinalCompleteBuildResult::Built;
    note::TextEdit edit{{11}, 0, L"Z"};
    const bool edited = core.Apply(edit) == note::NoteTextApplyResult::Applied;
    note::NoteRenderFinalLocalPatchInput local;
    local.layout_key = input.layout_key;
    local.owner_input = input.owner_input;
    local.edit = edit;
    const auto patchResult = built && edited
        ? note::NoteRenderFinalTransaction::BuildLocalPlainTextPatch(before, core, local, provider, &patched)
        : note::NoteRenderFinalCompleteBuildResult::InvalidEdit;
    result.reuse = patchResult == note::NoteRenderFinalCompleteBuildResult::Built && patched &&
        note::NoteRenderFinalTransaction::BuildComplete(core, input, provider, &complete) ==
            note::NoteRenderFinalCompleteBuildResult::Built && complete &&
        DisplayLine(*patched->source_plan(), 1) == DisplayLine(*complete->source_plan(), 1) &&
        patched->source_plan()->SharesLinePayloadForDifferentialTest(*before->source_plan(), {0}) &&
        patched->source_plan()->SharesLinePayloadForDifferentialTest(*before->source_plan(), {2});
    const note::TextEdit boundaryEdits[] = {
        {{7}, 0, L" "}, {{8}, 0, L" "}, {{9}, 0, L" "},
        {{7}, 1, L""}, {{8}, 1, L""}, {{7}, 2, L"\t"}
    };
    for (const auto& boundaryEdit : boundaryEdits) {
        core.Reset(note::NoteId{12002}, {L"boundary.clro", L"boundary"}, L"before\n  alpha\nsuffix", 1, 1);
        before.reset(); patched.reset(); complete.reset();
        if (note::NoteRenderFinalTransaction::BuildComplete(core, input, provider, &before) !=
                note::NoteRenderFinalCompleteBuildResult::Built ||
            core.Apply(boundaryEdit) != note::NoteTextApplyResult::Applied ||
            note::NoteRenderFinalTransaction::BuildComplete(core, input, provider, &complete) !=
                note::NoteRenderFinalCompleteBuildResult::Built) {
            result.editing = false;
            continue;
        }
        local.edit = boundaryEdit;
        const auto boundaryResult = note::NoteRenderFinalTransaction::BuildLocalPlainTextPatch(
            before, core, local, provider, &patched);
        const bool valid = boundaryResult == note::NoteRenderFinalCompleteBuildResult::Built
            ? patched && DisplayLine(*patched->source_plan(), 1) == DisplayLine(*complete->source_plan(), 1)
            : boundaryResult == note::NoteRenderFinalCompleteBuildResult::RequiresNativeFallback && !patched;
        if (!valid) std::cerr << "whitespace: local/full mismatch at offset=" << boundaryEdit.start.value << '\n';
        result.editing = result.editing && valid;
    }
    return result;
}
} // namespace note_whitespace_test
