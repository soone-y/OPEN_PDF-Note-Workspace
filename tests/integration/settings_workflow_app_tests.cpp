// Real UI, real disk and real process restarts in a new owned fixture per case.
// No physical keyboard, clipboard, OS scale change or user workspace is used.
#include "app_test_driver.h"
#include "core/command_ids.h"
#include "core/json_string.h"
#include "core/sha256.h"
#include "settings/settings_navigation.h"
#include "clrop/json.h"
#include <array>
#include <map>
#include <string_view>

static HANDLE workflowChildJob = nullptr;
static void Write(const fs::path& path, const std::string& bytes) {
    std::wstring error;
    Require(atomic_write::AtomicWriteUtf8(path, bytes, path.parent_path(), &error), "write owned workflow fixture");
}
[[nodiscard]] static std::string Read(const fs::path& path) {
    std::string bytes;
    const bool read = ReadFileBytesWin32(path, bytes);
    if (!read) std::cerr << "Unreadable workflow evidence: " << path.u8string() << '\n';
    Require(read, "read workflow evidence");
    return bytes;
}
[[nodiscard]] static bool Contains(const fs::path& path, const std::string& token) {
    std::string bytes;
    return ReadFileBytesWin32(path, bytes) && bytes.find(token) != std::string::npos;
}

class CrashGate {
    std::map<std::wstring, std::wstring> previous_;
    std::map<std::wstring, bool> present_;
    HANDLE arm_ = nullptr, ready_ = nullptr, release_ = nullptr;
public:
    CrashGate(const CrashGate&) = delete;
    CrashGate& operator=(const CrashGate&) = delete;
    explicit CrashGate(const wchar_t* point) {
        const auto prefix = L"Local\\PdfNoteWorkflow_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        const std::array<std::pair<const wchar_t*, std::wstring>, 6> variables{{
            {L"PDF_NOTE_SMALL_ENABLE_FAULT_INJECTION", L"1"},
            {L"PDF_NOTE_SMALL_THROW_POINT", L""},
            {L"PDF_NOTE_SMALL_PAUSE_POINT", point},
            {L"PDF_NOTE_SMALL_PAUSE_ARM_EVENT", prefix + L"_arm"},
            {L"PDF_NOTE_SMALL_PAUSE_READY_EVENT", prefix + L"_ready"},
            {L"PDF_NOTE_SMALL_PAUSE_RELEASE_EVENT", prefix + L"_release"},
        }};
        // Capture before mutation so destruction restores the caller's process
        // environment. Handles are not inherited; the child opens exact names.
        for (const auto& [key, value] : variables) {
            const DWORD count = GetEnvironmentVariableW(key, nullptr, 0);
            present_[key] = count != 0;
            std::wstring old(count, L'\0');
            if (count) old.resize(GetEnvironmentVariableW(key, old.data(), count));
            previous_[key] = old;
        }
        arm_ = CreateEventW(nullptr, TRUE, FALSE, variables[3].second.c_str());
        ready_ = CreateEventW(nullptr, TRUE, FALSE, variables[4].second.c_str());
        release_ = CreateEventW(nullptr, TRUE, FALSE, variables[5].second.c_str());
        if (!arm_ || !ready_ || !release_) { Restore(); throw std::runtime_error("create crash handshake"); }
        for (const auto& [key, value] : variables) {
            if (!SetEnvironmentVariableW(key, value.empty() ? nullptr : value.c_str())) {
                Restore(); throw std::runtime_error("set isolated crash environment");
            }
        }
    }
    void Restore() {
        for (const auto& [key, value] : previous_) SetEnvironmentVariableW(key.c_str(), present_[key] ? value.c_str() : nullptr);
        if (release_) { SetEvent(release_); CloseHandle(release_); release_ = nullptr; }
        if (ready_) { CloseHandle(ready_); ready_ = nullptr; }
        if (arm_) { CloseHandle(arm_); arm_ = nullptr; }
    }
    ~CrashGate() { Restore(); }
    void Arm() { Require(SetEvent(arm_), "arm checkpoint only after application startup"); }
    void Wait() { Require(WaitForSingleObject(ready_, 10000) == WAIT_OBJECT_0, "child reached the exact durable checkpoint"); }
};

class OwnedApp {
    PROCESS_INFORMATION process_{};
    fs::path initializedWorkspace_;
public:
    HWND main = nullptr;
    OwnedApp() = default;
    OwnedApp(const OwnedApp&) = delete;
    OwnedApp& operator=(const OwnedApp&) = delete;
    ~OwnedApp() {
        if (process_.hProcess) {
            if (WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process_.hProcess, 79); WaitForSingleObject(process_.hProcess, 3000);
            }
            CloseHandle(process_.hThread); CloseHandle(process_.hProcess);
        }
    }
    void Start(const fs::path& executable, const fs::path& workspace,
               const std::vector<fs::path>& externalRoots = {}) {
        // A real restart retains app-level settings, including tool order.
        // Only a new case/workspace receives a fresh owned setup file.
        if (initializedWorkspace_ != workspace) {
            std::string setup = "{\"workspaceRootMode\":\"absolute\",\"workspaceRoot\":\"" + json_string::Escape(workspace.u8string()) + "\",\"tempExternalLectureDirs\":[";
            for (size_t index = 0; index < externalRoots.size(); ++index) {
                if (index) setup += ',';
                setup += '"' + json_string::Escape(externalRoots[index].u8string()) + '"';
            }
            Write(executable.parent_path() / L"pdf_note_workspace_setup.json", setup + "]}");
            initializedWorkspace_ = workspace;
        }
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        std::wstring command = L"\"" + executable.wstring() + L"\"";
        Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                               executable.parent_path().c_str(), &startup, &process_), "launch owned workflow app");
        Require(workflowChildJob && AssignProcessToJobObject(workflowChildJob, process_.hProcess) &&
            ResumeThread(process_.hThread) != static_cast<DWORD>(-1), "assign exact owned child to watchdog job before it runs");
        childPid = process_.dwProcessId;
        main = WaitWindow(L"PdfWorkspaceMainWnd");
        Require(Until([&] { return IsWindowEnabled(main) && IsWindowVisible(main); }), "workflow app ready");
    }
    void Stop(bool crash = false) {
        if (crash) Require(TerminateProcess(process_.hProcess, 79), "terminate only the owned checkpoint child");
        else Require(PostMessageW(main, WM_CLOSE, 0, 0), "normal workflow exit");
        Require(WaitForSingleObject(process_.hProcess, 10000) == WAIT_OBJECT_0, "workflow child is joined");
        DWORD code = 0;
        Require(GetExitCodeProcess(process_.hProcess, &code) && code == (crash ? 79u : 0u), "workflow exit status is verified");
        CloseHandle(process_.hThread); CloseHandle(process_.hProcess); process_ = {}; main = nullptr;
    }
};

[[nodiscard]] static std::array<HWND, 4> Lists(HWND main) {
    std::array<HWND, 4> lists{};
    HWND previous = nullptr;
    for (auto& list : lists) {
        list = FindWindowExW(main, previous, WC_LISTBOXW, nullptr);
        Require(list != nullptr, "workflow navigation list exists"); previous = list;
    }
    return lists;
}
static void Select(HWND main, HWND list, int index) {
    Require(Send(list, LB_SETCURSEL, index) != static_cast<DWORD_PTR>(LB_ERR), "select existing workflow row");
    Send(main, WM_COMMAND, MAKEWPARAM(0, LBN_SELCHANGE), reinterpret_cast<LPARAM>(list));
}
static void SelectSession(HWND main) {
    auto lists = Lists(main);
    Require(Send(lists[0], LB_GETCOUNT) == 1, "one owned lecture only"); Select(main, lists[0], 0);
    Require(Send(lists[1], LB_GETCOUNT) == 1, "one owned session only"); Select(main, lists[1], 0);
}
[[nodiscard]] static HWND OpenSettings(HWND main) {
    Require(PostMessageW(main, WM_COMMAND, ID_SETTINGS_GENERAL, 0), "open workflow settings");
    return WaitWindow(L"UnifiedSettingsShellClass");
}
static void Page(HWND settings, int index) {
    // This is the same navigation message sent by category controls. Keep the
    // process boundary pointer-free (WM_NOTIFY would need remote NMHDR storage).
    Send(settings, WM_APP + 140, index);
    HWND tree = GetDlgItem(settings, 7100);
    Require(tree && !WindowText(GetDlgItem(settings, 7103)).empty(), "real settings sidebar navigation");
    auto root = Send(tree, TVM_GETNEXTITEM, TVGN_ROOT);
    DWORD_PTR expected = 0;
    for (int group = 0; group < settings_navigation::GroupCount; ++group) {
        auto leaf = Send(tree, TVM_GETNEXTITEM, TVGN_CHILD, static_cast<LPARAM>(root));
        for (const auto page : settings_navigation::sidebarOrder) if (settings_navigation::entries[page].group == group) {
            if (page == index) expected = leaf;
            leaf = Send(tree, TVM_GETNEXTITEM, TVGN_NEXT, static_cast<LPARAM>(leaf));
        }
        root = Send(tree, TVM_GETNEXTITEM, TVGN_NEXT, static_cast<LPARAM>(root));
    }
    Require(expected && Send(tree, TVM_GETNEXTITEM, TVGN_CARET) == expected, "sidebar selection follows the requested page");
}
static void CloseSettings(HWND settings, int action, const char* context = "workflow") {
    Require(PostMessageW(settings, WM_COMMAND, action, 0), "request settings close");
    const bool closed = Until([&] {
        HWND prompt = Window(L"SilentDialogClass");
        // FindWindow can observe WM_CREATE before the modal dialog is ready.
        // Match WaitWindow's visible boundary before choosing Discard drafts.
        if (prompt && IsWindowVisible(prompt) && IsWindowEnabled(prompt) && GetWindow(prompt, GW_OWNER) == settings) {
            const HWND discard = GetDlgItem(prompt, 5103);
            if (discard && IsWindowEnabled(discard)) Send(prompt, WM_COMMAND, 5103);
        }
        return !IsWindow(settings);
    });
    if (!closed) {
        std::cerr << "Settings close timeout: context=" << context << " action=" << action << " checks=" << checks << '\n';
        EnumWindows([](HWND window, LPARAM) -> BOOL {
            DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
            if (pid != childPid || !IsWindowVisible(window)) return TRUE;
            char name[128]{}; GetClassNameA(window, name, 128);
            std::cerr << "Owned window: class=" << name << " hwnd=" << window << " owner=" << GetWindow(window, GW_OWNER)
                      << " visible=" << IsWindowVisible(window) << " enabled=" << IsWindowEnabled(window) << '\n';
            return TRUE;
        }, 0);
    }
    Require(closed, "workflow settings close at expected boundary");
}
struct Draft {
    int layout = 0, noteFont = 0, headingFont = 0, lens = 0;
    bool clroAsciiOnly = false;
    int bottomSpaceMode = 0;
    std::wstring bottomSpaceFixed, bottomSpaceFont, bottomSpaceViewport;
    std::wstring hex;
};
static void Seed(const fs::path& workspace, bool flat, const fs::path& lectureRoot = {});
[[nodiscard]] static Draft Capture(HWND settings) {
    Draft draft;
    Page(settings, 8); draft.layout = static_cast<int>(Send(DescendantId(settings, 4214), CB_GETCURSEL));
    Page(settings, 1); draft.noteFont = static_cast<int>(Send(DescendantId(settings, 4114), CB_GETCURSEL));
    Require(DescendantId(settings, 4227) != nullptr, "CLRO ASCII-only setting exists");
    draft.clroAsciiOnly = Send(DescendantId(settings, 4227), BM_GETCHECK) == BST_CHECKED;
    Require(DescendantId(settings, 4250) && DescendantId(settings, 4251) &&
        DescendantId(settings, 4252) && DescendantId(settings, 4253), "bottom space controls exist");
    draft.bottomSpaceMode = static_cast<int>(Send(DescendantId(settings, 4250), CB_GETCURSEL));
    draft.bottomSpaceFixed = WindowText(DescendantId(settings, 4251));
    draft.bottomSpaceFont = WindowText(DescendantId(settings, 4252));
    draft.bottomSpaceViewport = WindowText(DescendantId(settings, 4253));
    Page(settings, 2); Require(Descendant(settings, L"MarkupSettingsDlgClass") != nullptr, "markup page was lazily created");
    draft.headingFont = static_cast<int>(Send(DescendantId(Descendant(settings, L"MarkupSettingsDlgClass"), 4201), CB_GETCURSEL));
    Page(settings, 3); draft.lens = static_cast<int>(Send(DescendantId(settings, 1013), CB_GETCURSEL));
    Page(settings, 4); draft.hex = WindowText(DescendantId(settings, 5224));
    Require(draft.layout >= 0 && draft.noteFont >= 0 && draft.headingFont >= 0 && draft.lens >= 0, "all captured choices are valid");
    return draft;
}
static void Expect(HWND settings, const Draft& expected) {
    const auto actual = Capture(settings);
    if (actual.layout != expected.layout || actual.noteFont != expected.noteFont || actual.headingFont != expected.headingFont ||
        actual.lens != expected.lens || actual.hex != expected.hex) {
        std::wcerr << L"Draft actual/expected: layout " << actual.layout << L'/' << expected.layout << L", note " << actual.noteFont << L'/' << expected.noteFont
            << L", heading " << actual.headingFont << L'/' << expected.headingFont << L", lens " << actual.lens << L'/' << expected.lens
            << L", color " << actual.hex << L'/' << expected.hex << L'\n';
    }
    Require(actual.layout == expected.layout && actual.noteFont == expected.noteFont &&
        actual.headingFont == expected.headingFont && actual.lens == expected.lens && actual.hex == expected.hex &&
        actual.clroAsciiOnly == expected.clroAsciiOnly && actual.bottomSpaceMode == expected.bottomSpaceMode &&
        actual.bottomSpaceFixed == expected.bottomSpaceFixed && actual.bottomSpaceFont == expected.bottomSpaceFont &&
        actual.bottomSpaceViewport == expected.bottomSpaceViewport,
        "all five settings tabs have the expected saved or draft values");
}
[[nodiscard]] static Draft Change(HWND settings, const Draft& old) {
    Draft changed = old;
    changed.layout = 1 - old.layout; changed.hex = L"#314159";
    Page(settings, 8); Send(DescendantId(settings, 4214), CB_SETCURSEL, changed.layout);
    Page(settings, 1);
    changed.clroAsciiOnly = !old.clroAsciiOnly;
    changed.bottomSpaceMode = (old.bottomSpaceMode + 1) % 3;
    changed.bottomSpaceFixed = old.bottomSpaceFixed == L"144" ? L"96" : L"144";
    changed.bottomSpaceFont = old.bottomSpaceFont == L"600" ? L"400" : L"600";
    changed.bottomSpaceViewport = old.bottomSpaceViewport == L"33" ? L"25" : L"33";
    Send(DescendantId(settings, 4250), CB_SETCURSEL, changed.bottomSpaceMode);
    Type(DescendantId(settings, 4251), changed.bottomSpaceFixed.c_str());
    Type(DescendantId(settings, 4252), changed.bottomSpaceFont.c_str());
    Type(DescendantId(settings, 4253), changed.bottomSpaceViewport.c_str());
    Send(DescendantId(settings, 4227), BM_SETCHECK, changed.clroAsciiOnly ? BST_CHECKED : BST_UNCHECKED);
    HWND font = DescendantId(settings, 4114); const int count = static_cast<int>(Send(font, CB_GETCOUNT));
    Require(count > 1, "note font has multiple choices"); changed.noteFont = (old.noteFont + 1) % count;
    Send(font, CB_SETCURSEL, changed.noteFont);
    Page(settings, 2); HWND heading = DescendantId(Descendant(settings, L"MarkupSettingsDlgClass"), 4201);
    const int headingCount = static_cast<int>(Send(heading, CB_GETCOUNT));
    Require(headingCount > 1, "markup heading has multiple font choices");
    changed.headingFont = (old.headingFont + 1) % headingCount; Send(heading, CB_SETCURSEL, changed.headingFont);
    Page(settings, 3);
    HWND lens = DescendantId(settings, 1013); Require(Send(lens, CB_GETCOUNT) > 1, "lens has multiple choices");
    changed.lens = 1 - old.lens; Send(lens, CB_SETCURSEL, changed.lens);
    Page(settings, 4); Type(DescendantId(settings, 5224), changed.hex.c_str());
    Expect(settings, changed); return changed;
}

static void SettingsMatrix(const fs::path& executable, const fs::path& workspace) {
    OwnedApp app; app.Start(executable, workspace);
    HWND settings = OpenSettings(app.main); const auto baseline = Capture(settings);
    const auto palettePath = workspace / L"__pdf_note_workspace__" / L"__settings__" / L"user_palette.json";
    const auto configPath = workspace / L"workspace.json";
    Type(DescendantId(settings, 5224), L"#000001");
    Send(Descendant(settings, L"PaletteSettingsDlgClass"), WM_COMMAND, 5282);
    Type(DescendantId(settings, 5224), baseline.hex.c_str());
    Send(Descendant(settings, L"PaletteSettingsDlgClass"), WM_COMMAND, 5282);
    Page(settings, 0); CloseSettings(settings, IDOK, "establish saved pages");
    settings = OpenSettings(app.main); const auto changed = Change(settings, baseline);
    // Revert only the selected note page; drafts in other pages remain intact.
    Page(settings, 1); Send(settings, WM_COMMAND, 7102);
    Draft reverted = changed; reverted.noteFont = baseline.noteFont; reverted.clroAsciiOnly = baseline.clroAsciiOnly;
    reverted.bottomSpaceMode = baseline.bottomSpaceMode;
    reverted.bottomSpaceFixed = baseline.bottomSpaceFixed; reverted.bottomSpaceFont = baseline.bottomSpaceFont;
    reverted.bottomSpaceViewport = baseline.bottomSpaceViewport; Expect(settings, reverted);
    CloseSettings(settings, IDCANCEL, "cancel multi-tab drafts");
    Draft immediate = baseline; immediate.hex = changed.hex;
    settings = OpenSettings(app.main); Expect(settings, immediate);
    (void)Change(settings, baseline);
    Send(Descendant(settings, L"PaletteSettingsDlgClass"), WM_COMMAND, 5282);
    const auto paletteBytes = Read(palettePath);
    const auto configBytes = Read(configPath);
    Type(DescendantId(settings, 5220), L"999");
    Require(WindowText(DescendantId(settings, 5220)) == L"999", "invalid RGB draft reached the real editor unchanged");
    Page(settings, 0);
    Send(settings, WM_COMMAND, 7101);
    if (!IsWindow(settings) || !Equals(configPath, configBytes) || !Equals(palettePath, paletteBytes))
        std::cerr << "Invalid-draft boundary: open=" << IsWindow(settings) << " config_unchanged=" << Equals(configPath, configBytes)
            << " palette_unchanged=" << Equals(palettePath, paletteBytes) << '\n';
    Require(IsWindow(settings) && Equals(configPath, configBytes) && Equals(palettePath, paletteBytes),
            "invalid late color draft prevents every earlier page from saving");
    Type(DescendantId(settings, 5224), changed.hex.c_str());
    Send(Descendant(settings, L"PaletteSettingsDlgClass"), WM_COMMAND, 5282);
    Page(settings, 0);
    HANDLE lock = CreateFileW(configPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(lock != INVALID_HANDLE_VALUE, "hold owned first settings file against replacement");
    Send(settings, WM_COMMAND, 7101); CloseHandle(lock);
    Require(IsWindow(settings) && Equals(configPath, configBytes), "first-file save failure keeps settings open and original file intact");
    Expect(settings, changed); CloseSettings(settings, IDCANCEL, "cancel after first-file save failure");
    settings = OpenSettings(app.main); Expect(settings, immediate); (void)Change(settings, baseline);
    // Stock and palette are immediate independent saves; other tab drafts are not.
    Send(Descendant(settings, L"PaletteSettingsDlgClass"), WM_COMMAND, 5253);
    const auto stockPath = workspace / L"__pdf_note_workspace__" / L"__settings__" / L"color_stock.json";
    const auto stockBytes = Read(stockPath); Require(stockBytes.find("#314159") != std::string::npos, "independent stock save uses the edited color");
    CloseSettings(settings, IDCANCEL, "cancel after independent stock save"); app.Stop();
    app.Start(executable, workspace); settings = OpenSettings(app.main); Expect(settings, immediate);
    Require(Equals(stockPath, stockBytes) && Equals(palettePath, paletteBytes), "restart preserves stock and active palette but discards cancelled non-palette drafts");
    (void)Change(settings, baseline);
    lock = CreateFileW(palettePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(lock != INVALID_HANDLE_VALUE, "hold owned last settings file against replacement");
    Type(DescendantId(settings, 5224), L"#271828");
    Page(settings, 0);
    const auto beforePaletteFailure = Read(configPath);
    Send(settings, WM_COMMAND, 7101); CloseHandle(lock);
    Require(IsWindow(settings) && Equals(palettePath, paletteBytes) && Equals(configPath, beforePaletteFailure),
        "pending palette save failure prevents unrelated settings writes and preserves original files");
    Draft pending = changed; pending.hex = L"#271828";
    Expect(settings, pending);
    HWND retry = WaitWindow(L"PdfNotePendingWriteRetry");
    Require(WindowText(GetDlgItem(retry, 2)).find(L"#271828") != std::wstring::npos,
        "failed active palette colors are visible in the retry window");
    Send(retry, WM_COMMAND, 3);
    Require(Until([&] { return !IsWindowVisible(retry) && Contains(palettePath, "#271828"); }),
        "active palette retry commits the saved colors after the lock is released");
    CloseSettings(settings, IDCANCEL, "cancel after palette retry"); app.Stop();
    app.Start(executable, workspace); settings = OpenSettings(app.main);
    immediate.hex = pending.hex;
    Expect(settings, immediate); Require(Equals(stockPath, stockBytes), "palette retry preserves stock and cancelled non-palette settings");
    (void)Change(settings, baseline); Page(settings, 0); Send(settings, WM_COMMAND, 7101);
    Require(IsWindow(settings), "successful explicit Save keeps settings open");
    CloseSettings(settings, IDCANCEL, "close after explicit save"); app.Stop();
    app.Start(executable, workspace); settings = OpenSettings(app.main); Expect(settings, changed);
    CloseSettings(settings, IDCANCEL, "close restarted settings"); app.Stop();
    std::cout << "[PASS] multi-tab cancel/revert/explicit failure/palette auto-save/restart\n";
}

static void PaletteDocumentRetryMatrix(const fs::path& executable, const fs::path& workspace) {
    Seed(workspace, false);
    OwnedApp app; app.Start(executable, workspace);
    HWND settings = OpenSettings(app.main); Page(settings, 4);
    const auto page = Descendant(settings, L"PaletteSettingsDlgClass");
    Require(page != nullptr, "palette page exists for retry checks");
    Type(DescendantId(settings, 5224), L"#010203");
    Send(page, WM_COMMAND, 5282);
    Send(page, WM_COMMAND, 5253);
    const auto stockPath = workspace / L"__pdf_note_workspace__" / L"__settings__" / L"color_stock.json";
    const auto beforeStock = Read(stockPath);
    Type(DescendantId(settings, 5224), L"#040506");
    Send(page, WM_COMMAND, 5282);
    HANDLE lock = CreateFileW(stockPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(lock != INVALID_HANDLE_VALUE, "lock owned stock file against replacement");
    Send(page, WM_COMMAND, 5253);
    HWND retry = WaitWindow(L"PdfNotePendingWriteRetry");
    Require(Equals(stockPath, beforeStock) && WindowText(GetDlgItem(retry, 2)).find(L"#040506") != std::wstring::npos,
        "failed stock save retains original bytes and the requested color");
    CloseHandle(lock);
    CloseSettings(settings, IDCANCEL);
    Send(retry, WM_COMMAND, 3);
    Require(Until([&] { return !IsWindowVisible(retry) && Contains(stockPath, "#040506"); }),
        "stock retry succeeds after the settings page closes");

    settings = OpenSettings(app.main); Page(settings, 4);
    const auto sets = DescendantId(settings, 5262);
    Require(sets && Send(sets, CB_SETCURSEL, 1) != static_cast<DWORD_PTR>(CB_ERR), "select first user palette set");
    Send(Descendant(settings, L"PaletteSettingsDlgClass"), WM_COMMAND, 5265);
    const auto setsPath = workspace / L"__pdf_note_workspace__" / L"__settings__" / L"palette_presets.json";
    const auto beforeSets = Read(setsPath);
    Require(Send(sets, CB_SETCURSEL, 2) != static_cast<DWORD_PTR>(CB_ERR), "select empty second user palette set");
    lock = CreateFileW(setsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(lock != INVALID_HANDLE_VALUE, "lock owned palette set file against replacement");
    Send(Descendant(settings, L"PaletteSettingsDlgClass"), WM_COMMAND, 5265);
    retry = WaitWindow(L"PdfNotePendingWriteRetry");
    Require(Equals(setsPath, beforeSets) && WindowText(GetDlgItem(retry, 2)).find(L"users") != std::wstring::npos,
        "failed palette set save retains original bytes and the requested document");
    CloseHandle(lock);
    CloseSettings(settings, IDCANCEL);
    Send(retry, WM_COMMAND, 3);
    Require(Until([&] { std::string bytes; return !IsWindowVisible(retry) && ReadFileBytesWin32(setsPath, bytes) && bytes != beforeSets; }),
        "palette set retry succeeds after the settings page closes");
    app.Stop();
    std::cout << "[PASS] palette stock and set retry after settings close\n";
}

static void ActivePaletteDetachedRetry(const fs::path& executable, const fs::path& workspace) {
    Seed(workspace, false);
    OwnedApp app; app.Start(executable, workspace);
    HWND settings = OpenSettings(app.main); Page(settings, 4);
    auto page = Descendant(settings, L"PaletteSettingsDlgClass");
    Type(DescendantId(settings, 5224), L"#001122"); Send(page, WM_COMMAND, 5282);
    const auto palettePath = workspace / L"__pdf_note_workspace__" / L"__settings__" / L"user_palette.json";
    const auto before = Read(palettePath);
    HANDLE lock = CreateFileW(palettePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(lock != INVALID_HANDLE_VALUE, "lock owned active palette file against replacement");
    Type(DescendantId(settings, 5224), L"#AABBCC"); Send(page, WM_COMMAND, 5282);
    HWND retry = WaitWindow(L"PdfNotePendingWriteRetry");
    Require(GetWindow(retry, GW_OWNER) == app.main, "palette retry window belongs to the stable main window");
    Require(Equals(palettePath, before), "failed active palette save preserves original bytes");
    Require(PostMessageW(settings, WM_COMMAND, IDCANCEL, 0), "request closing failed palette editor");
    HWND discard = WaitWindow(L"SilentDialogClass");
    Send(discard, WM_COMMAND, 5102);
    Require(Until([&] { return !IsWindow(settings); }), "failed palette editor closes after explicit discard choice");
    settings = OpenSettings(app.main); Page(settings, 4);
    CloseSettings(settings, IDCANCEL);
    Require(Window(L"PdfNotePendingWriteRetry") == retry && IsWindowVisible(retry),
        "opening and closing a fresh palette page does not erase the older retry window");
    Require(Send(GetDlgItem(retry, 1), LB_GETCOUNT) == 1,
        "opening and closing a fresh palette page preserves one pending attempt");
    CloseHandle(lock);
    Send(retry, WM_COMMAND, 3);
    Require(Until([&] { return !IsWindowVisible(retry) && Contains(palettePath, "#AABBCC"); }),
        "active palette retry succeeds after the original settings page closes");
    app.Stop();
    std::cout << "[PASS] active palette retry survives settings close and fresh no-op page\n";
}

[[nodiscard]] static std::string Pdf() {
    std::string pdf = "%PDF-1.4\n";
    const std::array<std::string, 3> objects{{"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> >>"}};
    std::vector<size_t> offsets;
    for (size_t i = 0; i < objects.size(); ++i) { offsets.push_back(pdf.size()); pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n"; }
    const auto xref = pdf.size(); pdf += "xref\n0 4\n0000000000 65535 f \n";
    for (auto offset : offsets) { const auto text = std::to_string(offset); pdf += std::string(10 - text.size(), '0') + text + " 00000 n \n"; }
    return pdf + "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
}
[[nodiscard]] static std::string AnnotationFixture(const fs::path& path) {
    const auto pdf = Pdf(); core_hash::Sha256 hash;
    hash.Update(reinterpret_cast<const std::uint8_t*>(pdf.data()), pdf.size());
    std::string digest; constexpr char hex[] = "0123456789abcdef";
    for (auto byte : hash.Finalize()) { digest += hex[byte >> 4]; digest += hex[byte & 15]; }
    return "{\"version\":1,\"pdf_id\":{\"path\":\"" + json_string::Escape(path.u8string()) +
        "\",\"size\":" + std::to_string(pdf.size()) + ",\"page_count\":1,\"page_sizes_pt\":[[100,100]],\"sha256\":\"" + digest +
        "\"},\"pages\":[{\"page\":0,\"items\":[{\"type\":\"line\",\"id\":\"workflow-line\","
        "\"created\":\"2026-10-10T00:00:00Z\",\"updated\":\"2026-10-10T00:00:00Z\",\"p1\":[10,10],\"p2\":[40,40],\"width\":2,\"color\":\"#123456\"}]}]}";
}
static void CheckAnnotation(const fs::path& path) {
    clrop::Document annotations; std::wstring error;
    Require(clrop::ParseClropFromJson(Read(path), annotations, error) && annotations.pages.size() == 1 &&
        annotations.pages[0].items.size() == 1 && annotations.pages[0].items[0].id == L"workflow-line",
        "production parser retains the annotation across workflow/restart");
    const auto& line = annotations.pages[0].items[0];
    Require(annotations.version == 1 && annotations.pages[0].page == 0 &&
        annotations.pdfId.size == Pdf().size() && annotations.pdfId.pageCount == 1 &&
        annotations.pdfId.pageSizesPt == std::vector<std::array<double, 2>>{{100, 100}} &&
        line.kind == clrop::Item::Kind::Line && line.p1 == std::optional<std::array<double, 2>>{{10, 10}} &&
        line.p2 == std::optional<std::array<double, 2>>{{40, 40}} && line.width == 2 && line.color == "#123456" &&
        line.created == L"2026-10-10T00:00:00Z" && line.updated == line.created,
        "annotation geometry/style/timestamps and PDF identity dimensions survive the entire workflow");
}
static void Seed(const fs::path& workspace, bool flat, const fs::path& lectureRoot) {
    fs::create_directories(workspace);
    fs::create_directories((lectureRoot.empty() ? workspace / L"講義 空白" : lectureRoot) / L"第01回");
    Write(workspace / L"workspace.json", "{\"classesDir\":\".\",\"startupSelectFirstSession\":false,\"noteRenderEnabled\":false,\"noteRawOnly\":true,\"sessionFileLayout\":\"" +
        std::string(flat ? "session_root" : "separate_directories") + "\"}");
}
static void BottomPanePersistenceMatrix(const fs::path& executable, const fs::path& workspace) {
    Seed(workspace, false);
    OwnedApp app; app.Start(executable, workspace);
    const auto config = workspace / L"workspace.json";
    const auto checked = [&](UINT command) {
        const UINT state = GetMenuState(GetMenu(app.main), command, MF_BYCOMMAND);
        Require(state != static_cast<UINT>(-1), "bottom-pane menu command remains reachable");
        return (state & MF_CHECKED) != 0;
    };
    const auto open = [&] {
        Require(PostMessageW(app.main, WM_COMMAND, ID_SETTINGS_BOTTOM_PANE, 0), "open persistent bottom-pane settings from its menu");
        HWND shell = WaitWindow(L"UnifiedSettingsShellClass");
        Require(IsWindowVisible(DescendantId(shell, 4107)), "bottom-pane menu opens its dedicated editing page");
        return shell;
    };
    auto before = Read(config);
    Send(app.main, WM_COMMAND, ID_VIEW_BOTTOM_HEADINGS);
    Require(checked(ID_VIEW_BOTTOM_HEADINGS) && Equals(config, before), "View changes only the temporary bottom-pane mode");
    HWND shell = open();
    Require(Send(DescendantId(shell, 4107), CB_GETCURSEL) == 0, "dialog shows persisted default rather than temporary View choice");
    CloseSettings(shell, IDCANCEL);
    Require(checked(ID_VIEW_BOTTOM_HEADINGS) && Equals(config, before), "closing unchanged settings preserves the temporary mode and saved default");
    shell = open(); Send(shell, WM_COMMAND, 7101);
    Require(checked(ID_VIEW_BOTTOM_NOTE), "applying an unchanged saved choice overrides the temporary View mode");
    const UINT modes[] = {ID_VIEW_BOTTOM_NOTE, ID_VIEW_BOTTOM_HEADINGS, ID_VIEW_BOTTOM_MATH, ID_VIEW_BOTTOM_ASSIST};
    const char* pins[] = {"note", "note", "math", "note"};
    const char* noteModes[] = {"legacy", "headings", "legacy", "assist"};
    for (int index = 0; index < 4; ++index) {
        Send(DescendantId(shell, 4107), CB_SETCURSEL, index); Send(shell, WM_COMMAND, 7101);
        const auto bytes = Read(config);
        Require(checked(modes[index]) && bytes.find(std::string("\"bottomPanePin\": \"") + pins[index] + "\"") != std::string::npos &&
            bytes.find(std::string("\"bottomNoteMode\": \"") + noteModes[index] + "\"") != std::string::npos,
            "bottom-pane settings persist and activate the dialog selection");
    }
    CloseSettings(shell, IDCANCEL);
    before = Read(config); Send(app.main, WM_COMMAND, ID_VIEW_BOTTOM_MATH);
    Require(checked(ID_VIEW_BOTTOM_MATH) && Equals(config, before), "temporary MathBox does not overwrite the saved assist mode");
    shell = OpenSettings(app.main);
    Require(!DescendantId(shell, 4107), "general settings do not duplicate the bottom-pane editor");
    HWND restore = DescendantId(shell, 4223);
    Send(restore, BM_CLICK); Send(shell, WM_COMMAND, 7101);
    Require(checked(ID_VIEW_BOTTOM_MATH) && Read(config).find("\"bottomNoteMode\": \"assist\"") != std::string::npos,
        "saving unrelated settings preserves the temporary bottom-pane mode");
    CloseSettings(shell, IDCANCEL);
    shell = open(); Send(DescendantId(shell, 4107), CB_SETCURSEL, 1);
    before = Read(config);
    HANDLE lock = CreateFileW(config.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(lock != INVALID_HANDLE_VALUE, "hold owned bottom-pane settings file against replacement");
    Send(shell, WM_COMMAND, 7101); CloseHandle(lock);
    Require(IsWindow(shell) && Equals(config, before) && checked(ID_VIEW_BOTTOM_MATH) &&
        Send(DescendantId(shell, 4107), CB_GETCURSEL) == 1,
        "failed bottom-pane save preserves disk, temporary mode and retryable dialog draft");
    Send(shell, WM_COMMAND, 7101);
    Require(checked(ID_VIEW_BOTTOM_HEADINGS) && Read(config).find("\"bottomNoteMode\": \"headings\"") != std::string::npos,
        "retry commits the dialog selection before replacing the temporary mode");
    CloseSettings(shell, IDCANCEL);
    Send(app.main, WM_COMMAND, ID_VIEW_BOTTOM_ASSIST);
    app.Stop(); app.Start(executable, workspace);
    Require(checked(ID_VIEW_BOTTOM_HEADINGS), "restart restores the dialog default rather than the final temporary mode");
    shell = open();
    Require(Send(DescendantId(shell, 4107), CB_GETCURSEL) == 1, "saved bottom-pane selection survives restart");
    CloseSettings(shell, IDCANCEL); app.Stop();
    std::cout << "[PASS] bottom-pane persistent default / temporary View / dialog precedence / failure / restart\n";
}

static void CentralGuideMatrix(const fs::path& executable, const fs::path& workspace) {
    Seed(workspace, false);
    const auto config = workspace / L"workspace.json";
    OwnedApp app; app.Start(executable, workspace);
    const auto view = [&] {
        HWND result = Descendant(app.main, L"PdfWorkspacePdfView");
        Require(result != nullptr, "central guide owns the actual PDF pane");
        RedrawWindow(result, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        return result;
    };
    const auto guide = [&] { return WindowText(view()); };
    Require(Until([&] { return guide().find(L"1.") != std::wstring::npos; }), "default guide includes numbered usage");
    Require(guide().find(L"5.") != std::wstring::npos && guide().find(L"Ctrl+S") != std::wstring::npos &&
        guide().find(L"Ctrl+Alt+1") != std::wstring::npos, "guide includes usage, global actions and current bindings");
    const auto hasDisplayHint = [&] {
        const auto text = guide();
        return text.find(L"Change this guide to Shortcuts only or Hidden under Settings") != std::wstring::npos ||
            text.find(L"この案内は、設定 → 環境 → 画面・共通操作で「ショートカットのみ」「表示しない」に変更できます。") != std::wstring::npos;
    };
    Require(hasDisplayHint(), "guide explains its optional display modes and settings location");
    HWND shell = OpenSettings(app.main);
    HWND choice = DescendantId(shell, 4226);
    Require(choice && Send(choice, CB_GETCOUNT) == 3 && Send(choice, CB_GETCURSEL) == 0, "three guide choices and initial default");
    Send(choice, CB_SETCURSEL, 1); Page(shell, 5); Page(shell, 0);
    Require(Send(DescendantId(shell, 4226), CB_GETCURSEL) == 1, "guide draft survives page navigation");
    Send(shell, WM_COMMAND, 7102);
    Require(Send(DescendantId(shell, 4226), CB_GETCURSEL) == 0, "guide draft reverts independently");
    Send(DescendantId(shell, 4226), CB_SETCURSEL, 1); Send(shell, WM_COMMAND, 7101);
    Require(guide().find(L"1.") == std::wstring::npos && guide().find(L"Ctrl+S") != std::wstring::npos,
        "applied shortcut-only guide omits the numbered instructions");
    Require(Read(config).find("\"emptyViewGuideMode\": 1") != std::string::npos, "guide setting persists through normal config I/O");
    const auto before = Read(config);
    Send(DescendantId(shell, 4226), CB_SETCURSEL, 2); CloseSettings(shell, IDCANCEL);
    Require(Equals(config, before) && !guide().empty(), "cancel retains saved guide and file bytes");
    app.Stop(); app.Start(executable, workspace);
    Require(guide().find(L"1.") == std::wstring::npos && guide().find(L"Ctrl+S") != std::wstring::npos,
        "shortcut-only preference survives process restart");
    Require(hasDisplayHint(), "shortcut-only guide retains its display-settings explanation");
    shell = OpenSettings(app.main); Send(DescendantId(shell, 4226), CB_SETCURSEL, 2);
    HANDLE lock = CreateFileW(config.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(lock != INVALID_HANDLE_VALUE, "lock owned guide configuration against replacement");
    const auto lockedBytes = Read(config);
    Send(shell, WM_COMMAND, 7101); CloseHandle(lock);
    Require(Equals(config, lockedBytes) && guide().find(L"Ctrl+S") != std::wstring::npos &&
        Send(DescendantId(shell, 4226), CB_GETCURSEL) == 2, "failed guide save retains disk, live mode and retry draft");
    Send(shell, WM_COMMAND, 7101); CloseSettings(shell, IDCANCEL);
    Require(guide().empty(), "hidden guide clears both canvas text and accessible text");
    app.Stop(); app.Start(executable, workspace); Require(guide().empty(), "hidden guide survives restart");
    shell = OpenSettings(app.main); Send(DescendantId(shell, 4226), CB_SETCURSEL, 0);
    Send(shell, WM_COMMAND, 7101);
    Page(shell, 12);
    Require(PostMessageW(DescendantId(shell, 4112), BM_CLICK, 0, 0), "open actual annotation shortcut editor");
    HWND editor = WaitWindow(L"AnnotShortcutEditorDlgClass");
    const auto originalKey = WindowText(GetDlgItem(editor, 5218));
    HWND keyChoice = GetDlgItem(editor, 5202);
    Require((GetWindowLongPtrW(keyChoice, GWL_STYLE) & 3) == CBS_DROPDOWNLIST,
        "main key is selected rather than entered as shortcut syntax");
    auto selectKey = [&](UINT vk, bool ctrl, bool alt, bool shift) {
        const auto count = Send(keyChoice, CB_GETCOUNT);
        Require(count > 0 && count < 256, "bounded shortcut key catalog");
        int found = -1;
        for (DWORD_PTR i = 0; i < count; ++i)
            if (Send(keyChoice, CB_GETITEMDATA, i) == vk) { found = static_cast<int>(i); break; }
        Require(found >= 0, "main key catalog contains requested virtual key");
        Send(keyChoice, CB_SETCURSEL, found);
        Send(GetDlgItem(editor, 5215), BM_SETCHECK, ctrl ? BST_CHECKED : BST_UNCHECKED);
        Send(GetDlgItem(editor, 5217), BM_SETCHECK, alt ? BST_CHECKED : BST_UNCHECKED);
        Send(GetDlgItem(editor, 5216), BM_SETCHECK, shift ? BST_CHECKED : BST_UNCHECKED);
        Send(editor, WM_COMMAND, MAKEWPARAM(5202, CBN_SELCHANGE), reinterpret_cast<LPARAM>(keyChoice));
    };
    selectKey('S', true, false, false);
    Require(WindowText(GetDlgItem(editor, 5218)) == L"Ctrl+S" && !IsWindowEnabled(GetDlgItem(editor, IDOK)),
        "reserved save key is previewed and rejected inline");
    const auto reservedText = WindowText(GetDlgItem(editor, 5219));
    selectKey('2', true, false, false);
    Require(!IsWindowEnabled(GetDlgItem(editor, IDOK)) && WindowText(GetDlgItem(editor, 5219)) != reservedText,
        "duplicate key identifies a different conflict inline");
    selectKey('G', true, false, false);
    Require(IsWindowEnabled(GetDlgItem(editor, IDOK)), "common action overrides remain explicit and assignable");
    const auto overrideText = WindowText(GetDlgItem(editor, 5219));
    selectKey(VK_F12, true, true, false);
    Require(WindowText(GetDlgItem(editor, 5218)) == L"Ctrl+Alt+F12" &&
        WindowText(GetDlgItem(editor, 5219)) != overrideText, "selectors compose a valid chord and clear override warning");
    // Posted keys pass through the actual modal message loop, not just WndProc.
    // No physical input or global keyboard state is modified by this probe.
    Send(GetDlgItem(editor, 5220), BM_CLICK);
    Require(!IsWindowEnabled(GetDlgItem(editor, IDOK)), "recording suspends dialog execution");
    Require(PostMessageW(GetDlgItem(editor, 5220), WM_KEYDOWN, VK_ESCAPE, 1) &&
        PostMessageW(GetDlgItem(editor, 5220), WM_KEYUP, VK_ESCAPE, (1LL << 31) | (1LL << 30) | 1), "cancel key recording through modal loop");
    Require(Until([&] { return IsWindow(editor) && IsWindowEnabled(GetDlgItem(editor, IDOK)); }) &&
        WindowText(GetDlgItem(editor, 5218)) == L"Ctrl+Alt+F12", "Esc cancels recording only and retains selector draft");
    Send(GetDlgItem(editor, 5220), BM_CLICK);
    const auto recordingText = WindowText(GetDlgItem(editor, 5219));
    Require(PostMessageW(GetDlgItem(editor, 5220), WM_KEYDOWN, VK_RETURN, 1) &&
        PostMessageW(GetDlgItem(editor, 5220), WM_KEYUP, VK_RETURN, (1LL << 31) | (1LL << 30) | 1), "record unsupported Enter");
    Require(Until([&] { return WindowText(GetDlgItem(editor, 5219)) != recordingText; }) && IsWindow(editor) &&
        !IsWindowEnabled(GetDlgItem(editor, IDOK)), "recorded Enter cannot execute the save/default button");
    Send(GetDlgItem(editor, 5220), BM_CLICK);
    const auto shortcutFile = workspace / L"__pdf_note_workspace__" / L"__settings__" / L"tool_shortcuts.json";
    const bool hadShortcutFile = fs::exists(shortcutFile);
    const auto savedShortcuts = hadShortcutFile ? Read(shortcutFile) : std::string{};
    Require(PostMessageW(editor, WM_COMMAND, IDCANCEL, 0), "discard shortcut selector draft");
    Require(Until([&] { return !IsWindow(editor); }) && fs::exists(shortcutFile) == hadShortcutFile &&
        (!hadShortcutFile || Equals(shortcutFile, savedShortcuts)), "cancel leaves shortcut file unchanged");
    Require(PostMessageW(DescendantId(shell, 4112), BM_CLICK, 0, 0), "reopen discarded shortcut editor");
    editor = WaitWindow(L"AnnotShortcutEditorDlgClass");
    keyChoice = GetDlgItem(editor, 5202);
    Require(WindowText(GetDlgItem(editor, 5218)) == originalKey, "cancel preserves the runtime binding");
    selectKey(VK_F12, true, true, false);
    Require(PostMessageW(editor, WM_COMMAND, IDOK, 0), "save custom annotation key");
    Require(Until([&] { return !IsWindow(editor); }), "custom key accepted through real editor");
    CloseSettings(shell, IDCANCEL);
    Require(guide().find(L"Ctrl+Alt+F12") != std::wstring::npos && guide().find(originalKey + L"  ") == std::wstring::npos,
        "guide reflects the accepted user binding and removes the previous chord");
    Require(SetWindowPos(app.main, nullptr, 0, 0, 920, 640, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), "compact guide viewport");
    HWND pdf = view(); SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_ALL;
    Require(GetScrollInfo(pdf, SB_VERT, &info) && info.nPage > 0 && info.nMax >= static_cast<int>(info.nPage),
        "compact guide exposes overflow rather than dropping shortcuts");
    Send(pdf, WM_VSCROLL, SB_BOTTOM); GetScrollInfo(pdf, SB_VERT, &info);
    Require(info.nPos == info.nMax-static_cast<int>(info.nPage)+1, "scrollbar reaches the last guide row");
    Send(pdf, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA));
    SCROLLINFO after{}; after.cbSize = sizeof(after); after.fMask = SIF_ALL;
    GetScrollInfo(pdf, SB_VERT, &after);
    Require(after.nPos < info.nPos, "wheel scrolls the empty guide");
    Send(pdf, WM_KEYDOWN, VK_HOME); GetScrollInfo(pdf, SB_VERT, &after);
    Require(after.nPos == 0, "Home returns to the first guide row");
    app.Stop(); app.Start(executable, workspace);
    Require(guide().find(L"Ctrl+Alt+F12") != std::wstring::npos, "custom guide binding survives restart");
    const auto source = workspace / L"講義 空白" / L"第01回" / L"pdf" / L"guide.pdf";
    fs::create_directories(source.parent_path()); Write(source, Pdf());
    HWND documentPane = view();
    SelectSession(app.main);
    Require(Until([&] { return WindowText(documentPane).empty(); }), "opening a PDF invalidates and removes the guide");
    Require(Read(source) == Pdf(), "guide and navigation preserve PDF bytes");
    app.Stop();
    Write(config, "{\"classesDir\":\".\",\"restoreLastSession\":false,\"emptyViewGuideMode\":99}");
    app.Start(executable, workspace);
    Require(guide().find(L"1.") != std::wstring::npos, "unknown guide preference falls back to full guide");
    app.Stop();
    const auto noteWorkspace = workspace.parent_path() / L"central_guide_note_only";
    Seed(noteWorkspace, false);
    const auto note = noteWorkspace / L"講義 空白" / L"第01回" / L"note" / L"guide.clro";
    fs::create_directories(note.parent_path()); Write(note, "guide note fixture");
    app.Start(executable, noteWorkspace); documentPane = view();
    Require(!WindowText(documentPane).empty(), "note-only workspace starts with the guide before selection");
    SelectSession(app.main);
    Require(Until([&] { return WindowText(documentPane).empty(); }), "opening only a note also hides and repaints the central guide");
    Require(Read(note) == "guide note fixture", "guide transition preserves note bytes");
    app.Stop();
    std::cout << "[PASS] central guide / modes / rollback / custom bindings / scrolling / restart / PDF transition\n";
}

static void MarkerDisplayDefaultsMatrix(const fs::path& executable, const fs::path& workspace) {
    Seed(workspace,false);
    const auto oldFile=workspace/L"legacy_marker.clrop";
    const std::string oldBytes=R"({"version":1,"pdf_id":{"size":1},"pages":[]})";
    Write(oldFile,oldBytes);
    const auto config=workspace/L"workspace.json";
    OwnedApp app;app.Start(executable,workspace);
    HWND shell=OpenSettings(app.main);Page(shell,3);
    const auto choose=[&](HWND settings,int index) {
        HWND combo=DescendantId(settings,1039);
        Require(combo && Send(combo,CB_GETCOUNT)==2,"marker default exposes exactly two display methods");
        Send(combo,CB_SETCURSEL,index);
        Send(GetParent(combo),WM_COMMAND,MAKEWPARAM(1039,CBN_SELCHANGE),reinterpret_cast<LPARAM>(combo));
    };
    Require(Send(DescendantId(shell,1039),CB_GETCURSEL)==0,"new markers default to Multiply");
    const auto before=Read(config);
    choose(shell,1);CloseSettings(shell,IDCANCEL);
    Require(Equals(config,before),"cancel preserves the marker default on disk");
    shell=OpenSettings(app.main);Page(shell,3);
    Require(Send(DescendantId(shell,1039),CB_GETCURSEL)==0,"cancel restores the new-marker default");
    choose(shell,1);Page(shell,0);
    HANDLE lock=CreateFileW(config.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    Require(lock!=INVALID_HANDLE_VALUE,"hold marker-default settings against replacement");
    Send(shell,WM_COMMAND,7101);CloseHandle(lock);
    Require(IsWindow(shell) && Equals(config,before),"failed marker-default save preserves the file and retryable draft");
    CloseSettings(shell,IDCANCEL);
    shell=OpenSettings(app.main);Page(shell,3);
    Require(Send(DescendantId(shell,1039),CB_GETCURSEL)==0,"failed-save cancellation restores runtime default");
    choose(shell,1);CloseSettings(shell,IDOK);app.Stop();
    app.Start(executable,workspace);shell=OpenSettings(app.main);Page(shell,3);
    Require(Send(DescendantId(shell,1039),CB_GETCURSEL)==1 && Read(config).find("\"newMarkerDisplayMode\": \"legacy\"")!=std::string::npos,
        "Legacy creation default survives restart");
    choose(shell,0);CloseSettings(shell,IDOK);app.Stop();
    app.Start(executable,workspace);shell=OpenSettings(app.main);Page(shell,3);
    Require(Send(DescendantId(shell,1039),CB_GETCURSEL)==0 && Equals(oldFile,oldBytes),
        "Multiply default survives restart without rewriting old annotations");
    CloseSettings(shell,IDCANCEL);app.Stop();
    std::cout<<"[PASS] marker display defaults / cancel / failed save / restart / old data preserved\n";
}

static void RegroupedSettingsMatrix(const fs::path& executable, const fs::path& workspace) {
    MarkerDisplayDefaultsMatrix(executable,workspace.parent_path()/(workspace.filename().wstring()+L"_marker_defaults"));
    CentralGuideMatrix(executable, workspace.parent_path() / (workspace.filename().wstring() + L"_central_guide"));
    Seed(workspace, false);
    OwnedApp app; app.Start(executable, workspace);
    HWND shell = OpenSettings(app.main), tree = GetDlgItem(shell, 7100);
    Require(tree && Send(tree, TVM_GETCOUNT) == settings_navigation::GroupCount + settings_navigation::Count,
        "all settings groups and pages exist");
    auto group = Send(tree, TVM_GETNEXTITEM, TVGN_ROOT);
    int groups = 0, leaves = 0;
    while (group) {
        auto leaf = Send(tree, TVM_GETNEXTITEM, TVGN_CHILD, static_cast<LPARAM>(group));
        Require(leaf != 0, "each settings group has an actual editing page");
        while (leaf) { ++leaves; leaf = Send(tree, TVM_GETNEXTITEM, TVGN_NEXT, static_cast<LPARAM>(leaf)); }
        ++groups; group = Send(tree, TVM_GETNEXTITEM, TVGN_NEXT, static_cast<LPARAM>(group));
    }
    Require(groups == settings_navigation::GroupCount && leaves == settings_navigation::Count,
        "sidebar grouping has no extra empty or missing entries");
    const std::array<int, settings_navigation::Count> ids = {4223,4114,4201,1013,5224,4224,4124,4230,4214,4211,4222,4207,4242,4241,4239,4107};
    for (int page = 0; page < settings_navigation::Count; ++page) {
        Page(shell, page);
        struct VisibleControl { int id; bool found = false; } visible{ids[page]};
        EnumChildWindows(shell, [](HWND control, LPARAM param) -> BOOL {
            auto& expected = *reinterpret_cast<VisibleControl*>(param);
            if (GetDlgCtrlID(control) == expected.id && IsWindowVisible(control)) { expected.found = true; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&visible));
        Require(visible.found, "settings page visibly exposes its owned control");
    }
    Page(shell, 9);
    const auto logLocation = DescendantId(shell, 4243);
    Require(logLocation && PostMessageW(logLocation, BM_CLICK, 0, 0), "review log location using the shared path dialog");
    HWND pathDialog = WaitWindow(L"SilentDialogClass");
    const auto pathText = WindowText(GetDlgItem(pathDialog, 5101));
    const bool compactLogPath = pathText.find(L"__log__") != std::wstring::npos &&
        pathText.find(workspace.wstring()) == std::wstring::npos;
    const bool settingsOwner = GetWindow(pathDialog, GW_OWNER) == shell;
    if (!compactLogPath || !settingsOwner) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, pathText.data(), static_cast<int>(pathText.size()), nullptr, 0, nullptr, nullptr);
        std::string text(size > 0 ? size : 0, '\0');
        if (size > 0 && !WideCharToMultiByte(CP_UTF8, 0, pathText.data(), static_cast<int>(pathText.size()), text.data(), size, nullptr, nullptr))
            text = "<UTF-8 conversion failed>";
        std::cerr << "Diagnostic path observation: workspace=" << workspace.u8string()
                  << " dialog=" << pathDialog << " owner=" << GetWindow(pathDialog, GW_OWNER)
                  << " settings=" << shell << " text=" << text << '\n';
    }
    Require(settingsOwner, "diagnostic location dialog belongs to the requested settings window");
    Require(compactLogPath,
        "diagnostic location uses a compact display instead of an inline full path");
    // Queue the close command like real button input: a cross-process Send can
    // destroy the dialog while its modal GetMessage still waits for a message.
    Require(PostMessageW(pathDialog, WM_COMMAND, 5102, 0), "queue log location dialog close");
    Require(Until([&] { return !IsWindow(pathDialog) && IsWindowEnabled(shell); }), "log location dialog restores its settings owner");
    Page(shell, 4);
    RECT hex{}, eye{};
    Require(GetWindowRect(DescendantId(shell, 5224), &hex) && GetWindowRect(DescendantId(shell, 5227), &eye), "palette color editor rectangles exist");
    Require(hex.right <= eye.left || hex.bottom <= eye.top, "hex field and screen-color button do not overlap");
    Require(PostMessageW(shell, WM_CLOSE, 0, 0), "close settings after navigation without edits");
    Require(Until([&] { return !IsWindow(shell) || Window(L"SilentDialogClass"); }) && !IsWindow(shell),
        "visiting all settings pages does not manufacture an unsaved draft");
    shell = OpenSettings(app.main); tree = GetDlgItem(shell, 7100);
    auto root = Send(tree, TVM_GETNEXTITEM, TVGN_ROOT);
    for (int groupIndex = 0; groupIndex < settings_navigation::GroupCount; ++groupIndex) {
        Send(tree, TVM_SELECTITEM, TVGN_CARET, static_cast<LPARAM>(root));
        Require(Send(tree, TVM_GETNEXTITEM, TVGN_CARET) == Send(tree, TVM_GETNEXTITEM, TVGN_CHILD, static_cast<LPARAM>(root)),
            "selecting a group opens and selects its first editing page");
        Page(shell, settings_navigation::firstPages[groupIndex]);
        root = Send(tree, TVM_GETNEXTITEM, TVGN_NEXT, static_cast<LPARAM>(root));
    }
    Page(shell, 1);
    Send(shell, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(tree), TRUE);
    Require(PostMessageW(tree, WM_KEYDOWN, VK_TAB, 0), "Tab from settings navigation");
    Require(Until([&] {
        GUITHREADINFO info{}; info.cbSize = sizeof(info);
        return GetGUIThreadInfo(GetWindowThreadProcessId(shell, nullptr), &info) && info.hwndFocus &&
            info.hwndFocus != tree && IsChild(shell, info.hwndFocus) &&
            GetParent(info.hwndFocus) != shell;
    }), "Tab reaches the editor through embedded page containers");
    Page(shell, 7); const auto originalColor = WindowText(DescendantId(shell, 4235));
    Page(shell, 14); const auto originalPlaceholder = WindowText(DescendantId(shell, 4239));
    Send(DescendantId(shell, 4238), BM_CLICK);
    Type(DescendantId(shell, 4239), L"formula");
    Page(shell, 7); Type(DescendantId(shell, 4235), L"#123456");
    Page(shell, 14); Require(WindowText(DescendantId(shell, 4239)) == L"formula", "hidden page keeps text draft");
    Send(shell, WM_COMMAND, 7102);
    Require(!IsWindowEnabled(app.main), "reverting an embedded page keeps the main window modal");
    Require(WindowText(DescendantId(shell, 4239)) == originalPlaceholder, "revert discards only current quick-note draft");
    Page(shell, 7); Require(WindowText(DescendantId(shell, 4235)) == L"#123456", "revert retains quick-PDF draft");
    Require(PostMessageW(shell, WM_CLOSE, 0, 0), "close edited settings");
    HWND prompt = WaitWindow(L"SilentDialogClass"); Send(prompt, WM_COMMAND, 5104);
    Require(Until([&] { return !IsWindow(prompt) && IsWindowEnabled(shell); }), "continue editing keeps dialog and draft");
    Require(WindowText(DescendantId(shell, 4235)) == L"#123456", "continue editing retains text");
    const auto config = workspace / L"workspace.json"; const auto before = Read(config);
    Type(DescendantId(shell, 4235), L"invalid"); Page(shell, 0); Send(DescendantId(shell, 4223), BM_SETCHECK, BST_UNCHECKED);
    Send(shell, WM_COMMAND, 7101);
    Require(IsWindow(shell) && Equals(config, before), "invalid quick background prevents earlier settings writes");
    Type(DescendantId(shell, 4235), L"#123456");
    Require(PostMessageW(shell, WM_CLOSE, 0, 0), "close valid dirty settings");
    prompt = WaitWindow(L"SilentDialogClass"); Send(prompt, WM_COMMAND, 5102);
    Require(Until([&] { return !IsWindow(shell); }), "save-and-close choice saves all drafts");
    app.Stop(); app.Start(executable, workspace); shell = OpenSettings(app.main);
    Page(shell, 7); Require(WindowText(DescendantId(shell, 4235)) == L"#123456", "quick-PDF settings survive restart");
    Type(DescendantId(shell, 4235), originalColor.c_str()); CloseSettings(shell, IDCANCEL);
    shell = OpenSettings(app.main); Page(shell, 7);
    Require(WindowText(DescendantId(shell, 4235)) == L"#123456", "discard leaves saved quick-PDF value intact");
    Page(shell, 14); Send(DescendantId(shell, 4238), BM_CLICK);
    Type(DescendantId(shell, 4239), L"equation"); CloseSettings(shell, IDOK);
    app.Stop(); app.Start(executable, workspace); shell = OpenSettings(app.main); Page(shell, 14);
    Require(WindowText(DescendantId(shell, 4239)) == L"equation" && Send(DescendantId(shell, 4238), BM_GETCHECK) == BST_CHECKED,
        "quick-note settings survive restart independently of quick-PDF");
    Page(shell, 3);
    const auto selectTool = [&](HWND settings, int index) {
        HWND page = Descendant(settings, L"AnnotSettingsDlgClass");
        HWND list = DescendantId(page, 1014);
        Require(Send(list, LB_GETCOUNT) > static_cast<DWORD_PTR>(index), "annotation tool row exists");
        Send(list, LB_SETCURSEL, index);
        Send(page, WM_COMMAND, MAKEWPARAM(1014, LBN_SELCHANGE), reinterpret_cast<LPARAM>(list));
        return page;
    };
    HWND annotation = selectTool(shell, 3);
    HWND state = DescendantId(annotation, 1017);
    const auto originalState = Send(state, CB_GETCURSEL);
    Send(state, CB_SETCURSEL, 1);
    Send(annotation, WM_COMMAND, MAKEWPARAM(1017, CBN_SELCHANGE), reinterpret_cast<LPARAM>(state));
    Send(annotation, WM_COMMAND, 1016); // Move this tool down one row.
    Page(shell, 0); CloseSettings(shell, IDOK); app.Stop();
    app.Start(executable, workspace); shell = OpenSettings(app.main); Page(shell, 3);
    annotation = selectTool(shell, 4); state = DescendantId(annotation, 1017);
    Require(Send(state, CB_GETCURSEL) == 1, "annotation tool state and order survive navigation, saving and restart");
    Send(annotation, WM_COMMAND, 1015); // Restore the fixture's original order and state.
    Send(state, CB_SETCURSEL, originalState);
    Send(annotation, WM_COMMAND, MAKEWPARAM(1017, CBN_SELCHANGE), reinterpret_cast<LPARAM>(state));
    CloseSettings(shell, IDOK); app.Stop();
    std::cout << "[PASS] six-group navigation / drafts / close choices / quick preferences / validation / restart\n";
}

static void OpenPreferencesMatrix(const fs::path& executable, const fs::path& workspace) {
    Seed(workspace, false);
    OwnedApp app; app.Start(executable, workspace);
    HWND settings = OpenSettings(app.main);
    const auto expect = [&](HWND shell, int pdf, int note, bool resume) {
        Page(shell, 5);
        Require(Send(DescendantId(shell, 4224), CB_GETCURSEL) == static_cast<DWORD_PTR>(pdf), "PDF open preference survives the real settings UI");
        Page(shell, 0);
        Require((Send(DescendantId(shell, 4223), BM_GETCHECK) == BST_CHECKED) == resume, "startup preference survives the real settings UI");
        Page(shell, 10);
        Require(Send(DescendantId(shell, 4225), CB_GETCURSEL) == static_cast<DWORD_PTR>(note), "new note format survives the real settings UI");
    };
    const auto change = [&](HWND shell) {
        Page(shell, 5);
        Send(DescendantId(shell, 4224), CB_SETCURSEL, 1);
        Page(shell, 0); Send(DescendantId(shell, 4223), BM_SETCHECK, BST_UNCHECKED);
        Page(shell, 10); Send(DescendantId(shell, 4225), CB_SETCURSEL, 1);
    };
    expect(settings, 0, 0, true);
    change(settings); CloseSettings(settings, IDCANCEL);
    settings = OpenSettings(app.main); expect(settings, 0, 0, true);
    change(settings); Page(settings, 10); Send(settings, WM_COMMAND, 7102);
    expect(settings, 1, 0, false); // Reverting Note leaves the other draft intact.
    change(settings); CloseSettings(settings, IDOK); app.Stop();
    app.Start(executable, workspace);
    const auto lists = Lists(app.main);
    Require(static_cast<int>(Send(lists[0], LB_GETCURSEL)) == -1 && Send(lists[3], LB_GETCOUNT) == 0,
        "workspace-only startup opens no previous course, session or note");
    settings = OpenSettings(app.main); expect(settings, 1, 1, false); CloseSettings(settings, IDCANCEL);
    SelectSession(app.main);
    Require(PostMessageW(app.main, WM_COMMAND, ID_FILE_NEW_CLRO, 0), "create a note using the saved default");
    const auto noteDir = workspace / L"講義 空白" / L"第01回" / L"note";
    Require(Until([&] {
        if (!fs::is_directory(noteDir)) return false;
        for (const auto& entry : fs::directory_iterator(noteDir)) if (entry.path().extension() == L".md") return true;
        return false;
    }), "normal creation uses the persisted Markdown extension");
    settings = OpenSettings(app.main); Page(settings, 0);
    Send(DescendantId(settings, 4223), BM_SETCHECK, BST_CHECKED); CloseSettings(settings, IDOK); app.Stop();
    app.Start(executable, workspace);
    Require(Until([&] { return Send(Lists(app.main)[3], LB_GETCOUNT) == 1; }),
        "reenabling resume retains and restores the last session");
    app.Stop();
    Write(workspace / L"workspace.json", "{\"classesDir\":\".\",\"restoreLastSession\":false,\"newNoteExtension\":\".exe\",\"pdfOpenViewMode\":\"invalid\"}");
    app.Start(executable, workspace);
    settings = OpenSettings(app.main); expect(settings, 0, 0, false); CloseSettings(settings, IDCANCEL); app.Stop();
    std::cout << "[PASS] opening preferences / cancel / revert / restart / actual note creation / invalid values\n";
}
static void OverlongFontNameStartup(const fs::path& executable, const fs::path& workspace) {
    Seed(workspace, false);
    const std::string longFace(32, 'X'); // LOGFONTW::lfFaceName includes the terminator.
    Write(workspace / L"workspace.json", "{\"classesDir\":\".\",\"startupSelectFirstSession\":false,"
        "\"noteFontName\":\"" + longFace + "\",\"noteRenderFontName\":\"" + longFace +
        "\",\"noteRenderJpFontName\":\"" + longFace + "\"}");
    OwnedApp app;
    app.Start(executable, workspace);
    app.Stop();
    std::cout << "[PASS] overlong configured note font starts and exits normally\n";
}
[[nodiscard]] static std::vector<fs::path> Manifests(const fs::path& workspace) {
    const auto directory = workspace / L"__pdf_note_workspace__" / L"__escape__" / L"operation_transactions";
    std::vector<fs::path> found;
    if (fs::exists(directory)) for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        if (entry.path().filename() == L"manifest.tsv") found.push_back(entry.path());
    }
    return found;
}
static void CrashOrganization(const fs::path& executable, const fs::path& workspace, const wchar_t* point, bool flat,
                               bool external = false) {
    const auto lecture = external
        ? workspace.parent_path() / (workspace.filename().wstring() + L"_external")
        : workspace / L"講義 空白";
    // Create the owned input in place. A preparatory rename is not part of
    // the application's organization or crash/recovery behavior under test.
    Seed(workspace, flat, lecture);
    const auto roots = external ? std::vector<fs::path>{lecture} : std::vector<fs::path>{};
    const auto session = lecture / L"第01回";
    const auto source = (flat ? session / L"pdf" : session) / L"bundle.pdf";
    const auto destination = (flat ? session : session / L"pdf") / L"bundle.pdf";
    auto sourceClrop = source; sourceClrop.replace_extension(L".clrop");
    auto destClrop = destination; destClrop.replace_extension(L".clrop");
    auto sourceHistory = source; sourceHistory.replace_extension(L".annot_history.json");
    auto destHistory = destination; destHistory.replace_extension(L".annot_history.json");
    const auto clrop = AnnotationFixture(source); Write(source, Pdf()); Write(sourceClrop, clrop); Write(sourceHistory, "owned history evidence");
    {
        CrashGate gate(point); OwnedApp app; app.Start(executable, workspace, roots); SelectSession(app.main);
        gate.Arm(); Require(PostMessageW(app.main, WM_COMMAND, ID_FILE_ORGANIZE_SESSION_FILES, 0), "start real organization checkpoint case"); gate.Wait();
        const auto manifests = Manifests(workspace); Require(manifests.size() == 1, "checkpoint has exactly one durable transaction");
        const bool before = wcscmp(point, L"operation_before_moves") == 0;
        Require(Equals(before ? source : destination, Pdf()), "PDF is at the expected physical checkpoint, not just a timer estimate");
        app.Stop(true);
    }
    OwnedApp recovered; recovered.Start(executable, workspace, roots); SelectSession(recovered.main);
    const bool committed = wcscmp(point, L"operation_committed") == 0;
    const auto keptPdf = committed ? destination : source;
    const auto keptClrop = committed ? destClrop : sourceClrop;
    const auto keptHistory = committed ? destHistory : sourceHistory;
    Require(Until([&] { return Equals(keptPdf, Pdf()) && Equals(keptHistory, "owned history evidence"); }), "startup keeps committed bundles or restores incomplete bundles");
    CheckAnnotation(keptClrop);
    if (!committed) Require(Equals(keptClrop, clrop), "incomplete transaction restores exact original annotation bytes");
    Require(!fs::exists(committed ? source : destination) && !fs::exists(committed ? sourceClrop : destClrop) &&
        !fs::exists(committed ? sourceHistory : destHistory), "restart leaves no split or duplicate bundle members");
    const auto manifests = Manifests(workspace); Require(manifests.size() == 1, "restart retains recovery evidence");
    Require(Read(manifests[0]).find(committed ? "state=committed" : "state=recovered") != std::string::npos,
            "restarted application records the verified terminal recovery state");
    if (external) Require(!fs::exists(lecture / L"__pdf_note_workspace__"), "external recovery evidence stays in workspace");
    recovered.Stop(); std::wcout << (external ? L"[PASS] external real kill/restart " : L"[PASS] real kill/restart ")
        << point << L" / " << (flat ? L"root" : L"separate") << L'\n';
}

static void BrowserSelect(HWND browser, const std::wstring& label, bool open) {
    HWND list = GetDlgItem(browser, 102);
    const auto index = Send(list, LB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(label.c_str()));
    if (index == static_cast<DWORD_PTR>(LB_ERR)) {
        std::wcerr << L"Missing browser entry: " << label << L"\n";
        const int count = static_cast<int>(Send(list, LB_GETCOUNT));
        for (int row = 0; row < count; ++row) {
            const auto length = Send(list, LB_GETTEXTLEN, row);
            Require(length < 32768, "bounded browser row diagnostics");
            std::wstring text(length + 1, L'\0');
            text.resize(Send(list, LB_GETTEXT, row, reinterpret_cast<LPARAM>(text.data())));
            std::wcerr << text << L'\n';
        }
    }
    Require(index != static_cast<DWORD_PTR>(LB_ERR), "owned browser entry was found");
    if (GetWindowLongPtrW(list, GWL_STYLE) & (LBS_EXTENDEDSEL | LBS_MULTIPLESEL)) {
        Require(Send(list, LB_SETSEL, FALSE, -1) != static_cast<DWORD_PTR>(LB_ERR) &&
            Send(list, LB_SETSEL, TRUE, index) != static_cast<DWORD_PTR>(LB_ERR) &&
            Send(list, LB_SETCARETINDEX, index) != static_cast<DWORD_PTR>(LB_ERR), "select owned multiple-selection browser row");
    } else {
        Require(Send(list, LB_SETCURSEL, index) == index, "select owned single-selection browser row");
    }
    if (open) Send(browser, WM_COMMAND, MAKEWPARAM(102, LBN_DBLCLK), reinterpret_cast<LPARAM>(list));
}
static void BrowserFolder(HWND browser, const fs::path& folder) {
    Send(browser, WM_COMMAND, 202);
    BrowserSelect(browser, L"[DRV] " + folder.root_path().wstring(), true);
    for (const auto& component : folder.relative_path()) BrowserSelect(browser, L"[DIR] " + component.wstring(), true);
}
static void Import(HWND main, const fs::path& source, const fs::path& expected) {
    Require(PostMessageW(main, WM_COMMAND, ID_FILE_IMPORT_FILE, 0), "start real menu import");
    HWND browser = WaitWindow(L"LocalPathBrowserDlg"); BrowserFolder(browser, source.parent_path());
    BrowserSelect(browser, L"      " + source.filename().wstring(), false); Send(browser, WM_COMMAND, IDOK);
    Require(Until([&] { return !IsWindow(browser) && IsWindowEnabled(main) && fs::exists(expected); }), "actual menu import creates the selected layout destination");
    Send(main, WM_NULL); Require(Equals(expected, Read(source)), "import preserves selected source bytes");
}
static void ImportRestartExport(const fs::path& executable, const fs::path& workspace, bool flat) {
    Seed(workspace, flat); const auto session = workspace / L"講義 空白" / L"第01回";
    const auto sourceDir = workspace.parent_path() / L"外部 読込元";
    const auto pdf = sourceDir / L"資料.pdf", note = sourceDir / L"ノート.clro", annotation = sourceDir / L"資料.clrop";
    Write(pdf, Pdf()); Write(note, "original imported note"); Write(annotation, AnnotationFixture(pdf));
    const auto pdfDir = flat ? session : session / L"pdf", noteDir = flat ? session : session / L"note";
    OwnedApp app; app.Start(executable, workspace); SelectSession(app.main);
    Import(app.main, pdf, pdfDir / pdf.filename()); Import(app.main, annotation, pdfDir / annotation.filename()); Import(app.main, note, noteDir / note.filename());
    auto lists = Lists(app.main); Require(Send(lists[2], LB_GETCOUNT) == 1 && Send(lists[3], LB_GETCOUNT) == 1, "both imported file types are available through normal navigation");
    Select(app.main, lists[3], 0); HWND editor = Descendant(app.main, L"RICHEDIT50W");
    if (!editor) editor = Descendant(app.main, L"RichEdit20W");
    Require(editor && WindowText(editor).find(L"original imported note") != std::wstring::npos, "actual editor reads the imported note");
    Type(editor, L"edited workflow note"); Send(app.main, WM_COMMAND, ID_FILE_SAVE_NOTE);
    Require(Until([&] { return Contains(noteDir / note.filename(), "edited workflow note"); }), "note edits integrate through real save action");
    Select(app.main, lists[2], 0);
    // Switch placement and run organization, not merely its already-correct no-op.
    HWND settings = OpenSettings(app.main); Page(settings, 8); Send(DescendantId(settings, 4214), CB_SETCURSEL, flat ? 0 : 1); CloseSettings(settings, IDOK);
    Require(PostMessageW(app.main, WM_COMMAND, ID_FILE_ORGANIZE_SESSION_FILES, 0), "organize imported and edited files into the other placement");
    const auto movedPdf = (flat ? session / L"pdf" : session) / pdf.filename();
    const auto movedNote = (flat ? session / L"note" : session) / note.filename();
    auto movedClrop = movedPdf; movedClrop.replace_extension(L".clrop");
    Require(Until([&] { return fs::exists(movedPdf) && fs::exists(movedNote) && fs::exists(movedClrop); }), "real import/edit/organization workflow completes");
    app.Stop(); app.Start(executable, workspace); SelectSession(app.main); lists = Lists(app.main);
    Select(app.main, lists[3], 0); editor = Descendant(app.main, L"RICHEDIT50W");
    if (!editor) editor = Descendant(app.main, L"RichEdit20W");
    Require(WindowText(editor).find(L"edited workflow note") != std::wstring::npos, "restart reloads the integrated note edit");
    Select(app.main, lists[2], 0); CheckAnnotation(movedClrop);
    Require(Equals(movedPdf, Pdf()) && Equals(pdf, Pdf()) && Equals(note, "original imported note") && Equals(annotation, AnnotationFixture(pdf)),
            "workflow preserves original PDF and all external import sources");
    // Use the real output dialog and its self-owned browser to select a fixture
    // directory. No default desktop/downloads output or user's save destination.
    const auto outputDir = workspace.parent_path() / L"出力 空白"; fs::create_directories(outputDir);
    Require(PostMessageW(app.main, WM_COMMAND, ID_FILE_EXPORT_PDF, 0), "open actual PDF output dialog after restart");
    HWND output = WaitWindow(L"UnifiedExportDialog");
    Require(PostMessageW(output, WM_COMMAND, 4112, 0), "choose owned output folder");
    HWND browser = WaitWindow(L"LocalPathBrowserDlg"); BrowserFolder(browser, outputDir); Send(browser, WM_COMMAND, IDOK);
    Require(Until([&] { return !IsWindow(browser) && WindowText(GetDlgItem(output, 4111)) == outputDir.wstring(); }),
        "explicitly selected destination has returned through the nested browser to the output dialog");
    Type(GetDlgItem(output, 4113), L"workflow_export"); Send(output, WM_COMMAND, IDOK);
    const auto exported = outputDir / L"workflow_export.pdf";
    Require(Until([&] { return fs::exists(exported); }, 15000), "real output creates a PDF after import/edit/organization/restart");
    Require(Read(exported).rfind("%PDF-", 0) == 0 && fs::file_size(exported) > 100, "workflow output is a nonempty PDF");
    HWND results = WaitWindow(L"ExportResultsDialog"); Send(results, WM_COMMAND, 4904);
    Require(Until([&] { return !IsWindow(results) && IsWindowEnabled(app.main); }), "output completion is acknowledged before application exit");
    app.Stop(); std::cout << "[PASS] native import/edit/organize/restart/output / " << (flat ? "root" : "separate") << '\n';
}

static void ExternalFolderOperations(const fs::path& executable, const fs::path& workspace, bool flat) {
    const auto lecture = workspace.parent_path() / L"registered lecture";
    Seed(workspace, flat, lecture);
    const auto session = lecture / L"第01回";
    const auto sourcePdf = (flat ? session / L"pdf" : session) / L"bundle.pdf";
    const auto sourceNote = (flat ? session / L"note" : session) / L"bundle.clro";
    const auto pdf = (flat ? session : session / L"pdf") / sourcePdf.filename();
    const auto note = (flat ? session : session / L"note") / sourceNote.filename();
    auto sourceClrop = sourcePdf; sourceClrop.replace_extension(L".clrop");
    auto clrop = pdf; clrop.replace_extension(L".clrop");
    auto sourceHistory = sourcePdf; sourceHistory.replace_extension(L".annot_history.json");
    auto history = pdf; history.replace_extension(L".annot_history.json");
    Write(sourcePdf, Pdf()); Write(sourceNote, "paired external note");
    Write(sourceClrop, AnnotationFixture(sourcePdf)); Write(sourceHistory, "[]");
    OwnedApp app; app.Start(executable, workspace, {lecture}); SelectSession(app.main);
    Require(PostMessageW(app.main, WM_COMMAND, ID_FILE_ORGANIZE_SESSION_FILES, 0), "organize registered temporary folder through actual menu");
    Require(Until([&] { return fs::exists(pdf) && fs::exists(note) && fs::exists(clrop) && fs::exists(history); }),
        "external PDF, note, annotation and history reach configured placement");
    Require(!fs::exists(sourcePdf) && !fs::exists(sourceNote) && !fs::exists(sourceClrop) && !fs::exists(sourceHistory),
        "organization leaves no split external bundle");
    auto lists = Lists(app.main); Select(app.main, lists[2], 0);
    const auto historyBefore = Read(history);
    const auto renamedPdf = pdf.parent_path() / L"renamed.pdf";
    const auto renamedNote = note.parent_path() / L"renamed.clro";
    auto renamedClrop = renamedPdf; renamedClrop.replace_extension(L".clrop");
    auto renamedHistory = renamedPdf; renamedHistory.replace_extension(L".annot_history.json");
    Require(PostMessageW(app.main, WM_COMMAND, ID_OP_RENAME_PDF, 0), "rename external PDF through actual menu");
    HWND name = WaitWindow(L"SimpleInputDlg"); Type(GetDlgItem(name, 101), L"renamed.pdf"); Send(name, WM_COMMAND, IDOK);
    HWND follow = WaitWindow(L"SilentDialogClass");
    Require(GetDlgItem(follow, 5103) != nullptr, "paired-note confirmation offers a separate PDF-only choice");
    Send(follow, WM_COMMAND, 5102);
    Require(Until([&] { return fs::exists(renamedPdf) && fs::exists(renamedNote) && fs::exists(renamedClrop) && fs::exists(renamedHistory); }),
        "paired rename moves all external bundle members");
    Require(Equals(renamedPdf, Pdf()) && Equals(renamedNote, "paired external note") && Equals(renamedHistory, historyBefore),
        "rename preserves PDF, note and history contents");
    CheckAnnotation(renamedClrop);
    Require(Until([&] {
        const auto manifests = Manifests(workspace);
        return manifests.size() == 3 && std::all_of(manifests.begin(), manifests.end(), [&](const fs::path& path) {
            return Contains(path, "state=committed");
        });
    }), "paired rename reaches durable commit before sending the next user command");
    Require(PostMessageW(app.main, WM_COMMAND, ID_EDIT_UNDO, 0), "queue external undo after rename completion");
    Require(Until([&] { return fs::exists(pdf) && fs::exists(note) && !fs::exists(renamedPdf) && !fs::exists(renamedNote); }),
        "external paired rename can be undone through actual menu");
    Require(Until([&] {
        const UINT state = GetMenuState(GetMenu(app.main), ID_EDIT_REDO, MF_BYCOMMAND);
        return state != static_cast<UINT>(-1) && !(state & (MF_DISABLED | MF_GRAYED));
    }), "undo finishes reopening documents and publishes the Redo action");
    Require(PostMessageW(app.main, WM_COMMAND, ID_EDIT_REDO, 0), "queue external redo after undo completion");
    Require(Until([&] { return fs::exists(renamedPdf) && fs::exists(renamedNote) && !fs::exists(pdf) && !fs::exists(note); }),
        "external paired rename can be redone through actual menu");
    Require(Until([&] {
        const UINT state = GetMenuState(GetMenu(app.main), ID_EDIT_REDO, MF_BYCOMMAND);
        return state != static_cast<UINT>(-1) && (state & (MF_DISABLED | MF_GRAYED));
    }), "redo finishes reopening documents before restart");
    Require(fs::exists(workspace / L"__pdf_note_workspace__" / L"__settings__" / L"note_identity_registry.bin") &&
        !fs::exists(lecture / L"__pdf_note_workspace__"), "durable identities and operation evidence belong to workspace");
    HWND editor = Descendant(app.main, L"RICHEDIT50W");
    if (!editor) editor = Descendant(app.main, L"RichEdit20W");
    Require(editor != nullptr, "external note editor is available for protected-work check");
    Type(editor, L"protected external edit");
    HWND pdfView = Descendant(app.main, L"PdfWorkspacePdfView");
    Require(pdfView != nullptr, "external PDF view can receive focus");
    Send(pdfView, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(-5, -5));
    Send(pdfView, WM_LBUTTONUP, 0, MAKELPARAM(-5, -5));
    GUITHREADINFO focus{}; focus.cbSize = sizeof(focus);
    const bool queried = GetGUIThreadInfo(GetWindowThreadProcessId(app.main, nullptr), &focus) != FALSE;
    if (!queried || focus.hwndFocus != pdfView) {
        const HWND foreground = GetForegroundWindow(); DWORD foregroundPid = 0;
        GetWindowThreadProcessId(foreground, &foregroundPid);
        char focusClass[128]{}; GetClassNameA(focus.hwndFocus, focusClass, 128);
        std::cerr << "Undo focus prerequisite: queried=" << queried << " expected=" << pdfView
                  << " actual=" << focus.hwndFocus << " class=" << focusClass
                  << " foreground=" << foreground << " foreground_pid=" << foregroundPid
                  << " owned_main=" << app.main << " owned_pid=" << childPid << '\n';
    }
    Require(queried && focus.hwndFocus == pdfView,
        "protected-note check routes Undo from the PDF pane");
    Send(app.main, WM_COMMAND, ID_EDIT_UNDO);
    Require(fs::exists(renamedPdf) && fs::exists(renamedNote) && !fs::exists(pdf) &&
        WindowText(editor) == L"protected external edit", "file undo preserves protected note work from another pane");
    Type(editor, L"paired external note"); Send(app.main, WM_COMMAND, ID_FILE_SAVE_NOTE);
    Require(Until([&] { return Equals(renamedNote, "paired external note"); }), "restore and save the owned note after protected-work check");
    app.Stop(); app.Start(executable, workspace); SelectSession(app.main);
    lists = Lists(app.main); Select(app.main, lists[2], 0);
    CheckAnnotation(renamedClrop);
    Require(Equals(renamedPdf, Pdf()) && Equals(renamedNote, "paired external note"), "restart reopens external renamed members");
    app.Stop();
    std::cout << "[PASS] external organize / paired rename / undo / redo / restart / " << (flat ? "root" : "separate") << '\n';
}

static void CrashSettings(const fs::path& executable, const fs::path& workspace) {
    Seed(workspace, false);
    const auto palettePath = workspace / L"__pdf_note_workspace__" / L"__settings__" / L"user_palette.json";
    Write(palettePath, "{\"colors\":[\"#FF8C00\"]}");
    Draft before, changed; std::string palette, paletteAfter;
    {
        CrashGate gate(L"settings_after_workspace_write"); OwnedApp app; app.Start(executable, workspace);
        HWND settings = OpenSettings(app.main); before = Capture(settings); CloseSettings(settings, IDOK);
        palette = Read(palettePath);
        settings = OpenSettings(app.main); changed = Change(settings, before);
        Send(Descendant(settings, L"PaletteSettingsDlgClass"), WM_COMMAND, 5282);
        paletteAfter = Read(palettePath);
        Require(paletteAfter != palette, "palette change commits independently before the settings checkpoint");
        gate.Arm(); Require(PostMessageW(settings, WM_COMMAND, 7101, 0), "start multi-file settings checkpoint"); gate.Wait();
        Require(Read(workspace / L"workspace.json").find("\"sessionFileLayout\": \"session_root\"") != std::string::npos &&
            Equals(palettePath, paletteAfter),
            "checkpoint preserves the earlier palette auto-save");
        app.Stop(true);
    }
    OwnedApp restart; restart.Start(executable, workspace); HWND settings = OpenSettings(restart.main);
    // Palette auto-save is independent; the settings checkpoint committed Basic,
    // and the palette retains its independently saved value.
    Draft expected = before; expected.layout = changed.layout; expected.hex = changed.hex;
    Expect(settings, expected); Require(Equals(palettePath, paletteAfter),
        "restart preserves independently saved palette");
    CloseSettings(settings, IDCANCEL); restart.Stop(); std::cout << "[PASS] multi-file settings kill/restart / exact partial-save contract\n";
}

int main(int argc, char** argv) {
    try {
        std::cout << std::unitbuf;
        // The driver owns the job handle, not the user's processes. Closing it
        // (including when the watchdog kills this driver) kills only its apps.
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        struct JobOwner { HANDLE handle; ~JobOwner() { if (handle) CloseHandle(handle); } } jobOwner{job};
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        Require(job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
            "watchdog termination cannot leave owned app children behind");
        workflowChildJob = job;
        const DWORD length = GetEnvironmentVariableW(L"WORKSPACE_MEMO_APP_TEST_ROOT", nullptr, 0);
        Require(length > 0, "owned workflow root provided"); std::wstring path(length, L'\0');
        path.resize(GetEnvironmentVariableW(L"WORKSPACE_MEMO_APP_TEST_ROOT", path.data(), length));
        const fs::path root = fs::path(path).lexically_normal();
        const auto executable = root / L"app" / (L"pdf_note_settings_workflow_" + root.filename().wstring() + L".exe");
        const auto prefix = (fs::current_path() / L"out" / L"tests").wstring() + L"\\";
        Require(root.is_absolute() && root.wstring().compare(0, prefix.size(), prefix) == 0, "normalized workflow root stays strictly within out/tests");
        Require(SetEnvironmentVariableW(L"PDF_NOTE_SMALL_UI_AUTOMATION", nullptr) &&
            SetEnvironmentVariableW(L"PDF_NOTE_SMALL_AUTOMATION_WORKSPACE_ROOT", nullptr), "use normal startup with explicit owned setup");
        for (const auto name : {L"PDF_NOTE_SMALL_ENABLE_FAULT_INJECTION", L"PDF_NOTE_SMALL_THROW_POINT",
            L"PDF_NOTE_SMALL_PAUSE_POINT", L"PDF_NOTE_SMALL_PAUSE_ARM_EVENT", L"PDF_NOTE_SMALL_PAUSE_READY_EVENT", L"PDF_NOTE_SMALL_PAUSE_RELEASE_EVENT"}) {
            Require(SetEnvironmentVariableW(name, nullptr), "baseline children do not inherit unrelated developer fault injection");
        }
        const auto suffix = L"_workflow_" + std::to_wstring(GetCurrentProcessId());
        Require(SetEnvironmentVariableW(L"PDF_NOTE_SMALL_INSTANCE_SUFFIX", suffix.c_str()), "own workflow instance identity");
        if (argc == 2 && std::string_view(argv[1]) == "--regrouped-only") {
            RegroupedSettingsMatrix(executable, root / L"regrouped");
            std::cout << "Settings/workspace workflows: " << checks << " checks passed.\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--font-boundary-only") {
            OverlongFontNameStartup(executable, root / L"font_boundary");
            std::cout << "Settings/workspace workflows: " << checks << " checks passed.\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--external-only") {
            for (bool flat : {false, true}) {
                const auto group = root / (flat ? L"root" : L"separate");
                ExternalFolderOperations(executable, group / L"external_operations", flat);
                int index = 0;
                for (const auto point : {L"operation_before_moves", L"operation_after_first_move", L"operation_after_all_moves", L"operation_committed"})
                    CrashOrganization(executable, group / (L"external_crash_" + std::to_wstring(++index)), point, flat, true);
            }
            std::cout << "Settings/workspace workflows: " << checks << " checks passed.\n";
            return 0;
        }
        Seed(root / L"matrix", false); SettingsMatrix(executable, root / L"matrix");
        OpenPreferencesMatrix(executable, root / L"open_preferences");
        RegroupedSettingsMatrix(executable, root / L"regrouped");
        BottomPanePersistenceMatrix(executable, root / L"bottom_pane");
        PaletteDocumentRetryMatrix(executable, root / L"palette_retry");
        ActivePaletteDetachedRetry(executable, root / L"palette_detached");
        OverlongFontNameStartup(executable, root / L"font_boundary");
        CrashSettings(executable, root / L"settings_crash");
        for (bool flat : {false, true}) {
            const auto group = root / (flat ? L"root" : L"separate");
            ImportRestartExport(executable, group / L"import", flat);
            ExternalFolderOperations(executable, group / L"external_operations", flat);
            int index = 0;
            for (const auto point : {L"operation_before_moves", L"operation_after_first_move", L"operation_after_all_moves", L"operation_committed"}) {
                CrashOrganization(executable, group / (L"crash_" + std::to_wstring(++index)), point, flat);
                CrashOrganization(executable, group / (L"external_crash_" + std::to_wstring(index)), point, flat, true);
            }
        }
        std::cout << "Settings/workspace workflows: " << checks << " checks passed.\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << " checks=" << checks << '\n'; return 1; }
}
