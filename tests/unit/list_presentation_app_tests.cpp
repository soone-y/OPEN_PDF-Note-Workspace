// Link the application and inspect its real owner-drawn list controls.
#include "core/app_core.h"
#include "core/localization.h"
#include "ui/listbox_presentation.h"
#include "ui/listbox_scroll.h"
#include "ui/dialogs/annot_math_panel.h"
#include "ui/core/main_window_api.h"
#include "ui/layout.h"
#include "workspace/list_label_batch.h"
#include "workspace/workspace_tree.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <commctrl.h>

extern "C" int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int);
// UI-thread timer ledger: readiness=4931, unexpected modal guard=4932.
static constexpr UINT_PTR kReadyTimer = 4931;
static constexpr UINT_PTR kModalGuardTimer = 4932;
static UINT_PTR readyTimer = 0;
static bool completed = false;
static bool running = false;
static std::filesystem::path root;
static constexpr int kItems = 1000;

struct ControlObservation { int disables = 0, enables = 0, positions = 0, erases = 0, paints = 0, renders = 0; };
static LRESULT CALLBACK ObserveControl(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR, DWORD_PTR data) {
    auto& observed = *reinterpret_cast<ControlObservation*>(data);
    if (message == WM_SETREDRAW) { if (wParam) ++observed.enables; else ++observed.disables; }
    if (message == WM_WINDOWPOSCHANGED) ++observed.positions;
    if (message == WM_ERASEBKGND) ++observed.erases;
    if (message == WM_PAINT) ++observed.paints;
    return DefSubclassProc(window, message, wParam, lParam);
}

struct ListRenderObservation { const HWND* lists; ControlObservation* observations; };
static LRESULT CALLBACK ObserveListRendering(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR, DWORD_PTR data) {
    const auto& state = *reinterpret_cast<ListRenderObservation*>(data);
    HWND list = nullptr;
    if (message == WM_CTLCOLORLISTBOX) list = reinterpret_cast<HWND>(lParam);
    if (message == WM_DRAWITEM && lParam) {
        const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item.CtlType == ODT_LISTBOX) list = item.hwndItem;
    }
    for (size_t i = 0; i < 5; ++i)
        if (list == state.lists[i]) ++state.observations[i].renders;
    return DefSubclassProc(window, message, wParam, lParam);
}

static void Write(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec); assert(!ec);
    std::ofstream file(path, std::ios::binary);
    file << text; file.close(); assert(file);
}

static void CheckLinearBatch() {
    for (const int count : {1000, 10000}) {
        std::vector<std::wstring> names;
        for (int i = 0; i < count; ++i) names.push_back(L"資料" + std::to_wstring(i));
        names.back() = names.front();
        size_t baseCalls = 0;
        const workspace_display::LabelBatch batch(names, [&](const std::wstring& name) { ++baseCalls; return name; });
        for (size_t i = 0; i < names.size(); ++i) {
            assert(batch.Base(i) == names[i]);
            assert(batch.Duplicated(names[i]) == (i == 0 || i + 1 == names.size()));
        }
        assert(baseCalls == names.size());
    }
}

static void CheckLabels(const std::filesystem::path& lecture, const std::filesystem::path& session) {
    // The list and the temporary-path registry both store canonical paths.
    const std::vector<std::wstring> lectures{CanonicalOrSelf(lecture).wstring(),
        CanonicalOrSelf(root / L"other/LECTURE").wstring(), CanonicalOrSelf(root / L"third/lecture").wstring()};
    const auto lectureLabels = LectureDisplayLabelsForPaths(lectures);
    for (size_t i = 0; i < lectures.size(); ++i)
        assert(lectureLabels[i] == LectureDisplayLabelForPath(lectures[i], lectures));
    const auto savedExternal = g_tempExternalLectures;
    g_tempExternalLectures.push_back({lectures.front()});
    const auto externalLabels = LectureDisplayLabelsForPaths(lectures);
    for (size_t i = 0; i < lectures.size(); ++i)
        assert(externalLabels[i] == LectureDisplayLabelForPath(lectures[i], lectures));
    g_tempExternalLectures = savedExternal;
    const std::vector<SessionEntry> sessions{{L"資料", session.wstring()},
        {L"資料", (root / L"other/session").wstring()}, {L"", lecture.wstring()}};
    const auto sessionLabels = SessionDisplayLabelsForEntries(sessions, lecture.wstring());
    for (size_t i = 0; i < sessions.size(); ++i)
        assert(sessionLabels[i] == SessionDisplayLabelForEntry(sessions[i], sessions, lecture.wstring()));
    const std::vector<FileEntry> files{{(session / L"pdf/same.pdf").wstring(), true},
        {(session / L"same.pdf").wstring(), true}, {(session / L"資料.txt").wstring(), false},
        {(session / L"other/SAME.PDF").wstring(), true}, {L"", false}};
    const auto fileLabels = FileDisplayLabelsForPaths(files, session);
    for (size_t i = 0; i < files.size(); ++i)
        assert(fileLabels[i] == FileDisplayLabelForPath(files[i].path, files, session));
}

static void CheckLabelCost(const std::filesystem::path& session) {
    for (const size_t count : {128u, 512u, 1000u}) {
        const std::vector<FileEntry> files(g_pdfFiles.begin(), g_pdfFiles.begin() + count);
        std::vector<std::wstring> individual;
        const auto begin = std::chrono::steady_clock::now();
        for (const auto& file : files) individual.push_back(FileDisplayLabelForPath(file.path, files, session));
        const auto middle = std::chrono::steady_clock::now();
        const auto batch = FileDisplayLabelsForPaths(files, session);
        const auto end = std::chrono::steady_clock::now();
        assert(batch == individual);
        std::cout << "labels=" << count << " individual_us="
            << std::chrono::duration_cast<std::chrono::microseconds>(middle - begin).count()
            << " batch_us=" << std::chrono::duration_cast<std::chrono::microseconds>(end - middle).count() << '\n';
    }
}

static void CheckLayout() {
    // Observe pane layout directly, independently of document-opening work.
    // The PDF/note container windows exist even before a document is open.
    ui::ApplyLayout(g_hMainWnd, ui::LayoutPass::Commit);
    const auto saved = ui::GetLayoutState();
    ControlObservation observed[6];
    const HWND noteView = g_hNoteEdit;
    assert(noteView);
    const HWND controls[]{g_hLectureList, g_hSessionList, g_hPdfList, g_hNoteList, g_hPdfView, noteView};
    constexpr UINT_PTR kObserver = 4933;
    for (size_t i = 0; i < 6; ++i)
        assert(SetWindowSubclass(controls[i], ObserveControl, kObserver, reinterpret_cast<DWORD_PTR>(&observed[i])));
    const auto unchanged = ui::ApplyLayout(g_hMainWnd, ui::LayoutPass::Live);
    if (unchanged.anyBoundsChanged || unchanged.anyVisibilityChanged)
        std::cerr << "Unsettled layout: bounds=" << unchanged.anyBoundsChanged << " visible=" << unchanged.anyVisibilityChanged
            << " pdf=" << unchanged.pdfViewBoundsChanged << " noteEdit=" << unchanged.noteEditorBoundsChanged
            << " noteRender=" << unchanged.noteRenderBoundsChanged << " bottom=" << unchanged.bottomPaneBoundsChanged
            << " annot=" << unchanged.annotPanelBoundsChanged << " shortcut=" << unchanged.shortcutPanelBoundsChanged << std::endl;
    assert(!unchanged.anyBoundsChanged && !unchanged.anyVisibilityChanged);
    for (const auto& control : observed) assert(control.positions == 0 && control.erases == 0);
    RECT pdfBefore{}, pdfAfter{};
    assert(GetWindowRect(g_hPdfView, &pdfBefore));
    for (int step = 0; step < 10; ++step) {
        auto state = saved;
        state.leftSplit1 += step % 2 ? 0 : 4;
        ui::SetLayoutState(state);
        const auto changed = ui::ApplyLayout(g_hMainWnd, ui::LayoutPass::Live);
        assert(changed.anyBoundsChanged);
    }
    assert(GetWindowRect(g_hPdfView, &pdfAfter));
    assert(EqualRect(&pdfBefore, &pdfAfter));
    assert(observed[0].positions > 0 && observed[1].positions > 0);
    assert(observed[4].positions == 0 && observed[4].erases == 0);
    assert(observed[5].positions == 0 && observed[5].erases == 0);
    for (size_t i = 0; i < 6; ++i) assert(RemoveWindowSubclass(controls[i], ObserveControl, kObserver));
    ui::SetLayoutState(saved);
    ui::ApplyLayout(g_hMainWnd, ui::LayoutPass::Commit);
    std::cout << "Layout presentation PASS: no-op layout preserves controls; 10 live split moves keep unaffected PDF/note views intact.\n";
}

static void CheckPaint(HWND list, int openIndex) {
    assert(ui::OpenListIndex(list) == openIndex);
    HDC dc = CreateCompatibleDC(nullptr); assert(dc);
    HBITMAP bitmap = CreateBitmap(480, 32, 1, 32, nullptr); assert(bitmap);
    const auto oldBitmap = SelectObject(dc, bitmap);
    DRAWITEMSTRUCT item{};
    item.CtlType = ODT_LISTBOX; item.hwndItem = list;
    item.itemID = static_cast<UINT>(openIndex); item.itemAction = ODA_DRAWENTIRE;
    item.hDC = dc; item.rcItem = {0, 0, 480, 32};
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) assert(SendMessageW(g_hMainWnd, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&item)));
    const auto openColor = GetPixel(dc, 1, 1);
    item.itemID = static_cast<UINT>(openIndex == 0 ? 1 : 0);
    assert(SendMessageW(g_hMainWnd, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&item)));
    assert(GetPixel(dc, 1, 1) == g_theme.panelBg && openColor != g_theme.panelBg);
    item.itemState = ODS_SELECTED;
    assert(SendMessageW(g_hMainWnd, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&item)));
    assert(GetPixel(dc, 1, 1) == g_theme.selectionBg);
    SelectObject(dc, oldBitmap); DeleteObject(bitmap); DeleteDC(dc);
    std::cout << "paint_200_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count() << '\n';
}

static void CheckEmptyListPixels(const std::filesystem::path& session) {
    const HWND list = g_hPdfList;
    RECT before{}; assert(GetWindowRect(list, &before));
    MapWindowPoints(nullptr, GetParent(list), reinterpret_cast<POINT*>(&before), 2);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"sample.pdf"));
    SendMessageW(list, LB_SETCURSEL, 0, 0);
    for (const int height : {80, 160, 240}) {
        SendMessageW(list, WM_HSCROLL, SB_LINERIGHT, 0);
        SendMessageW(list, WM_HSCROLL, SB_LINERIGHT, 0);
        assert(SetWindowPos(list, nullptr, before.left, before.top, 320, height,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW));
        RECT bounds{}, client{};
        assert(GetWindowRect(list, &bounds) && bounds.bottom - bounds.top == height);
        assert(GetClientRect(list, &client) && client.bottom > 40);
        HDC dc = CreateCompatibleDC(nullptr); assert(dc);
        HBITMAP bitmap = CreateBitmap(client.right, client.bottom, 1, 32, nullptr); assert(bitmap);
        const auto oldBitmap = SelectObject(dc, bitmap);
        HBRUSH poison = CreateSolidBrush(RGB(255, 0, 255)); assert(poison);
        FillRect(dc, &client, poison); DeleteObject(poison);
        // Native wheel scrolling requests an off-screen client image with
        // PRF_ERASEBKGND as well as PRF_CLIENT. This is a separate entry point
        // from our WM_PAINT buffer: the caller's background must be initialized.
        const int savedClientDc = SaveDC(dc); assert(savedClientDc);
        SendMessageW(list, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        assert(RestoreDC(dc, savedClientDc));
        assert(GetPixel(dc, client.right - 2, client.bottom - 2) == RGB(255, 0, 255));
        const int savedPrintDc = SaveDC(dc); assert(savedPrintDc);
        SendMessageW(list, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc),
            PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
        assert(RestoreDC(dc, savedPrintDc));
        for (int y = 40; y < client.bottom; y += 7)
            for (int x = 1; x < client.right; x += 11) assert(GetPixel(dc, x, y) == g_theme.panelBg);
        ui::DrawListBoxClient(list, dc, client, g_theme.panelBg);
        POINT origin{};
        assert(GetViewportOrgEx(dc, &origin) && origin.x == 0 && origin.y == 0);
        assert(GetWindowOrgEx(dc, &origin) && origin.x == 0 && origin.y == 0);
        // Selected native owner-draw row is present, while every old pixel in
        // the blank region has been replaced by the current theme background.
        assert(GetPixel(dc, 5, 5) == g_theme.selectionBg);
        const int rowHeight = static_cast<int>(SendMessageW(list, LB_GETITEMHEIGHT, 0, 0));
        assert(rowHeight > 4);
        std::vector<COLORREF> printedText;
        for (int y = 2; y < rowHeight - 2; ++y)
            for (int x = 6; x < 120; ++x) printedText.push_back(GetPixel(dc, x, y));
        DRAWITEMSTRUCT row{};
        row.CtlType = ODT_LISTBOX; row.hwndItem = list; row.itemID = 0;
        row.itemAction = ODA_DRAWENTIRE; row.itemState = ODS_SELECTED;
        row.hDC = dc; row.rcItem = {0, 0, client.right, rowHeight};
        const auto oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0)));
        assert(SendMessageW(g_hMainWnd, WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&row)));
        size_t pixel = 0;
        for (int y = 2; y < rowHeight - 2; ++y)
            for (int x = 6; x < 120; ++x) assert(printedText[pixel++] == GetPixel(dc, x, y));
        SelectObject(dc, oldFont);
        for (int y = 40; y < client.bottom; y += 7)
            for (int x = 1; x < client.right; x += 11) assert(GetPixel(dc, x, y) == g_theme.panelBg);
        SelectObject(dc, oldBitmap); DeleteObject(bitmap); DeleteDC(dc);
    }
    assert(SetWindowPos(list, nullptr, before.left, before.top, before.right - before.left, before.bottom - before.top,
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW));
    std::wstring pdf, note;
    LoadFiles({L"session0", session.wstring()}, pdf, note);
    SyncLeftPaneSelection();
    std::cout << "List blank-area PASS: native print-client background and buffered paint at 3 heights; selected row and text preserved.\n";
}

static void CheckListScroll() {
    const auto savedAnnotations = g_annots;
    const auto savedPdf = g_pdf.path;
    const bool mainVisible = IsWindowVisible(g_hMainWnd) != FALSE;
    const bool wasTopmost = (GetWindowLongPtrW(g_hMainWnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    ShowWindow(g_hMainWnd, SW_SHOWNOACTIVATE);
    assert(SetWindowPos(g_hMainWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
    g_pdf.path = (root / L"viewport.pdf").wstring();
    g_annots.clear();
    for (int n = 0; n < 300; ++n) {
        Annotation annotation;
        annotation.id = L"scroll_" + std::to_wstring(n);
        annotation.type = Annotation::Type::TextBox;
        annotation.text = L"scroll row " + std::to_wstring(n);
        g_annots.push_back(std::move(annotation));
    }
    RefreshAnnotPanel();
    SendMessageW(g_hAnnotList, LB_SETCURSEL, 10, 0);
    SyncAnnotPanelSelectionFromList();
    SendMessageW(g_hAnnotList, LB_SETTOPINDEX, 100, 0);
    RefreshAnnotPanel();
    assert(SendMessageW(g_hAnnotList, LB_GETTOPINDEX, 0, 0) == 100);
    assert(SendMessageW(g_hAnnotList, LB_GETCURSEL, 0, 0) == 10);
    auto inserted = g_annots.front(); inserted.id = L"inserted";
    g_annots.insert(g_annots.begin(), inserted);
    RefreshAnnotPanel();
    assert(SendMessageW(g_hAnnotList, LB_GETTOPINDEX, 0, 0) == 101);
    assert(SendMessageW(g_hAnnotList, LB_GETCURSEL, 0, 0) == 11);
    RequestAnnotPanelRevealLatest(); RefreshAnnotPanel();
    assert(SendMessageW(g_hAnnotList, LB_GETCURSEL, 0, 0) == 300);
    SendMessageW(g_hAnnotList, LB_SETTOPINDEX, 100, 0);
    RefreshAnnotPanel();
    assert(SendMessageW(g_hAnnotList, LB_GETTOPINDEX, 0, 0) == 100);

    const HWND lists[]{g_hLectureList, g_hSessionList, g_hPdfList, g_hNoteList, g_hAnnotList};
    bool visible[5]{};
    UINT systemRows = 3;
    assert(SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &systemRows, 0));
    int rows[5]{};
    ControlObservation observations[5];
    ListRenderObservation rendering{lists, observations};
    constexpr UINT_PTR observer = 4935;
    for (size_t i = 0; i < 5; ++i) {
        const auto list = lists[i];
        visible[i] = IsWindowVisible(list) != FALSE;
        ShowWindow(list, SW_SHOWNA);
        assert(IsWindowVisible(list));
        RECT client{}; assert(GetClientRect(list, &client));
        const int height = static_cast<int>(SendMessageW(list, LB_GETITEMHEIGHT, 0, 0));
        rows[i] = systemRows == WHEEL_PAGESCROLL
            ? std::max(1, static_cast<int>(client.bottom) / std::max(1, height) - 1) : static_cast<int>(systemRows);
        assert(rows[i] >= 0 && rows[i] < 100);
        SendMessageW(list, LB_SETCURSEL, 10, 0);
        SendMessageW(list, LB_SETTOPINDEX, 100, 0);
        UpdateWindow(list);
        assert(SetWindowSubclass(list, ObserveControl, observer, reinterpret_cast<DWORD_PTR>(&observations[i])));
    }
    assert(SetWindowSubclass(g_hMainWnd, ObserveListRendering, 4936, reinterpret_cast<DWORD_PTR>(&rendering)));
    // Interleave unit deltas among all five real controls: fractional input
    // from one pane must never be completed by a different pane's packet.
    for (int packet = 0; packet < 120; ++packet) {
        for (size_t i = 0; i < 5; ++i) {
            const auto before = SendMessageW(lists[i], LB_GETTOPINDEX, 0, 0);
            const int rendersBefore = observations[i].renders;
            SendMessageW(lists[i], WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-1)), 0);
            assert(SendMessageW(lists[i], LB_GETTOPINDEX, 0, 0) == 100 + (packet + 1) * rows[i] / 120);
            assert(SendMessageW(lists[i], LB_GETCURSEL, 0, 0) == 10);
            if (before != SendMessageW(lists[i], LB_GETTOPINDEX, 0, 0)) {
                if (observations[i].renders <= rendersBefore) {
                    RECT bounds{}; GetClientRect(lists[i], &bounds);
                    std::cerr << "Missing scroll rendering: list=" << i << " size=" << bounds.right << 'x' << bounds.bottom
                        << " packet=" << packet << " visible=" << IsWindowVisible(lists[i])
                        << " iconic=" << IsIconic(g_hMainWnd) << " pending=" << GetUpdateRect(lists[i], nullptr, FALSE)
                        << " batch=" << (GetPropW(lists[i], ui::kListBoxUpdateProperty) != nullptr) << '\n';
                }
                assert(observations[i].renders > rendersBefore);
                assert(!GetUpdateRect(lists[i], nullptr, FALSE));
            }
        }
    }
    for (size_t i = 0; i < 5; ++i) {
        SendMessageW(lists[i], LB_SETTOPINDEX, 100, 0);
        SendMessageW(lists[i], WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-120)), 0);
        assert(SendMessageW(lists[i], LB_GETTOPINDEX, 0, 0) == 100 + rows[i]);
        if (rows[i] > 0) {
            assert(observations[i].renders > 0);
            assert(!GetUpdateRect(lists[i], nullptr, FALSE));
        }
        assert(RemoveWindowSubclass(lists[i], ObserveControl, observer));
        if (!visible[i]) ShowWindow(lists[i], SW_HIDE);
    }
    assert(RemoveWindowSubclass(g_hMainWnd, ObserveListRendering, 4936));
    g_pdf.path = (root / L"different.pdf").wstring();
    RefreshAnnotPanel();
    assert(SendMessageW(g_hAnnotList, LB_GETTOPINDEX, 0, 0) == 0);
    assert(SendMessageW(g_hAnnotList, LB_GETCURSEL, 0, 0) == LB_ERR);
    g_annots = savedAnnotations; g_pdf.path = savedPdf; RefreshAnnotPanel();
    if (!wasTopmost) SetWindowPos(g_hMainWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!mainVisible) ShowWindow(g_hMainWnd, SW_HIDE);
    std::cout << "List scroll PASS: 5 real controls, interleaved precision packets, split/grouped distance, selection and immediate paint; annotation refresh anchor/reorder/reveal/document switch.\n";
}

static void CheckContextReload(const std::filesystem::path& lecture, const std::filesystem::path& session) {
    const auto savedPdf = g_pdf.path;
    const auto savedNote = g_currentNotePath;
    const bool savedDirty = g_noteDirty;
    g_pdf.path = (session / L"pdf0.pdf").wstring();
    g_currentNotePath = (session / L"note0.txt").wstring();
    g_noteDirty = true;
    const auto openPdf = g_pdf.path;
    const auto openNote = g_currentNotePath;
    const auto openSession = g_currentSessionPath;
    const auto openLecture = g_currentLecturePath;
    std::error_code ec;
    std::filesystem::create_directory(root / L"workspace/zz_added", ec); assert(!ec);
    std::filesystem::create_directory(lecture / L"zz_added", ec); assert(!ec);
    Write(session / L"zz_added.pdf", "external pdf");
    Write(session / L"zz_added.txt", "external note");
    assert(std::filesystem::remove(session / L"pdf999.pdf", ec) && !ec);
    assert(std::filesystem::remove(session / L"note999.txt", ec) && !ec);
    const HWND lists[]{g_hLectureList, g_hSessionList, g_hPdfList, g_hNoteList};
    for (const auto list : lists) SendMessageW(list, LB_SETTOPINDEX, 100, 0);
    for (const auto list : lists) {
        assert(RefreshWorkspaceList(list));
        assert(g_currentLecturePath == openLecture && g_currentSessionPath == openSession);
        assert(g_pdf.path == openPdf && g_currentNotePath == openNote && g_noteDirty);
        for (const auto pane : lists) assert(SendMessageW(pane, LB_GETTOPINDEX, 0, 0) == 100);
    }
    assert(g_lectures.size() == kItems + 1 && g_sessions.size() == kItems + 1);
    assert(g_pdfFiles.size() == kItems && g_noteFiles.size() == kItems);
    const auto hasPath = [](const std::vector<FileEntry>& files, const std::filesystem::path& path) {
        return std::any_of(files.begin(), files.end(), [&](const auto& file) { return file.path == path.wstring(); });
    };
    assert(hasPath(g_pdfFiles, session / L"zz_added.pdf") && !hasPath(g_pdfFiles, session / L"pdf999.pdf"));
    assert(hasPath(g_noteFiles, session / L"zz_added.txt") && !hasPath(g_noteFiles, session / L"note999.txt"));
    assert(std::filesystem::file_size(session / L"note0.txt", ec) == 4 && !ec);
    g_pdf.path = savedPdf;
    g_currentNotePath = savedNote;
    g_noteDirty = savedDirty;
    SyncLeftPaneSelection();
    std::cout << "List context reload PASS: 4 directory lists reflect external additions/removals; open paths, dirty note and scroll positions preserved.\n";
}

static void CALLBACK GuardModal(HWND, UINT, UINT_PTR, DWORD) {
    HWND window = nullptr;
    while ((window = FindWindowExW(nullptr, window, L"SilentDialogClass", nullptr))) {
        if (GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId()) {
            wchar_t text[256]{}; GetWindowTextW(window, text, 256);
            std::wcerr << L"Unexpected dialog: " << text << std::endl;
            assert(false);
        }
    }
}

static void CALLBACK CheckApp(HWND, UINT, UINT_PTR, DWORD) {
    if (running || completed) return;
    if (!g_hMainWnd || !g_hNoteList || !g_hNoteEdit || !g_hBottomNote || g_workspaceRoot.empty()) return;
    if (SendMessageW(g_hLectureList, LB_GETCOUNT, 0, 0) < kItems) return;
    running = true;
    KillTimer(nullptr, readyTimer);
    const auto guard = SetTimer(nullptr, kModalGuardTimer, 20, GuardModal); assert(guard);
    assert(std::filesystem::path(g_workspaceRoot) == root / L"workspace");
    const auto lecture = root / L"workspace/lecture";
    const auto session = lecture / L"session0";
    CheckLinearBatch();
    CheckLabels(lecture, session);

    const auto start = std::chrono::steady_clock::now();
    LoadLectures();
    assert(g_lectures.size() == kItems && SendMessageW(g_hLectureList, LB_GETCOUNT, 0, 0) == kItems);
    g_currentLecturePath = lecture.wstring();
    LoadSessions(g_currentLecturePath);
    assert(g_sessions.size() == kItems && SendMessageW(g_hSessionList, LB_GETCOUNT, 0, 0) == kItems);
    std::wstring pdf, note;
    LoadFiles({L"session0", session.wstring()}, pdf, note);
    assert(g_pdfFiles.size() == kItems && g_noteFiles.size() == kItems);
    assert(SendMessageW(g_hPdfList, LB_GETCOUNT, 0, 0) == kItems);
    assert(SendMessageW(g_hNoteList, LB_GETCOUNT, 0, 0) == kItems);
    for (int i = 0; i < kItems; ++i) {
        assert(ui::ListBoxText(g_hPdfList, i) == std::filesystem::path(g_pdfFiles[static_cast<size_t>(i)].path).filename().wstring());
        assert(ui::ListBoxText(g_hNoteList, i) == std::filesystem::path(g_noteFiles[static_cast<size_t>(i)].path).filename().wstring());
    }
    std::cout << "reload_4x1000_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count() << '\n';
    CheckLabelCost(session);
    CheckLayout();
    CheckEmptyListPixels(session);
    CheckListScroll();

    g_pdf.path = g_pdfFiles.back().path;
    g_currentNotePath = g_noteFiles.back().path;
    SyncLeftPaneSelection();
    const HWND lists[]{g_hLectureList, g_hSessionList, g_hPdfList, g_hNoteList};
    for (const HWND list : lists) {
        const int index = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        assert(index >= 0);
        // A transient native selection must not change the actual open marker.
        SendMessageW(list, LB_SETCURSEL, index == 0 ? 1 : 0, 0);
        CheckPaint(list, index);
        SyncLeftPaneSelection();
        const LONG_PTR style = GetWindowLongPtrW(list, GWL_STYLE);
        ControlObservation observed;
        constexpr UINT_PTR kRedrawObserver = 4934;
        assert(SetWindowSubclass(list, ObserveControl, kRedrawObserver, reinterpret_cast<DWORD_PTR>(&observed)));
        {
            const ui::ListBoxUpdate first(list);
            { const ui::ListBoxUpdate nested(list); }
            assert(observed.disables == 1 && observed.enables == 0);
        }
        assert(observed.disables == 1 && observed.enables == 1);
        assert(RemoveWindowSubclass(list, ObserveControl, kRedrawObserver));
        assert((GetWindowLongPtrW(list, GWL_STYLE) & WS_VISIBLE) == (style & WS_VISIBLE));
    }

    ShowWindow(g_hNoteList, SW_HIDE);
    LoadFiles({L"session0", session.wstring()}, pdf, note);
    assert(!(GetWindowLongPtrW(g_hNoteList, GWL_STYLE) & WS_VISIBLE));
    ShowWindow(g_hNoteList, SW_SHOWNA);
    SyncLeftPaneSelection();
    const std::wstring longLabel(2048, L'資');
    const auto last = SendMessageW(g_hNoteList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(longLabel.c_str()));
    assert(ui::ListBoxText(g_hNoteList, static_cast<int>(last)) == longLabel);
    ui::RememberOpenListIndex(g_hNoteList, static_cast<int>(last));
    CheckPaint(g_hNoteList, static_cast<int>(last));
    assert(SendMessageW(g_hNoteList, LB_DELETESTRING, last, 0) != LB_ERR);
    g_currentNotePath = (session / L"missing.txt").wstring();
    SyncLeftPaneSelection();
    assert(ui::OpenListIndex(g_hNoteList) == -1);
    g_currentNotePath.clear(); g_pdf.path.clear();
    SyncLeftPaneSelection();
    assert(ui::OpenListIndex(g_hPdfList) == -1 && ui::OpenListIndex(g_hNoteList) == -1);
    CheckLabels(lecture, session);
    CheckContextReload(lecture, session);
    std::error_code ec;
    assert(std::filesystem::file_size(session / L"note0.txt", ec) == 4 && !ec);
    KillTimer(nullptr, guard);
    completed = true;
    std::cout << "List presentation PASS: 4 x 1000 entries, linear base extraction, label compatibility, open/selected colors, hidden/nested redraw, 2048-character label, failed-open clearing.\n";
    PostMessageW(g_hMainWnd, WM_CLOSE, 0, 0);
}

int wmain() {
    std::cout << std::unitbuf;
    std::error_code ec;
    root = std::filesystem::current_path(ec); assert(!ec);
    assert(!std::filesystem::exists(root / L"workspace", ec) && !ec);
    const auto lecture = root / L"workspace/lecture";
    const auto session = lecture / L"session0";
    for (int i = 0; i < kItems; ++i) {
        const auto path = i == 0 ? lecture : root / L"workspace" / (L"lecture" + std::to_wstring(i));
        std::filesystem::create_directories(path, ec); assert(!ec);
        std::filesystem::create_directories(lecture / (L"session" + std::to_wstring(i)), ec); assert(!ec);
        Write(session / (L"pdf" + std::to_wstring(i) + L".pdf"), "data");
        Write(session / (L"note" + std::to_wstring(i) + L".txt"), "note");
    }
    Write(root / L"workspace/workspace.json", "{\"classesDir\":\".\",\"sessionAutoOpenMode\":\"off\"}");
    Write(root / L"pdf_note_workspace_setup.json", "{\"workspaceRootMode\":\"relative\",\"workspaceRoot\":\"workspace\"}");
    readyTimer = SetTimer(nullptr, kReadyTimer, 50, CheckApp); assert(readyTimer);
    wchar_t command[] = L"";
    const int result = wWinMain(GetModuleHandleW(nullptr), nullptr, command, SW_HIDE);
    assert(result == 0 && completed);
    return 0;
}
