#pragma once

#include "core/note_text_metadata.h"
#include "note/note_export.h"
#include "note/note_workspace_service.h"
#include "search/search_note_preferences.h"

#include <algorithm>
#include <iostream>

namespace note_syntax_preferences_test {
inline bool Run() {
    bool ok = true;
    size_t checks = 0;
    const auto check = [&](bool value, const char* label, unsigned mask) {
        ++checks;
        if (!value) std::cerr << "syntax preferences: " << label << " mask=" << mask << "\n";
        ok = ok && value;
    };
    const auto same = [](const WorkspaceConfig& a, const WorkspaceConfig& b) {
        return a.clroAsciiOnly == b.clroAsciiOnly && a.noteQuoteLazyContinuation == b.noteQuoteLazyContinuation &&
            a.clroListImplicitContinuation == b.clroListImplicitContinuation && a.clroListBodyAlignment == b.clroListBodyAlignment;
    };
    const std::wstring raw = L"<c=赤>COLOR</>\n\n- ITEM\nTAIL\n\n> QUOTE\nREST";
    note::NoteWorkspaceService service;
    const auto identity = note::BuildSnapshotIdentity({3301}, 1, 1, u8"<c=赤>COLOR</>\n\n- ITEM\nTAIL\n\n> QUOTE\nREST");
    for (unsigned mask = 0; mask < 16; ++mask) {
        WorkspaceConfig config;
        config.clroAsciiOnly = (mask & 1) != 0;
        config.noteQuoteLazyContinuation = (mask & 2) != 0;
        config.clroListImplicitContinuation = (mask & 4) != 0;
        config.clroListBodyAlignment = (mask & 8) != 0 && !config.clroListImplicitContinuation;
        search::Writer wire;
        search::WriteNoteSyntaxPreferences(wire, config);
        wire.Put<uint32_t>(0x24681357);
        WorkspaceConfig received;
        search::Reader reader(wire.bytes);
        search::ReadNoteSyntaxPreferences(reader, received);
        check(same(config, received) && reader.Get<uint32_t>() == 0x24681357,
              "all four child-process fields preserve order and following data", mask);
        reader.End();
        for (const auto* file : {L"syntax.clro", L"syntax.CLRO", L"syntax.md", L"syntax.markdown", L"syntax.txt", L"syntax.tex"}) {
            const auto metadata = BuildNoteTextMetadata(received, file);
            const bool clro = note::IsClroMarkdownFile(file);
            check(metadata.file_name == file && metadata.title == L"syntax" &&
                  metadata.clro_ascii_only == (clro && config.clroAsciiOnly) &&
                  metadata.quote_lazy_continuation == (clro && config.noteQuoteLazyContinuation) &&
                  metadata.clro_list_implicit_continuation == (clro && config.clroListImplicitContinuation) &&
                  metadata.clro_list_body_alignment == (clro && config.clroListBodyAlignment),
                  "shared metadata applies preferences only to CLRO", mask);
        }
        const auto metadata = BuildNoteTextMetadata(config, L"syntax.clro");
        const auto model = note::MakeNoteTextModel(metadata, raw, 1);
        const auto doc = note::ParseNoteDocument(model);
        const auto index = service.ResolveIndex(identity, L"syntax.clro",
            BuildNoteTextMetadata(received, L"syntax.clro"), raw, note::NoteContentKind::Markdown);
        check(index && index->valid && index->text_model.meta.clro_ascii_only == config.clroAsciiOnly &&
              index->text_model.meta.quote_lazy_continuation == config.noteQuoteLazyContinuation &&
              index->text_model.meta.clro_list_implicit_continuation == config.clroListImplicitContinuation &&
              index->text_model.meta.clro_list_body_alignment == config.clroListBodyAlignment,
              "search index and cache use the received preference set", mask);
        if (!index || !index->valid) continue;
        const auto inBlock = [&](const note::NoteDocument& document, note::BlockKind kind, size_t offset) {
            return std::any_of(document.blocks.begin(), document.blocks.end(), [&](const auto& block) {
                return block.kind == kind && block.span.start.value <= offset && offset < block.span.end.value;
            });
        };
        for (const auto* document : {&doc, &index->document}) {
            check(document->style_spans.empty() == config.clroAsciiOnly,
                  "Japanese color interpretation agrees in editor and search", mask);
            check(inBlock(*document, note::BlockKind::List, raw.find(L"TAIL")) == config.clroListImplicitContinuation,
                  "implicit list scope agrees in editor and search", mask);
            check(inBlock(*document, note::BlockKind::Quote, raw.find(L"REST")) == config.noteQuoteLazyContinuation,
                  "implicit quote scope agrees in editor and search", mask);
        }
        check(note::ExportPlainText(model, doc, {}) == note::ExportPlainText(index->text_model, index->document, {}),
              "export interpretation agrees with the search snapshot", mask);
        check(index->text_model.raw == raw && model.raw == raw && index->text_model.revision == 1,
              "preference changes preserve canonical text and revision", mask);
        const auto cached = service.ResolveIndex(identity, L"syntax.clro", metadata, raw, note::NoteContentKind::Markdown);
        check(cached == index, "unchanged preference set reuses the index", mask);
    }
    WorkspaceConfig previous;
    previous.clroAsciiOnly = true;
    previous.clroListImplicitContinuation = true;
    for (const std::string& invalid : {std::string(), std::string(1, '\0'), std::string(2, '\0'),
                                     std::string("\2\0\0", 3), std::string("\0\2\0", 3), std::string("\0\0\2", 3)}) {
        auto config = previous;
        search::Reader reader(invalid);
        bool rejected = false;
        try { search::ReadNoteSyntaxPreferences(reader, config); }
        catch (const std::runtime_error&) { rejected = true; }
        check(rejected && same(config, previous), "malformed preferences cannot partially replace destination", 0);
    }
    for (const std::string& invalid : {std::string(3, '\0'),
        std::string("\2\0\0\0", 4), std::string("\0\2\0\0", 4),
        std::string("\0\0\2\0", 4), std::string("\0\0\0\2", 4),
        std::string("\0\0\1\1", 4)}) {
        auto config = previous;
        search::Reader reader(invalid);
        bool rejected = false;
        try { search::ReadNoteSyntaxPreferences(reader, config); }
        catch (const std::runtime_error&) { rejected = true; }
        check(rejected && same(config, previous), "four-field and conflicting policies reject atomically", 0);
    }
    note::NoteTextCore core;
    core.Reset({3302}, BuildNoteTextMetadata(previous, L"plain.md"), L"unchanged", 4, 4);
    check(!core.SetClroAsciiOnly(true) && !core.SetQuoteLazyContinuation(true) &&
          !core.SetClroListImplicitContinuation(true) && !core.SetClroListBodyAlignment(true) && core.MatchesRaw(L"unchanged") && core.content_revision() == 4,
          "CLRO preference updates are a no-op on other formats", 0);
    std::cout << "Note syntax preference checks=" << checks << " result=" << (ok ? "PASS" : "FAIL") << "\n";
    return ok;
}
} // namespace note_syntax_preferences_test
