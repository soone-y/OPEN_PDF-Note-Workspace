#pragma once
#include "note/note_markdown_shortcut.h"
#include "note/note_kernel.h"
#include <iostream>

namespace note_markdown_shortcut_test {
inline bool Run() {
    size_t checks = 0;
    bool ok = true;
    const auto check = [&](bool value, const char* label, size_t index) {
        ++checks;
        if (!value) std::cerr << "bold shortcut: " << label << " case=" << index << "\n";
        ok = ok && value;
    };
    struct Case { const wchar_t* raw; size_t start; size_t end; const wchar_t* expected; };
    const Case cases[] = {
        {L"abc", 0, 3, L"**abc**"},
        {L"**abc**", 7, 7, L"abc"},
        {L"**abc**", 3, 3, L"abc"},
        {L"**abc**", 2, 2, L"abc"},
        {L"**abc**", 5, 5, L"abc"},
        {L"**abc**", 0, 7, L"abc"},
        {L"**abc**", 2, 5, L"abc"},
        {L"**abcdef**", 4, 6, L"**ab**cd**ef**"},
        {L"**ab**cd", 3, 8, L"**abcd**"},
        {L"one **two** three", 0, 17, L"**one two three**"},
        {L"**a** **b**", 0, 11, L"a b"},
        {L"***abc***", 9, 9, L"*abc*"},
        {L"***abc***", 0, 9, L"*abc*"},
        {L"a\n\nb", 0, 4, L"**a**\n\n**b**"},
        {L"**a**\n**b**", 0, 11, L"a\nb"},
        {L"**[abc](target)**", 16, 16, L"[abc](target)"},
        {L"**[a](x**y)**", 13, 13, L"[a](x**y)"},
        {L"**`abc`**", 9, 9, L"`abc`"},
        {L"**x \\* y**", 10, 10, L"x \\* y"},
        {L"前😀後", 0, 4, L"**前😀後**"},
        {L"  abc  ", 0, 7, L"  **abc**  "},
        {L"", 0, 0, L"****"},
        {L"****", 2, 2, L""},
        {L"`**abc**`", 5, 5, nullptr},
        {L"```\n**abc**\n```", 8, 8, nullptr},
        {L"$x**y**$", 4, 4, nullptr},
    };
    for (const auto* file : {L"shortcut.clro", L"shortcut.md"}) {
        size_t index = 0;
        for (const auto& test : cases) {
            ++index;
            const auto model = note::MakeNoteTextModel({file, L""}, test.raw, 1);
            const auto proposal = note::BuildMarkdownBoldToggle(model, {{test.start}, {test.end}});
            if (!test.expected) { check(!proposal, "literal protection", index); continue; }
            check(proposal.has_value(), "proposal", index);
            if (!proposal) continue;
            std::wstring changed = test.raw;
            changed.replace(proposal->edit.start.value, proposal->edit.deleted_len, proposal->edit.inserted_text);
            if (changed != test.expected) std::wcerr << L"actual=" << changed << L" expected=" << test.expected << L"\n";
            check(changed == test.expected, "toggle and mixed selection", index);
            check(proposal->selection.start <= proposal->selection.end &&
                  proposal->selection.end.value <= proposal->edit.inserted_text.size(), "UTF-16 selection", index);
            note::LocalNoteKernel kernel;
            kernel.Reset({2801}, {file, L""}, test.raw, 1, 1, note::NoteContentKind::Markdown);
            const note::NoteTextSelection before{{test.start}, {test.end}};
            const note::NoteTextSelection after{proposal->selection.start + proposal->edit.start.value,
                                              proposal->selection.end + proposal->edit.start.value};
            check(kernel.ApplyUserEdit(proposal->edit, before, after,
                      note::NoteHistoryOperationKind::Replace, 100, false).applied() &&
                  kernel.text_core().MatchesRaw(changed), "single replacement", index);
            const auto undo = kernel.Undo(false);
            check(undo && undo->applied() && kernel.text_core().MatchesRaw(test.raw) && !kernel.CanUndo(),
                  "one undo restores complete source", index);
            const auto redo = kernel.Redo(false);
            check(redo && redo->applied() && kernel.text_core().MatchesRaw(changed) && !kernel.CanRedo(),
                  "one redo reapplies complete source", index);
        }
    }
    // Repeat the real command sequence using its returned caret position.
    auto model = note::MakeNoteTextModel({L"repeat.clro", L""}, L"abc", 1);
    const auto add = note::BuildMarkdownBoldToggle(model, {{0}, {3}});
    check(add.has_value(), "repeat add", 0);
    if (add) {
        model.raw.replace(add->edit.start.value, add->edit.deleted_len, add->edit.inserted_text);
        const size_t caret = add->edit.start.value + add->selection.end.value;
        const auto remove = note::BuildMarkdownBoldToggle(model, {{caret}, {caret}});
        check(remove.has_value(), "repeat removes after closing marker", 0);
        if (remove) {
            model.raw.replace(remove->edit.start.value, remove->edit.deleted_len, remove->edit.inserted_text);
            check(model.raw == L"abc", "repeat original source", 0);
        }
    }
    note::LocalNoteKernel history;
    history.Reset({2802}, {L"history.clro", L""}, L"lead ", 1, 1, note::NoteContentKind::Markdown);
    check(history.ApplyUserEdit({{5}, 0, L"abc"}, {{5}, {5}}, {{8}, {8}},
          note::NoteHistoryOperationKind::Typing, 100, false).applied(), "history preceding input", 0);
    auto boldModel = note::MakeNoteTextModel({L"history.clro", L""}, L"lead abc", 1);
    const auto command = note::BuildMarkdownBoldToggle(boldModel, {{5}, {8}});
    check(command && history.ApplyUserEdit(command->edit, {{5}, {8}}, {{12}, {12}},
          note::NoteHistoryOperationKind::Replace, 110, false).applied(), "history command boundary", 0);
    check(history.ApplyUserEdit({{12}, 0, L"!"}, {{12}, {12}}, {{13}, {13}},
          note::NoteHistoryOperationKind::Typing, 120, false).applied(), "history following input", 0);
    for (const auto* expected : {L"lead **abc**", L"lead abc", L"lead "}) {
        const auto replay = history.Undo(false);
        check(replay && replay->applied() && history.text_core().MatchesRaw(expected), "undo distinct steps", 0);
    }
    for (const auto* expected : {L"lead abc", L"lead **abc**", L"lead **abc**!"}) {
        const auto replay = history.Redo(false);
        check(replay && replay->applied() && history.text_core().MatchesRaw(expected), "redo distinct steps", 0);
    }
    std::cout << "Bold shortcut checks=" << checks << " result=" << (ok ? "PASS" : "FAIL") << "\n";
    return ok;
}
} // namespace note_markdown_shortcut_test
