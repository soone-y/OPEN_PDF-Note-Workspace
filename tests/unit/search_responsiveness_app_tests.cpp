// Replace only search.o when linking: exercise the production scheduler and
// native result list without starting or changing the user's application.
#include "search/search.cpp"
#include "fpdf_edit.h"
#include <cassert>
#include <iostream>

static int checks = 0;
static void Require(bool value) { assert(value); ++checks; }

int wmain() {
    FPDF_InitLibrary();
    const HWND window = CreateWindowW(L"STATIC", L"owned search test", 0,
        0, 0, 500, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(window != nullptr);
    const HWND list = CreateWindowW(L"LISTBOX", L"", WS_CHILD | LBS_NOINTEGRALHEIGHT,
        0, 0, 500, 250, window, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(list != nullptr);
    const auto ui = GetSearchUiStrings();
    {
        SearchCtx ctx;
        ctx.job.includePdfs = true;
        for (int i = 0; i < 2000; ++i)
            ctx.job.pdfFiles.emplace_back(L"missing_search_fixture_" + std::to_wstring(i) + L".pdf");
        std::vector<SearchResultItem> found;
        ProcessPdfPages(&ctx, ui, found, 1, SearchClock::now() + std::chrono::seconds(10));
        Require(ctx.job.processedFiles == kSearchPdfTransitionsPerTick &&
                ctx.job.pdfIndex == kSearchPdfTransitionsPerTick);
        const size_t before = ctx.job.pdfIndex;
        ProcessPdfPages(&ctx, ui, found, 1, SearchClock::now());
        Require(ctx.job.pdfIndex == before);
        Require(found.empty());
        std::cout << "Search failed-PDF budget PASS: 120 of 2000 files attempted; expired deadline yields.\n";
    }
    {
        SearchCtx ctx;
        ctx.job.enumerationDone = false;
        std::error_code ec;
        const auto root = std::filesystem::current_path(ec);
        Require(!ec);
        ctx.job.roots.push_back(root);
        std::vector<SearchResultItem> found;
        ProcessEnumeration(&ctx, ui, found, 120, SearchClock::now());
        Require(!ctx.job.iterValid && ctx.job.rootIndex == 0 && !ctx.job.enumerationDone);
    }
    {
        SearchCtx ctx;
        ctx.job.includePdfs = true;
        ctx.job.queryTermsLower = {{L"needle"}};
        const FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
        Require(doc != nullptr);
        const FPDF_PAGE page = FPDFPage_New(doc, 0, 100, 100);
        Require(page != nullptr);
        const HDC dc = CreateCompatibleDC(nullptr);
        Require(dc != nullptr);
        const HFONT gdiFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, 0, 0, L"Arial");
        Require(gdiFont != nullptr);
        const auto previous = SelectObject(dc, gdiFont);
        const DWORD size = GetFontData(dc, 0, 0, nullptr, 0);
        Require(size != GDI_ERROR && size > 0);
        std::vector<unsigned char> bytes(size);
        Require(GetFontData(dc, 0, 0, bytes.data(), size) == size);
        SelectObject(dc, previous); DeleteObject(gdiFont); DeleteDC(dc);
        const FPDF_FONT font = FPDFText_LoadFont(doc, bytes.data(), size, FPDF_FONT_TRUETYPE, 1);
        Require(font != nullptr);
        const FPDF_PAGEOBJECT text = FPDFPageObj_CreateTextObj(doc, font, 12);
        Require(text != nullptr);
        const unsigned short needle[] = {'n', 'e', 'e', 'd', 'l', 'e', 0};
        Require(FPDFText_SetText(text, needle) != FALSE);
        FPDFPageObj_Transform(text, 1, 0, 0, 1, 10, 40);
        FPDFPage_InsertObject(page, text);
        Require(FPDFPage_GenerateContent(page) != FALSE);
        FPDF_ClosePage(page);
        PdfScanState scan;
        scan.doc = doc; scan.pageCount = 1; scan.ownsDoc = true;
        ctx.job.pdf = scan;
        std::vector<SearchResultItem> found;
        ProcessPdfPages(&ctx, ui, found, 1, SearchClock::now() + std::chrono::seconds(10));
        Require(ctx.job.pdf && ctx.job.pdf->pageIndex == 1 && ctx.job.processedPages == 1);
        Require(found.size() == 1 && found[0].kind == SearchResultKind::PdfPage &&
                found[0].hitCount == 1 && found[0].pageIndex == 0);
        FPDFFont_Close(font);
        ProcessPdfPages(&ctx, ui, found, 1, SearchClock::now() + std::chrono::seconds(10));
        Require(!ctx.job.pdf && ctx.job.processedFiles == 1);
        Require(IsSearchJobComplete(ctx.job));
    }
    {
        SearchCtx ctx;
        ctx.results = list;
        ctx.job.active = true;
        std::wstring text;
        for (int i = 0; i < 1001; ++i) text += L"needle immutable snapshot\n";
        const auto identity = note::BuildSnapshotIdentity(note::NoteId{987654}, 1, 0, WideToUTF8(text));
        const auto began = SearchClock::now();
        SearchNoteText(text, {{L"needle"}}, {}, ui, L"test", L"owned.txt", L"", identity, ctx.job);
        const auto submitMs = std::chrono::duration_cast<std::chrono::milliseconds>(SearchClock::now() - began).count();
        Require(ctx.job.noteScan.has_value());
        text.assign(L"edited after submission");
        const auto deadline = SearchClock::now() + std::chrono::seconds(20);
        while (ctx.job.active && SearchClock::now() < deadline) {
            ProcessSearchTick(window, &ctx);
            Sleep(1); // Test only: UI never waits for worker completion.
        }
        Require(!ctx.job.active && ctx.job.results.size() == 1001);
        for (size_t i = 0; i < ctx.job.results.size(); ++i) {
            const auto& item = ctx.job.results[i];
            Require(item.lineNumber == static_cast<int>(i + 1));
            Require(note::SameSnapshotIdentity(item.noteSnapshotIdentity, identity));
            Require(item.textStart == i * std::wstring(L"needle immutable snapshot\n").size());
        }
        std::cout << "Search note worker PASS: immutable snapshot, 1001 lines/offsets/identities; submit_ms=" << submitMs << ".\n";
        SendMessageW(list, LB_RESETCONTENT, 0, 0);

        ResetSearchState(&ctx, ui);
        ctx.job.active = true;
        const std::wstring oldText(200000, L'x');
        const auto oldIdentity = note::BuildSnapshotIdentity(note::NoteId{987655}, 1, 0, WideToUTF8(oldText));
        SearchNoteText(oldText, {{L"x"}}, {}, ui, L"old", L"old.txt", L"", oldIdentity, ctx.job);
        auto oldCompletion = ctx.job.noteScan->completion;
        CancelSearchJob(window, &ctx, false);
        Require(oldCompletion->canceled && !ctx.job.noteScan);
        ResetSearchState(&ctx, ui);
        ctx.job.active = true;
        const auto freshIdentity = note::BuildSnapshotIdentity(note::NoteId{987656}, 1, 0, "fresh needle");
        SearchNoteText(L"fresh needle", {{L"needle"}}, {}, ui, L"fresh", L"fresh.txt", L"", freshIdentity, ctx.job);
        const auto freshDeadline = SearchClock::now() + std::chrono::seconds(20);
        while (ctx.job.active && SearchClock::now() < freshDeadline) {
            ProcessSearchTick(window, &ctx);
            Sleep(1);
        }
        Require(!ctx.job.active && ctx.job.results.size() == 1);
        Require(ctx.job.results[0].path == L"fresh.txt");
        Require(note::SameSnapshotIdentity(ctx.job.results[0].noteSnapshotIdentity, freshIdentity));
        SendMessageW(list, LB_RESETCONTENT, 0, 0);

        // Represent a caught worker exception at the real UI completion boundary.
        ResetSearchState(&ctx, ui);
        ctx.job.active = true;
        NoteScanState failure;
        failure.completion = std::make_shared<search::NoteSearchCompletion>();
        failure.completion->ready = failure.completion->failed = true;
        ctx.job.noteScan = std::move(failure);
        ProcessSearchTick(window, &ctx);
        Require(!ctx.job.active && ctx.job.canceled);
        Require(ctx.job.matchCount == 0 && ctx.job.results.size() == 1 &&
                ctx.job.results[0].kind == SearchResultKind::Info);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        std::cout << "Search worker lifecycle PASS: cancel/restart discards old completion; failure stops, keeps results.\n";
    }
    {
        SearchCtx ctx;
        ctx.results = list;
        ctx.job.active = true;
        // A burst from a high-hit document: preserve order and every hit while
        // returning to the message loop between bounded native insertions.
        for (int i = 0; i < 1001; ++i) {
            SearchResultItem item;
            item.kind = SearchResultKind::NoteLine;
            item.title = item.display = std::to_wstring(i);
            ctx.job.pendingResults.push_back(std::move(item));
        }
        int turns = 0;
        while (ctx.job.active && turns++ < 500) {
            const size_t before = ctx.job.results.size();
            ProcessSearchTick(window, &ctx);
            const size_t added = ctx.job.results.size() - before;
            Require(added <= kSearchResultsPerTick);
            Require(SendMessageW(list, LB_GETCOUNT, 0, 0) == static_cast<LRESULT>(ctx.job.results.size()));
            // A queued operation can run after EVERY chunk, before any next
            // timer becomes due; no nested pumping inside search is needed.
            constexpr UINT probe = WM_APP + 1;
            Require(PostMessageW(window, probe, 0, 0) != FALSE);
            MSG message{};
            Require(PeekMessageW(&message, window, probe, probe, PM_REMOVE) != FALSE);
            Require(message.message == probe);
        }
        Require(!ctx.job.active && turns >= 32 && turns < 500);
        Require(ctx.job.pendingResults.empty() && ctx.job.matchCount == 1001);
        for (size_t i = 0; i < ctx.job.results.size(); ++i)
            Require(ctx.job.results[i].display == std::to_wstring(i));
        std::cout << "Search publication PASS: 1001 ordered hits, max 32 per turn, queued operation between turns.\n";
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
    }
    {
        SearchCtx ctx;
        ctx.results = list;
        ctx.job.active = true;
        ctx.job.includePdfs = true;
        ctx.job.pdfFiles.emplace_back(L"missing1.pdf");
        ctx.job.pdfFiles.emplace_back(L"missing2.pdf");
        ctx.job.pdfFiles.resize(2000, L"missing.pdf");
        ProcessSearchTick(window, &ctx);
        Require(ctx.job.active && ctx.job.pdfIndex > 0 && ctx.job.pdfIndex <= kSearchPdfTransitionsPerTick);
        MSG message{};
        // Even when work is pending, the next timer must not already be due.
        Require(!PeekMessageW(&message, window, WM_TIMER, WM_TIMER, PM_REMOVE));
        SearchResultItem item;
        item.kind = SearchResultKind::NoteLine;
        item.title = item.display = L"published";
        AppendResults(&ctx, {item});
        ctx.job.pendingResults.push_back(item);
        const size_t scanned = ctx.job.pdfIndex;
        CancelSearchJob(window, &ctx, false);
        Require(!ctx.job.active && ctx.job.canceled && ctx.job.results.size() == 1);
        const size_t before = ctx.job.pendingResults.size();
        ProcessSearchTick(window, &ctx);
        Require(ctx.job.pendingResults.size() == before && ctx.job.pdfIndex == scanned);
        Require(!PeekMessageW(&message, window, WM_TIMER, WM_TIMER, PM_REMOVE));
    }
    Require(DestroyWindow(window) != FALSE);
    FPDF_DestroyLibrary();
    std::cout << "Search responsiveness PASS: " << checks << " checks; hidden native controls, real PDFium, deadline/cancel/completion.\n";
    return 0;
}
