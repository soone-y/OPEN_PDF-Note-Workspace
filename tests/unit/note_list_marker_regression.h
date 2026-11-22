#pragma once

#include "note/note_render_final_gdi_measurement_provider.h"

#include <algorithm>
#include <iostream>
#include <limits>

namespace note_list_marker_test {
inline bool Run(HDC dc) {
    if (!dc) return false;
    struct Case { const wchar_t* raw; size_t line; const wchar_t* marker; bool continuation = false; };
    const Case cases[] = {
        {L"1. 本文 alpha beta gamma delta epsilon zeta", 0, L"1."},
        {L"99. 本文 alpha beta gamma delta epsilon zeta", 0, L"99."},
        {L"999999999. 本文 alpha beta gamma delta epsilon zeta", 0, L"999999999."},
        {L"1. parent\n\n   99. 本文 alpha beta gamma delta epsilon zeta", 2, L"99."},
        {L"> 99. 本文 alpha beta gamma delta epsilon zeta", 0, L"99."},
        {L"::: note\n99. 本文 alpha beta gamma delta epsilon zeta\n:::", 1, L"99."},
        {L"- 本文 alpha beta gamma delta epsilon zeta", 0, L"•"},
        {L"- first\nTARGET", 1, nullptr, true},
        {L"9. first\n   TARGET", 1, nullptr, true},
        {L"- [x] first\n  TARGET", 1, nullptr, true},
    };
    bool ok = true;
    size_t checks = 0;
    const auto check = [&](bool value, const char* label) {
        ++checks;
        if (!value) std::cerr << "list marker: " << label << " check=" << checks << "\n";
        ok = ok && value;
    };
    for (const auto* face : {L"Consolas", L"Segoe UI"}) {
        for (int height : {16, 32}) {
            HFONT font = CreateFontW(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, face);
            if (!font) return false;
            HGDIOBJ previous = SelectObject(dc, font);
            if (!previous || previous == HGDI_ERROR) { DeleteObject(font); return false; }
            SIZE gap{};
            const bool measured = GetTextExtentPoint32W(dc, L" ", 1, &gap) != FALSE && gap.cx > 0;
            check(measured, "base-font gap measurement");
            for (bool wrap : {false, true}) {
                for (const auto& test : cases) {
                    note::NoteTextCore core;
                    core.Reset({2501}, {test.continuation ? L"list.md" : L"list.clro", L""}, test.raw, 1, 1);
                    note::NoteRenderFinalCompleteBuildInput input;
                    input.content_kind = note::NoteContentKind::Markdown;
                    input.layout_key = {wrap ? 120u : 640u, 96, 96, 1, 4, 8, wrap};
                    input.owner_input.render_active = true;
                    input.owner_input.editor_text_core_current = true;
                    input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
                    note::NoteRenderFinalGdiMeasurementProvider provider(dc);
                    std::shared_ptr<const note::NoteRenderFinalPublication> publication;
                    const auto result = note::NoteRenderFinalTransaction::BuildComplete(core, input, provider, &publication);
                    note::NoteRenderLinePlacement placement;
                    const bool built = result == note::NoteRenderFinalCompleteBuildResult::Built && publication &&
                        publication->placement() && publication->placement()->ResolveLine({test.line}, &placement);
                    check(built, "publication and list placement");
                    if (!built) continue;
                    const auto marker = std::find_if(placement.decorations.begin(), placement.decorations.end(), [](const auto& value) {
                        return value.kind == note::NoteRenderVisualDecorationKind::ListMarker;
                    });
                    if (test.continuation) {
                        check(marker == placement.decorations.end() &&
                              std::none_of(placement.decorations.begin(), placement.decorations.end(), [](const auto& value) {
                                  return value.kind == note::NoteRenderVisualDecorationKind::TaskCheckbox;
                              }), "continuation rows have no duplicate marker or checkbox");
                        note::NoteRenderSourceLinePlan sourceLine;
                        check(publication->source_plan()->ResolveLine({test.line}, &sourceLine) &&
                              sourceLine.decoration.has(note::NoteRenderSourceLineDecorationListItem) &&
                              sourceLine.decoration.list_continuation,
                              "continuation rows retain list indentation");
                        continue;
                    }
                    check(marker != placement.decorations.end() && marker->text == test.marker, "number and bullet text");
                    if (marker == placement.decorations.end()) continue;
                    SIZE glyphs{};
                    const bool glyphSize = GetTextExtentPoint32W(dc, marker->text.data(),
                        static_cast<int>(marker->text.size()), &glyphs) != FALSE;
                    check(glyphSize && marker->left_px >= 8 && marker->right_px - marker->left_px >= glyphs.cx,
                          "entire marker fits beyond the initial left padding");
                    int firstText = std::numeric_limits<int>::max();
                    for (const auto& run : placement.runs) {
                        for (const auto& fragment : run.fragments) {
                            if (fragment.width_px > 0) firstText = std::min(firstText, fragment.x_px);
                        }
                    }
                    check(firstText != std::numeric_limits<int>::max() && firstText - marker->right_px >= gap.cx,
                          "body and wrapped rows retain a measured gap after the marker");
                    check(core.CopyRawRange({0}, core.text_length()) == test.raw && core.content_revision() == 1,
                          "layout retains original text and revision");
                }
            }
            SelectObject(dc, previous);
            DeleteObject(font);
        }
    }
    std::cout << "List marker checks=" << checks << " result=" << (ok ? "PASS" : "FAIL") << "\n";
    return ok;
}
} // namespace note_list_marker_test
