#pragma once

#include "note/note_export.h"
#include "note/note_kernel.h"
#include "note/note_parser.h"
#include "note/note_workspace_service.h"

#include <algorithm>
#include <iostream>

namespace note_quote_test {
inline int DepthAt(const note::NoteDocument& doc, size_t offset, note::BlockKind kind) {
    return static_cast<int>(std::count_if(doc.blocks.begin(), doc.blocks.end(), [&](const auto& block) {
        return block.kind == kind && block.span.start.value <= offset && offset < block.span.end.value;
    }));
}

inline bool Run() {
    struct Case {
        const wchar_t* raw;
        int strict_depth;
        int lazy_depth;
        int list_depth;
    };
    const Case cases[] = {
        {L"> quoted\nTARGET", 0, 1, 0},
        {L">\nTARGET", 0, 0, 0},
        {L"> quoted\n> TARGET", 1, 1, 0},
        {L"> quoted\n\nTARGET", 0, 0, 0},
        {L"> quoted\r\nTARGET", 0, 1, 0},
        {L"> quoted\rTARGET", 0, 1, 0},
        {L"   > quoted\nTARGET", 0, 1, 0},
        {L">> deep\n> TARGET", 1, 2, 0},
        {L">> deep\nTARGET", 0, 2, 0},
        {L">> deep\n>> TARGET", 2, 2, 0},
        {L"- item\nTARGET", 0, 0, 1},
        {L"- > quoted\nTARGET", 0, 1, 1},
        {L"> - item\n> TARGET", 1, 1, 1},
        {L"> - item\nTARGET", 0, 1, 0},
        {L"\\> literal\nTARGET", 0, 0, 0},
        {L"text > literal\nTARGET", 0, 0, 0},
        {L"```\n> literal\nTARGET\n```", 0, 0, 0},
        {L"> ```\n> literal\nTARGET", 0, 0, 0},
        {L"> **bold\nTARGET**", 0, 1, 0},
    };
    bool ok = true;
    size_t checks = 0;
    const auto check = [&](bool value, const char* label) {
        ++checks;
        if (!value) std::cerr << "quote continuation: " << label << "\n";
        ok = ok && value;
    };
    for (const auto* file : {L"quote.md", L"quote.clro"}) {
        for (const auto& test : cases) {
            for (bool lazy : {false, true}) {
                note::NoteMetadata metadata{file, L""};
                metadata.quote_lazy_continuation = lazy;
                metadata.clro_list_implicit_continuation = true;
                const bool effectiveLazy = lazy || std::wstring_view(file) == L"quote.md";
                const auto model = note::MakeNoteTextModel(metadata, test.raw, 17);
                const auto doc = note::ParseNoteDocument(model);
                const size_t offset = model.raw.find(L"TARGET");
                check(DepthAt(doc, offset, note::BlockKind::Quote) == (effectiveLazy ? test.lazy_depth : test.strict_depth),
                      "explicit/missing marker and nested quote depth");
                // Without an outer quote marker, all its nested containers close.
                const int listDepth = effectiveLazy && test.raw == std::wstring(L"> - item\nTARGET") ? 1 : test.list_depth;
                check(DepthAt(doc, offset, note::BlockKind::List) == listDepth,
                      "list lazy continuation remains available");
                check(note::ExportPlainText(model, doc, {}).find("TARGET") != std::string::npos &&
                      model.raw == test.raw && model.revision == 17, "export content and original source survive");
            }
        }
    }

    const std::wstring raw = L"> quoted\nTARGET [link][ref]\n\n[ref]: local.md\n";
    note::NoteMetadata metadata{L"quote.clro", L""};
    note::LocalNoteKernel kernel;
    kernel.Reset({2301}, metadata, raw, 10, 2, note::NoteContentKind::Markdown);
    (void)kernel.RefreshDerived();
    const auto edit = kernel.ApplyUserEdit({{raw.size()}, 0, L"end"}, {{0}, {0}},
        {{raw.size() + 3}, {raw.size() + 3}}, note::NoteHistoryOperationKind::Typing, 1, true);
    check(edit.applied(), "fixture edit accepted");
    const auto revision = kernel.text_core().content_revision();
    const auto root = kernel.text_core().TakeSnapshot();
    check(kernel.SetQuoteLazyContinuation(true) && !kernel.CanReadSyntax(), "switch invalidates derived syntax");
    (void)kernel.RefreshDerived();
    check(kernel.CanReadSyntax() && DepthAt(kernel.document(), raw.find(L"TARGET"), note::BlockKind::Quote) == 1 &&
          kernel.text_core().content_revision() == revision && kernel.text_core().MatchesSnapshot(root),
          "switch rebuilds syntax while preserving canonical root and revision");
    check(std::any_of(kernel.document().inlines.begin(), kernel.document().inlines.end(), [](const auto& node) {
        return node.kind == note::InlineKind::Link && node.target == L"local.md";
    }), "reference links across quote boundary resolve in one parse");
    const auto undo = kernel.Undo(true);
    check(undo.has_value() && kernel.text_core().CopyRawRange({0}, kernel.text_core().text_length()) == raw,
          "undo survives policy switch");
    check(kernel.SetQuoteLazyContinuation(false) && !kernel.SetQuoteLazyContinuation(false), "same policy is a no-op");
    const auto redo = kernel.Redo(true);
    (void)kernel.RefreshDerived();
    check(redo.has_value() && kernel.text_core().CopyRawRange({0}, kernel.text_core().text_length()) == raw + L"end" &&
          DepthAt(kernel.document(), raw.find(L"TARGET"), note::BlockKind::Quote) == 0, "redo uses new policy");

    note::NoteWorkspaceService service;
    const auto identity = note::BuildSnapshotIdentity({2302}, 10, 2, "quote-policy-fixture");
    const auto strictIndex = service.ResolveIndex(identity, L"quote.clro", metadata, raw, note::NoteContentKind::Markdown);
    metadata.quote_lazy_continuation = true;
    const auto lazyIndex = service.ResolveIndex(identity, L"quote.clro", metadata, raw, note::NoteContentKind::Markdown);
    check(strictIndex && lazyIndex && strictIndex != lazyIndex &&
          DepthAt(strictIndex->document, raw.find(L"TARGET"), note::BlockKind::Quote) == 0 &&
          DepthAt(lazyIndex->document, raw.find(L"TARGET"), note::BlockKind::Quote) == 1,
          "same revision under different quote policy cannot reuse stale index");

    std::wstring deep;
    for (size_t depth = 0; depth < 256; ++depth) deep += std::wstring(depth * 2, L' ') + L"- item\n";
    for (size_t line = 0; line < 4096; ++line) deep += L"continuation\n";
    deep += L"TARGET";
    const auto doc = note::ParseNoteDocument(note::MakeNoteTextModel({L"deep.md", L""}, deep));
    check(DepthAt(doc, deep.find(L"TARGET"), note::BlockKind::List) == 256,
          "deep implicit list parents retain cached linear scanning");
    std::cout << "Quote continuation checks=" << checks << " result=" << (ok ? "PASS" : "FAIL") << "\n";
    return ok;
}
} // namespace note_quote_test
