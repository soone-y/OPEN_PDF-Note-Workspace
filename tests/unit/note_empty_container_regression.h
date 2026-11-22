#pragma once

#include "note/note_kernel.h"
#include "note/note_render_final_win32_adapter.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace note_empty_container_test {
inline bool Run(HDC dc, std::wstring (*decode)(const std::string&)) {
    if (!dc || !decode) return false;
    struct Case { std::wstring raw; size_t caret_line; };
    std::vector<Case> cases = {
        {L"-", 0}, {L"- ", 0}, {L"* ", 0}, {L"+ ", 0}, {L"1. ", 0}, {L"9) ", 0},
        {L"- [ ] ", 0}, {L"- [x] ", 0}, {L">", 0}, {L"> ", 0},
        {L"alpha\n\n- ", 2}, {L"alpha\n\n- \n\n\n", 2},
        {L"alpha\n\n- \n\nbeta", 2}, {L"alpha\n\n* \n\nbeta", 2},
        {L"alpha\n\n1. \n\nbeta", 2}, {L"alpha\n\n> \n\nbeta", 2},
        {L"- first\n- \n- last", 1}, {L"- outer\n  - \n- last", 1},
        {L"# heading\n\nalpha\n\n- \n\nbeta", 4},
    };
    bool ok = true;
    size_t checks = 0;
    const auto check = [&](bool value, const char* label, size_t index) {
        ++checks;
        if (!value) std::cerr << "empty container: " << label << " case=" << index << "\n";
        ok = ok && value;
    };
    std::ifstream sampleFile(std::filesystem::path(
        L"release_assets/sample_workspace/ja/01_講義サンプル/第01回_基本操作/ノート_基本操作.clro"),
        std::ios::binary);
    const std::string sampleBytes((std::istreambuf_iterator<char>(sampleFile)), {});
    const std::wstring sample = decode(sampleBytes);
    check(!sampleBytes.empty() && !sample.empty(), "distribution fixture is available", 0);
    if (sample.empty()) return false;
    for (const auto* tail : {L"\n\n- \n\n\n\n\n", L"\n\n- \n\nTAIL"}) {
        const size_t caretLine = note::BuildLineStarts(sample + L"\n\n").size() - 1;
        cases.push_back({sample + tail, caretLine});
    }
    for (int height : {16, 30}) {
        HFONT font = CreateFontW(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!font) return false;
        HGDIOBJ previous = SelectObject(dc, font);
        if (!previous || previous == HGDI_ERROR) { DeleteObject(font); return false; }
        for (const auto* file : {L"empty.clro", L"empty.md"}) {
            for (const auto* newline : {L"\n", L"\r\n", L"\r"}) {
                size_t index = 0;
                for (const auto& test : cases) {
                    // Normalize the sample's physical line breaks before varying them.
                    std::wstring raw;
                    for (size_t at = 0; at < test.raw.size(); ++at) {
                        if (test.raw[at] == L'\r') {
                            if (at + 1 < test.raw.size() && test.raw[at + 1] == L'\n') ++at;
                            raw += newline;
                        } else if (test.raw[at] == L'\n') raw += newline;
                        else raw += test.raw[at];
                    }
                    note::NoteTextCore core;
                    core.Reset({3001}, {file, L""}, raw, 1, 1);
                    const auto starts = note::BuildLineStarts(raw);
                    for (bool wrap : {false, true}) {
                        note::NoteRenderFinalWin32AdapterState state;
                        note::NoteRenderFinalWin32AdapterStateInput input;
                        input.text_core = &core;
                        input.layout_key = {wrap ? 160u : 640u, 96, 96, 1, 4, 8, wrap};
                        input.measurement_dc = dc;
                        input.owner_input.render_active = true;
                        input.owner_input.editor_text_core_current = true;
                        input.owner_input.visible_lines = {{0}, {core.logical_line_count()}};
                        input.editor_caret = {starts[test.caret_line]};
                        input.editor_selection = {input.editor_caret, input.editor_caret};
                        for (bool editing : {false, true}) {
                            if (editing) {
                                input.owner_input.caret_line = test.caret_line;
                                input.owner_input.requested_editor_lines = {{{test.caret_line}, {test.caret_line + 1}}};
                            }
                            (void)state.Refresh(input);
                            check(state.has_frame(), "empty marker publishes a complete hybrid frame", index);
                            if (!state.has_frame()) continue;
                            const auto& plan = state.frame()->presentation()->owner_plan();
                            size_t rawLines = 0;
                            bool exact = plan && plan->frame().frame_kind == note::NotePresentationFrameKind::DrawCommitted;
                            if (plan) {
                                for (const auto& range : plan->owner_ranges()) {
                                    if (range.owner == note::NotePresentationLineOwner::NativeEditor) {
                                        rawLines += range.lines.last_exclusive.value - range.lines.first.value;
                                        exact = exact && range.lines.first.value == test.caret_line &&
                                            range.lines.last_exclusive.value == test.caret_line + 1;
                                    }
                                }
                            }
                            check(exact && rawLines == (editing ? 1u : 0u),
                                  "only the requested editing row uses raw ownership", index);
                            check(core.MatchesRaw(raw) && core.content_revision() == 1,
                                  "publication preserves source and revision", index);
                        }
                    }
                    ++index;
                }
            }
        }
        SelectObject(dc, previous);
        DeleteObject(font);
    }
    std::cout << "Empty container checks=" << checks << " result=" << (ok ? "PASS" : "FAIL") << "\n";
    return ok;
}
} // namespace note_empty_container_test
