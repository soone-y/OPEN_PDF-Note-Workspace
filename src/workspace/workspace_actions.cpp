#include "ui/core/main_window_api.h"
#include "workspace/workspace_config_io.h"
#include "core/app_core.h"
#include "core/localization.h"
#include "core/ui_prompts.h"
#include "core/ui_notify.h"
#include "core/atomic_write.h"
#include "bridge/view_bridge.h"
#include <fpdfview.h>
#include <fpdf_save.h>
#include <fpdf_edit.h>
#include "app/main_escape_backup.h"
#include "workspace/workspace_actions.h"
#include "workspace/workspace_write_lock.h"
#include <windows.h>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <array>
#include <map>
#include <atomic>
#include <cwctype>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include "office/docx_space_protection.h"

// file: main/workspace_actions.cppinc
// NOTE: Included by workspace_controller.cppinc. This fragment owns user-triggered
// workspace mutations such as create/import/open helpers.
void ShowNewLectureDialog(HWND owner) {
    // Creating and selecting a new lecture clears the current session state.
    // Do not continue if preserving the active note failed.
    if (!SaveNoteIfDirty(owner)) return;
    std::wstring name;
    if (!PromptNewLectureName(owner, name)) return;
    name = TrimWhitespace(name);
    if (name.empty()) return;
    std::wstring createdPath;
    bool ok = CreateLectureFolder(name, createdPath);
    if (!ok) {
        ShowSoftNotice(owner, GetUiText().errLectureCreate, SoftNoticeKind::Error);
        return;
    }
    // refresh lists and select
    g_currentLecturePath = createdPath;
    UpdateLectureOpenTime(createdPath);
    ResetSessionAndFiles();
    LoadLectures();
    int sel = -1;
    for (size_t i = 0; i < g_lectures.size(); ++i) {
        if (g_lectures[i] == createdPath) {
            sel = static_cast<int>(i);
            break;
        }
    }
    if (sel >= 0) {
        SendMessageW(g_hLectureList, LB_SETCURSEL, sel, 0);
    }
    if (name != std::filesystem::path(createdPath).filename().wstring()) {
        ShowSoftNotice(owner, GetUiText().errLectureExists);
    }
}

bool CreateLectureFolder(const std::wstring& baseName, std::wstring& outPath) {
    auto classesPath = WorkspaceClassesPath(g_workspaceRoot, g_config);
    std::error_code ec;
    std::filesystem::create_directories(classesPath, ec);
    if (ec) return false;
    std::wstring finalName = baseName;
    std::filesystem::path candidate = classesPath / finalName;
    int idx = 2;
    while (std::filesystem::exists(candidate, ec) && !ec) {
        finalName = baseName + L" (" + std::to_wstring(idx++) + L")";
        candidate = classesPath / finalName;
    }
    if (ec) return false;
    std::filesystem::create_directories(candidate, ec);
    if (ec) return false;
    outPath = candidate.wstring();
    return true;
}

std::wstring TodayDateForName() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[32]{};
    swprintf(buf, 32, L"%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

void PushUniqueSuggestion(std::vector<std::wstring>& suggestions, const std::wstring& value) {
    if (value.empty()) return;
    if (std::find(suggestions.begin(), suggestions.end(), value) == suggestions.end()) {
        suggestions.push_back(value);
    }
}

std::wstring ToJapaneseNumeralForName(int value) {
    if (value <= 0 || value >= 10000) return std::to_wstring(value);
    const wchar_t* digits[] = { L"", L"一", L"二", L"三", L"四", L"五", L"六", L"七", L"八", L"九" };
    const struct {
        int amount;
        const wchar_t* label;
    } units[] = {
        { 1000, L"千" },
        { 100, L"百" },
        { 10, L"十" },
    };

    std::wstring out;
    int rest = value;
    for (const auto& unit : units) {
        int n = rest / unit.amount;
        if (n > 0) {
            if (n > 1) out += digits[n];
            out += unit.label;
            rest %= unit.amount;
        }
    }
    if (rest > 0) out += digits[rest];
    return out.empty() ? L"零" : out;
}

bool ValidateCreateFileSystemName(HWND owner,
                                         const std::wstring& name,
                                         const std::wstring& title) {
    const bool invalid =
        name.empty() || name == L"." || name == L".." ||
        name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos ||
        name.back() == L'.' || name.back() == L' ';
    if (!invalid) return true;
    ShowSilentMessageDialog(
        owner, title,
        localization::Text(L"workspace.actions.19659680818f").c_str(),
        SoftNoticeKind::Warning);
    return false;
}

std::vector<std::wstring> NewSessionNameSuggestions() {
    std::vector<std::wstring> suggestions;
    const int next = NextSessionNumberForSuggestions(g_sessions, ParseSessionNumberingMode(g_config.sessionNumberingMode));
    PushUniqueSuggestion(suggestions, std::to_wstring(next));
    PushUniqueSuggestion(suggestions, L"第" + std::to_wstring(next) + L"回");
    PushUniqueSuggestion(suggestions, L"第" + ToJapaneseNumeralForName(next) + L"回");
    PushUniqueSuggestion(suggestions, ToJapaneseNumeralForName(next));
    PushUniqueSuggestion(suggestions, TodayDateForName());
    PushUniqueSuggestion(suggestions, L"補講");
    PushUniqueSuggestion(suggestions, L"試験");
    return suggestions;
}

bool IsPathDirectChildOf(const std::filesystem::path& path,
                                const std::filesystem::path& parent) {
    std::error_code ec;
    auto pathParent = std::filesystem::weakly_canonical(path.parent_path(), ec);
    if (ec) pathParent = path.parent_path();
    ec.clear();
    auto canonParent = std::filesystem::weakly_canonical(parent, ec);
    if (ec) canonParent = parent;
    return ToLowerAscii(pathParent.wstring()) == ToLowerAscii(canonParent.wstring());
}

std::optional<std::filesystem::path> PromptCreatePathWithSaveDialog(
    HWND owner,
    const std::filesystem::path& initialDir,
    const std::wstring& initialName,
    const std::wstring& title,
    const COMDLG_FILTERSPEC* filters,
    UINT filterCount,
    UINT filterIndex,
    const std::wstring& defaultExt) {
    IFileSaveDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        ShowSoftNotice(owner,
                       localization::Text(L"workspace.actions.ed35ae62796a").c_str(),
                       SoftNoticeKind::Warning);
        return std::nullopt;
    }
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOREADONLYRETURN;
        dialog->SetOptions(options);
    }
    if (!title.empty()) dialog->SetTitle(title.c_str());
    if (!initialName.empty()) dialog->SetFileName(initialName.c_str());
    if (filters && filterCount > 0) {
        dialog->SetFileTypes(filterCount, filters);
        dialog->SetFileTypeIndex(filterIndex);
    }
    if (!defaultExt.empty()) dialog->SetDefaultExtension(defaultExt.c_str());
    if (!initialDir.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initialDir.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    hr = dialog->Show(MainDialogOwner(owner));
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        dialog->Release();
        return std::nullopt;
    }
    if (FAILED(hr)) {
        dialog->Release();
        ShowSoftNotice(owner,
                       localization::Text(L"workspace.actions.edff29fbf236").c_str(),
                       SoftNoticeKind::Warning);
        return std::nullopt;
    }
    IShellItem* item = nullptr;
    std::optional<std::filesystem::path> result;
    if (SUCCEEDED(dialog->GetResult(&item)) && item) {
        PWSTR raw = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
            result = std::filesystem::path(raw);
            CoTaskMemFree(raw);
        }
        item->Release();
    }
    dialog->Release();
    return result;
}

std::optional<std::wstring> PromptSessionNameWithSaveDialog(HWND owner,
                                                                   const std::filesystem::path& lectureDir,
                                                                   const std::wstring& initialName) {
    auto picked = PromptCreatePathWithSaveDialog(
        owner,
        lectureDir,
        initialName,
        localization::Text(g_config.studentMode ? L"workspace.actions.session.create_title.student"
                                                 : L"workspace.actions.session.create_title.parent"),
        nullptr,
        0,
        0,
        L"");
    if (!picked) return std::nullopt;
    if (!IsPathDirectChildOf(*picked, lectureDir)) {
        ShowSoftNotice(owner,
                        localization::Text(g_config.studentMode ? L"workspace.actions.session.outside_folder.student"
                                                                 : L"workspace.actions.session.outside_folder.parent"),
                        SoftNoticeKind::Warning);
        return std::nullopt;
    }
    return picked->filename().wstring();
}

void ShowNewSessionDialog(HWND owner) {
    // Creating and selecting a session can replace the active note context.
    // Do not continue if preserving the active note failed.
    if (!SaveNoteIfDirty(owner)) return;
    // determine lecture
    if (g_currentLecturePath.empty()) {
        int sel = static_cast<int>(SendMessageW(g_hLectureList, LB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < static_cast<int>(g_lectures.size())) {
            g_currentLecturePath = g_lectures[static_cast<size_t>(sel)];
        }
    }
    if (g_currentLecturePath.empty()) {
        ShowSoftNotice(owner, GetUiText().errSessionNoLecture, SoftNoticeKind::Warning);
        return;
    }
    std::wstring name;
    std::vector<std::wstring> suggestions = NewSessionNameSuggestions();
    std::wstring initial = suggestions.empty() ? L"" : suggestions.front();
    PromptCreateNameResult prompt = PromptCreateName(
        owner,
        GetUiText().dlgNewSessionTitle,
        GetUiText().dlgNewSessionLabel,
        initial,
        suggestions,
        true,
        name);
    std::filesystem::path lectureDir(g_currentLecturePath);
    if (prompt == PromptCreateNameResult::Explorer) {
        auto pickedName = PromptSessionNameWithSaveDialog(owner, lectureDir, initial);
        if (!pickedName) return;
        name = *pickedName;
    } else if (prompt == PromptCreateNameResult::Create) {
        name = TrimWhitespace(name);
    } else {
        return;
    }
    if (name.empty()) return;
    if (!ValidateCreateFileSystemName(owner, name, GetUiText().dlgNewSessionTitle)) return;

    const auto& ui = GetUiText();
    std::error_code ec;

    std::filesystem::path sessionPath = lectureDir / name;
    const bool sessionExists = std::filesystem::exists(sessionPath, ec);
    if (ec) {
        ShowSoftNotice(owner, ui.errSessionCreate, SoftNoticeKind::Error);
        return;
    }
    if (sessionExists) {
        ShowSoftNotice(owner, ui.errSessionExists, SoftNoticeKind::Warning);
        return;
    }
    std::filesystem::create_directories(NoteDirectoryForSession(sessionPath), ec);
    if (ec) {
        ShowSoftNotice(owner, ui.errSessionCreate, SoftNoticeKind::Error);
        return;
    }
    ReloadSessionsAndSelect(g_currentLecturePath, name, true);
    ClearNoteEditorSilently(owner);
    RefreshStatusDisplay(owner);
}

std::filesystem::path ExeDirPath() {
    std::vector<wchar_t> exePath(512, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
        if (len > 0 && len + 1 < exePath.size()) {
            return std::filesystem::path(std::wstring(exePath.data(), len)).parent_path();
        }
        if (len == 0 || exePath.size() >= 32768) {
            std::error_code ec;
            auto cur = std::filesystem::current_path(ec);
            return ec ? std::filesystem::path{} : cur;
        }
        exePath.resize(exePath.size() * 2, L'\0');
    }
}

bool AddTempExternalLecture(HWND owner,
                                   const std::wstring& lectureDir,
                                   bool persistAndRefresh = true) {
    const auto& ui = GetUiText();
    std::filesystem::path p = std::filesystem::path(lectureDir);
    if (lectureDir.rfind(L"\\\\", 0) == 0) {
        const std::wstring msg = localization::Text(L"workspace.actions.fdb3da0b190c").c_str();
        ShowSoftNotice(owner, msg, SoftNoticeKind::Warning);
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || !std::filesystem::is_directory(p, ec)) {
        ShowSilentMessageDialog(owner, ui.menuFile, L"フォルダが見つかりません。",
                                SoftNoticeKind::Warning, {{L"", p.wstring()}});
        return false;
    }
    if (!VerifyWorkspaceWritableForEditing(owner)) return false;
    if (!VerifyDirReadableWritableForEditing(
            owner, p, g_config.studentMode ? L"workspace.directory.external.student"
                                           : L"workspace.directory.external.parent")) {
        return false;
    }

    std::wstring canon = CanonicalOrSelf(p).wstring();
    if (canon.empty()) canon = p.wstring();
    std::wstring workspaceCanon;
    if (!g_workspaceRoot.empty()) {
        workspaceCanon = CanonicalOrSelf(std::filesystem::path(g_workspaceRoot)).wstring();
        if (workspaceCanon.empty()) workspaceCanon = g_workspaceRoot;
    }
    std::wstring classesCanon;
    if (!g_workspaceRoot.empty()) {
        auto classesPath = WorkspaceClassesPath(g_workspaceRoot, g_config);
        classesCanon = CanonicalOrSelf(classesPath).wstring();
        if (classesCanon.empty()) classesCanon = classesPath.wstring();
    }

    if ((!workspaceCanon.empty() && canon == workspaceCanon) ||
        (!classesCanon.empty() && canon == classesCanon)) {
        const std::wstring msg = localization::Text(L"workspace.actions.b1780cc78837").c_str();
        ShowSoftNotice(owner, msg, SoftNoticeKind::Info);
        return false;
    }

    for (const auto& item : g_tempExternalLectures) {
        if (item.path == canon) {
            const std::wstring msg = localization::Text(
                g_config.studentMode ? L"workspace.actions.external.already_added.student"
                                    : L"workspace.actions.external.already_added.parent");
            ShowSoftNotice(owner, msg, SoftNoticeKind::Info);
            return false;
        }
    }
    for (const auto& item : g_lectures) {
        if (item == canon) {
            const std::wstring msg = localization::Text(
                g_config.studentMode ? L"workspace.actions.external.already_listed.student"
                                    : L"workspace.actions.external.already_listed.parent");
            ShowSoftNotice(owner, msg, SoftNoticeKind::Info);
            return false;
        }
    }

    g_tempExternalLectures.push_back({canon});
    if (!persistAndRefresh) return true;

    std::wstring persistError;
    if (!PersistTempExternalLecturesToSetup(&persistError)) {
        if (owner) {
            std::wstring msg = localization::Text(
                g_config.studentMode ? L"workspace.actions.external.save_failed.one.student"
                                    : L"workspace.actions.external.save_failed.one.parent");
            if (!persistError.empty() && persistError.find(L":\\") == std::wstring::npos &&
                persistError.find(L"\\\\") == std::wstring::npos) {
                msg += L"\n\n" + persistError;
            }
            ShowSilentMessageDialog(owner, GetUiText().menuAddTempExternalLecture, msg, SoftNoticeKind::Warning);
        }
    }
    s_ignoreLectureSelChange = true;
    LoadLectures();
    s_ignoreLectureSelChange = false;
    if (owner) RefreshStatusDisplay(owner);
    return true;
}

void AddTempExternalLectures(HWND owner, const std::vector<std::wstring>& lectureDirs) {
    if (lectureDirs.empty()) return;
    if (lectureDirs.size() == 1) {
        AddTempExternalLecture(owner, lectureDirs.front());
        return;
    }

    size_t added = 0;
    for (const auto& lectureDir : lectureDirs) {
        if (AddTempExternalLecture(owner, lectureDir, false)) {
            ++added;
        }
    }
    if (added == 0) return;

    std::wstring persistError;
    if (!PersistTempExternalLecturesToSetup(&persistError)) {
        if (owner) {
            std::wstring msg = localization::Text(
                g_config.studentMode ? L"workspace.actions.external.save_failed.multiple.student"
                                    : L"workspace.actions.external.save_failed.multiple.parent");
            if (!persistError.empty() && persistError.find(L":\\") == std::wstring::npos &&
                persistError.find(L"\\\\") == std::wstring::npos) {
                msg += L"\n\n" + persistError;
            }
            ShowSilentMessageDialog(owner, GetUiText().menuAddTempExternalLecture, msg, SoftNoticeKind::Warning);
        }
    }
    s_ignoreLectureSelChange = true;
    LoadLectures();
    s_ignoreLectureSelChange = false;
    if (owner) {
        RefreshStatusDisplay(owner);
        std::wstring msg = localization::Format(
            g_config.studentMode ? L"workspace.actions.external.added.student"
                                : L"workspace.actions.external.added.parent",
            {{L"COUNT", std::to_wstring(added)}});
        ShowSoftNotice(owner, msg, SoftNoticeKind::Info);
    }
}

std::optional<std::wstring> PickWorkspaceFolder(HWND parent) {
    auto result = PromptExistingLocalPath(parent, DialogWorkspaceSelectionInitialFolder(),
                                          GetUiText().menuOpenWs, /*requireDirectory=*/true,
                                          std::filesystem::path(g_workspaceRoot));
    if (result) {
        // No-network requirement: block UNC / device prefix paths for workspace root selection.
        if (result->rfind(L"\\\\", 0) == 0) {
            const std::wstring msg = localization::Text(L"workspace.actions.e36d2c0dd193").c_str();
            ShowSoftNotice(parent, msg, SoftNoticeKind::Warning);
            return std::nullopt;
        }
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(std::filesystem::path(*result), isReparse) && isReparse) {
            const std::wstring msg = localization::Text(L"workspace.actions.0fe9e19f36a0").c_str();
            ShowSoftNotice(parent, msg, SoftNoticeKind::Warning);
            return std::nullopt;
        }
    }
    return result;
}

std::optional<std::wstring> PickFolderWithInitial(HWND parent,
                                                         const std::filesystem::path& initialDir,
                                                         const std::wstring& title) {
    return PromptExistingLocalPath(parent, initialDir, title, /*requireDirectory=*/true);
}

std::vector<std::wstring> PickFoldersWithInitial(HWND parent,
                                                        const std::filesystem::path& initialDir,
                                                        const std::wstring& title) {
    return PromptExistingLocalFolders(parent, initialDir, title, /*allowMultiple=*/true);
}

std::wstring s_newNoteExtension = L".clro";

bool IsSupportedNewNoteExtension(const std::wstring& ext) {
    std::wstring lower = ToLowerAscii(ext);
    return lower == L".clro" || lower == L".txt" || lower == L".csv" || lower == L".md";
}

std::wstring DefaultNewNoteStem() {
    if ((g_pdf.kind != DocKind::None) && !CurrentLogicalPdfPath().empty()) {
        std::wstring stem = std::filesystem::path(CurrentLogicalPdfPath()).stem().wstring();
        if (!stem.empty()) return stem;
    }

    int sIdx = CurrentSessionIndex();
    if (sIdx >= 0 && sIdx < static_cast<int>(g_sessions.size())) {
        std::wstring sessionName = g_sessions[static_cast<size_t>(sIdx)].displayName;
        if (!sessionName.empty()) return sessionName;
    }
    if (!g_currentSessionPath.empty()) {
        std::wstring sessionName = std::filesystem::path(g_currentSessionPath).filename().wstring();
        if (!sessionName.empty()) return sessionName;
    }

    if (!g_currentLecturePath.empty()) {
        std::wstring lectureName = std::filesystem::path(g_currentLecturePath).filename().wstring();
        if (!lectureName.empty()) return lectureName;
    }
    return localization::Text(L"workspace.actions.6e2cda623cf3").c_str();
}

bool ValidateNewNoteFileName(HWND owner, const std::wstring& name) {
    return ValidateCreateFileSystemName(owner, name, localization::Text(L"workspace.actions.a06a1d412684").c_str());
}

bool TryCreateEmptyNoteFile(const std::filesystem::path& target, DWORD* outError) {
    HANDLE h = CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(target).c_str(),
                           GENERIC_WRITE,
                           0,
                           nullptr,
                           CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (outError) *outError = GetLastError();
        return false;
    }
    CloseHandle(h);
    if (outError) *outError = ERROR_SUCCESS;
    return true;
}

bool BuildNamedNoteFileName(HWND owner,
                                   std::wstring input,
                                   const std::wstring& defaultExt,
                                   std::wstring& outFileName) {
    input = TrimWhitespace(input);
    if (input.empty()) {
        ShowSoftNotice(owner,
                       localization::Text(L"workspace.actions.6c9d4ec3a471").c_str(),
                       SoftNoticeKind::Warning);
        return false;
    }
    if (!ValidateNewNoteFileName(owner, input)) return false;

    std::filesystem::path inputPath(input);
    std::wstring ext = ToLowerAscii(inputPath.extension().wstring());
    if (ext.empty()) {
        input += defaultExt;
    } else if (!IsSupportedNewNoteExtension(ext)) {
        ShowSilentMessageDialog(
            owner,
            localization::Text(L"workspace.actions.a06a1d412684").c_str(),
            localization::Text(L"workspace.actions.ac64a932d393").c_str(),
            SoftNoticeKind::Warning);
        return false;
    }
    if (!ValidateNewNoteFileName(owner, input)) return false;
    outFileName = input;
    return true;
}

void CreateNewNoteInSession(HWND hWnd,
                                   const std::wstring& requestedExt,
                                   const std::wstring* requestedName) {
    const auto& ui = GetUiText();
    if (g_currentSessionPath.empty()) {
        ShowSoftNotice(hWnd, ui.errNewClroNoSession, SoftNoticeKind::Warning);
        return;
    }
    std::filesystem::path dir(g_currentSessionPath);
    std::error_code existsEc;
    if (!std::filesystem::exists(dir, existsEc) || existsEc) {
        ShowSoftNotice(hWnd, ui.errNewClroCreate, SoftNoticeKind::Error);
        return;
    }
    std::wstring ext = ToLowerAscii(requestedExt);
    if (!IsSupportedNewNoteExtension(ext)) ext = L".clro";

    bool usePdfName = (g_pdf.kind != DocKind::None) && !CurrentLogicalPdfPath().empty();
    std::wstring baseName = DefaultNewNoteStem();

    auto makeName = [&](int idx) {
        if (!usePdfName) {
            if (idx == 0) return baseName + ext;
            return baseName + L"(" + std::to_wstring(idx) + L")" + ext;
        }
        if (idx <= 1) return baseName + ext;
        return baseName + L"_Page" + std::to_wstring(idx) + ext;
    };
    std::filesystem::path noteDir = NoteDirectoryForSession(dir);
    std::error_code ec;
    std::filesystem::create_directories(noteDir, ec);
    if (ec) {
        ShowSoftNotice(hWnd, ui.errNewClroCreate, SoftNoticeKind::Error);
        return;
    }
    std::filesystem::path target;
    if (requestedName) {
        std::wstring fileName;
        if (!BuildNamedNoteFileName(hWnd, *requestedName, ext, fileName)) return;
        target = noteDir / fileName;
        if (std::filesystem::exists(target, ec) && !ec) {
            ShowSoftNotice(hWnd,
                           localization::Text(L"workspace.actions.48370412e312").c_str(),
                           SoftNoticeKind::Warning);
            return;
        }
        if (ec) {
            ShowSilentMessageDialog(hWnd, localization::Text(L"workspace.actions.f310a5e3cf9a").c_str(), ui.errNewClroCreate, SoftNoticeKind::Error);
            return;
        }
        DWORD createError = ERROR_SUCCESS;
        if (!TryCreateEmptyNoteFile(target, &createError)) {
            ShowSilentMessageDialog(hWnd, localization::Text(L"workspace.actions.f310a5e3cf9a").c_str(),
                           createError == ERROR_FILE_EXISTS
                               ? (localization::Text(L"workspace.actions.48370412e312").c_str())
                               : ui.errNewClroCreate,
                           createError == ERROR_FILE_EXISTS ? SoftNoticeKind::Warning : SoftNoticeKind::Error);
            return;
        }
    } else {
        int startIdx = usePdfName ? 1 : 0;
        for (int i = startIdx; i < startIdx + 1000; ++i) {
            auto cand = noteDir / makeName(i);
            DWORD createError = ERROR_SUCCESS;
            if (TryCreateEmptyNoteFile(cand, &createError)) {
                target = cand;
                break;
            }
            if (createError != ERROR_FILE_EXISTS && createError != ERROR_ALREADY_EXISTS) {
                continue;
            }
        }
    }
    if (target.empty()) {
        ShowSilentMessageDialog(hWnd, localization::Text(L"workspace.actions.f310a5e3cf9a").c_str(), ui.errNewClroCreate, SoftNoticeKind::Error);
        return;
    }

    RefreshCurrentSessionFiles();
    LoadNoteFile(hWnd, target.wstring());
    SyncBottomPaneAfterNoteLoad(hWnd);
    RefreshStatusDisplay(hWnd);
    ShowSoftNotice(hWnd,
                   localization::Text(L"workspace.actions.dd0ff1e37f8e").c_str());
}

void CreateNewClroInSession(HWND hWnd) {
    CreateNewNoteInSession(hWnd, s_newNoteExtension, nullptr);
}

void CreateNewNoteWithExtensionInSession(HWND hWnd, const std::wstring& extension) {
    CreateNewNoteInSession(hWnd, extension, nullptr);
}

std::filesystem::path CurrentNoteDirectory() {
    if (g_currentSessionPath.empty()) return {};
    return NoteDirectoryForSession(std::filesystem::path(g_currentSessionPath));
}


std::wstring DefaultBlankPdfFileName() {
    return L"blank.pdf";
}

std::filesystem::path CurrentPdfDirectory() {
    if (g_currentSessionPath.empty()) return {};
    return PdfDirectoryForSession(std::filesystem::path(g_currentSessionPath));
}

bool SessionFilesUseRootDirectory() {
    std::wstring value = g_config.sessionFileLayout;
    std::transform(value.begin(), value.end(), value.begin(), ::towlower);
    return value == L"session_root" || value == L"root" || value == L"flat";
}

std::filesystem::path NoteDirectoryForSession(const std::filesystem::path& sessionRoot) {
    return SessionFilesUseRootDirectory() ? sessionRoot : sessionRoot / L"note";
}

std::filesystem::path PdfDirectoryForSession(const std::filesystem::path& sessionRoot) {
    return SessionFilesUseRootDirectory() ? sessionRoot : sessionRoot / L"pdf";
}

bool TryParsePositiveDoubleToken(const std::wstring& token, double* out) {
    if (out) *out = 0.0;
    if (token.empty()) return false;
    wchar_t* end = nullptr;
    const double v = std::wcstod(token.c_str(), &end);
    if (end == token.c_str() || !std::isfinite(v) || v <= 0.0) return false;
    while (end && *end) {
        if (!iswspace(*end)) return false;
        ++end;
    }
    if (out) *out = v;
    return true;
}

bool TryParsePositiveIntToken(const std::wstring& token, int minValue, int maxValue, int* out) {
    if (out) *out = 0;
    if (token.empty()) return false;
    wchar_t* end = nullptr;
    const long v = std::wcstol(token.c_str(), &end, 10);
    if (end == token.c_str()) return false;
    while (end && *end) {
        if (!iswspace(*end)) return false;
        ++end;
    }
    if (v < minValue || v > maxValue) return false;
    if (out) *out = static_cast<int>(v);
    return true;
}

bool TryResolveBlankPdfPreset(const std::wstring& token, double* outWPt, double* outHPt) {
    std::wstring key = ToLowerAscii(TrimWhitespace(token));
    std::replace(key.begin(), key.end(), L'_', L'-');
    bool landscape = false;
    auto stripSuffix = [&](const std::wstring& suffix) {
        if (key.size() >= suffix.size() &&
            key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
            key.resize(key.size() - suffix.size());
            landscape = true;
        }
    };
    stripSuffix(L"-landscape");
    stripSuffix(L"-l");
    if (!key.empty() && key.back() == L'l') {
        key.pop_back();
        landscape = true;
    }

    double w = 0.0;
    double h = 0.0;
    if (key == L"a4") {
        w = 595.0;
        h = 842.0;
    } else if (key == L"a5") {
        w = 420.0;
        h = 595.0;
    } else if (key == L"b5") {
        w = 516.0;
        h = 729.0;
    } else if (key == L"letter") {
        w = 612.0;
        h = 792.0;
    } else {
        return false;
    }
    if (landscape) std::swap(w, h);
    if (outWPt) *outWPt = w;
    if (outHPt) *outHPt = h;
    return true;
}

bool TryParseBlankPdfSizeToken(std::wstring token, double* outWPt, double* outHPt) {
    token = ToLowerAscii(TrimWhitespace(token));
    std::replace(token.begin(), token.end(), L'*', L'x');
    const size_t x = token.find(L'x');
    if (x == std::wstring::npos) return TryResolveBlankPdfPreset(token, outWPt, outHPt);

    std::wstring unit = L"mm";
    auto stripUnit = [&](const std::wstring& suffix, const std::wstring& parsedUnit) {
        if (token.size() >= suffix.size() &&
            token.compare(token.size() - suffix.size(), suffix.size(), suffix) == 0) {
            token.resize(token.size() - suffix.size());
            unit = parsedUnit;
            return true;
        }
        return false;
    };
    stripUnit(L"mm", L"mm") || stripUnit(L"pt", L"pt") || stripUnit(L"in", L"in");

    std::wstring wToken = token.substr(0, x);
    std::wstring hToken = token.substr(x + 1);
    double w = 0.0;
    double h = 0.0;
    if (!TryParsePositiveDoubleToken(wToken, &w) || !TryParsePositiveDoubleToken(hToken, &h)) {
        return false;
    }
    if (unit == L"mm") {
        w = w * 72.0 / 25.4;
        h = h * 72.0 / 25.4;
    } else if (unit == L"in") {
        w *= 72.0;
        h *= 72.0;
    }
    if (outWPt) *outWPt = w;
    if (outHPt) *outHPt = h;
    return true;
}

bool TryParseBlankPdfSpec(const std::wstring& input, BlankPdfSpec* out) {
    if (out) *out = {};
    std::wstring normalized = TrimWhitespace(input);
    for (wchar_t& ch : normalized) {
        if (ch == L',' || ch == L';' || ch == L'\t') ch = L' ';
    }
    std::wstringstream ss(normalized);
    std::vector<std::wstring> tokens;
    for (std::wstring token; ss >> token;) tokens.push_back(token);
    if (tokens.empty() || tokens.size() > 3) return false;

    BlankPdfSpec spec{};
    if (tokens.size() == 3) {
        double wMm = 0.0;
        double hMm = 0.0;
        if (!TryParsePositiveDoubleToken(tokens[0], &wMm) ||
            !TryParsePositiveDoubleToken(tokens[1], &hMm) ||
            !TryParsePositiveIntToken(tokens[2], 1, 500, &spec.pageCount)) {
            return false;
        }
        spec.widthPt = wMm * 72.0 / 25.4;
        spec.heightPt = hMm * 72.0 / 25.4;
    } else {
        if (!TryParseBlankPdfSizeToken(tokens[0], &spec.widthPt, &spec.heightPt)) return false;
        if (tokens.size() == 2 &&
            !TryParsePositiveIntToken(tokens[1], 1, 500, &spec.pageCount)) {
            return false;
        }
    }
    if (spec.widthPt < 72.0 || spec.heightPt < 72.0 ||
        spec.widthPt > 2880.0 || spec.heightPt > 2880.0) {
        return false;
    }
    if (out) *out = spec;
    return true;
}

std::optional<std::filesystem::path> PromptBlankPdfPath(HWND owner,
                                                               const std::filesystem::path& pdfDir) {
    std::vector<std::wstring> suggestions;
    PushUniqueSuggestion(suggestions, DefaultBlankPdfFileName());
    PushUniqueSuggestion(suggestions, TodayDateForName() + L".pdf");
    std::wstring fileName;
    PromptCreateNameResult prompt = PromptCreateName(
        owner,
        localization::Text(L"workspace.actions.1632a8d9e2b2").c_str(),
        localization::Text(L"workspace.actions.509bc61f7926").c_str(),
        DefaultBlankPdfFileName(),
        suggestions,
        true,
        fileName);
    std::optional<std::filesystem::path> picked;
    if (prompt == PromptCreateNameResult::Explorer) {
        COMDLG_FILTERSPEC filters[] = {
            { L"PDF (*.pdf)", L"*.pdf" },
        };
        picked = PromptCreatePathWithSaveDialog(
            owner,
            pdfDir,
            DefaultBlankPdfFileName(),
            GetUiText().menuCreateBlankPdf,
            filters,
            static_cast<UINT>(std::size(filters)),
            1,
            L"pdf");
        if (!picked) return std::nullopt;
        if (!IsPathDirectChildOf(*picked, pdfDir)) {
            ShowSoftNotice(owner, localization::Text(
                g_config.studentMode ? L"workspace.actions.pdf.outside_folder.student"
                                    : L"workspace.actions.pdf.outside_folder.parent"),
                            SoftNoticeKind::Warning);
            return std::nullopt;
        }
        fileName = picked->filename().wstring();
    } else if (prompt == PromptCreateNameResult::Create) {
        fileName = TrimWhitespace(fileName);
    } else {
        return std::nullopt;
    }
    if (fileName.empty()) return std::nullopt;

    std::filesystem::path filePath(fileName);
    if (ToLowerAscii(filePath.extension().wstring()).empty()) {
        fileName += L".pdf";
        filePath = std::filesystem::path(fileName);
    }
    if (ToLowerAscii(filePath.extension().wstring()) != L".pdf") {
        ShowSoftNotice(owner,
                       localization::Text(L"workspace.actions.1bdb54355ce8").c_str(),
                       SoftNoticeKind::Warning);
        return std::nullopt;
    }
    if (!ValidateCreateFileSystemName(owner, fileName, GetUiText().menuCreateBlankPdf)) {
        return std::nullopt;
    }
    if (picked) {
        *picked = picked->parent_path() / filePath.filename();
    } else {
        picked = pdfDir / filePath.filename();
    }
    std::error_code ec;
    if (std::filesystem::exists(*picked, ec) && !ec) {
        ShowSoftNotice(owner,
                       localization::Text(L"workspace.actions.d992635ac5cc").c_str(),
                       SoftNoticeKind::Warning);
        return std::nullopt;
    }
    if (ec) {
        ShowSoftNotice(owner,
                       localization::Text(L"workspace.actions.9ca7365c19be").c_str(),
                       SoftNoticeKind::Error);
        return std::nullopt;
    }
    return picked;
}

bool VerifyBlankPdfFile(const std::filesystem::path& path,
                               const BlankPdfSpec& spec,
                               std::wstring* outErr) {
    HANDLE h = CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(path).c_str(),
                           GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (outErr) *outErr = atomic_write::Win32ErrorMessage(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
        size.QuadPart > static_cast<LONGLONG>(std::numeric_limits<unsigned long>::max())) {
        CloseHandle(h);
        if (outErr) *outErr = localization::Text(L"workspace.actions.90df9eb1a93b").c_str();
        return false;
    }
    auto readBlock = [](void* param,
                        unsigned long position,
                        unsigned char* outBuffer,
                        unsigned long readSize) -> int {
        HANDLE file = reinterpret_cast<HANDLE>(param);
        if (!file || file == INVALID_HANDLE_VALUE || !outBuffer || readSize == 0) return 0;
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(position);
        ov.OffsetHigh = static_cast<DWORD>(static_cast<unsigned long long>(position) >> 32);
        DWORD read = 0;
        if (!ReadFile(file, outBuffer, readSize, &read, &ov)) {
            DWORD e = GetLastError();
            if (e != ERROR_IO_PENDING || !GetOverlappedResult(file, &ov, &read, TRUE)) return 0;
        }
        return read == readSize ? 1 : 0;
    };
    FPDF_FILEACCESS access{};
    access.m_FileLen = static_cast<unsigned long>(size.QuadPart);
    access.m_GetBlock = readBlock;
    access.m_Param = reinterpret_cast<void*>(h);

    std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
    FPDF_DOCUMENT doc = FPDF_LoadCustomDocument(&access, nullptr);
    if (!doc) {
        CloseHandle(h);
        if (outErr) *outErr = localization::Text(L"workspace.actions.1062240cf162").c_str();
        return false;
    }
    bool ok = FPDF_GetPageCount(doc) == spec.pageCount;
    double w = 0.0;
    double hPt = 0.0;
    if (ok) {
        ok = !!FPDF_GetPageSizeByIndex(doc, 0, &w, &hPt) &&
             std::abs(w - spec.widthPt) < 1.0 &&
             std::abs(hPt - spec.heightPt) < 1.0;
    }
    FPDF_CloseDocument(doc);
    CloseHandle(h);
    if (!ok && outErr) {
        *outErr = localization::Text(L"workspace.actions.cafe823212dd").c_str();
    }
    return ok;
}

bool SaveBlankPdfDocumentAtomically(const std::filesystem::path& dest,
                                           const BlankPdfSpec& spec,
                                           std::wstring* outErr) {
    if (outErr) outErr->clear();
    if (dest.empty() || dest.parent_path().empty()) {
        if (outErr) *outErr = localization::Text(L"workspace.actions.c1917fc7de7f").c_str();
        return false;
    }
    std::error_code ec;
    if (std::filesystem::exists(dest, ec) && !ec) {
        if (outErr) *outErr = localization::Text(L"workspace.actions.98eb9325bc9a").c_str();
        return false;
    }
    std::filesystem::path tmp;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    if (!atomic_write::CreateUniqueTempFile(dest, dest.parent_path(), &tmp, &hFile, outErr)) {
        return false;
    }

    struct Writer : FPDF_FILEWRITE { HANDLE hFile; };
    auto writeBlock = [](FPDF_FILEWRITE* p, const void* data, unsigned long size) -> int {
        Writer* writer = static_cast<Writer*>(p);
        DWORD written = 0;
        if (!writer || writer->hFile == INVALID_HANDLE_VALUE) return 0;
        if (!WriteFile(writer->hFile, data, size, &written, nullptr)) return 0;
        return written == size ? 1 : 0;
    };
    Writer writer{};
    writer.version = 1;
    writer.WriteBlock = writeBlock;
    writer.hFile = hFile;

    bool ok = false;
    {
        std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
        FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
        if (doc) {
            ok = true;
            for (int i = 0; i < spec.pageCount; ++i) {
                FPDF_PAGE page = FPDFPage_New(doc, i, spec.widthPt, spec.heightPt);
                if (!page) {
                    ok = false;
                    break;
                }
                FPDFPage_GenerateContent(page);
                FPDF_ClosePage(page);
            }
            if (ok) ok = !!FPDF_SaveAsCopy(doc, &writer, 0);
            FPDF_CloseDocument(doc);
        }
    }
    if (ok && !FlushFileBuffers(hFile)) {
        ok = false;
        if (outErr) *outErr = atomic_write::Win32ErrorMessage(GetLastError());
    }
    CloseHandle(hFile);
    hFile = INVALID_HANDLE_VALUE;
    if (!ok) {
        if (outErr && outErr->empty()) {
            *outErr = localization::Text(L"workspace.actions.6b9703a9c4ba").c_str();
        }
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        return false;
    }
    if (!VerifyBlankPdfFile(tmp, spec, outErr)) {
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        return false;
    }
    if (std::filesystem::exists(dest, ec) && !ec) {
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        if (outErr) *outErr = localization::Text(L"workspace.actions.98eb9325bc9a").c_str();
        return false;
    }
    return atomic_write::AtomicReplaceFile(dest, tmp, dest.parent_path(), outErr);
}

void CreateBlankPdfInCurrentSession(HWND hWnd) {
    const auto& ui = GetUiText();
    if (g_currentSessionPath.empty()) {
        ShowSoftNotice(hWnd, ui.errNewClroNoSession, SoftNoticeKind::Warning);
        return;
    }
    std::filesystem::path pdfDir = CurrentPdfDirectory();
    std::error_code ec;
    std::filesystem::create_directories(pdfDir, ec);
    if (ec) {
        ShowSoftNotice(hWnd,
                       localization::Text(L"workspace.actions.bfa5601b5176").c_str(),
                       SoftNoticeKind::Error);
        return;
    }

    BlankPdfDialogOptions options{};
    if (!PromptBlankPdfOptions(hWnd, ui.menuCreateBlankPdf, options)) return;
    BlankPdfSpec spec{};
    spec.widthPt = options.widthPt;
    spec.heightPt = options.heightPt;
    spec.pageCount = options.pageCount;

    auto dest = PromptBlankPdfPath(hWnd, pdfDir);
    if (!dest) return;
    std::wstring err;
    if (!SaveBlankPdfDocumentAtomically(*dest, spec, &err)) {
        ShowSilentMessageDialog(hWnd,
                                ui.menuCreateBlankPdf,
                                err.empty() ? (localization::Text(L"workspace.actions.6b9703a9c4ba").c_str())
                                            : err,
                                SoftNoticeKind::Error);
        return;
    }
    RefreshCurrentSessionFiles();
    OpenPdfIfDifferent(hWnd, dest->wstring());
    RefreshStatusDisplay(hWnd);
    ShowSoftNotice(hWnd,
                   localization::Text(L"workspace.actions.32aab0abf740").c_str());
}

std::optional<std::wstring> PromptNoteFileNameWithSaveDialog(HWND owner,
                                                                    const std::filesystem::path& noteDir,
                                                                    const std::wstring& initialName,
                                                                    const std::wstring& defaultExt) {
    COMDLG_FILTERSPEC filters[] = {
        { L"Markdown note (*.md)", L"*.md" },
        { L"CLRO note (*.clro)", L"*.clro" },
        { L"Text note (*.txt)", L"*.txt" },
        { L"CSV note (*.csv)", L"*.csv" },
    };
    UINT filterIndex = 1;
    std::wstring ext = ToLowerAscii(defaultExt);
    if (ext == L".clro") filterIndex = 2;
    else if (ext == L".txt") filterIndex = 3;
    else if (ext == L".csv") filterIndex = 4;
    std::wstring defaultExtNoDot = ext;
    if (!defaultExtNoDot.empty() && defaultExtNoDot.front() == L'.') {
        defaultExtNoDot.erase(defaultExtNoDot.begin());
    }

    auto picked = PromptCreatePathWithSaveDialog(
        owner,
        noteDir,
        initialName,
        localization::Text(L"workspace.actions.ac4902b8a482").c_str(),
        filters,
        static_cast<UINT>(std::size(filters)),
        filterIndex,
        defaultExtNoDot);
    if (!picked) return std::nullopt;
    if (!IsPathDirectChildOf(*picked, noteDir)) {
        ShowSoftNotice(owner, localization::Text(
            g_config.studentMode ? L"workspace.actions.note.outside_folder.student"
                                : L"workspace.actions.note.outside_folder.parent"),
                        SoftNoticeKind::Warning);
        return std::nullopt;
    }
    std::wstring fileName = picked->filename().wstring();
    std::filesystem::path filePath(fileName);
    std::wstring pickedExt = ToLowerAscii(filePath.extension().wstring());
    if (pickedExt.empty()) {
        fileName += defaultExt;
    } else if (!IsSupportedNewNoteExtension(pickedExt)) {
        ShowSoftNotice(owner,
                       localization::Text(L"workspace.actions.31261ba9f8be").c_str(),
                       SoftNoticeKind::Warning);
        return std::nullopt;
    }
    return fileName;
}

std::optional<std::wstring> PromptCurrentNoteFileNameWithSaveDialog(HWND owner,
                                                                          const std::wstring& initialName,
                                                                          const std::wstring& defaultExt) {
    if (g_currentSessionPath.empty()) {
        ShowSoftNotice(owner, GetUiText().errNewClroNoSession, SoftNoticeKind::Warning);
        return std::nullopt;
    }
    std::filesystem::path noteDir = CurrentNoteDirectory();
    std::error_code ec;
    std::filesystem::create_directories(noteDir, ec);
    if (ec) {
        ShowSoftNotice(owner, GetUiText().errNewClroCreate, SoftNoticeKind::Error);
        return std::nullopt;
    }
    return PromptNoteFileNameWithSaveDialog(owner, noteDir, initialName, defaultExt);
}

std::vector<std::wstring> NewNoteNameSuggestions(const std::wstring& ext) {
    std::vector<std::wstring> suggestions;
    const std::wstring stem = DefaultNewNoteStem();
    PushUniqueSuggestion(suggestions, stem + ext);
    PushUniqueSuggestion(suggestions, TodayDateForName() + ext);
    PushUniqueSuggestion(suggestions, (localization::Text(L"workspace.actions.6e2cda623cf3").c_str()) + ext);
    if (!g_currentSessionPath.empty()) {
        std::wstring sessionName = std::filesystem::path(g_currentSessionPath).filename().wstring();
        PushUniqueSuggestion(suggestions, sessionName + ext);
    }
    if (!g_currentLecturePath.empty()) {
        std::wstring lectureName = std::filesystem::path(g_currentLecturePath).filename().wstring();
        PushUniqueSuggestion(suggestions, lectureName + ext);
    }
    return suggestions;
}

bool ShowNewNoteButtonContextMenu(HWND hWnd, LPARAM lParam) {
    HMENU menu = CreatePopupMenu();
    HMENU extMenu = CreatePopupMenu();
    if (!menu || !extMenu) {
        if (extMenu) DestroyMenu(extMenu);
        if (menu) DestroyMenu(menu);
        return false;
    }

    struct ExtItem {
        UINT id;
        const wchar_t* ext;
    };
    constexpr ExtItem kExtItems[] = {
        { 1, L".md" },
        { 2, L".clro" },
        { 3, L".txt" },
        { 4, L".csv" },
    };
    for (const auto& item : kExtItems) {
        UINT flags = MF_STRING;
        if (ToLowerAscii(s_newNoteExtension) == item.ext) flags |= MF_CHECKED;
        AppendMenuW(extMenu, flags, item.id, item.ext + 1);
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(extMenu),
                localization::Text(L"workspace.actions.53cb693b4b81").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 10,
                localization::Text(L"workspace.actions.2ae31ad840c1").c_str());

    POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (pt.x == -1 && pt.y == -1) {
        RECT rc{};
        GetWindowRect(g_hBtnNewNote, &rc);
        pt.x = rc.left + (rc.right - rc.left) / 2;
        pt.y = rc.bottom;
    }
    const UINT cmd = TrackPopupMenu(menu,
                                    TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                    pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(menu);

    for (const auto& item : kExtItems) {
        if (cmd == item.id) {
            s_newNoteExtension = item.ext;
            ShowSoftNotice(hWnd,
                           localization::Format(L"workspace.actions.note.extension",
                                                {{L"EXT", s_newNoteExtension}}),
                           SoftNoticeKind::Info);
            return true;
        }
    }
    if (cmd == 10) {
        if (!SaveNoteIfDirty(hWnd)) return true;
        std::vector<std::wstring> suggestions = NewNoteNameSuggestions(s_newNoteExtension);
        std::wstring input = suggestions.empty() ? (DefaultNewNoteStem() + s_newNoteExtension) : suggestions.front();
        std::wstring name;
        PromptCreateNameResult prompt = PromptCreateName(
            hWnd,
            localization::Text(L"workspace.actions.a06a1d412684").c_str(),
            localization::Text(L"workspace.actions.6c9910698a48").c_str(),
            input,
            suggestions,
            true,
            name);
        if (prompt == PromptCreateNameResult::Explorer) {
            auto pickedName = PromptCurrentNoteFileNameWithSaveDialog(hWnd, input, s_newNoteExtension);
            if (pickedName) {
                CreateNewNoteInSession(hWnd, s_newNoteExtension, &*pickedName);
            }
            return true;
        }
        if (prompt != PromptCreateNameResult::Create) {
            return true;
        }
        CreateNewNoteInSession(hWnd, s_newNoteExtension, &name);
        return true;
    }
    return cmd != 0;
}

enum class ImportOneResult {
    Imported,
    Skipped,
    Canceled,
    Failed
};


bool IsUnsupportedImportSourcePath(const std::filesystem::path& path) {
    std::wstring s = path.wstring();
    std::replace(s.begin(), s.end(), L'/', L'\\');
    if (s.rfind(L"\\\\?\\UNC\\", 0) == 0 || s.rfind(L"\\\\?\\unc\\", 0) == 0) return true;
    if (s.rfind(L"\\\\?\\", 0) == 0) {
        const bool extendedDrivePath =
            s.size() >= 7 &&
            ((s[4] >= L'A' && s[4] <= L'Z') || (s[4] >= L'a' && s[4] <= L'z')) &&
            s[5] == L':' &&
            s[6] == L'\\';
        return !extendedDrivePath;
    }
    if (s.rfind(L"\\\\.\\", 0) == 0) return true;
    return IsUncPathString(s);
}

std::filesystem::path ImportDestinationDirForSource(const std::filesystem::path& sessionRoot,
                                                           const std::filesystem::path& src) {
    auto ext = src.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (IsPdfFile(src) || IsImageFile(src) || ext == L".clrop" || IsOfficeImportSourcePath(src)) {
        return PdfDirectoryForSession(sessionRoot);
    }
    if (IsNoteFile(src)) {
        return NoteDirectoryForSession(sessionRoot);
    }
    return sessionRoot;
}

bool IsOfficeImportSourcePath(const std::filesystem::path& path) {
    auto ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L".docx" || ext == L".pptx";
}

bool IsDocxImportSourcePath(const std::filesystem::path& path) {
    auto ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L".docx";
}

// Office conversion is reachable only through a runtime that has passed
// tools/libreoffice_runtime_gate.py. The retained third_party administrative
// image is intentionally not searched here because it is kept for comparison
// and still contains prohibited communication-capable imports.
constexpr bool kOfficePdfConversionApprovedForUse = true;

void CleanupImportTempFile(HANDLE* handle, const std::filesystem::path& tmp) {
    if (handle && *handle != INVALID_HANDLE_VALUE) {
        CloseHandle(*handle);
        *handle = INVALID_HANDLE_VALUE;
    }
    if (!tmp.empty()) {
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
    }
}

bool CopyFileForImportSafely(const std::filesystem::path& src,
                                    const std::filesystem::path& dest,
                                    std::wstring* outErr) {
    if (outErr) outErr->clear();
    if (src.empty() || dest.empty()) {
        if (outErr) *outErr = localization::Text(L"workspace.actions.70e485841f70").c_str();
        return false;
    }

    if (!dest.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec) {
            if (outErr) *outErr = (localization::Text(L"workspace.actions.af61d0579ff4").c_str()) +
                                  dest.parent_path().wstring();
            return false;
        }
    }

    std::filesystem::path tmp;
    HANDLE tmpHandle = INVALID_HANDLE_VALUE;
    if (!atomic_write::CreateUniqueTempFile(dest, dest.parent_path(), &tmp, &tmpHandle, outErr)) {
        return false;
    }

    HANDLE srcHandle = CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(src).c_str(),
                                   GENERIC_READ,
                                   FILE_SHARE_READ,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,
                                   nullptr);
    if (srcHandle == INVALID_HANDLE_VALUE) {
        if (outErr) {
            DWORD e = GetLastError();
            *outErr = (localization::Text(L"workspace.actions.4804e4b7f042").c_str()) +
                      src.wstring() + L"\n\n" + atomic_write::Win32ErrorMessage(e);
        }
        CleanupImportTempFile(&tmpHandle, tmp);
        return false;
    }

    BY_HANDLE_FILE_INFORMATION sourceInfo{};
    if (!GetFileInformationByHandle(srcHandle, &sourceInfo) ||
        (sourceInfo.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        if (outErr) *outErr = localization::Text(L"workspace.actions.ff84456beeca").c_str();
        CloseHandle(srcHandle);
        CleanupImportTempFile(&tmpHandle, tmp);
        return false;
    }

    LARGE_INTEGER expectedSize{};
    if (!GetFileSizeEx(srcHandle, &expectedSize) || expectedSize.QuadPart < 0) {
        if (outErr) {
            DWORD e = GetLastError();
            *outErr = (localization::Text(L"workspace.actions.b92de0f5edfd").c_str()) +
                      src.wstring() + L"\n\n" + atomic_write::Win32ErrorMessage(e);
        }
        CloseHandle(srcHandle);
        CleanupImportTempFile(&tmpHandle, tmp);
        return false;
    }

    std::array<uint8_t, 64 * 1024> buffer{};
    unsigned long long copied = 0;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(srcHandle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            if (outErr) {
                DWORD e = GetLastError();
                *outErr = (localization::Text(L"workspace.actions.ecc0976805b1").c_str()) +
                          src.wstring() + L"\n\n" + atomic_write::Win32ErrorMessage(e);
            }
            CloseHandle(srcHandle);
            CleanupImportTempFile(&tmpHandle, tmp);
            return false;
        }
        if (read == 0) break;

        DWORD written = 0;
        if (!WriteFile(tmpHandle, buffer.data(), read, &written, nullptr) || written != read) {
            if (outErr) {
                DWORD e = GetLastError();
                *outErr = (localization::Text(L"workspace.actions.63859a0cd226").c_str()) +
                          tmp.wstring() + L"\n\n" + atomic_write::Win32ErrorMessage(e);
            }
            CloseHandle(srcHandle);
            CleanupImportTempFile(&tmpHandle, tmp);
            return false;
        }
        copied += static_cast<unsigned long long>(read);
    }
    CloseHandle(srcHandle);

    if (copied != static_cast<unsigned long long>(expectedSize.QuadPart)) {
        if (outErr) *outErr = localization::Text(L"workspace.actions.d715db855a1b").c_str();
        CleanupImportTempFile(&tmpHandle, tmp);
        return false;
    }
    if (!FlushFileBuffers(tmpHandle)) {
        if (outErr) {
            DWORD e = GetLastError();
            *outErr = (localization::Text(L"workspace.actions.bfa46a3e7ff5").c_str()) +
                      tmp.wstring() + L"\n\n" + atomic_write::Win32ErrorMessage(e);
        }
        CleanupImportTempFile(&tmpHandle, tmp);
        return false;
    }
    if (!CloseHandle(tmpHandle)) {
        tmpHandle = INVALID_HANDLE_VALUE;
        if (outErr) {
            DWORD e = GetLastError();
            *outErr = (localization::Text(L"workspace.actions.964d288267b1").c_str()) +
                      tmp.wstring() + L"\n\n" + atomic_write::Win32ErrorMessage(e);
        }
        CleanupImportTempFile(nullptr, tmp);
        return false;
    }
    tmpHandle = INVALID_HANDLE_VALUE;

    std::wstring replaceErr;
    if (!atomic_write::AtomicReplaceFile(dest, tmp, dest.parent_path(), &replaceErr)) {
        if (outErr) *outErr = replaceErr.empty()
            ? (localization::Text(L"workspace.actions.11095a6f622f").c_str())
            : replaceErr;
        return false;
    }
    return true;
}

struct DirectoryImportPlan {
    std::filesystem::path sourceRoot;
    std::filesystem::path destRoot;
    std::vector<std::filesystem::path> directories;
    std::vector<std::filesystem::path> files;
};

bool IsWorkspaceReservedImportDirectoryName(const std::filesystem::path& path) {
    return ToLowerAscii(path.filename().wstring()) == L"__resource__";
}

bool ValidateDirectoryImportSource(HWND owner,
                                          const std::filesystem::path& src,
                                          const std::wstring& title) {
    if (src.empty()) return false;
    if (IsUnsupportedImportSourcePath(src)) {
        const std::wstring msg = localization::Text(L"workspace.actions.fdb3da0b190c").c_str();
        ShowSoftNotice(owner, msg, SoftNoticeKind::Warning);
        return false;
    }
    bool isReparse = false;
    if (TryIsReparsePointNoFollow(src, isReparse) && isReparse) {
        const std::wstring msg = localization::Text(L"workspace.actions.feeb29c6c55e").c_str();
        ShowSoftNotice(owner, msg, SoftNoticeKind::Warning);
        return false;
    }
    if (IsWorkspaceReservedImportDirectoryName(src)) {
        const std::wstring msg = localization::Text(L"workspace.actions.484daf708787").c_str();
        ShowSoftNotice(owner, msg, SoftNoticeKind::Warning);
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(src, ec) || ec ||
        !std::filesystem::is_directory(src, ec) || ec) {
        ShowSilentMessageDialog(owner, title,
                                localization::Text(L"workspace.actions.740250a3ddc0"),
                                SoftNoticeKind::Warning, {{L"", src.wstring()}});
        return false;
    }
    std::wstring readErr;
    if (!TryOpenDirForList(src, &readErr)) {
        std::wstring msg = localization::Text(L"workspace.actions.1b1250fdd02b").c_str();
        if (!readErr.empty()) msg += L"\n\n" + readErr;
        ShowSilentMessageDialog(owner, title, msg, SoftNoticeKind::Warning,
                                {{L"", src.wstring()}});
        return false;
    }
    return true;
}

bool BuildDirectoryImportPlan(const std::filesystem::path& src,
                                     const std::filesystem::path& dest,
                                     DirectoryImportPlan* outPlan,
                                     std::wstring* outErr) {
    if (outPlan) *outPlan = {};
    if (outErr) outErr->clear();
    if (src.empty() || dest.empty()) {
        if (outErr) *outErr = localization::Text(L"workspace.actions.70e485841f70").c_str();
        return false;
    }

    DirectoryImportPlan plan;
    plan.sourceRoot = src;
    plan.destRoot = dest;

    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        src, std::filesystem::directory_options::none, ec);
    if (ec) {
        if (outErr) *outErr = src.wstring() + L"\n\n" + UTF8ToWide(ec.message());
        return false;
    }

    std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            if (outErr) *outErr = UTF8ToWide(ec.message());
            return false;
        }

        const std::filesystem::path p = it->path();
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(p, isReparse) && isReparse) {
            if (outErr) {
                *outErr = localization::Text(L"workspace.actions.ffa85e221b63").c_str();
                *outErr += p.wstring();
            }
            return false;
        }

        std::error_code relEc;
        std::filesystem::path rel = std::filesystem::relative(p, src, relEc);
        const bool relEscapesRoot = !rel.empty() && rel.begin()->wstring() == L"..";
        if (relEc || rel.empty() || relEscapesRoot) {
            if (outErr) {
                *outErr = localization::Text(L"workspace.actions.115f58824162").c_str();
                *outErr += p.wstring();
            }
            return false;
        }

        std::error_code stEc;
        if (it->is_directory(stEc) && !stEc) {
            if (IsWorkspaceReservedImportDirectoryName(p)) {
                if (outErr) {
                    *outErr = localization::Text(L"workspace.actions.3a663010fdc5").c_str();
                    *outErr += p.wstring();
                }
                return false;
            }
            plan.directories.push_back(rel);
            continue;
        }
        if (stEc) {
            if (outErr) *outErr = p.wstring() + L"\n\n" + UTF8ToWide(stEc.message());
            return false;
        }

        stEc.clear();
        if (it->is_regular_file(stEc) && !stEc) {
            plan.files.push_back(rel);
            continue;
        }
        if (stEc) {
            if (outErr) *outErr = p.wstring() + L"\n\n" + UTF8ToWide(stEc.message());
            return false;
        }
    }

    if (outPlan) *outPlan = std::move(plan);
    return true;
}

bool IsDirectoryImportRollbackPathAllowed(const std::filesystem::path& path,
                                                 const std::filesystem::path& destRoot,
                                                 const std::filesystem::path& allowedRoot) {
    return !path.empty() && !destRoot.empty() && !allowedRoot.empty() &&
           IsPathUnderRoot(path, allowedRoot) && IsPathUnderRoot(path, destRoot);
}

bool RollbackCreatedDirectoryImportEntries(
    const std::filesystem::path& destRoot,
    const std::filesystem::path& allowedRoot,
    const std::vector<std::filesystem::path>& createdFiles,
    std::vector<std::filesystem::path> createdDirs,
    std::wstring* outErr) {
    if (outErr) outErr->clear();
    if (!IsDirectoryImportRollbackPathAllowed(destRoot, destRoot, allowedRoot)) {
        if (outErr) {
            *outErr = localization::Text(L"workspace.actions.523722630388").c_str();
        }
        return false;
    }

    bool ok = true;
    std::wstring errors;
    auto appendError = [&](const std::filesystem::path& path, const std::error_code& ec) {
        ok = false;
        if (!errors.empty()) errors += L"\n\n";
        errors += path.wstring() + L"\n" + UTF8ToWide(ec.message());
    };

    for (const auto& file : createdFiles) {
        if (!IsDirectoryImportRollbackPathAllowed(file, destRoot, allowedRoot)) {
            ok = false;
            if (!errors.empty()) errors += L"\n\n";
            errors += localization::Format(L"workspace.actions.import.rollback_file_skipped",
                                           {{L"PATH", file.wstring()}});
            continue;
        }
        std::error_code ec;
        const bool removed = std::filesystem::remove(file, ec);
        if (ec) appendError(file, ec);
        (void)removed;
    }

    std::sort(createdDirs.begin(), createdDirs.end(), [](const auto& a, const auto& b) {
        return a.wstring().size() > b.wstring().size();
    });
    createdDirs.erase(std::unique(createdDirs.begin(), createdDirs.end()), createdDirs.end());

    for (const auto& dir : createdDirs) {
        if (!IsDirectoryImportRollbackPathAllowed(dir, destRoot, allowedRoot)) {
            ok = false;
            if (!errors.empty()) errors += L"\n\n";
            errors += localization::Format(L"workspace.actions.import.rollback_folder_skipped",
                                           {{L"PATH", dir.wstring()}});
            continue;
        }
        std::error_code ec;
        const bool removed = std::filesystem::remove(dir, ec);
        if (ec) appendError(dir, ec);
        (void)removed;
    }

    if (!ok && outErr) *outErr = errors;
    return ok;
}

bool ExecuteDirectoryImportPlan(const DirectoryImportPlan& plan,
                                       const std::filesystem::path& allowedDestRoot,
                                       std::wstring* outErr) {
    if (outErr) outErr->clear();
    if (plan.sourceRoot.empty() || plan.destRoot.empty() ||
        allowedDestRoot.empty() ||
        !IsPathUnderRoot(plan.destRoot, allowedDestRoot)) {
        if (outErr) *outErr = localization::Text(L"workspace.actions.8f473bf6a3e9").c_str();
        return false;
    }
    if (IsPathUnderRoot(plan.destRoot, plan.sourceRoot) ||
        IsPathUnderRoot(plan.sourceRoot, plan.destRoot)) {
        if (outErr) {
            *outErr = localization::Text(L"workspace.actions.41f55052d6b1").c_str();
        }
        return false;
    }

    std::error_code ec;
    if (std::filesystem::exists(plan.destRoot, ec) && !ec) {
        if (outErr) {
            *outErr = (localization::Text(L"workspace.actions.3cf2beeadd3f").c_str()) +
                      plan.destRoot.wstring();
        }
        return false;
    }
    if (ec) {
        if (outErr) *outErr = plan.destRoot.wstring() + L"\n\n" + UTF8ToWide(ec.message());
        return false;
    }

    std::filesystem::create_directories(plan.destRoot, ec);
    if (ec) {
        if (outErr) {
            *outErr = (localization::Text(L"workspace.actions.af61d0579ff4").c_str()) +
                      plan.destRoot.wstring();
        }
        return false;
    }

    std::vector<std::filesystem::path> createdDirs{plan.destRoot};
    std::vector<std::filesystem::path> createdFiles;

    auto rollback = [&](const std::wstring& err) -> bool {
        std::wstring rollbackErr;
        RollbackCreatedDirectoryImportEntries(plan.destRoot, allowedDestRoot,
                                              createdFiles, createdDirs, &rollbackErr);
        if (outErr) {
            *outErr = err;
            if (!rollbackErr.empty()) {
                if (!outErr->empty()) *outErr += L"\n\n";
                *outErr += rollbackErr;
            }
        }
        return false;
    };

    for (const auto& rel : plan.directories) {
        const std::filesystem::path createdDir = plan.destRoot / rel;
        std::filesystem::create_directories(createdDir, ec);
        if (ec) {
            std::wstring err = (localization::Text(L"workspace.actions.f5f02a8a236a").c_str()) +
                               createdDir.wstring();
            return rollback(err);
        }
        createdDirs.push_back(createdDir);
    }

    for (const auto& rel : plan.files) {
        const std::filesystem::path srcFile = plan.sourceRoot / rel;
        const std::filesystem::path destFile = plan.destRoot / rel;
        std::wstring copyErr;
        if (!CopyFileForImportSafely(srcFile, destFile, &copyErr)) {
            std::wstring err = copyErr.empty()
                ? (localization::Text(L"workspace.actions.67c8af2a773e").c_str())
                : copyErr;
            return rollback(err);
        }
        createdFiles.push_back(destFile);
    }
    return true;
}

std::optional<std::filesystem::path> PickDirectoryImportSource(HWND owner,
                                                                      const std::wstring& title,
                                                                      const std::filesystem::path& initial) {
    auto picked = PromptExistingLocalPathAppFirst(owner, initial, title, /*requireDirectory=*/true);
    if (!picked) return std::nullopt;
    std::filesystem::path src(*picked);
    if (!ValidateDirectoryImportSource(owner, src, title)) return std::nullopt;
    return CanonicalOrSelf(src);
}

bool ImportDirectoryAsLecture(HWND hWnd) {
    const auto& ui = GetUiText();
    if (g_workspaceRoot.empty() || !VerifyWorkspaceWritableForEditing(hWnd)) {
        return false;
    }
    const std::wstring title = ui.menuImportDirAsLecture;
    std::filesystem::path classesPath = WorkspaceClassesPath(g_workspaceRoot, g_config);
    std::error_code ec;
    std::filesystem::create_directories(classesPath, ec);
    if (ec) {
        ShowSilentMessageDialog(hWnd, title,
                                localization::Format(
                                    g_config.studentMode
                                        ? L"workspace.actions.import.destination_folder_failed.student"
                                        : L"workspace.actions.import.destination_folder_failed.parent",
                                    {{L"PATH", classesPath.wstring()}}),
                                SoftNoticeKind::Error);
        return false;
    }
    if (!VerifyDirReadableWritableForEditing(hWnd, classesPath,
                                             g_config.studentMode ? L"workspace.directory.lecture_import.student"
                                                                  : L"workspace.directory.lecture_import.parent")) {
        return false;
    }
    auto src = PickDirectoryImportSource(hWnd, title, DialogDownloadsInitialFolder());
    if (!src) return false;

    std::wstring workspaceLockError;
    WorkspaceOperationLock workspaceLock(std::filesystem::path(g_workspaceRoot), &workspaceLockError);
    if (!workspaceLock.acquired()) {
        ShowSoftNotice(hWnd, localization::Text(L"workspace.actions.4618bb4d74d4").c_str(),
            SoftNoticeKind::Warning);
        return false;
    }

    std::filesystem::path dest = classesPath / src->filename();
    DirectoryImportPlan plan;
    std::wstring err;
    if (!BuildDirectoryImportPlan(*src, dest, &plan, &err) ||
        !ExecuteDirectoryImportPlan(plan, classesPath, &err)) {
        ShowSilentMessageDialog(hWnd, title,
                                err.empty() ? ui.errImportFile : err,
                                SoftNoticeKind::Error);
        return false;
    }

    g_currentLecturePath = dest.wstring();
    UpdateLectureOpenTime(g_currentLecturePath);
    ResetSessionAndFiles();
    LoadLectures();
    SyncLeftPaneSelection();
    RefreshMainWindowUiState(hWnd);
    ShowSoftNotice(hWnd, localization::Text(
                       g_config.studentMode ? L"workspace.actions.import.directory_as_lecture.student"
                                           : L"workspace.actions.import.directory_as_lecture.parent"),
                   SoftNoticeKind::Info);
    return true;
}

bool ImportDirectoryAsSession(HWND hWnd) {
    const auto& ui = GetUiText();
    if (g_currentLecturePath.empty()) {
        ShowSoftNotice(hWnd, ui.errSessionNoLecture, SoftNoticeKind::Warning);
        return false;
    }
    if (!VerifyWorkspaceWritableForEditing(hWnd)) return false;
    const std::wstring title = ui.menuImportDirAsSession;
    std::filesystem::path lectureDir(g_currentLecturePath);
    if (!VerifyDirReadableWritableForEditing(hWnd, lectureDir,
                                             g_config.studentMode ? L"workspace.directory.session_import.student"
                                                                  : L"workspace.directory.session_import.parent")) {
        return false;
    }
    auto src = PickDirectoryImportSource(hWnd, title, DialogDownloadsInitialFolder());
    if (!src) return false;

    std::wstring workspaceLockError;
    WorkspaceOperationLock workspaceLock(std::filesystem::path(g_workspaceRoot), &workspaceLockError);
    if (!workspaceLock.acquired()) {
        ShowSoftNotice(hWnd, localization::Text(L"workspace.actions.4618bb4d74d4").c_str(),
            SoftNoticeKind::Warning);
        return false;
    }

    std::filesystem::path dest = lectureDir / src->filename();
    DirectoryImportPlan plan;
    std::wstring err;
    if (!BuildDirectoryImportPlan(*src, dest, &plan, &err) ||
        !ExecuteDirectoryImportPlan(plan, lectureDir, &err)) {
        ShowSilentMessageDialog(hWnd, title,
                                err.empty() ? ui.errImportFile : err,
                                SoftNoticeKind::Error);
        return false;
    }

    ReloadSessionsAndSelect(g_currentLecturePath, dest.filename().wstring(), true);
    ShowSoftNotice(hWnd,
                   localization::Text(g_config.studentMode
                                          ? L"workspace.actions.import.directory_as_session.student"
                                          : L"workspace.actions.import.directory_as_session.parent").c_str(),
                   SoftNoticeKind::Info);
    return true;
}

int ImportPdfGetBlockFromHandle(void* param,
                                       unsigned long position,
                                       unsigned char* outBuffer,
                                       unsigned long size) {
    if (!param || !outBuffer || size == 0) return 0;
    HANDLE h = reinterpret_cast<HANDLE>(param);
    if (!h || h == INVALID_HANDLE_VALUE) return 0;

    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(position);
    ov.OffsetHigh = static_cast<DWORD>(static_cast<unsigned long long>(position) >> 32);
    DWORD read = 0;
    if (ReadFile(h, outBuffer, size, &read, &ov)) {
        return (read == size) ? 1 : 0;
    }
    DWORD e = GetLastError();
    if (e == ERROR_IO_PENDING && GetOverlappedResult(h, &ov, &read, TRUE)) {
        return (read == size) ? 1 : 0;
    }
    return 0;
}

bool ValidateImportPdfFile(const std::filesystem::path& pdfPath, std::wstring* outErr) {
    if (outErr) outErr->clear();
    HANDLE h = CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(pdfPath).c_str(),
                           GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (outErr) {
            DWORD e = GetLastError();
            *outErr = (localization::Text(L"workspace.actions.91db73f5380a").c_str()) +
                      pdfPath.wstring() + L"\n\n" + atomic_write::Win32ErrorMessage(e);
        }
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 5 ||
        size.QuadPart > static_cast<LONGLONG>(std::numeric_limits<unsigned long>::max())) {
        CloseHandle(h);
        if (outErr) *outErr = localization::Text(L"workspace.actions.995575d4aacb").c_str();
        return false;
    }

    char magic[5]{};
    DWORD read = 0;
    OVERLAPPED magicOv{};
    BOOL magicRead = ReadFile(h, magic, 5, &read, &magicOv);
    if (!magicRead && GetLastError() == ERROR_IO_PENDING) {
        magicRead = GetOverlappedResult(h, &magicOv, &read, TRUE);
    }
    if (!magicRead || read != 5 ||
        magic[0] != '%' || magic[1] != 'P' || magic[2] != 'D' || magic[3] != 'F' || magic[4] != '-') {
        CloseHandle(h);
        if (outErr) *outErr = localization::Text(L"workspace.actions.fe2df113e4fe").c_str();
        return false;
    }

    FPDF_FILEACCESS access{};
    access.m_FileLen = static_cast<unsigned long>(size.QuadPart);
    access.m_GetBlock = ImportPdfGetBlockFromHandle;
    access.m_Param = reinterpret_cast<void*>(h);
    std::lock_guard<std::recursive_mutex> pdfiumLock(g_pdfiumMutex);
    FPDF_DOCUMENT doc = FPDF_LoadCustomDocument(&access, nullptr);
    if (!doc) {
        CloseHandle(h);
        if (outErr) *outErr = localization::Text(L"workspace.actions.447941b1dec5").c_str();
        return false;
    }
    const int pageCount = FPDF_GetPageCount(doc);
    FPDF_CloseDocument(doc);
    CloseHandle(h);
    if (pageCount <= 0) {
        if (outErr) *outErr = localization::Text(L"workspace.actions.75e818157a0d").c_str();
        return false;
    }
    return true;
}

bool IsSameImportPath(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    if (lhs.empty() || rhs.empty()) return false;
    return CanonicalOrSelf(lhs) == CanonicalOrSelf(rhs);
}

bool IsCurrentOpenImportDestination(const std::filesystem::path& dest) {
    if (dest.empty()) return false;
    if (!g_currentNotePath.empty() && IsSameImportPath(dest, std::filesystem::path(g_currentNotePath))) {
        return true;
    }
    std::wstring currentPdf = CurrentLogicalPdfPath();
    if (!currentPdf.empty() && IsSameImportPath(dest, std::filesystem::path(currentPdf))) {
        return true;
    }
    return false;
}

bool BackupImportOverwriteTarget(const std::filesystem::path& dest,
                                        std::filesystem::path* outBackup,
                                        std::wstring* outErr) {
    if (outBackup) outBackup->clear();
    if (outErr) outErr->clear();

    std::error_code ec;
    if (!std::filesystem::exists(dest, ec) || ec) {
        return !ec;
    }
    if (g_workspaceRoot.empty() || !EnsureWorkspaceResourceDirs(nullptr)) {
        if (outErr) *outErr = localization::Text(L"workspace.actions.97457f438056").c_str();
        return false;
    }

    std::filesystem::path backupDir = EscapeRootPath() / L"import_overwrite" / NowTimestampString();
    std::filesystem::create_directories(backupDir, ec);
    if (ec) {
        if (outErr) *outErr = (localization::Text(L"workspace.actions.6433f07a6a99").c_str()) +
                              backupDir.wstring();
        return false;
    }

    std::filesystem::path backupPath = atomic_write::MakeUniqueDestInDir(backupDir, dest.filename());
    std::wstring copyErr;
    if (!CopyFileForImportSafely(dest, backupPath, &copyErr)) {
        if (outErr) *outErr = copyErr.empty()
            ? (localization::Text(L"workspace.actions.e116d079d96d").c_str())
            : copyErr;
        return false;
    }
    if (outBackup) *outBackup = backupPath;
    return true;
}

ImportOneResult ImportPreparedFileToDestination(HWND hWnd,
                                                       const std::filesystem::path& copySource,
                                                       const std::filesystem::path& dest,
                                                       std::wstring* outFailure) {
    const auto& ui = GetUiText();
    if (outFailure) outFailure->clear();

    if (copySource.empty() || dest.empty()) {
        if (outFailure) *outFailure = localization::Text(L"workspace.actions.70e485841f70").c_str();
        return ImportOneResult::Failed;
    }
    if (IsSameImportPath(copySource, dest)) {
        return ImportOneResult::Skipped;
    }

    std::error_code ec;
    const bool destExists = std::filesystem::exists(dest, ec);
    if (ec) {
        if (outFailure) *outFailure = localization::Text(L"workspace.actions.9959cd2c9201").c_str();
        return ImportOneResult::Failed;
    }
    if (destExists) {
        if (IsCurrentOpenImportDestination(dest)) {
            if (outFailure) *outFailure = localization::Text(L"workspace.actions.33dcdb477f46").c_str();
            return ImportOneResult::Failed;
        }
        std::wstring msg = ui.msgImportFileOverwrite + L"\n" + dest.wstring();
        SilentDialogOptions confirm;
        confirm.title = ui.menuImportFile;
        confirm.message = msg;
        confirm.kind = SoftNoticeKind::Warning;
        confirm.buttons = SilentDialogButtons::YesNo;
        confirm.defaultResult = SilentDialogResult::No;
        confirm.escapeResult = SilentDialogResult::No;
        if (ShowSilentDialog(hWnd, confirm) != SilentDialogResult::Yes) {
            return ImportOneResult::Skipped;
        }
        std::filesystem::path backupPath;
        std::wstring backupErr;
        if (!BackupImportOverwriteTarget(dest, &backupPath, &backupErr)) {
            if (outFailure) *outFailure = backupErr.empty()
                ? (localization::Text(L"workspace.actions.565d6c1fd36f").c_str())
                : backupErr;
            return ImportOneResult::Failed;
        }
    }

    std::wstring copyErr;
    if (!CopyFileForImportSafely(copySource, dest, &copyErr)) {
        if (outFailure) *outFailure = copyErr.empty()
            ? (localization::Text(L"workspace.actions.9f4d1364d7ec").c_str())
            : copyErr;
        return ImportOneResult::Failed;
    }
    return ImportOneResult::Imported;
}

std::wstring QuoteWindowsCommandLineArg(const std::wstring& arg) {
    std::wstring out = L"\"";
    size_t slashCount = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++slashCount;
            continue;
        }
        if (ch == L'"') {
            out.append(slashCount * 2 + 1, L'\\');
            out.push_back(ch);
            slashCount = 0;
            continue;
        }
        out.append(slashCount, L'\\');
        slashCount = 0;
        out.push_back(ch);
    }
    out.append(slashCount * 2, L'\\');
    out.push_back(L'"');
    return out;
}

bool FileUrlFromLocalPath(const std::filesystem::path& path,
                          std::wstring* outUrl,
                          std::wstring* outErr) {
    if (!outUrl) return false;
    outUrl->clear();
    if (outErr) outErr->clear();

    try {
        std::error_code ec;
        const std::filesystem::path absolutePath = std::filesystem::absolute(path, ec);
        if (ec || absolutePath.empty()) {
            if (outErr) {
                *outErr = L"Failed to resolve a local file URL path: " + path.wstring();
                if (ec) *outErr += L" (" + UTF8ToWide(ec.message()) + L")";
            }
            return false;
        }

        std::wstring w = absolutePath.wstring();
        std::replace(w.begin(), w.end(), L'\\', L'/');
        if (w.size() >= 2 && w[1] == L':') {
            w.insert(w.begin(), L'/');
        }

        std::string utf8 = WideToUTF8(w);
        std::string encoded;
        constexpr char hex[] = "0123456789ABCDEF";
        for (unsigned char c : utf8) {
            const bool keep =
                (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~' ||
                c == '/' || c == ':';
            if (keep) {
                encoded.push_back(static_cast<char>(c));
            } else {
                encoded.push_back('%');
                encoded.push_back(hex[(c >> 4) & 0xF]);
                encoded.push_back(hex[c & 0xF]);
            }
        }
        *outUrl = L"file://" + UTF8ToWide(encoded);
        return true;
    } catch (...) {
        if (outErr) *outErr = L"Failed to construct a local file URL path: " + path.wstring();
        return false;
    }
}

bool WriteLibreOfficeProfilePathConfig(const std::filesystem::path& profileDir,
                                              const std::filesystem::path& officeTempDir,
                                              std::wstring* outErr) {
    std::error_code ec;
    std::filesystem::path userDir = profileDir / L"user";
    std::filesystem::path workDir = officeTempDir / L"work";
    std::filesystem::path backupDir = officeTempDir / L"backup";
    std::filesystem::path tempDir = officeTempDir / L"temp";
    std::filesystem::create_directories(userDir, ec);
    if (!ec) std::filesystem::create_directories(workDir, ec);
    if (!ec) std::filesystem::create_directories(backupDir, ec);
    if (!ec) std::filesystem::create_directories(tempDir, ec);
    if (ec) {
        if (outErr) *outErr = (localization::Text(L"workspace.actions.93a04e4e1379").c_str()) +
                              officeTempDir.wstring();
        return false;
    }

    std::wstring workFileUrl;
    std::wstring backupFileUrl;
    std::wstring tempFileUrl;
    std::wstring urlErr;
    if (!FileUrlFromLocalPath(workDir, &workFileUrl, &urlErr) ||
        !FileUrlFromLocalPath(backupDir, &backupFileUrl, &urlErr) ||
        !FileUrlFromLocalPath(tempDir, &tempFileUrl, &urlErr)) {
        if (outErr) {
            *outErr = (localization::Text(L"workspace.actions.93a04e4e1379").c_str()) +
                      officeTempDir.wstring();
            if (!urlErr.empty()) *outErr += L"\n" + urlErr;
        }
        return false;
    }
    const std::string workUrl = WideToUTF8(workFileUrl);
    const std::string backupUrl = WideToUTF8(backupFileUrl);
    const std::string tempUrl = WideToUTF8(tempFileUrl);

    auto appendSinglePath = [](std::ostringstream& xml,
                               const char* name,
                               const std::string& url) {
        xml << "  <item oor:path=\"/org.openoffice.Office.Paths/Paths/" << name << "\">\n"
            << "    <prop oor:name=\"IsSinglePath\" oor:op=\"fuse\"><value>true</value></prop>\n"
            << "    <prop oor:name=\"WritePath\" oor:op=\"fuse\"><value>" << url << "</value></prop>\n"
            << "    <prop oor:name=\"UserPaths\" oor:op=\"fuse\"><value><it>" << url << "</it></value></prop>\n"
            << "  </item>\n";
    };
    auto appendCommonPath = [](std::ostringstream& xml,
                               const char* group,
                               const std::string& workUrl,
                               const std::string& backupUrl) {
        xml << "  <item oor:path=\"/org.openoffice.Office.Common/Path/" << group << "\">\n"
            << "    <prop oor:name=\"Work\" oor:op=\"fuse\"><value>" << workUrl << "</value></prop>\n"
            << "    <prop oor:name=\"Backup\" oor:op=\"fuse\"><value>" << backupUrl << "</value></prop>\n"
            << "  </item>\n";
    };

    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<oor:data xmlns:oor=\"http://openoffice.org/2001/registry\" "
        << "xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
        << "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n";
    appendSinglePath(xml, "Work", workUrl);
    appendSinglePath(xml, "Backup", backupUrl);
    appendSinglePath(xml, "Temp", tempUrl);
    xml << "  <item oor:path=\"/org.openoffice.Office.Paths/Variables\">\n"
        << "    <prop oor:name=\"Work\" oor:op=\"fuse\"><value>" << workUrl << "</value></prop>\n"
        << "  </item>\n";
    appendCommonPath(xml, "Current", workUrl, backupUrl);
    appendCommonPath(xml, "Default", workUrl, backupUrl);
    xml << "</oor:data>\n";

    const std::filesystem::path configPath = userDir / L"registrymodifications.xcu";
    const std::string data = xml.str();
    std::wstring writeErr;
    if (!atomic_write::AtomicWriteUtf8(configPath, data, userDir, profileDir, &writeErr)) {
        if (outErr) *outErr = (localization::Text(L"workspace.actions.9b716b3f9fd8").c_str()) +
                              configPath.wstring() +
                              (writeErr.empty() ? L"" : L"\n" + writeErr);
        return false;
    }
    return true;
}

void AddLibreOfficeSofficeCandidate(std::vector<std::filesystem::path>& candidates,
                                           const std::filesystem::path& candidate) {
    if (candidate.empty()) return;
    candidates.push_back(candidate);
}

void AddLibreOfficeImageCandidate(std::vector<std::filesystem::path>& candidates,
                                         const std::filesystem::path& imageRoot) {
    if (imageRoot.empty()) return;
    AddLibreOfficeSofficeCandidate(candidates, imageRoot / L"program" / L"soffice.com");
}

void AddCustomLibreOfficeRuntimeCandidates(std::vector<std::filesystem::path>& candidates,
                                                  const std::filesystem::path& base) {
    if (base.empty()) return;
    // New distributables use the short lo/ root so that LibreOffice's own deep
    // registry paths remain extractable on systems using legacy path limits.
    AddLibreOfficeImageCandidate(candidates, base / L"lo");
    // Keep these legacy candidates for previously created release sets.
    AddLibreOfficeImageCandidate(candidates, base / L"third_party" / L"libreoffice" / L"custom_runtime" / L"instdir");
    AddLibreOfficeImageCandidate(candidates, base / L"libreoffice" / L"custom_runtime" / L"instdir");
}

bool IsUsableLibreOfficeSofficeCandidate(const std::filesystem::path& cand) {
    if (cand.empty()) return false;
    if (IsUnsupportedImportSourcePath(cand)) return false;
    bool isReparse = false;
    if (TryIsReparsePointNoFollow(cand, isReparse) && isReparse) return false;
    std::error_code ec;
    return std::filesystem::exists(cand, ec) && !ec &&
           std::filesystem::is_regular_file(cand, ec) && !ec;
}

std::filesystem::path FindLibreOfficeSoffice() {
    std::vector<std::filesystem::path> candidates;
    std::vector<std::filesystem::path> bases;
    std::error_code ec;
    std::filesystem::path exeDir = ExeDirPath();
    if (!exeDir.empty()) {
        bases.push_back(exeDir);
        bases.push_back(exeDir.parent_path());
        bases.push_back(exeDir.parent_path().parent_path());
    }
    for (const auto& base : bases) {
        AddCustomLibreOfficeRuntimeCandidates(candidates, base);
    }

    std::unordered_set<std::wstring> seen;
    for (const auto& cand : candidates) {
        std::wstring key = cand.wstring();
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);
        if (!seen.insert(key).second) continue;
        if (IsUsableLibreOfficeSofficeCandidate(cand)) return cand;
    }
    return {};
}

bool HasOfficeConversionFeature() {
    return !kIsLiteEdition;
}

bool PathIsWithinDirectory(const std::filesystem::path& child,
                                  const std::filesystem::path& parent) {
    if (child.empty() || parent.empty()) return false;
    std::error_code ec;
    std::filesystem::path canonParent = std::filesystem::weakly_canonical(parent, ec);
    if (ec) return false;
    ec.clear();
    std::filesystem::path canonChild = std::filesystem::weakly_canonical(child, ec);
    if (ec) return false;

    std::wstring parentKey = canonParent.wstring();
    std::wstring childKey = canonChild.wstring();
    std::replace(parentKey.begin(), parentKey.end(), L'/', L'\\');
    std::replace(childKey.begin(), childKey.end(), L'/', L'\\');
    if (!parentKey.empty() && parentKey.back() != L'\\') parentKey.push_back(L'\\');
    return childKey.rfind(parentKey, 0) == 0;
}

void RemoveEmptyDirsDeepestFirstBestEffort(std::vector<std::filesystem::path> dirs,
                                                  const std::filesystem::path& allowedRoot) {
    std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) {
        return a.wstring().size() > b.wstring().size();
    });
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
    for (const auto& dir : dirs) {
        if (!PathIsWithinDirectory(dir, allowedRoot)) continue;
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(dir, isReparse) && isReparse) continue;
        std::error_code ec;
        std::filesystem::remove(dir, ec);
    }
}

void CleanupGeneratedPycacheDirBestEffort(const std::filesystem::path& dir,
                                                 const std::filesystem::path& imageRoot) {
    if (dir.empty() || dir.filename() != L"__pycache__" ||
        !PathIsWithinDirectory(dir, imageRoot)) {
        return;
    }
    bool rootIsReparse = false;
    if (TryIsReparsePointNoFollow(dir, rootIsReparse) && rootIsReparse) return;

    std::vector<std::filesystem::path> dirs{dir};
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
        const auto path = it->path();
        if (!PathIsWithinDirectory(path, dir)) {
            it.disable_recursion_pending();
            continue;
        }
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(path, isReparse) && isReparse) {
            it.disable_recursion_pending();
            continue;
        }
        std::error_code stEc;
        if (it->is_directory(stEc) && !stEc) {
            dirs.push_back(path);
            continue;
        }
        stEc.clear();
        if (it->is_regular_file(stEc) && !stEc) {
            std::wstring ext = path.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (ext == L".pyc" || ext == L".pyo") {
                std::error_code rmEc;
                std::filesystem::remove(path, rmEc);
            }
        }
    }
    RemoveEmptyDirsDeepestFirstBestEffort(std::move(dirs), imageRoot);
}

void CleanupLibreOfficePythonCacheBestEffort(const std::filesystem::path& soffice) {
    if (soffice.empty() || soffice.parent_path().empty()) return;
    std::filesystem::path imageRoot = soffice.parent_path().parent_path();
    if (imageRoot.empty()) return;

    std::vector<std::filesystem::path> cacheDirs;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             imageRoot, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (!it->is_directory(ec) || ec) {
            ec.clear();
            continue;
        }
        if (it->path().filename() == L"__pycache__" &&
            PathIsWithinDirectory(it->path(), imageRoot)) {
            cacheDirs.push_back(it->path());
            it.disable_recursion_pending();
        }
    }

    for (const auto& dir : cacheDirs) {
        CleanupGeneratedPycacheDirBestEffort(dir, imageRoot);
    }
}

constexpr wchar_t kOfficeImportTempMarkerFile[] = L".pdf_note_workspace_office_import_tmp";

bool CreateOfficeImportTempMarker(const std::filesystem::path& dir) {
    if (dir.empty()) return false;
    const std::filesystem::path marker = dir / kOfficeImportTempMarkerFile;
    HANDLE h = CreateFileW(ToExtendedWin32PathIfAbsoluteLocal(marker).c_str(),
                           GENERIC_WRITE,
                           0,
                           nullptr,
                           CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const char payload[] = "pdf-note-workspace office import temp\n";
    DWORD written = 0;
    const bool ok = WriteFile(h, payload, static_cast<DWORD>(sizeof(payload) - 1), &written, nullptr) &&
                    written == sizeof(payload) - 1;
    CloseHandle(h);
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(marker, ec);
    }
    return ok;
}

bool HasOfficeImportTempMarker(const std::filesystem::path& dir) {
    if (dir.empty()) return false;
    const std::filesystem::path marker = dir / kOfficeImportTempMarkerFile;
    std::error_code ec;
    return std::filesystem::exists(marker, ec) && !ec &&
           std::filesystem::is_regular_file(marker, ec) && !ec;
}

void CleanupMarkedOfficeImportTempDirBestEffort(const std::filesystem::path& dir,
                                                       const std::filesystem::path& allowedRoot) {
    if (dir.empty() || !PathIsWithinDirectory(dir, allowedRoot) ||
        !HasOfficeImportTempMarker(dir)) {
        return;
    }
    bool rootIsReparse = false;
    if (TryIsReparsePointNoFollow(dir, rootIsReparse) && rootIsReparse) return;

    std::vector<std::filesystem::path> dirs{dir};
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
        const auto path = it->path();
        if (!PathIsWithinDirectory(path, dir)) {
            it.disable_recursion_pending();
            continue;
        }
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(path, isReparse) && isReparse) {
            it.disable_recursion_pending();
            continue;
        }
        std::error_code stEc;
        if (it->is_directory(stEc) && !stEc) {
            dirs.push_back(path);
            continue;
        }
        stEc.clear();
        if (it->is_regular_file(stEc) && !stEc) {
            std::error_code rmEc;
            std::filesystem::remove(path, rmEc);
        }
    }
    RemoveEmptyDirsDeepestFirstBestEffort(std::move(dirs), allowedRoot);
}

std::filesystem::path OfficeImportTempRootPath() {
    std::vector<wchar_t> tempPath(512, L'\0');
    for (;;) {
        const DWORD length = GetTempPathW(static_cast<DWORD>(tempPath.size()), tempPath.data());
        if (length > 0 && length < tempPath.size()) {
            return std::filesystem::path(std::wstring(tempPath.data(), length)) /
                   L"PDFNoteWorkspace" / L"office_import";
        }
        if (length == 0 || tempPath.size() >= 32768) return {};
        // When the supplied buffer is insufficient, GetTempPathW returns the
        // required size including the terminator.
        tempPath.resize(std::min<size_t>(std::max<size_t>(tempPath.size() * 2, length + 1), 32768), L'\0');
    }
}

std::filesystem::path MakeOfficeImportTempRoot(std::wstring* outErr) {
    if (outErr) outErr->clear();
    std::filesystem::path root = OfficeImportTempRootPath();
    if (root.empty()) {
        if (outErr) *outErr = localization::Text(L"workspace.actions.f6429af02b47").c_str();
        return {};
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        if (outErr) *outErr = (localization::Text(L"workspace.actions.4296f2fa17ed").c_str()) +
                              root.wstring();
        return {};
    }
    bool isReparse = false;
    if (!TryIsReparsePointNoFollow(root, isReparse) || isReparse) {
        if (outErr) *outErr = localization::Text(L"workspace.actions.ee81c0d4841b").c_str();
        return {};
    }
    return root;
}

std::filesystem::path MakeUniqueOfficeImportTempDir(std::wstring* outErr) {
    std::filesystem::path root = MakeOfficeImportTempRoot(outErr);
    if (root.empty()) return {};
    DWORD pid = GetCurrentProcessId();
    ULONGLONG tick = GetTickCount64();
    std::error_code ec;
    for (int i = 0; i < 64; ++i) {
        std::filesystem::path dir = root / (NowTimestampString() + L"_" +
                                            std::to_wstring(pid) + L"_" +
                                            std::to_wstring(static_cast<unsigned long long>(tick)) + L"_" +
                                            std::to_wstring(i));
        ec.clear();
        if (std::filesystem::create_directory(dir, ec) && !ec) {
            if (CreateOfficeImportTempMarker(dir)) return dir;
            std::error_code rmEc;
            std::filesystem::remove(dir, rmEc);
        }
    }
    if (outErr) *outErr = localization::Text(L"workspace.actions.c9a52caf90ee").c_str();
    return {};
}

void RemoveOfficeImportTempDirBestEffort(const std::filesystem::path& dir) {
    if (dir.empty()) return;
    std::filesystem::path root = OfficeImportTempRootPath();
    if (root.empty()) return;
    std::error_code ec;
    std::filesystem::path canonRoot = std::filesystem::weakly_canonical(root, ec);
    if (ec) return;
    ec.clear();
    std::filesystem::path canonDir = std::filesystem::weakly_canonical(dir, ec);
    if (ec) return;
    std::wstring rootKey = canonRoot.wstring();
    std::wstring dirKey = canonDir.wstring();
    std::replace(rootKey.begin(), rootKey.end(), L'/', L'\\');
    std::replace(dirKey.begin(), dirKey.end(), L'/', L'\\');
    if (!rootKey.empty() && rootKey.back() != L'\\') rootKey.push_back(L'\\');
    if (dirKey.rfind(rootKey, 0) != 0) return;
    CleanupMarkedOfficeImportTempDirBestEffort(canonDir, canonRoot);
}

bool IsOfficeConversionWaitDispatchMessage(const MSG& msg) {
    switch (msg.message) {
    case WM_PAINT:
    case WM_NCPAINT:
    case WM_ERASEBKGND:
    case WM_TIMER:
    case kMsgPdfVirtualRenderComplete:
        return true;
    default:
        return false;
    }
}

enum class OfficeConversionWaitResult {
    Completed,
    Canceled,
    TimedOut,
    Failed
};

void PumpOfficeConversionWaitMessages() {
    MSG msg{};
    int processed = 0;
    while (processed < 64 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        HWND progressWindow = s_officeConversionProgress.window;
        const bool isProgressMessage =
            progressWindow &&
            (msg.hwnd == progressWindow || (msg.hwnd && IsChild(progressWindow, msg.hwnd)));
        if (isProgressMessage) {
            if (!IsDialogMessageW(progressWindow, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        } else if (msg.message == WM_CLOSE && msg.hwnd == g_hMainWnd) {
            RequestOfficeConversionCancel(true);
        } else if (IsOfficeConversionWaitDispatchMessage(msg)) {
            DispatchMessageW(&msg);
        }
        ++processed;
    }
}

OfficeConversionWaitResult WaitForLibreOfficeLauncher(HANDLE process, DWORD timeoutMs) {
    if (!process) return OfficeConversionWaitResult::Failed;
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (IsOfficeConversionCancelRequested()) {
            return OfficeConversionWaitResult::Canceled;
        }

        DWORD waitMs = 250;
        if (timeoutMs != INFINITE) {
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= timeoutMs) return OfficeConversionWaitResult::TimedOut;
            waitMs = static_cast<DWORD>(std::min<ULONGLONG>(timeoutMs - elapsed, 250));
        }

        const DWORD wait = MsgWaitForMultipleObjects(1, &process, FALSE, waitMs, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) return OfficeConversionWaitResult::Completed;
        if (wait == WAIT_OBJECT_0 + 1) {
            PumpOfficeConversionWaitMessages();
            PulseOfficeConversionProgress(g_hMainWnd);
            continue;
        }
        if (wait == WAIT_TIMEOUT) {
            PulseOfficeConversionProgress(g_hMainWnd);
            continue;
        }
        return OfficeConversionWaitResult::Failed;
    }
}

void AppendOfficeConversionDiagnostic(const std::wstring& message) {
    if (g_workspaceRoot.empty() || message.empty() || !g_config.debugLogs.officeConversion) return;
    const std::filesystem::path logDir = std::filesystem::path(g_workspaceRoot) /
                                         L"__resource__" / L"__log__";
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);
    if (ec) return;
    bool isReparse = false;
    if (!TryIsReparsePointNoFollow(logDir, isReparse) || isReparse) return;

    const std::filesystem::path logPath = logDir / L"office_conversion.log";
    std::ofstream out(logPath, std::ios::binary | std::ios::app);
    if (!out) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t timestamp[48]{};
    swprintf_s(timestamp, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    const std::string utf8 = WideToUTF8(std::wstring(timestamp) + L" " + message + L"\n");
    out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

void AppendOfficeConversionOutputSnapshot(const std::filesystem::path& outputDir) {
    std::wstring message = L"output_snapshot";
    std::error_code ec;
    size_t count = 0;
    for (std::filesystem::directory_iterator it(outputDir, ec), end;
         !ec && it != end && count < 32; it.increment(ec), ++count) {
        std::error_code fileEc;
        const bool regular = it->is_regular_file(fileEc) && !fileEc;
        if (regular) {
            const uintmax_t size = std::filesystem::file_size(it->path(), fileEc);
            if (!fileEc) message += L" | file_size=" + std::to_wstring(size);
        }
    }
    message += L" | entries=" + std::to_wstring(count);
    if (ec) message += L" | enumerate_error=" + std::to_wstring(ec.value());
    AppendOfficeConversionDiagnostic(message);
}

bool WaitForExpectedLibreOfficePdf(const std::filesystem::path& expectedPdf,
                                   DWORD timeoutMs,
                                   bool* outCanceled) {
    if (outCanceled) *outCanceled = false;
    const ULONGLONG start = GetTickCount64();
    uintmax_t observedSize = 0;
    ULONGLONG stableSince = 0;
    bool observed = false;

    for (;;) {
        if (IsOfficeConversionCancelRequested()) {
            if (outCanceled) *outCanceled = true;
            return false;
        }

        std::error_code ec;
        const bool exists = std::filesystem::exists(expectedPdf, ec) && !ec &&
                            std::filesystem::is_regular_file(expectedPdf, ec) && !ec;
        if (exists) {
            const uintmax_t size = std::filesystem::file_size(expectedPdf, ec);
            if (!ec && size >= 5) {
                const ULONGLONG now = GetTickCount64();
                if (observed && observedSize == size && now - stableSince >= 750) {
                    return true;
                }
                if (!observed || observedSize != size) {
                    observedSize = size;
                    stableSince = now;
                    observed = true;
                }
            }
        }

        const ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed >= timeoutMs) return false;
        const DWORD waitMs = static_cast<DWORD>(std::min<ULONGLONG>(timeoutMs - elapsed, 100));
        if (MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLINPUT) == WAIT_OBJECT_0) {
            PumpOfficeConversionWaitMessages();
        }
        PulseOfficeConversionProgress(g_hMainWnd);
    }
}

bool ValidateLibreOfficeHandoffPathBudget(const std::filesystem::path& soffice,
                                          const std::filesystem::path& sourceOfficeCopy,
                                          const std::filesystem::path& outDir,
                                          const std::filesystem::path& profileDir,
                                          std::wstring* outErr);

bool RunLibreOfficePdfConversion(const std::filesystem::path& sourceOfficeCopy,
                                        const std::filesystem::path& outDir,
                                        const std::filesystem::path& profileDir,
                                        std::wstring* outErr,
                                        bool* outCanceled) {
    if (outErr) outErr->clear();
    if (outCanceled) *outCanceled = false;
    std::filesystem::path soffice = FindLibreOfficeSoffice();
    if (soffice.empty()) {
        if (outErr) *outErr = localization::Text(L"workspace.actions.94c16d03c7a1").c_str();
        return false;
    }
    if (!ValidateLibreOfficeHandoffPathBudget(soffice, sourceOfficeCopy, outDir, profileDir, outErr)) {
        return false;
    }
    CleanupLibreOfficePythonCacheBestEffort(soffice);

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        if (outErr) *outErr = (localization::Text(L"workspace.actions.2b4708c6cff7").c_str()) +
                              outDir.wstring();
        return false;
    }
    std::filesystem::create_directories(profileDir, ec);
    if (ec) {
        if (outErr) *outErr = (localization::Text(L"workspace.actions.021e608ea70b").c_str()) +
                              profileDir.wstring();
        return false;
    }
    std::filesystem::path officeLocalDir = profileDir.parent_path() / L"local";
    if (!WriteLibreOfficeProfilePathConfig(profileDir, officeLocalDir, outErr)) {
        return false;
    }

    std::wstring profileUrl;
    std::wstring profileUrlErr;
    if (!FileUrlFromLocalPath(profileDir, &profileUrl, &profileUrlErr)) {
        CleanupLibreOfficePythonCacheBestEffort(soffice);
        if (outErr) {
            *outErr = (localization::Text(L"workspace.actions.021e608ea70b").c_str()) + profileDir.wstring();
            if (!profileUrlErr.empty()) *outErr += L"\n" + profileUrlErr;
        }
        return false;
    }

    std::wstring cmd = QuoteWindowsCommandLineArg(soffice.wstring()) +
        L" --headless --nologo --nodefault --nolockcheck --nofirststartwizard --norestore" +
        L" " + QuoteWindowsCommandLineArg(L"-env:UserInstallation=" + profileUrl) +
        L" --convert-to pdf" +
        L" --outdir " + QuoteWindowsCommandLineArg(outDir.wstring()) +
        L" " + QuoteWindowsCommandLineArg(sourceOfficeCopy.wstring());
    AppendOfficeConversionDiagnostic(L"start");

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    BOOL created = CreateProcessW(soffice.c_str(),
                                  mutableCmd.data(),
                                  nullptr,
                                  nullptr,
                                  FALSE,
                                  CREATE_NO_WINDOW,
                                  nullptr,
                                  outDir.c_str(),
                                  &si,
                                  &pi);
    DWORD createErr = created ? 0 : GetLastError();
    if (!created) {
        CleanupLibreOfficePythonCacheBestEffort(soffice);
        AppendOfficeConversionDiagnostic(L"CreateProcess failed error=" + std::to_wstring(createErr));
        if (outErr) *outErr = (localization::Text(L"workspace.actions.77746f1852f2").c_str()) +
                              soffice.wstring() + L"\n\n" + atomic_write::Win32ErrorMessage(createErr);
        return false;
    }
    AppendOfficeConversionDiagnostic(L"process_started pid=" + std::to_wstring(pi.dwProcessId));

    constexpr DWORD kConvertTimeoutMs = 5 * 60 * 1000;
    ShowSoftNotice(g_hMainWnd,
                   localization::Text(L"workspace.actions.16e28f2bd7d3").c_str(),
                   SoftNoticeKind::Info);
    OfficeConversionWaitResult wait = WaitForLibreOfficeLauncher(pi.hProcess, kConvertTimeoutMs);
    if (wait != OfficeConversionWaitResult::Completed) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CleanupLibreOfficePythonCacheBestEffort(soffice);
        AppendOfficeConversionDiagnostic(L"process_wait_failed result=" + std::to_wstring(static_cast<int>(wait)));
        if (wait == OfficeConversionWaitResult::Canceled) {
            if (outCanceled) *outCanceled = true;
            if (outErr) *outErr = localization::Text(L"workspace.actions.10e2c376855f").c_str();
        } else if (wait == OfficeConversionWaitResult::TimedOut) {
            if (outErr) *outErr = localization::Text(L"workspace.actions.c64dc4973a9a").c_str();
        } else if (outErr) {
            *outErr = localization::Text(L"workspace.actions.71d5b158f91d").c_str();
        }
        return false;
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exitCode != 0) {
        CleanupLibreOfficePythonCacheBestEffort(soffice);
        AppendOfficeConversionDiagnostic(L"process_exit_code=" + std::to_wstring(exitCode));
        if (outErr) *outErr = (localization::Text(L"workspace.actions.d64078e4d8b8").c_str()) +
                              std::to_wstring(exitCode);
        return false;
    }

    // soffice.com can exit before its conversion worker finishes. Keep the private
    // output directory alive until the PDF has stopped growing.
    bool outputWaitCanceled = false;
    const std::filesystem::path expectedPdf = outDir / (sourceOfficeCopy.stem().wstring() + L".pdf");
    const bool outputReady = WaitForExpectedLibreOfficePdf(expectedPdf, 30 * 1000, &outputWaitCanceled);
    AppendOfficeConversionDiagnostic(L"process_exit_code=0 output_ready=" +
                                     std::to_wstring(outputReady ? 1 : 0) +
                                     L" output_wait_canceled=" + std::to_wstring(outputWaitCanceled ? 1 : 0));
    if (!outputReady) AppendOfficeConversionOutputSnapshot(outDir);
    if (outputWaitCanceled) {
        CleanupLibreOfficePythonCacheBestEffort(soffice);
        if (outCanceled) *outCanceled = true;
        if (outErr) *outErr = localization::Text(L"workspace.actions.10e2c376855f").c_str();
        return false;
    }
    CleanupLibreOfficePythonCacheBestEffort(soffice);
    return true;
}

bool FindLibreOfficeGeneratedPdf(const std::filesystem::path& outputDir,
                                 const std::filesystem::path& expectedPdf,
                                 std::filesystem::path* outPdf,
                                 std::wstring* outErr) {
    if (outPdf) outPdf->clear();
    if (outErr) outErr->clear();

    std::error_code ec;
    if (std::filesystem::exists(expectedPdf, ec) && !ec &&
        std::filesystem::is_regular_file(expectedPdf, ec) && !ec) {
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(expectedPdf, isReparse) && !isReparse) {
            if (outPdf) *outPdf = expectedPdf;
            return true;
        }
    }

    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::directory_iterator it(outputDir, ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::filesystem::path candidate = it->path();
        bool isReparse = false;
        if (!TryIsReparsePointNoFollow(candidate, isReparse) || isReparse) continue;
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        std::wstring extension = candidate.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        if (extension == L".pdf") candidates.push_back(candidate);
    }

    if (!ec && candidates.size() == 1) {
        if (outPdf) *outPdf = candidates.front();
        return true;
    }

    if (outErr) {
        *outErr = localization::Text(L"workspace.actions.efb39a85323d").c_str();
        *outErr += localization::Text(L"workspace.actions.c16fa891e7b4").c_str();
        *outErr += expectedPdf.wstring();
        if (ec) {
            *outErr += localization::Text(L"workspace.actions.687bffd72ae9").c_str();
            *outErr += atomic_write::Win32ErrorMessage(ec.value());
        } else if (!candidates.empty()) {
            *outErr += localization::Text(L"workspace.actions.5cf5a5862008").c_str();
            for (const auto& candidate : candidates) {
                *outErr += L"- " + candidate.filename().wstring() + L"\n";
            }
        }
    }
    return false;
}

bool ValidateLibreOfficeConversionPathBudget(const std::filesystem::path& tempDir,
                                             const std::filesystem::path& source,
                                             std::wstring* outErr) {
    // LibreOffice still contains components that do not reliably handle long Win32 paths.
    constexpr size_t kSafePathLength = 220;
    const std::filesystem::path stagedInput = tempDir / L"input" / source.filename();
    const std::filesystem::path expectedPdf = tempDir / L"output" /
                                              (source.stem().wstring() + L".pdf");
    const std::filesystem::path profileProbe = tempDir / L"profile" / L"user" /
        L"uno_packages" / L"cache" / L"registry" /
        L"com.sun.star.comp.deployment.configuration.PackageRegistryBackend" / L"backenddb.xml";
    const std::array<std::filesystem::path, 3> paths = {stagedInput, expectedPdf, profileProbe};
    for (const auto& path : paths) {
        if (path.wstring().size() <= kSafePathLength) continue;
        if (outErr) {
            *outErr = localization::Text(L"workspace.actions.2f67bd1d6fec").c_str();
            *outErr += L"\n" + path.wstring() + L"\nlength=" + std::to_wstring(path.wstring().size());
        }
        return false;
    }
    return true;
}

// LibreOffice may detach its actual converter from soffice.com.  Put the
// launcher in a kill-on-close Job Object before waiting for output so cancel,
// logoff and shutdown apply to the complete process tree, not just the first
// process we happened to create.
struct OfficeConversionWorkerControl {
    std::atomic_bool cancelRequested{false};
    std::mutex jobMutex;
    HANDLE job = nullptr;
};

void AddImportFailure(ImportBatchStats& stats, const std::filesystem::path& src,
                      const std::wstring& detail);
static void ShowOfficeOpenConversionResult(HWND hWnd, const ImportBatchStats& stats, size_t selectedCount);
void ShowOfficeConversionBatchResult(HWND hWnd, const ImportBatchStats& stats, size_t selectedCount);

static void CancelOfficeConversionWorker(OfficeConversionWorkerControl& control) {
    control.cancelRequested.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(control.jobMutex);
    if (control.job) TerminateJobObject(control.job, ERROR_CANCELLED);
}

static bool WaitForOfficeWorkerHandle(HANDLE process, DWORD timeoutMs,
                                      OfficeConversionWorkerControl& control) {
    const ULONGLONG started = GetTickCount64();
    for (;;) {
        if (control.cancelRequested.load(std::memory_order_acquire)) return false;
        const DWORD wait = WaitForSingleObject(process, 100);
        if (wait == WAIT_OBJECT_0) return true;
        if (wait != WAIT_TIMEOUT) return false;
        if (GetTickCount64() - started >= timeoutMs) return false;
    }
}

static bool WaitForOfficeWorkerPdf(const std::filesystem::path& expectedPdf,
                                   DWORD timeoutMs,
                                   OfficeConversionWorkerControl& control) {
    const ULONGLONG started = GetTickCount64();
    uintmax_t size = 0;
    ULONGLONG stableSince = 0;
    bool seen = false;
    for (;;) {
        if (control.cancelRequested.load(std::memory_order_acquire)) return false;
        std::error_code ec;
        if (std::filesystem::exists(expectedPdf, ec) && !ec &&
            std::filesystem::is_regular_file(expectedPdf, ec) && !ec) {
            const uintmax_t next = std::filesystem::file_size(expectedPdf, ec);
            const ULONGLONG now = GetTickCount64();
            if (!ec && next >= 5) {
                if (seen && next == size && now - stableSince >= 750) return true;
                if (!seen || next != size) { size = next; stableSince = now; seen = true; }
            }
        }
        if (GetTickCount64() - started >= timeoutMs) return false;
        Sleep(100);
    }
}

static bool RunLibreOfficePdfConversionInWorker(
    const std::filesystem::path& sourceOfficeCopy,
    const std::filesystem::path& outDir,
    const std::filesystem::path& profileDir,
    OfficeConversionWorkerControl& control,
    std::wstring* outErr) {
    if (outErr) outErr->clear();
    const std::filesystem::path soffice = FindLibreOfficeSoffice();
    if (soffice.empty()) {
        if (outErr) *outErr = localization::Text(L"workspace.actions.94c16d03c7a1").c_str();
        return false;
    }
    if (!ValidateLibreOfficeHandoffPathBudget(soffice, sourceOfficeCopy, outDir, profileDir, outErr)) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (!ec) std::filesystem::create_directories(profileDir, ec);
    if (ec || !WriteLibreOfficeProfilePathConfig(profileDir, profileDir.parent_path() / L"local", outErr)) {
        if (outErr && outErr->empty()) *outErr = localization::Text(L"workspace.actions.2b4708c6cff7").c_str();
        return false;
    }
    std::wstring profileUrl;
    std::wstring profileUrlErr;
    if (!FileUrlFromLocalPath(profileDir, &profileUrl, &profileUrlErr)) {
        if (outErr) {
            *outErr = (localization::Text(L"workspace.actions.021e608ea70b").c_str()) + profileDir.wstring();
            if (!profileUrlErr.empty()) *outErr += L"\n" + profileUrlErr;
        }
        return false;
    }
    std::wstring cmd = QuoteWindowsCommandLineArg(soffice.wstring()) +
        L" --headless --nologo --nodefault --nolockcheck --nofirststartwizard --norestore" +
        L" " + QuoteWindowsCommandLineArg(L"-env:UserInstallation=" + profileUrl) +
        L" --convert-to pdf --outdir " + QuoteWindowsCommandLineArg(outDir.wstring()) +
        L" " + QuoteWindowsCommandLineArg(sourceOfficeCopy.wstring());
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) { if (outErr) *outErr = localization::Text(L"workspace.actions.71d5b158f91d").c_str(); return false; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job); if (outErr) *outErr = localization::Text(L"workspace.actions.71d5b158f91d").c_str(); return false;
    }
    { std::lock_guard<std::mutex> lock(control.jobMutex); control.job = job;
      if (control.cancelRequested.load(std::memory_order_acquire)) TerminateJobObject(job, ERROR_CANCELLED); }
    if (control.cancelRequested.load(std::memory_order_acquire)) {
        { std::lock_guard<std::mutex> lock(control.jobMutex); control.job = nullptr; }
        CloseHandle(job);
        if (outErr) *outErr = localization::Text(L"workspace.actions.10e2c376855f").c_str();
        return false;
    }
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end()); mutableCmd.push_back(L'\0');
    const BOOL created = CreateProcessW(soffice.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, outDir.c_str(), &si, &pi);
    if (!created || !AssignProcessToJobObject(job, pi.hProcess)) {
        if (created) { TerminateProcess(pi.hProcess, ERROR_CANCELLED); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        { std::lock_guard<std::mutex> lock(control.jobMutex); control.job = nullptr; }
        CloseHandle(job);
        if (outErr) *outErr = localization::Text(L"workspace.actions.77746f1852f2").c_str();
        return false;
    }
    const bool launcherExited = WaitForOfficeWorkerHandle(pi.hProcess, 5 * 60 * 1000, control);
    DWORD exitCode = 1;
    if (launcherExited) GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    const std::filesystem::path expected = outDir / (sourceOfficeCopy.stem().wstring() + L".pdf");
    const bool outputReady = launcherExited && exitCode == 0 &&
        WaitForOfficeWorkerPdf(expected, 30 * 1000, control);
    if (!outputReady) {
        const bool canceledByUserOrExit = control.cancelRequested.load(std::memory_order_acquire);
        // A timeout still has to kill the complete process tree, but must stay
        // distinguishable from an explicit cancel in the result summary.
        { std::lock_guard<std::mutex> lock(control.jobMutex);
          if (control.job) TerminateJobObject(control.job, ERROR_TIMEOUT); }
        if (outErr) {
            if (canceledByUserOrExit) {
                *outErr = localization::Text(L"workspace.actions.10e2c376855f").c_str();
            } else if (launcherExited && exitCode != 0) {
                *outErr = (localization::Text(L"workspace.actions.d64078e4d8b8").c_str()) +
                          std::to_wstring(exitCode);
            } else {
                *outErr = localization::Text(L"workspace.actions.c64dc4973a9a").c_str();
            }
        }
    }
    { std::lock_guard<std::mutex> lock(control.jobMutex); control.job = nullptr; }
    CloseHandle(job);
    return outputReady;
}

enum class OfficeBackgroundBatchKind { Standard, OpenMissing };
struct OfficeBackgroundJob {
    OfficeConversionWorkerControl control;
    std::filesystem::path source, sessionRoot, tempDir, generatedPdf;
    std::wstring failure;
    bool canceled = false;
};
struct OfficeBackgroundResult { OfficeBackgroundJob* job = nullptr; };
struct OfficeBackgroundBatch {
    HWND owner = nullptr;
    OfficeBackgroundBatchKind kind = OfficeBackgroundBatchKind::Standard;
    size_t selectedCount = 0, completedCount = 0;
    ImportBatchStats stats;
    std::deque<std::unique_ptr<OfficeBackgroundJob>> pending;
    std::vector<std::unique_ptr<OfficeBackgroundJob>> active;
};
static std::unique_ptr<OfficeBackgroundBatch> s_officeBackgroundBatch;
constexpr size_t kMaxParallelOfficeConversions = 2;
static bool s_officeSecurityCheckNoticeShown = false;

static void RunOfficeBackgroundJob(OfficeBackgroundJob* job) {
    if (!job) return;
    try {
        std::wstring tempErr;
        job->tempDir = MakeUniqueOfficeImportTempDir(&tempErr);
        if (job->tempDir.empty()) job->failure = tempErr;
        if (job->failure.empty() && !ValidateLibreOfficeConversionPathBudget(job->tempDir, job->source, &job->failure)) {}
        const std::filesystem::path input = job->tempDir / L"input";
        const std::filesystem::path output = job->tempDir / L"output";
        const std::filesystem::path profile = job->tempDir / L"profile";
        const std::filesystem::path staged = input / job->source.filename();
        std::error_code ec;
        if (job->failure.empty()) { std::filesystem::create_directories(input, ec); if (ec) job->failure = localization::Text(L"workspace.actions.c529a2026b78").c_str(); }
        if (job->failure.empty() && !CopyFileForImportSafely(job->source, staged, &job->failure)) {}
        if (job->failure.empty() && !office::ValidateOfficePackageForOfflineConversion(staged, &job->failure)) {}
        if (job->failure.empty() && !RunLibreOfficePdfConversionInWorker(staged, output, profile, job->control, &job->failure)) {}
        if (job->failure.empty() && !FindLibreOfficeGeneratedPdf(output, output / (staged.stem().wstring() + L".pdf"), &job->generatedPdf, &job->failure)) {}
        if (job->failure.empty() && !ValidateImportPdfFile(job->generatedPdf, &job->failure)) {}
    } catch (...) {
        job->failure = localization::Text(L"workspace.actions.43223a01deaf").c_str();
    }
    job->canceled = job->control.cancelRequested.load(std::memory_order_acquire);
    auto* result = new OfficeBackgroundResult{job};
    if (!PostMessageW(g_hMainWnd, kMsgOfficeConversionWorkerComplete, 0, reinterpret_cast<LPARAM>(result))) delete result;
}

static void StartMoreOfficeBackgroundJobs() {
    if (!s_officeBackgroundBatch) return;
    while (!s_officeBackgroundBatch->pending.empty() &&
           s_officeBackgroundBatch->active.size() < kMaxParallelOfficeConversions) {
        auto job = std::move(s_officeBackgroundBatch->pending.front());
        s_officeBackgroundBatch->pending.pop_front();
        OfficeBackgroundJob* raw = job.get();
        s_officeBackgroundBatch->active.push_back(std::move(job));
        std::thread(RunOfficeBackgroundJob, raw).detach();
    }
}

static bool StartOfficeBackgroundBatch(HWND hWnd, const std::filesystem::path& sessionRoot,
                                       const std::vector<std::filesystem::path>& sources,
                                       OfficeBackgroundBatchKind kind) {
    if (sources.empty()) return false;
    if (s_officeBackgroundBatch) {
        ShowSoftNotice(hWnd, localization::Text(L"workspace.actions.office_conversion_already_running").c_str(),
                       SoftNoticeKind::Info);
        return false;
    }
    auto batch = std::make_unique<OfficeBackgroundBatch>();
    batch->owner = hWnd; batch->kind = kind; batch->selectedCount = sources.size();
    for (const auto& source : sources) {
        auto job = std::make_unique<OfficeBackgroundJob>(); job->source = source; job->sessionRoot = sessionRoot;
        batch->pending.push_back(std::move(job));
    }
    s_officeBackgroundBatch = std::move(batch);
    BeginOfficeConversionProgress(hWnd, sources.size(), sources.front());
    if (!s_officeSecurityCheckNoticeShown) {
        s_officeSecurityCheckNoticeShown = true;
        ShowSoftNotice(hWnd, localization::Text(L"workspace.actions.office_first_security_check").c_str(),
                       SoftNoticeKind::Info);
    }
    StartMoreOfficeBackgroundJobs();
    return true;
}

bool ValidateLibreOfficeHandoffPathBudget(const std::filesystem::path& soffice,
                                          const std::filesystem::path& sourceOfficeCopy,
                                          const std::filesystem::path& outDir,
                                          const std::filesystem::path& profileDir,
                                          std::wstring* outErr) {
    // Check the paths exactly as they will be handed to LibreOffice. This is
    // deliberately repeated after staging so a future entry point cannot
    // bypass the user-visible safety boundary.
    constexpr size_t kSafePathLength = 220;
    const std::filesystem::path expectedPdf = outDir /
        (sourceOfficeCopy.stem().wstring() + L".pdf");
    const std::filesystem::path profileRegistry = profileDir / L"user" /
        L"registrymodifications.xcu";
    const std::filesystem::path profileProbe = profileDir / L"user" /
        L"uno_packages" / L"cache" / L"registry" /
        L"com.sun.star.comp.deployment.configuration.PackageRegistryBackend" / L"backenddb.xml";
    const std::array<std::pair<const wchar_t*, std::filesystem::path>, 6> paths = {{
        {L"LibreOffice", soffice},
        {L"input", sourceOfficeCopy},
        {L"output folder", outDir},
        {L"expected PDF", expectedPdf},
        {L"profile", profileRegistry},
        {L"profile cache", profileProbe},
    }};
    for (const auto& [role, path] : paths) {
        const size_t length = path.wstring().size();
        if (length <= kSafePathLength) continue;
        if (outErr) {
            *outErr = localization::Text(L"workspace.actions.2f67bd1d6fec").c_str();
            *outErr += L"\n" + std::wstring(role) + L": " + path.wstring();
            *outErr += L"\nlength=" + std::to_wstring(length) +
                       L" (limit=" + std::to_wstring(kSafePathLength) + L")";
        }
        return false;
    }
    return true;
}

bool HasActiveOfficeConversionJobs() { return static_cast<bool>(s_officeBackgroundBatch); }
void CancelOfficeConversionJobsForExit() {
    if (!s_officeBackgroundBatch) return;
    for (auto& job : s_officeBackgroundBatch->pending) CancelOfficeConversionWorker(job->control);
    for (auto& job : s_officeBackgroundBatch->active) CancelOfficeConversionWorker(job->control);
}

void HandleOfficeConversionWorkerCompletion(HWND hWnd, LPARAM payload) {
    std::unique_ptr<OfficeBackgroundResult> result(reinterpret_cast<OfficeBackgroundResult*>(payload));
    if (!result || !s_officeBackgroundBatch || !result->job) return;
    auto& batch = *s_officeBackgroundBatch;
    auto it = std::find_if(batch.active.begin(), batch.active.end(), [&](const auto& p) { return p.get() == result->job; });
    if (it == batch.active.end()) return;
    OfficeBackgroundJob& job = **it;
    if (job.canceled) { batch.stats.canceled = true; ++batch.stats.skipped; }
    else if (!job.failure.empty()) AddImportFailure(batch.stats, job.source, job.failure);
    else {
        std::wstring failure;
        const ImportOneResult imported = ImportPreparedFileToDestination(hWnd, job.generatedPdf,
            PdfDirectoryForSession(job.sessionRoot) / (job.source.stem().wstring() + L".pdf"), &failure);
        if (imported == ImportOneResult::Imported) ++batch.stats.imported;
        else if (imported == ImportOneResult::Skipped) ++batch.stats.skipped;
        else AddImportFailure(batch.stats, job.source, failure);
    }
    RemoveOfficeImportTempDirBestEffort(job.tempDir);
    batch.active.erase(it); ++batch.completedCount;
    if (batch.stats.canceled) {
        while (!batch.pending.empty()) {
            ++batch.stats.skipped;
            ++batch.completedCount;
            batch.pending.pop_front();
        }
    } else StartMoreOfficeBackgroundJobs();
    if (batch.completedCount < batch.selectedCount) {
        UpdateOfficeConversionProgress(hWnd, batch.completedCount + 1, batch.selectedCount,
            !batch.active.empty() ? batch.active.front()->source : std::filesystem::path{});
        return;
    }
    const auto kind = batch.kind; const auto stats = batch.stats; const size_t total = batch.selectedCount;
    s_officeBackgroundBatch.reset(); EndOfficeConversionProgress(hWnd);
    if (stats.imported > 0) RefreshCurrentSessionFiles();
    if (kind == OfficeBackgroundBatchKind::OpenMissing) ShowOfficeOpenConversionResult(hWnd, stats, total);
    else ShowOfficeConversionBatchResult(hWnd, stats, total);
}

ImportOneResult ImportOfficeFileAsPdfToCurrentSession(HWND hWnd,
                                                             const std::filesystem::path& sessionRoot,
                                                             const std::filesystem::path& src,
                                                             std::wstring* outFailure) {
    struct ProgressEndGuard {
        HWND owner = nullptr;
        bool enabled = false;
        ~ProgressEndGuard() {
            if (enabled) EndOfficeConversionProgress(owner);
        }
    };

    const bool ownsProgress = !s_officeConversionProgress.active;
    if (ownsProgress) {
        BeginOfficeConversionProgress(hWnd, 1, src);
    }
    ProgressEndGuard progressGuard{hWnd, ownsProgress};

    if (outFailure) outFailure->clear();
    std::wstring tempErr;
    std::filesystem::path tempDir = MakeUniqueOfficeImportTempDir(&tempErr);
    if (tempDir.empty()) {
        if (outFailure) *outFailure = tempErr;
        return ImportOneResult::Failed;
    }
    if (!ValidateLibreOfficeConversionPathBudget(tempDir, src, outFailure)) {
        RemoveOfficeImportTempDirBestEffort(tempDir);
        return ImportOneResult::Failed;
    }

    std::filesystem::path inputDir = tempDir / L"input";
    std::filesystem::path outputDir = tempDir / L"output";
    std::filesystem::path profileDir = tempDir / L"profile";
    std::error_code ec;
    std::filesystem::create_directories(inputDir, ec);
    if (ec) {
        if (outFailure) *outFailure = localization::Text(L"workspace.actions.c529a2026b78").c_str();
        RemoveOfficeImportTempDirBestEffort(tempDir);
        return ImportOneResult::Failed;
    }

    std::filesystem::path stagedOffice = inputDir / src.filename();
    std::wstring copyErr;
    // Preserve the Office package byte-for-byte for production conversion.
    // Font substitution and WORD JOINER insertion alter DOCX internals and can
    // move Word line breaks, so those transforms remain comparison-test tools only.
    const bool staged = CopyFileForImportSafely(src, stagedOffice, &copyErr);
    if (!staged) {
        if (outFailure) *outFailure = copyErr.empty()
            ? (localization::Text(L"workspace.actions.cea093a731c2").c_str())
            : copyErr;
        RemoveOfficeImportTempDirBestEffort(tempDir);
        return ImportOneResult::Failed;
    }

    std::wstring packageSafetyErr;
    if (!office::ValidateOfficePackageForOfflineConversion(stagedOffice, &packageSafetyErr)) {
        if (outFailure) *outFailure = packageSafetyErr.empty()
            ? (localization::Text(L"workspace.actions.217b4e58bb76").c_str())
            : packageSafetyErr;
        RemoveOfficeImportTempDirBestEffort(tempDir);
        return ImportOneResult::Failed;
    }

    std::wstring convertErr;
    bool canceled = false;
    // All UI entry points intentionally use the same offline LibreOffice path.
    // Keeping the engine choice here prevents D&D, dialogs, and list actions from diverging.
    bool ok = RunLibreOfficePdfConversion(stagedOffice, outputDir, profileDir, &convertErr, &canceled);
    if (!ok) {
        if (outFailure) *outFailure = convertErr;
        RemoveOfficeImportTempDirBestEffort(tempDir);
        return canceled ? ImportOneResult::Canceled : ImportOneResult::Failed;
    }

    const std::filesystem::path expectedPdf = outputDir / (stagedOffice.stem().wstring() + L".pdf");
    std::filesystem::path generatedPdf;
    std::wstring outputErr;
    if (!FindLibreOfficeGeneratedPdf(outputDir, expectedPdf, &generatedPdf, &outputErr)) {
        AppendOfficeConversionDiagnostic(L"output_identification_failed");
        AppendOfficeConversionOutputSnapshot(outputDir);
        if (outFailure) *outFailure = outputErr;
        RemoveOfficeImportTempDirBestEffort(tempDir);
        return ImportOneResult::Failed;
    }

    std::wstring validateErr;
    if (!ValidateImportPdfFile(generatedPdf, &validateErr)) {
        if (outFailure) *outFailure = validateErr;
        RemoveOfficeImportTempDirBestEffort(tempDir);
        return ImportOneResult::Failed;
    }

    std::filesystem::path dest = PdfDirectoryForSession(sessionRoot) / (src.stem().wstring() + L".pdf");
    ImportOneResult result = ImportPreparedFileToDestination(hWnd, generatedPdf, dest, outFailure);
    RemoveOfficeImportTempDirBestEffort(tempDir);
    return result;
}

void AddImportFailure(ImportBatchStats& stats,
                             const std::filesystem::path& src,
                             const std::wstring& detail) {
    ++stats.failed;
    std::wstring line = src.wstring();
    if (!detail.empty()) {
        line += L"\n  ";
        line += detail;
    }
    stats.failures.push_back(std::move(line));
}

ImportOneResult ImportOneFileToCurrentSession(HWND hWnd,
                                                     const std::filesystem::path& sessionRoot,
                                                     const std::filesystem::path& src,
                                                     std::wstring* outFailure,
                                                     bool convertOffice = true) {
    if (outFailure) outFailure->clear();

    if (IsUnsupportedImportSourcePath(src)) {
        if (outFailure) *outFailure = localization::Text(L"workspace.actions.fdb3da0b190c").c_str();
        return ImportOneResult::Failed;
    }

    bool isReparse = false;
    if (TryIsReparsePointNoFollow(src, isReparse) && isReparse) {
        if (outFailure) *outFailure = localization::Text(L"workspace.actions.c561ef5cdc8e").c_str();
        return ImportOneResult::Failed;
    }

    std::error_code ec;
    if (!std::filesystem::exists(src, ec) || ec || !std::filesystem::is_regular_file(src, ec) || ec) {
        if (outFailure) *outFailure = localization::Text(L"workspace.actions.a3425c84bc38").c_str();
        return ImportOneResult::Failed;
    }

    if (IsOfficeImportSourcePath(src) && convertOffice) {
        if (!kOfficePdfConversionApprovedForUse) {
            if (outFailure) *outFailure = localization::Text(L"workspace.actions.26e0e3687e5f").c_str();
            return ImportOneResult::Failed;
        }
        if (!HasOfficeConversionFeature()) {
            if (outFailure) *outFailure = localization::Text(L"workspace.actions.98decb0bc01c").c_str();
            return ImportOneResult::Failed;
        }
        return ImportOfficeFileAsPdfToCurrentSession(hWnd, sessionRoot, src, outFailure);
    }

    std::filesystem::path destDir = ImportDestinationDirForSource(sessionRoot, src);
    std::filesystem::path dest = destDir / src.filename();
    return ImportPreparedFileToDestination(hWnd, src, dest, outFailure);
}

void ShowImportBatchResult(HWND hWnd, const ImportBatchStats& stats, size_t selectedCount) {
    const auto& ui = GetUiText();
    if (stats.canceled) {
        std::wstring msg = localization::Text(L"workspace.actions.21cca5c7f1a8").c_str();
        msg += (localization::Text(L"workspace.actions.56bc3fd9cae5").c_str()) + std::to_wstring(stats.imported);
        msg += (localization::Text(L"workspace.actions.a7f5466d8813").c_str()) + std::to_wstring(stats.skipped);
        ShowSoftNotice(hWnd, msg, SoftNoticeKind::Info);
        return;
    }
    if (stats.failed > 0) {
        std::wstring msg = (stats.imported > 0)
            ? (localization::Text(L"workspace.actions.790fe0cacd12").c_str())
            : ui.errImportFile;
        msg += (localization::Text(L"workspace.actions.56bc3fd9cae5").c_str()) + std::to_wstring(stats.imported);
        msg += (localization::Text(L"workspace.actions.2b8f653eb11b").c_str()) + std::to_wstring(stats.skipped);
        msg += (localization::Text(L"workspace.actions.69aba8f7920b").c_str()) + std::to_wstring(stats.failed);
        if (!stats.failures.empty()) {
            msg += localization::Text(L"workspace.actions.09edf61e698c").c_str();
            const size_t limit = std::min<size_t>(stats.failures.size(), 8);
            for (size_t i = 0; i < limit; ++i) {
                msg += L" - " + stats.failures[i] + L"\n";
            }
            if (stats.failures.size() > limit) {
                msg += L" - ...\n";
            }
        }
        ShowSilentMessageDialog(hWnd, ui.menuImportFile, msg,
                                stats.imported > 0 ? SoftNoticeKind::Warning : SoftNoticeKind::Error);
        return;
    }

    if (selectedCount > 1) {
        std::wstring msg = (localization::Text(L"workspace.actions.79e77c89e1fa").c_str()) +
                           std::to_wstring(stats.imported);
        if (stats.skipped > 0) {
            msg += (localization::Text(L"workspace.actions.2b8f653eb11b").c_str()) + std::to_wstring(stats.skipped);
        }
        ShowSoftNotice(hWnd, msg);
    }
}

std::wstring OfficeOpenConversionTitle() {
    return localization::Text(L"workspace.actions.ed397541e841").c_str();
}

std::wstring OfficeOpenRelativePath(const std::filesystem::path& sessionRoot,
                                           const std::filesystem::path& path) {
    std::filesystem::path rel = path.lexically_relative(sessionRoot);
    std::wstring relText = rel.wstring();
    if (rel.empty() || relText == L".." ||
        relText.rfind(L"..\\", 0) == 0 || relText.rfind(L"../", 0) == 0) {
        return path.filename().wstring();
    }
    return relText;
}

static std::wstring OfficeOpenPathKey(std::filesystem::path path) {
    std::error_code ec;
    path = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        ec.clear();
        path = std::filesystem::absolute(path, ec);
        if (ec) path.clear();
    }
    std::wstring key = path.wstring();
    std::replace(key.begin(), key.end(), L'/', L'\\');
    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
    return key;
}

static bool OfficeOpenPdfExists(const std::filesystem::path& pdfPath) {
    std::error_code ec;
    return std::filesystem::exists(pdfPath, ec) && !ec &&
           std::filesystem::is_regular_file(pdfPath, ec) && !ec;
}

bool HasSameStemPdfForOfficeOpen(const std::filesystem::path& sessionRoot,
                                        const std::filesystem::path& src) {
    const std::wstring pdfName = src.stem().wstring() + L".pdf";
    std::vector<std::filesystem::path> candidates;
    candidates.reserve(3);
    candidates.push_back(src.parent_path() / pdfName);
    candidates.push_back(sessionRoot / pdfName);
    candidates.push_back(sessionRoot / L"pdf" / pdfName);

    std::unordered_set<std::wstring> seen;
    for (const auto& cand : candidates) {
        std::wstring key = OfficeOpenPathKey(cand);
        if (key.empty() || !seen.insert(key).second) continue;
        if (OfficeOpenPdfExists(cand)) return true;
    }
    return false;
}

void CollectOfficeFilesMissingPdfForOpen(const std::filesystem::path& sessionRoot,
                                                std::vector<std::filesystem::path>& out) {
    out.clear();
    std::error_code ec;
    if (!std::filesystem::exists(sessionRoot, ec) || ec ||
        !std::filesystem::is_directory(sessionRoot, ec) || ec) {
        return;
    }

    std::vector<std::filesystem::path> scanDirs;
    scanDirs.reserve(2);
    scanDirs.push_back(sessionRoot);
    scanDirs.push_back(sessionRoot / L"pdf");

    std::vector<std::filesystem::path> candidates;
    std::unordered_set<std::wstring> seenSource;
    for (const auto& dir : scanDirs) {
        ec.clear();
        if (!std::filesystem::exists(dir, ec) || ec ||
            !std::filesystem::is_directory(dir, ec) || ec) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            bool isReparse = false;
            if (TryIsReparsePointNoFollow(entry.path(), isReparse) && isReparse) continue;
            std::error_code stEc;
            if (!entry.is_regular_file(stEc) || stEc) continue;
            const std::filesystem::path src = entry.path();
            if (!IsOfficeImportSourcePath(src)) continue;
            std::wstring fileName = src.filename().wstring();
            if (fileName.rfind(L"~$", 0) == 0) continue;
            if (IsUnsupportedImportSourcePath(src)) continue;
            if (HasSameStemPdfForOfficeOpen(sessionRoot, src)) continue;
            std::wstring sourceKey = OfficeOpenPathKey(src);
            if (sourceKey.empty() || !seenSource.insert(sourceKey).second) continue;
            candidates.push_back(src);
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.wstring() < b.wstring(); });

    std::unordered_set<std::wstring> seenDest;
    out.reserve(candidates.size());
    for (const auto& src : candidates) {
        std::filesystem::path dest = PdfDirectoryForSession(sessionRoot) / (src.stem().wstring() + L".pdf");
        std::wstring destKey = OfficeOpenPathKey(dest);
        if (destKey.empty() || !seenDest.insert(destKey).second) continue;
        out.push_back(src);
    }
}

static void ShowOfficeOpenConversionResult(HWND hWnd, const ImportBatchStats& stats, size_t totalCount) {
    const std::wstring title = OfficeOpenConversionTitle();
    if (stats.canceled) {
        std::wstring msg = localization::Text(L"workspace.actions.f7430cb55ab9").c_str();
        msg += (localization::Text(L"workspace.actions.cb818e7becbc").c_str()) + std::to_wstring(stats.imported);
        msg += (localization::Text(L"workspace.actions.a7f5466d8813").c_str()) + std::to_wstring(stats.skipped);
        ShowSoftNotice(hWnd, msg, SoftNoticeKind::Info);
        return;
    }
    if (stats.failed > 0) {
        std::wstring msg = (stats.imported > 0)
            ? (localization::Text(L"workspace.actions.74776536e502").c_str())
            : (localization::Text(L"workspace.actions.43223a01deaf").c_str());
        msg += (localization::Text(L"workspace.actions.cb818e7becbc").c_str()) + std::to_wstring(stats.imported);
        msg += (localization::Text(L"workspace.actions.2b8f653eb11b").c_str()) + std::to_wstring(stats.skipped);
        msg += (localization::Text(L"workspace.actions.69aba8f7920b").c_str()) + std::to_wstring(stats.failed);
        if (!stats.failures.empty()) {
            msg += localization::Text(L"workspace.actions.09edf61e698c").c_str();
            const size_t limit = std::min<size_t>(stats.failures.size(), 8);
            for (size_t i = 0; i < limit; ++i) {
                msg += L" - " + stats.failures[i] + L"\n";
            }
            if (stats.failures.size() > limit) {
                msg += L" - ...\n";
            }
        }
        ShowSilentMessageDialog(hWnd, title, msg,
                                stats.imported > 0 ? SoftNoticeKind::Warning : SoftNoticeKind::Error);
        return;
    }

    if (totalCount > 1) {
        std::wstring msg = (localization::Text(L"workspace.actions.95bd93f5ef87").c_str()) +
                           std::to_wstring(stats.imported);
        if (stats.skipped > 0) {
            msg += (localization::Text(L"workspace.actions.2b8f653eb11b").c_str()) + std::to_wstring(stats.skipped);
        }
        ShowSoftNotice(hWnd, msg);
    }
}

bool ConvertMissingOfficeFilesUnderDirectory(HWND hWnd, const std::filesystem::path& sessionRoot) {
    if (!kOfficePdfConversionApprovedForUse) {
        ShowSoftNotice(hWnd,
                       localization::Text(L"workspace.actions.26e0e3687e5f").c_str(),
                       SoftNoticeKind::Warning);
        return false;
    }
    if (!HasOfficeConversionFeature()) {
        ShowSoftNotice(hWnd,
                       localization::Text(L"workspace.actions.98decb0bc01c").c_str(),
                       SoftNoticeKind::Info);
        return false;
    }

    std::error_code ec;
    if (sessionRoot.empty() ||
        !std::filesystem::exists(sessionRoot, ec) || ec ||
        !std::filesystem::is_directory(sessionRoot, ec) || ec) {
        std::wstring msg = (localization::Text(L"workspace.actions.7da86d07346c").c_str()) +
                           sessionRoot.wstring();
        ShowSilentMessageDialog(hWnd, OfficeOpenConversionTitle(), msg, SoftNoticeKind::Error,
                                {{L"", sessionRoot.wstring()}});
        return false;
    }

    std::vector<std::filesystem::path> pending;
    CollectOfficeFilesMissingPdfForOpen(sessionRoot, pending);
    if (pending.empty()) {
        ShowSoftNotice(hWnd,
                       localization::Text(L"workspace.actions.4258d9b83683").c_str(),
                       SoftNoticeKind::Info);
        return false;
    }

    if (!StartOfficeBackgroundBatch(hWnd, sessionRoot, pending, OfficeBackgroundBatchKind::OpenMissing)) {
        ShowSoftNotice(hWnd, localization::Text(L"workspace.actions.16e28f2bd7d3").c_str(), SoftNoticeKind::Info);
        return false;
    }
    return true;
}

std::filesystem::path OfficeConversionInitialDirectory() {
    std::error_code ec;
    auto existingDir = [&](const std::wstring& pathText) -> std::filesystem::path {
        if (pathText.empty()) return {};
        std::filesystem::path path(pathText);
        ec.clear();
        if (std::filesystem::exists(path, ec) && !ec &&
            std::filesystem::is_directory(path, ec) && !ec) {
            return path;
        }
        return {};
    };

    std::filesystem::path initial = existingDir(g_currentSessionPath);
    if (!initial.empty()) return initial;
    initial = existingDir(g_currentLecturePath);
    if (!initial.empty()) return initial;
    initial = existingDir(g_workspaceRoot);
    if (!initial.empty()) return initial;
    return DialogWorkspaceInitialFolder();
}

void ShowOfficeConversionBatchResult(HWND hWnd,
                                            const ImportBatchStats& stats,
                                            size_t selectedCount) {
    const auto& ui = GetUiText();
    const std::wstring title = ui.menuConvertOfficeToPdf;
    if (stats.canceled) {
        std::wstring msg = localization::Text(L"workspace.actions.a48dae3b505b").c_str();
        msg += (localization::Text(L"workspace.actions.cb818e7becbc").c_str()) + std::to_wstring(stats.imported);
        msg += (localization::Text(L"workspace.actions.a7f5466d8813").c_str()) + std::to_wstring(stats.skipped);
        ShowSoftNotice(hWnd, msg, SoftNoticeKind::Info);
        return;
    }
    if (stats.failed > 0) {
        std::wstring msg = (stats.imported > 0)
            ? (localization::Text(L"workspace.actions.f2c014dae5c2").c_str())
            : (localization::Text(L"workspace.actions.43223a01deaf").c_str());
        msg += (localization::Text(L"workspace.actions.cb818e7becbc").c_str()) + std::to_wstring(stats.imported);
        msg += (localization::Text(L"workspace.actions.2b8f653eb11b").c_str()) + std::to_wstring(stats.skipped);
        msg += (localization::Text(L"workspace.actions.69aba8f7920b").c_str()) + std::to_wstring(stats.failed);
        if (!stats.failures.empty()) {
            msg += localization::Text(L"workspace.actions.09edf61e698c").c_str();
            const size_t limit = std::min<size_t>(stats.failures.size(), 8);
            for (size_t i = 0; i < limit; ++i) {
                msg += L" - " + stats.failures[i] + L"\n";
            }
            if (stats.failures.size() > limit) {
                msg += L" - ...\n";
            }
        }
        ShowSilentMessageDialog(hWnd, title, msg,
                                stats.imported > 0 ? SoftNoticeKind::Warning : SoftNoticeKind::Error);
        return;
    }

    std::wstring msg = (stats.imported > 0)
        ? ((localization::Text(L"workspace.actions.95bd93f5ef87").c_str()) +
           std::to_wstring(stats.imported))
        : (localization::Text(L"workspace.actions.dbd97e7d6086").c_str());
    if (stats.skipped > 0 || selectedCount > 1) {
        msg += (localization::Text(L"workspace.actions.2b8f653eb11b").c_str()) + std::to_wstring(stats.skipped);
    }
    ShowSoftNotice(hWnd, msg, stats.imported > 0 ? SoftNoticeKind::Info : SoftNoticeKind::Warning);
}

bool ConvertOfficeFilesToCurrentSession(HWND hWnd) {
    const auto& ui = GetUiText();
    if (!kOfficePdfConversionApprovedForUse) {
        ShowSoftNotice(hWnd,
                       localization::Text(L"workspace.actions.26e0e3687e5f").c_str(),
                       SoftNoticeKind::Warning);
        return false;
    }
    if (!HasOfficeConversionFeature()) {
        ShowSoftNotice(hWnd,
                       localization::Text(L"workspace.actions.98decb0bc01c").c_str(),
                       SoftNoticeKind::Info);
        return false;
    }
    if (g_currentSessionPath.empty()) {
        ShowSoftNotice(hWnd, ui.errNewClroNoSession, SoftNoticeKind::Warning);
        return false;
    }

    std::filesystem::path sessionRoot(g_currentSessionPath);
    std::error_code ec;
    if (!std::filesystem::exists(sessionRoot, ec) || ec ||
        !std::filesystem::is_directory(sessionRoot, ec) || ec) {
        std::wstring msg = localization::Format(
            g_config.studentMode ? L"workspace.actions.session.inaccessible.student"
                                 : L"workspace.actions.session.inaccessible.parent",
            {{L"PATH", sessionRoot.wstring()}});
        ShowSilentMessageDialog(hWnd, ui.menuConvertOfficeToPdf, msg, SoftNoticeKind::Error,
                                {{L"", sessionRoot.wstring()}});
        return false;
    }

    auto picked = PickOfficeFilesUnder(hWnd, OfficeConversionInitialDirectory(), ui.menuConvertOfficeToPdf);
    if (picked.empty()) return false;

    std::vector<std::filesystem::path> valid;
    for (size_t i = 0; i < picked.size(); ++i) {
        const auto& path = picked[i];
        std::filesystem::path src(path);
        std::wstring failure;

        if (!IsOfficeImportSourcePath(src)) {
            failure = localization::Text(L"workspace.actions.21b94e09fe6e").c_str();
            ShowSoftNotice(hWnd, failure, SoftNoticeKind::Warning);
            continue;
        }
        if (IsUnsupportedImportSourcePath(src)) {
            failure = localization::Text(L"workspace.actions.fdb3da0b190c").c_str();
            ShowSoftNotice(hWnd, failure, SoftNoticeKind::Warning);
            continue;
        }
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(src, isReparse) && isReparse) {
            failure = localization::Text(L"workspace.actions.c561ef5cdc8e").c_str();
            ShowSoftNotice(hWnd, failure, SoftNoticeKind::Warning);
            continue;
        }
        ec.clear();
        if (!std::filesystem::exists(src, ec) || ec ||
            !std::filesystem::is_regular_file(src, ec) || ec) {
            ec.clear();
            failure = localization::Text(L"workspace.actions.dd51d40539be").c_str();
            ShowSoftNotice(hWnd, failure, SoftNoticeKind::Warning);
            continue;
        }

        valid.push_back(src);
    }
    return StartOfficeBackgroundBatch(hWnd, sessionRoot, valid, OfficeBackgroundBatchKind::Standard);
}

bool ConvertOfficeFileToCurrentSession(HWND hWnd, const std::filesystem::path& src) {
    const auto& ui = GetUiText();
    if (!kOfficePdfConversionApprovedForUse) {
        ShowSoftNotice(hWnd,
                       localization::Text(L"workspace.actions.26e0e3687e5f").c_str(),
                       SoftNoticeKind::Warning);
        return false;
    }
    if (!HasOfficeConversionFeature()) {
        ShowSoftNotice(hWnd,
                       localization::Text(L"workspace.actions.98decb0bc01c").c_str(),
                       SoftNoticeKind::Info);
        return false;
    }
    if (g_currentSessionPath.empty()) {
        ShowSoftNotice(hWnd, ui.errNewClroNoSession, SoftNoticeKind::Warning);
        return false;
    }

    std::filesystem::path sessionRoot(g_currentSessionPath);
    std::error_code ec;
    if (!std::filesystem::exists(sessionRoot, ec) || ec ||
        !std::filesystem::is_directory(sessionRoot, ec) || ec) {
        std::wstring msg = localization::Format(
            g_config.studentMode ? L"workspace.actions.session.inaccessible.student"
                                 : L"workspace.actions.session.inaccessible.parent",
            {{L"PATH", sessionRoot.wstring()}});
        ShowSilentMessageDialog(hWnd, ui.menuConvertOfficeToPdf, msg, SoftNoticeKind::Error,
                                {{L"", sessionRoot.wstring()}});
        return false;
    }

    std::wstring failure;
    if (!IsOfficeImportSourcePath(src)) {
        failure = localization::Text(L"workspace.actions.21b94e09fe6e").c_str();
        ShowSilentMessageDialog(hWnd, ui.menuConvertOfficeToPdf, failure, SoftNoticeKind::Warning,
                                {{L"", src.wstring()}});
        return false;
    }
    if (IsUnsupportedImportSourcePath(src)) {
        failure = localization::Text(L"workspace.actions.fdb3da0b190c").c_str();
        ShowSilentMessageDialog(hWnd, ui.menuConvertOfficeToPdf, failure, SoftNoticeKind::Warning,
                                {{L"", src.wstring()}});
        return false;
    }
    bool isReparse = false;
    if (TryIsReparsePointNoFollow(src, isReparse) && isReparse) {
        failure = localization::Text(L"workspace.actions.c561ef5cdc8e").c_str();
        ShowSilentMessageDialog(hWnd, ui.menuConvertOfficeToPdf, failure, SoftNoticeKind::Warning,
                                {{L"", src.wstring()}});
        return false;
    }
    ec.clear();
    if (!std::filesystem::exists(src, ec) || ec ||
        !std::filesystem::is_regular_file(src, ec) || ec) {
        failure = localization::Text(L"workspace.actions.dd51d40539be").c_str();
        ShowSilentMessageDialog(hWnd, ui.menuConvertOfficeToPdf, failure, SoftNoticeKind::Warning,
                                {{L"", src.wstring()}});
        return false;
    }

    return StartOfficeBackgroundBatch(hWnd, sessionRoot, {src}, OfficeBackgroundBatchKind::Standard);
}

enum class DroppedOfficeImportChoice {
    Convert,
    ImportOriginal,
    Cancel,
};

DroppedOfficeImportChoice ConfirmDroppedOfficeConversion(
    HWND hWnd,
    const std::filesystem::path& sessionRoot,
    const std::vector<std::filesystem::path>& officeFiles,
    bool canConvert) {
    if (officeFiles.empty()) return DroppedOfficeImportChoice::Cancel;

    std::wstring msg;
    if (canConvert) {
        msg = localization::Text(L"workspace.actions.office_drop.convert_prompt");
    } else {
        msg = localization::Text(L"workspace.actions.office_drop.copy_prompt");
    }
    msg += localization::Text(L"workspace.actions.file_list_header");
    const size_t limit = std::min<size_t>(officeFiles.size(), 8);
    for (size_t i = 0; i < limit; ++i) {
        msg += L" - " + OfficeOpenRelativePath(sessionRoot, officeFiles[i]) + L"\n";
    }
    if (officeFiles.size() > limit) {
        msg += L" - ...\n";
    }

    SilentDialogOptions confirm;
    confirm.title = OfficeOpenConversionTitle();
    confirm.message = msg;
    confirm.kind = SoftNoticeKind::Info;
    confirm.buttons = canConvert ? SilentDialogButtons::YesNoCancel : SilentDialogButtons::YesNo;
    confirm.yesLabel = localization::Text(canConvert ? L"workspace.actions.office_drop.convert"
                                                      : L"workspace.actions.office_drop.import_original");
    confirm.noLabel = localization::Text(canConvert ? L"workspace.actions.office_drop.import_original"
                                                     : L"workspace.actions.common.cancel");
    confirm.cancelLabel = localization::Text(L"workspace.actions.common.cancel");
    confirm.defaultResult = SilentDialogResult::No;
    confirm.escapeResult = SilentDialogResult::Cancel;
    const SilentDialogResult result = ShowSilentDialog(hWnd, confirm);
    if (canConvert && result == SilentDialogResult::Yes) return DroppedOfficeImportChoice::Convert;
    if ((canConvert && result == SilentDialogResult::No) ||
        (!canConvert && result == SilentDialogResult::Yes)) {
        return DroppedOfficeImportChoice::ImportOriginal;
    }
    return DroppedOfficeImportChoice::Cancel;
}

static bool ConfirmDroppedFileImport(HWND hWnd, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return false;

    std::wstring message = localization::Text(L"workspace.actions.drop_files.prompt");
    message += localization::Text(L"workspace.actions.file_list_header");
    const size_t limit = std::min<size_t>(paths.size(), 8);
    for (size_t i = 0; i < limit; ++i) {
        message += L" - " + std::filesystem::path(paths[i]).filename().wstring() + L"\n";
    }
    if (paths.size() > limit) message += L" - ...\n";
    message += localization::Text(L"workspace.actions.drop_files.original_unchanged");

    SilentDialogOptions confirm;
    confirm.title = localization::Text(L"workspace.actions.drop_files.title");
    confirm.message = message;
    confirm.kind = SoftNoticeKind::Info;
    confirm.buttons = SilentDialogButtons::YesNo;
    confirm.yesLabel = localization::Text(L"workspace.actions.drop_files.copy_and_import");
    confirm.noLabel = localization::Text(L"workspace.actions.common.cancel");
    confirm.defaultResult = SilentDialogResult::No;
    confirm.escapeResult = SilentDialogResult::No;
    return ShowSilentDialog(hWnd, confirm) == SilentDialogResult::Yes;
}

bool ImportDroppedFilesToCurrentSession(HWND hWnd, const std::vector<std::wstring>& paths) {
    const auto& ui = GetUiText();
    if (paths.empty()) return false;
    if (g_currentSessionPath.empty()) {
        ShowSoftNotice(hWnd, ui.errNewClroNoSession, SoftNoticeKind::Warning);
        return false;
    }

    std::filesystem::path sessionRoot(g_currentSessionPath);
    std::error_code ec;
    if (!std::filesystem::exists(sessionRoot, ec) || !std::filesystem::is_directory(sessionRoot, ec)) {
        ShowSilentMessageDialog(hWnd, ui.menuImportFile, ui.errImportFile, SoftNoticeKind::Error,
                                {{L"", sessionRoot.wstring()}});
        return false;
    }
    if (!ConfirmDroppedFileImport(hWnd, paths)) return false;
    std::wstring workspaceLockError;
    WorkspaceOperationLock workspaceLock(std::filesystem::path(g_workspaceRoot), &workspaceLockError);
    if (!workspaceLock.acquired()) {
        ShowSoftNotice(hWnd, localization::Text(L"workspace.actions.4618bb4d74d4").c_str(),
            SoftNoticeKind::Warning);
        return false;
    }

    std::vector<std::filesystem::path> regularFiles;
    std::vector<std::filesystem::path> officeFiles;
    regularFiles.reserve(paths.size());
    officeFiles.reserve(paths.size());
    for (const auto& path : paths) {
        std::filesystem::path src(path);
        if (IsOfficeImportSourcePath(src)) {
            officeFiles.push_back(std::move(src));
        } else {
            regularFiles.push_back(std::move(src));
        }
    }

    ImportBatchStats stats;
    auto recordImportResult = [&](const std::filesystem::path& src,
                                  ImportOneResult result,
                                  const std::wstring& failure) {
        switch (result) {
        case ImportOneResult::Imported:
            ++stats.imported;
            break;
        case ImportOneResult::Skipped:
            ++stats.skipped;
            break;
        case ImportOneResult::Canceled:
            stats.canceled = true;
            break;
        case ImportOneResult::Failed:
            AddImportFailure(stats, src, failure);
            break;
        }
    };
    auto importOne = [&](const std::filesystem::path& src, bool convertOffice = true) {
        std::wstring failure;
        const ImportOneResult result =
            ImportOneFileToCurrentSession(hWnd, sessionRoot, src, &failure, convertOffice);
        recordImportResult(src, result, failure);
        return result;
    };

    for (const auto& src : regularFiles) {
        importOne(src);
    }

    if (!officeFiles.empty()) {
        const bool canConvert = HasOfficeConversionFeature() && kOfficePdfConversionApprovedForUse;
        const DroppedOfficeImportChoice officeChoice =
            ConfirmDroppedOfficeConversion(hWnd, sessionRoot, officeFiles, canConvert);
        if (officeChoice == DroppedOfficeImportChoice::ImportOriginal) {
            for (const auto& src : officeFiles) importOne(src, /*convertOffice=*/false);
        } else if (officeChoice == DroppedOfficeImportChoice::Convert) {
            // The regular-file portion has already been imported on the UI
            // thread.  Start Office files separately so drag and drop never
            // reintroduces the old synchronous LibreOffice wait loop.
            StartOfficeBackgroundBatch(hWnd, sessionRoot, officeFiles, OfficeBackgroundBatchKind::Standard);
        } else {
            stats.skipped += static_cast<int>(officeFiles.size());
        }
    }

    if (stats.imported > 0) {
        RefreshCurrentSessionFiles();
    }
    if (stats.imported == 0 && stats.failed == 0 && stats.skipped > 0 && paths.size() == 1) {
        ShowSoftNotice(hWnd,
                       localization::Text(L"workspace.actions.a2757ce1ac82").c_str(),
                       SoftNoticeKind::Info);
    } else {
        ShowImportBatchResult(hWnd, stats, paths.size());
    }
    return stats.imported > 0;
}

bool ImportFileToCurrentSession(HWND hWnd) {
    const auto& ui = GetUiText();
    if (g_currentSessionPath.empty()) {
        ShowSoftNotice(hWnd, ui.errNewClroNoSession, SoftNoticeKind::Warning);
        return false;
    }
    std::filesystem::path sessionRoot(g_currentSessionPath);
    std::error_code ec;
    if (!std::filesystem::exists(sessionRoot, ec) || !std::filesystem::is_directory(sessionRoot, ec)) {
        ShowSilentMessageDialog(hWnd, ui.menuImportFile, ui.errImportFile, SoftNoticeKind::Error,
                                {{L"", sessionRoot.wstring()}});
        return false;
    }
    auto initial = DialogDownloadsInitialFolder();

    auto picked = PickFilesUnder(hWnd, initial, ui.menuImportFile);
    if (picked.empty()) return false;

    std::wstring workspaceLockError;
    WorkspaceOperationLock workspaceLock(std::filesystem::path(g_workspaceRoot), &workspaceLockError);
    if (!workspaceLock.acquired()) {
        ShowSoftNotice(hWnd, localization::Text(L"workspace.actions.4618bb4d74d4").c_str(),
            SoftNoticeKind::Warning);
        return false;
    }

    ImportBatchStats stats;
    std::vector<std::filesystem::path> officeFiles;
    for (const auto& path : picked) {
        std::filesystem::path src(path);
        if (IsOfficeImportSourcePath(src)) {
            officeFiles.push_back(std::move(src));
            continue;
        }
        std::wstring failure;
        ImportOneResult result = ImportOneFileToCurrentSession(hWnd, sessionRoot, src, &failure);
        switch (result) {
        case ImportOneResult::Imported:
            ++stats.imported;
            break;
        case ImportOneResult::Skipped:
            ++stats.skipped;
            break;
        case ImportOneResult::Canceled:
            stats.canceled = true;
            break;
        case ImportOneResult::Failed:
            AddImportFailure(stats, src, failure);
            break;
        }
    }

    const bool officeStarted = !officeFiles.empty() && HasOfficeConversionFeature() &&
        kOfficePdfConversionApprovedForUse &&
        StartOfficeBackgroundBatch(hWnd, sessionRoot, officeFiles, OfficeBackgroundBatchKind::Standard);

    if (stats.imported > 0) {
        RefreshCurrentSessionFiles();
    }
    ShowImportBatchResult(hWnd, stats, picked.size());
    return stats.imported > 0 || officeStarted;
}
