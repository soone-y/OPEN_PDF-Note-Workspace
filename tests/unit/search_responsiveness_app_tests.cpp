// Replace only search.o when linking: exercise the production scheduler and
// native result list without starting or changing the user's application.
#include "search/search.cpp"
#include "fpdf_edit.h"
#include "fpdf_save.h"
#include <winioctl.h>
#include <cassert>
#include <iostream>
#include <fstream>

static int checks = 0;
static void RequireAt(bool value, const char* expression, int line) {
    if (!value) std::cerr << "Search check failed at line " << line << ": " << expression << "\n";
    assert(value); ++checks;
}
#define Require(value) RequireAt((value), #value, __LINE__)
static void MakeTestDirectory(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    Require(!error);
}
static std::filesystem::path fixtureRoot;
struct TestPdfWriter : FPDF_FILEWRITE {
    std::ofstream output;
    explicit TestPdfWriter(const std::filesystem::path& path) : output(path, std::ios::binary) {
        version = 1;
        WriteBlock = [](FPDF_FILEWRITE* self, const void* data, unsigned long size) {
            auto& out = static_cast<TestPdfWriter*>(self)->output;
            out.write(static_cast<const char*>(data), size); return out.good() ? 1 : 0;
        };
    }
};

static search::Writer SearchRequest(const std::filesystem::path& workspace,
                                   const std::vector<std::filesystem::path>& folders,
                                   int range = 3, const std::wstring& current = L"",
                                   const std::wstring& currentText = L"") {
    search::Writer request;
    request.Put<uint32_t>(search::kRequestVersion); request.Text(workspace.wstring()); request.Text(L"."); request.Text(L"__pdf_note_workspace__");
    request.Put<int32_t>(range); request.Text(L""); request.Put<uint8_t>(0); request.Put<uint32_t>(0);
    request.Put<uint32_t>(static_cast<uint32_t>(folders.size()));
    for (const auto& path : folders) { request.Text(path.wstring()); request.Put<uint8_t>(1); }
    request.Put<uint32_t>(7); request.Text(L"needle"); request.Put<uint8_t>(1); request.Put<uint8_t>(1);
    search::WriteNoteSyntaxPreferences(request, WorkspaceConfig{}); request.Put<uint32_t>(0);
    request.Text(current); request.Text(currentText);
    return request;
}
struct ProcessObservation {
    std::vector<SearchResultItem> results;
    size_t warnings = 0, messages = 0;
    bool done = false;
    SearchCtx progress;
};
static void Observe(search::Process& process, ProcessObservation& observation, bool continueWarnings = true) {
    const auto deadline = SearchClock::now() + std::chrono::seconds(20);
    while (!observation.done && SearchClock::now() < deadline) {
        std::string message;
        const auto state = process.Poll(message);
        if (state == search::Process::PollResult::Pending) { Sleep(1); continue; }
        Require(state == search::Process::PollResult::Message);
        ++observation.messages;
        search::Reader reader(message);
        const auto type = reader.Get<uint8_t>();
        if (type == 'R') observation.results.push_back(ReadSearchItem(reader));
        else {
            Require(type == 'P' || type == 'W' || type == 'D');
            ReadSearchProgress(reader, observation.progress);
            if (type == 'W') {
                ++observation.warnings;
                if (!continueWarnings) return;
                Require(process.Continue());
            }
            if (type == 'D') observation.done = true;
        }
        reader.End();
    }
    Require(observation.done);
}

int wmain() {
    int workerCode = 0;
    if (search::TryRunProcess(&workerCode)) return workerCode;
    std::cout << std::unitbuf;
    std::error_code fixtureError;
    fixtureRoot = std::filesystem::current_path(fixtureError) / L"scope_fixture";
    Require(!fixtureError);
    MakeTestDirectory(fixtureRoot / L"external" / L"nested");
    MakeTestDirectory(fixtureRoot / L"workspace" / L"unopened" / L"session");
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
        {
            TestPdfWriter writer(fixtureRoot / L"external" / L"document.pdf");
            Require(FPDF_SaveAsCopy(doc, &writer, 0) != FALSE);
        }
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
    {
        const auto workspace = fixtureRoot / L"workspace";
        const auto external = fixtureRoot / L"external";
        const auto first = external / L"first.txt", nested = external / L"nested" / L"second.txt";
        const auto unopened = workspace / L"unopened" / L"session" / L"note.txt";
        std::ofstream(first, std::ios::binary) << "needle outside\n";
        std::ofstream(nested, std::ios::binary) << "needle nested\n";
        std::ofstream(unopened, std::ios::binary) << "needle unopened\n";
        std::ofstream(external / L"broken.pdf") << "invalid PDF";
        search::Process process;
        auto request = SearchRequest(workspace, {external, external / L"nested"});
        Require(process.Start(request.bytes));
        const HANDLE child = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, process.processId());
        Require(child != nullptr);
        Require(GetPriorityClass(child) == IDLE_PRIORITY_CLASS);
        ProcessObservation observation;
        Observe(process, observation);
        Require(observation.results.size() == 4 && observation.progress.job.skipped == 1);
        size_t notes = 0, pdfs = 0, unreadable = 0;
        for (const auto& item : observation.results) {
            if (item.kind == SearchResultKind::Info) {
                ++unreadable; Require(item.path == external / L"broken.pdf"); continue;
            }
            Require(item.externalReadOnly && item.processSnapshot);
            if (item.kind == SearchResultKind::NoteLine) {
                ++notes; Require(item.noteSnapshotIdentity.content_fingerprint != 0 && item.lineNumber == 1);
            }
            if (item.kind == SearchResultKind::PdfPage) { ++pdfs; Require(item.pageIndex == 0); }
        }
        Require(notes == 2 && pdfs == 1 && unreadable == 1);
        process.Stop();
        Require(WaitForSingleObject(child, 5000) == WAIT_OBJECT_0); CloseHandle(child);
        std::string unchanged;
        Require(ReadFileBytesWin32(first, unchanged) && unchanged == "needle outside\n");
        size_t externalFiles = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(external))
            if (entry.is_regular_file()) ++externalFiles;
        Require(externalFiles == 4);

        request = SearchRequest(workspace, {}, 2);
        Require(process.Start(request.bytes));
        ProcessObservation whole;
        Observe(process, whole);
        Require(whole.results.size() == 1 && whole.results[0].path == unopened && !whole.results[0].externalReadOnly);
        process.Stop();

        request = SearchRequest(workspace, {external / L"nested"}, 3, nested.wstring(), L"current needle captured\n");
        Require(process.Start(request.bytes));
        ProcessObservation current;
        Observe(process, current);
        Require(current.results.size() == 1 && current.results[0].preview.find(L"current") != std::wstring::npos);
        Require(current.results[0].noteSnapshotIdentity.content_fingerprint == note::FingerprintSnapshotBytes("current needle captured\n"));
        process.Stop();
        std::cout << "Search process scope PASS: real child, low priority, unopened lecture, overlapping external roots, PDF, current edit, original unchanged.\n";

        request = SearchRequest(workspace, {fixtureRoot / L"missing_enabled", L"\\\\invalid\\share"});
        Require(process.Start(request.bytes));
        ProcessObservation unavailable; Observe(process, unavailable);
        Require(unavailable.results.size() == 2 && unavailable.progress.job.skipped == 2);
        for (const auto& item : unavailable.results) Require(item.kind == SearchResultKind::Info);
        process.Stop();

        SearchCtx limited;
        limited.results = list; limited.job.active = true;
        limited.resultBytes = 64 * 1024 * 1024;
        limited.process = std::make_unique<search::Process>();
        request = SearchRequest(workspace, {external / L"nested"});
        Require(limited.process->Start(request.bytes));
        const auto limitDeadline = SearchClock::now() + std::chrono::seconds(10);
        while (limited.job.active && SearchClock::now() < limitDeadline) {
            ProcessRemoteSearchTick(window, limited); Sleep(1);
        }
        Require(!limited.job.active && limited.job.canceled && !limited.process);
        Require(limited.idleStatusText == localization::Text(L"search.ui.result_limit"));
        Require(limited.job.results.size() == 1 && limited.job.results[0].kind == SearchResultKind::Info);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);

        auto deep = fixtureRoot / L"deep";
        const auto deepRoot = deep;
        for (int i = 0; i < 18; ++i) deep /= L"level";
        MakeTestDirectory(deep);
        std::ofstream(deep / L"deep.txt") << "needle deep\n";
        request = SearchRequest(workspace, {deepRoot});
        Require(process.Start(request.bytes));
        ProcessObservation paused;
        Observe(process, paused, false);
        Require(paused.warnings == 1 && !paused.done && paused.results.empty() && paused.progress.job.maxDepth >= 16);
        const HANDLE pausedChild = OpenProcess(SYNCHRONIZE, FALSE, process.processId());
        Require(pausedChild != nullptr);
        std::string message;
        Require(process.Poll(message) == search::Process::PollResult::Pending);
        Require(PostMessageW(window, WM_APP + 5, 0, 0) != FALSE);
        MSG pending{}; Require(PeekMessageW(&pending, window, WM_APP + 5, WM_APP + 5, PM_REMOVE) != FALSE);
        Require(process.Continue()); Observe(process, paused);
        Require(paused.done && paused.results.size() == 1);
        process.Stop(); Require(WaitForSingleObject(pausedChild, 5000) == WAIT_OBJECT_0); CloseHandle(pausedChild);
        Require(process.Start(request.bytes));
        ProcessObservation stopped; Observe(process, stopped, false);
        const HANDLE stoppedChild = OpenProcess(SYNCHRONIZE, FALSE, process.processId());
        Require(stoppedChild != nullptr);
        const auto beforeStop = SearchClock::now(); process.Stop();
        Require(SearchClock::now() - beforeStop < std::chrono::seconds(1));
        Require(WaitForSingleObject(stoppedChild, 5000) == WAIT_OBJECT_0); CloseHandle(stoppedChild);
        Require(process.Start("malformed"));
        const auto failureDeadline = SearchClock::now() + std::chrono::seconds(5);
        auto state = search::Process::PollResult::Pending;
        while (state == search::Process::PollResult::Pending && SearchClock::now() < failureDeadline) {
            state = process.Poll(message); Sleep(1);
        }
        Require(state == search::Process::PollResult::Failed);
        process.Stop();
        // Keep a real local file open behind an unacknowledged oplock break.
        // The worker reaches an indivisible OS read, while its UI owner must
        // still accept work and stop the child without waiting for that read.
        const auto blockedRoot = fixtureRoot / L"blocked";
        MakeTestDirectory(blockedRoot);
        const auto blockedPath = blockedRoot / L"locked.txt";
        std::ofstream(blockedPath, std::ios::binary) << "needle protected\n";
        const HANDLE locked = CreateFileW(blockedPath.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        Require(locked != INVALID_HANDLE_VALUE);
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Require(operation.hEvent != nullptr);
        DWORD returned = 0;
        const BOOL requested = DeviceIoControl(locked, FSCTL_REQUEST_OPLOCK_LEVEL_1,
            nullptr, 0, nullptr, 0, &returned, &operation);
        Require(!requested && GetLastError() == ERROR_IO_PENDING);
        request = SearchRequest(workspace, {blockedRoot});
        Require(process.Start(request.bytes));
        const HANDLE blockedChild = OpenProcess(SYNCHRONIZE, FALSE, process.processId());
        Require(blockedChild != nullptr);
        const auto blockedDeadline = SearchClock::now() + std::chrono::seconds(5);
        while (WaitForSingleObject(operation.hEvent, 0) == WAIT_TIMEOUT && SearchClock::now() < blockedDeadline) {
            const auto pendingState = process.Poll(message);
            Require(pendingState == search::Process::PollResult::Pending || pendingState == search::Process::PollResult::Message);
            Sleep(1);
        }
        Require(WaitForSingleObject(operation.hEvent, 0) == WAIT_OBJECT_0);
        Require(WaitForSingleObject(blockedChild, 0) == WAIT_TIMEOUT);
        Require(PostMessageW(window, WM_APP + 6, 0, 0) != FALSE);
        Require(PeekMessageW(&pending, window, WM_APP + 6, WM_APP + 6, PM_REMOVE) != FALSE);
        const auto blockedStop = SearchClock::now(); process.Stop();
        Require(SearchClock::now() - blockedStop < std::chrono::seconds(1));
        Require(WaitForSingleObject(blockedChild, 5000) == WAIT_OBJECT_0);
        CloseHandle(blockedChild); CloseHandle(locked); CloseHandle(operation.hEvent);
        Require(ReadFileBytesWin32(blockedPath, unchanged) && unchanged == "needle protected\n");
        std::cout << "Search blocked-read cancellation PASS: real oplock break, UI work, nonblocking stop, child exit, source unchanged.\n";
        std::cout << "Search process lifecycle PASS: deep scope pauses before content, explicit continue, UI work, cancel exits child, malformed request fails.\n";

        search::Groups store;
        Require(store.Load(workspace) && store.items().empty());
        search::Group group;
        group.name = L"external and missing";
        group.folders = {{external.wstring(), true}, {(fixtureRoot / L"missing").wstring(), false}};
        Require(store.Save({group}));
        search::Groups reloaded;
        Require(reloaded.Load(workspace) && reloaded.items().size() == 1);
        Require(reloaded.items()[0].folders[1].path == group.folders[1].path && !reloaded.items()[0].folders[1].enabled);
        auto updated = group; updated.name = L"updated";
        Require(store.Save({updated}));
        Require(!reloaded.Save({group}));
        const auto path = workspace / L"__pdf_note_workspace__" / L"__settings__" / L"search_groups.bin";
        std::string bytes; Require(ReadFileBytesWin32(path, bytes));
        Require(search::DecodeGroups(bytes)[0].name == L"updated");
        bool rejected = false;
        try { (void)search::DecodeGroups(std::string_view(bytes).substr(0, bytes.size() - 1)); } catch (...) { rejected = true; }
        Require(rejected);
        std::ofstream(path, std::ios::binary) << "damaged";
        search::Groups corrupt; Require(!corrupt.Load(workspace) && !corrupt.Save({group}));
        Require(ReadFileBytesWin32(path, bytes) && bytes == "damaged");
        std::cout << "Search groups PASS: explicit save/reload/update, missing folder retained, stale write rejected, corruption preserved.\n";
    }
    Require(DestroyWindow(window) != FALSE);
    FPDF_DestroyLibrary();
    std::cout << "Search responsiveness PASS: " << checks << " checks; hidden native controls, real PDFium, deadline/cancel/completion.\n";
    return 0;
}
