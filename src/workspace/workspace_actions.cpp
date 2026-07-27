#include "ui/core/main_window_api.h"
#include "workspace/workspace_config_io.h"
#include "core/app_core.h"
#include "core/ui_prompts.h"
#include "core/ui_notify.h"
#include "core/atomic_write.h"
#include "note_view/note_view.h"
#include <fpdfview.h>
#include <fpdf_save.h>
#include <fpdf_edit.h>
#include "app/main_escape_backup.h"
#include "workspace/workspace_actions.h"
#include <windows.h>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <array>
#include <map>
#include <cwctype>
#include <fstream>
#include "office/docx_space_protection.h"

// file: main/workspace_actions.cppinc
// NOTE: Included by workspace_controller.cppinc. This fragment owns user-triggered
// workspace mutations such as create/import/open helpers.
void ShowNewLectureDialog(HWND owner) {
    SaveNoteIfDirty(owner);
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
        IsEnglishUi()
            ? L"The name contains characters or an ending that Windows cannot use."
            : L"名前に、Windows で使用できない文字または末尾が含まれています。",
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
                       IsEnglishUi() ? L"Windows save dialog is not available."
                                     : L"Windows標準の保存ダイアログを開けません。",
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
                       IsEnglishUi() ? L"Windows save dialog failed."
                                     : L"Windows標準の保存ダイアログで失敗しました。",
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
        IsEnglishUi() ? (g_config.studentMode ? L"Create Session" : L"Create Child Item")
                      : (g_config.studentMode ? L"回次を作成" : L"下位項目を作成"),
        nullptr,
        0,
        0,
        L"");
    if (!picked) return std::nullopt;
    if (!IsPathDirectChildOf(*picked, lectureDir)) {
        ShowSoftNotice(owner,
                        IsEnglishUi() ? L"Create sessions directly under the current lecture folder."
                                      : (g_config.studentMode
                                             ? L"回次は現在の授業フォルダ直下に作成してください。"
                                             : L"下位項目は現在の上位項目フォルダ直下に作成してください。"),
                        SoftNoticeKind::Warning);
        return std::nullopt;
    }
    return picked->filename().wstring();
}

void ShowNewSessionDialog(HWND owner) {
    SaveNoteIfDirty(owner);
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

    // Directory format only.
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
    std::filesystem::create_directories(sessionPath / L"note", ec);
    if (ec) {
        ShowSoftNotice(owner, ui.errSessionCreate, SoftNoticeKind::Error);
        return;
    }
    ReloadSessionsAndSelect(g_currentLecturePath, name, true);
    ClearNoteEditorSilently(owner);
    RefreshStatusDisplay(owner);
}

std::filesystem::path ExeDirPath() {
    wchar_t exePath[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        std::error_code ec;
        auto cur = std::filesystem::current_path(ec);
        return ec ? std::filesystem::path{} : cur;
    }
    return std::filesystem::path(exePath).parent_path();
}

bool AddTempExternalLecture(HWND owner,
                                   const std::wstring& lectureDir,
                                   bool persistAndRefresh = true) {
    const auto& ui = GetUiText();
    std::filesystem::path p = std::filesystem::path(lectureDir);
    if (lectureDir.rfind(L"\\\\", 0) == 0) {
        const wchar_t* msg = IsEnglishUi()
            ? L"UNC/device paths are not supported."
            : L"UNC/デバイスパスは使用できません。";
        ShowSoftNotice(owner, msg, SoftNoticeKind::Warning);
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || !std::filesystem::is_directory(p, ec)) {
        std::wstring msg = L"フォルダが見つかりません。\n\n" + p.wstring();
        ShowSilentMessageDialog(owner, ui.menuFile, msg, SoftNoticeKind::Warning);
        return false;
    }
    if (!VerifyWorkspaceWritableForEditing(owner)) return false;
    if (!VerifyDirReadableWritableForEditing(
            owner, p, g_config.studentMode ? L"外部授業フォルダ" : L"外部上位項目フォルダ",
            g_config.studentMode ? L"External lecture folder" : L"External parent item folder")) {
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
        const wchar_t* msg = IsEnglishUi()
            ? L"This path overlaps with the current workspace."
            : L"このパスは現在のワークスペースと重複しています。";
        ShowSoftNotice(owner, msg, SoftNoticeKind::Info);
        return false;
    }

    for (const auto& item : g_tempExternalLectures) {
        if (item.path == canon) {
            const wchar_t* msg = IsEnglishUi()
                ? L"This temporary external lecture is already added."
                : (g_config.studentMode ? L"この一時外部授業はすでに追加されています。"
                                        : L"この一時外部上位項目はすでに追加されています。");
            ShowSoftNotice(owner, msg, SoftNoticeKind::Info);
            return false;
        }
    }
    for (const auto& item : g_lectures) {
        if (item == canon) {
            const wchar_t* msg = IsEnglishUi()
                ? L"This lecture folder is already listed."
                : (g_config.studentMode ? L"この授業フォルダはすでに一覧にあります。"
                                        : L"この上位項目フォルダはすでに一覧にあります。");
            ShowSoftNotice(owner, msg, SoftNoticeKind::Info);
            return false;
        }
    }

    g_tempExternalLectures.push_back({canon});
    if (!persistAndRefresh) return true;

    if (!PersistTempExternalLecturesToSetup()) {
        if (owner) {
            std::wstring msg = IsEnglishUi()
                ? L"Failed to save external lecture path (it may not persist after exit)."
                : (g_config.studentMode
                       ? L"外部授業パスを保存できませんでした（次回終了後に消える可能性があります）。"
                       : L"外部上位項目パスを保存できませんでした（次回終了後に消える可能性があります）。");
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

    if (!PersistTempExternalLecturesToSetup()) {
        if (owner) {
            std::wstring msg = IsEnglishUi()
                ? L"Failed to save external lecture paths (they may not persist after exit)."
                : (g_config.studentMode
                       ? L"外部授業パスを保存できませんでした（次回終了後に消える可能性があります）。"
                       : L"外部上位項目パスを保存できませんでした（次回終了後に消える可能性があります）。");
            ShowSilentMessageDialog(owner, GetUiText().menuAddTempExternalLecture, msg, SoftNoticeKind::Warning);
        }
    }
    s_ignoreLectureSelChange = true;
    LoadLectures();
    s_ignoreLectureSelChange = false;
    if (owner) {
        RefreshStatusDisplay(owner);
        std::wstring msg = IsEnglishUi()
            ? (L"Added temporary external lectures: " + std::to_wstring(added))
            : ((g_config.studentMode ? L"一時外部授業を追加しました: " : L"一時外部上位項目を追加しました: ") +
               std::to_wstring(added));
        ShowSoftNotice(owner, msg, SoftNoticeKind::Info);
    }
}

std::optional<std::wstring> PickWorkspaceFolder(HWND parent) {
    auto result = PromptExistingLocalPath(parent, DialogWorkspaceInitialFolder(),
                                          GetUiText().menuOpenWs, /*requireDirectory=*/true);
    if (result) {
        // No-network requirement: block UNC / device prefix paths for workspace root selection.
        if (result->rfind(L"\\\\", 0) == 0) {
            const wchar_t* msg = IsEnglishUi()
                ? L"UNC/device paths are not supported as a workspace folder."
                : L"UNC/デバイスパスはワークスペースフォルダとして使用できません。";
            ShowSoftNotice(parent, msg, SoftNoticeKind::Warning);
            return std::nullopt;
        }
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(std::filesystem::path(*result), isReparse) && isReparse) {
            const wchar_t* msg = IsEnglishUi()
                ? L"Reparse point folders (junction/symlink) are not supported as a workspace folder."
                : L"ジャンクション/シンボリックリンク等（reparse point）のフォルダはワークスペースとして使用できません。";
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
    return lower == L".clro" || lower == L".txt" || lower == L".csv" || lower == L".md" || lower == L".tex";
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
    return IsEnglishUi() ? L"note" : L"ノート";
}

bool ValidateNewNoteFileName(HWND owner, const std::wstring& name) {
    return ValidateCreateFileSystemName(owner, name, IsEnglishUi() ? L"Create Note" : L"ノート作成");
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
                       IsEnglishUi() ? L"Enter a note name." : L"ノート名を入力してください。",
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
            IsEnglishUi() ? L"Create Note" : L"ノート作成",
            IsEnglishUi()
                ? L"Use one of these extensions: .md, .clro, .txt, .tex, .csv"
                : L"拡張子は .md / .clro / .txt / .tex / .csv のいずれかにしてください。",
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
    // Notes are created under the note directory so the note list picks them up reliably.
    std::filesystem::path noteDir = dir / L"note";
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
                           IsEnglishUi() ? L"A note with the same name already exists."
                                         : L"同名のノートが存在します。",
                           SoftNoticeKind::Warning);
            return;
        }
        if (ec) {
            ShowSilentMessageDialog(hWnd, IsEnglishUi() ? L"Error" : L"エラー", ui.errNewClroCreate, SoftNoticeKind::Error);
            return;
        }
        DWORD createError = ERROR_SUCCESS;
        if (!TryCreateEmptyNoteFile(target, &createError)) {
            ShowSilentMessageDialog(hWnd, IsEnglishUi() ? L"Error" : L"エラー",
                           createError == ERROR_FILE_EXISTS
                               ? (IsEnglishUi() ? L"A note with the same name already exists."
                                                : L"同名のノートが存在します。")
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
        ShowSilentMessageDialog(hWnd, IsEnglishUi() ? L"Error" : L"エラー", ui.errNewClroCreate, SoftNoticeKind::Error);
        return;
    }

    RefreshCurrentSessionFiles();
    LoadNoteFile(hWnd, target.wstring());
    SyncBottomPaneAfterNoteLoad(hWnd);
    RefreshStatusDisplay(hWnd);
    ShowSoftNotice(hWnd,
                   IsEnglishUi() ? L"Note created." : L"ノートを作成しました。");
}

void CreateNewClroInSession(HWND hWnd) {
    CreateNewNoteInSession(hWnd, s_newNoteExtension, nullptr);
}

std::filesystem::path CurrentNoteDirectory() {
    if (g_currentSessionPath.empty()) return {};
    return std::filesystem::path(g_currentSessionPath) / L"note";
}


std::wstring DefaultBlankPdfFileName() {
    return L"blank.pdf";
}

std::filesystem::path CurrentPdfDirectory() {
    if (g_currentSessionPath.empty()) return {};
    return std::filesystem::path(g_currentSessionPath) / L"pdf";
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
        IsEnglishUi() ? L"Create Blank PDF" : L"白紙PDFを作成",
        IsEnglishUi() ? L"PDF file name" : L"PDFファイル名",
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
            ShowSoftNotice(owner,
                            IsEnglishUi() ? L"Create PDFs inside the current session's pdf folder."
                                          : (g_config.studentMode
                                                 ? L"PDFは現在の回次の pdf フォルダ内に作成してください。"
                                                 : L"PDFは現在の下位項目の pdf フォルダ内に作成してください。"),
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
                       IsEnglishUi() ? L"Use the .pdf extension."
                                     : L"拡張子は .pdf にしてください。",
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
                       IsEnglishUi() ? L"A PDF with the same name already exists."
                                     : L"同名のPDFが存在します。",
                       SoftNoticeKind::Warning);
        return std::nullopt;
    }
    if (ec) {
        ShowSoftNotice(owner,
                       IsEnglishUi() ? L"Could not check the destination PDF path."
                                     : L"PDF保存先を確認できませんでした。",
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
        if (outErr) *outErr = IsEnglishUi() ? L"Created PDF size is invalid."
                                            : L"作成したPDFのサイズが不正です。";
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
        if (outErr) *outErr = IsEnglishUi() ? L"PDFium could not reopen the created PDF."
                                            : L"作成したPDFをPDFiumで再読み込みできませんでした。";
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
        *outErr = IsEnglishUi() ? L"Created PDF validation failed."
                                : L"作成したPDFの検証に失敗しました。";
    }
    return ok;
}

bool SaveBlankPdfDocumentAtomically(const std::filesystem::path& dest,
                                           const BlankPdfSpec& spec,
                                           std::wstring* outErr) {
    if (outErr) outErr->clear();
    if (dest.empty() || dest.parent_path().empty()) {
        if (outErr) *outErr = IsEnglishUi() ? L"Invalid PDF destination."
                                            : L"PDF保存先が不正です。";
        return false;
    }
    std::error_code ec;
    if (std::filesystem::exists(dest, ec) && !ec) {
        if (outErr) *outErr = IsEnglishUi() ? L"Destination PDF already exists."
                                            : L"保存先PDFはすでに存在します。";
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
            *outErr = IsEnglishUi() ? L"Failed to create the blank PDF."
                                    : L"白紙PDFを作成できませんでした。";
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
        if (outErr) *outErr = IsEnglishUi() ? L"Destination PDF already exists."
                                            : L"保存先PDFはすでに存在します。";
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
                       IsEnglishUi() ? L"Failed to create the PDF folder."
                                     : L"PDFフォルダを作成できませんでした。",
                       SoftNoticeKind::Error);
        return;
    }

    std::wstring input;
    const std::wstring initial = L"A4, 1";
    const std::wstring title = ui.menuCreateBlankPdf +
        (IsEnglishUi() ? L" (A4, pages / 210x297mm, pages)"
                       : L"（A4, ページ数 / 210x297mm, ページ数）");
    if (!PromptSimpleText(hWnd, title, initial, input)) return;
    BlankPdfSpec spec{};
    if (!TryParseBlankPdfSpec(input, &spec)) {
        ShowSoftNotice(hWnd,
                       IsEnglishUi()
                           ? L"Enter size and pages like: A4, 3 or 210x297mm, 3."
                           : L"サイズとページ数は A4, 3 または 210x297mm, 3 のように入力してください。",
                       SoftNoticeKind::Warning);
        return;
    }

    auto dest = PromptBlankPdfPath(hWnd, pdfDir);
    if (!dest) return;
    std::wstring err;
    if (!SaveBlankPdfDocumentAtomically(*dest, spec, &err)) {
        ShowSilentMessageDialog(hWnd,
                                ui.menuCreateBlankPdf,
                                err.empty() ? (IsEnglishUi() ? L"Failed to create the blank PDF."
                                                             : L"白紙PDFを作成できませんでした。")
                                            : err,
                                SoftNoticeKind::Error);
        return;
    }
    RefreshCurrentSessionFiles();
    OpenPdfIfDifferent(hWnd, dest->wstring());
    RefreshStatusDisplay(hWnd);
    ShowSoftNotice(hWnd,
                   IsEnglishUi() ? L"Blank PDF created."
                                 : L"白紙PDFを作成しました。");
}

std::optional<std::wstring> PromptNoteFileNameWithSaveDialog(HWND owner,
                                                                    const std::filesystem::path& noteDir,
                                                                    const std::wstring& initialName,
                                                                    const std::wstring& defaultExt) {
    COMDLG_FILTERSPEC filters[] = {
        { L"Markdown note (*.md)", L"*.md" },
        { L"CLRO note (*.clro)", L"*.clro" },
        { L"Text note (*.txt)", L"*.txt" },
        { L"TeX note (*.tex)", L"*.tex" },
        { L"CSV note (*.csv)", L"*.csv" },
    };
    UINT filterIndex = 1;
    std::wstring ext = ToLowerAscii(defaultExt);
    if (ext == L".clro") filterIndex = 2;
    else if (ext == L".txt") filterIndex = 3;
    else if (ext == L".tex") filterIndex = 4;
    else if (ext == L".csv") filterIndex = 5;
    std::wstring defaultExtNoDot = ext;
    if (!defaultExtNoDot.empty() && defaultExtNoDot.front() == L'.') {
        defaultExtNoDot.erase(defaultExtNoDot.begin());
    }

    auto picked = PromptCreatePathWithSaveDialog(
        owner,
        noteDir,
        initialName,
        IsEnglishUi() ? L"Create Note" : L"ノートを作成",
        filters,
        static_cast<UINT>(std::size(filters)),
        filterIndex,
        defaultExtNoDot);
    if (!picked) return std::nullopt;
    if (!IsPathDirectChildOf(*picked, noteDir)) {
        ShowSoftNotice(owner,
                        IsEnglishUi() ? L"Create notes inside the current session's note folder."
                                      : (g_config.studentMode
                                             ? L"ノートは現在の回次の note フォルダ内に作成してください。"
                                             : L"ノートは現在の下位項目の note フォルダ内に作成してください。"),
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
                       IsEnglishUi() ? L"Use .md, .clro, .txt, .tex, or .csv."
                                     : L"拡張子は .md / .clro / .txt / .tex / .csv のいずれかにしてください。",
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
    PushUniqueSuggestion(suggestions, (IsEnglishUi() ? L"note" : L"ノート") + ext);
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
        { 4, L".tex" },
        { 5, L".csv" },
    };
    for (const auto& item : kExtItems) {
        UINT flags = MF_STRING;
        if (ToLowerAscii(s_newNoteExtension) == item.ext) flags |= MF_CHECKED;
        AppendMenuW(extMenu, flags, item.id, item.ext + 1);
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(extMenu),
                IsEnglishUi() ? L"Change extension" : L"拡張子を変更する");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 10,
                IsEnglishUi() ? L"Create with name..." : L"名前を付けて作成...");

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
                           IsEnglishUi()
                               ? (L"New note extension: " + s_newNoteExtension)
                               : (L"新規ノートの拡張子: " + s_newNoteExtension),
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
            IsEnglishUi() ? L"Create Note" : L"ノート作成",
            IsEnglishUi() ? L"Note name" : L"ノート名",
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
    if (IsPdfFile(src) || IsImageFile(src) || ext == L".clrop") {
        return sessionRoot / L"pdf";
    }
    if (IsNoteFile(src)) {
        return sessionRoot / L"note";
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
        if (outErr) *outErr = IsEnglishUi() ? L"Invalid import path." : L"取り込みパスが不正です。";
        return false;
    }

    if (!dest.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec) {
            if (outErr) *outErr = (IsEnglishUi() ? L"Failed to create destination folder:\n" : L"取り込み先フォルダを作成できません:\n") +
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
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                   nullptr);
    if (srcHandle == INVALID_HANDLE_VALUE) {
        if (outErr) {
            DWORD e = GetLastError();
            *outErr = (IsEnglishUi() ? L"Failed to open source file:\n" : L"取り込み元ファイルを開けません:\n") +
                      src.wstring() + L"\n\n" + atomic_write::Win32ErrorMessage(e);
        }
        CleanupImportTempFile(&tmpHandle, tmp);
        return false;
    }

    LARGE_INTEGER expectedSize{};
    if (!GetFileSizeEx(srcHandle, &expectedSize) || expectedSize.QuadPart < 0) {
        if (outErr) {
            DWORD e = GetLastError();
            *outErr = (IsEnglishUi() ? L"Failed to read source file size:\n" : L"取り込み元ファイルサイズを確認できません:\n") +
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
                *outErr = (IsEnglishUi() ? L"Failed while reading source file:\n" : L"取り込み元ファイルの読み込み中に失敗しました:\n") +
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
                *outErr = (IsEnglishUi() ? L"Failed while writing imported file:\n" : L"取り込み先への書き込み中に失敗しました:\n") +
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
        if (outErr) *outErr = IsEnglishUi() ? L"Source file changed during import." : L"取り込み中に元ファイルが変化しました。";
        CleanupImportTempFile(&tmpHandle, tmp);
        return false;
    }
    if (!FlushFileBuffers(tmpHandle)) {
        if (outErr) {
            DWORD e = GetLastError();
            *outErr = (IsEnglishUi() ? L"Failed to flush imported file:\n" : L"取り込み一時ファイルのフラッシュに失敗しました:\n") +
                      tmp.wstring() + L"\n\n" + atomic_write::Win32ErrorMessage(e);
        }
        CleanupImportTempFile(&tmpHandle, tmp);
        return false;
    }
    if (!CloseHandle(tmpHandle)) {
        tmpHandle = INVALID_HANDLE_VALUE;
        if (outErr) {
            DWORD e = GetLastError();
            *outErr = (IsEnglishUi() ? L"Failed to close imported temp file:\n" : L"取り込み一時ファイルを閉じられません:\n") +
                      tmp.wstring() + L"\n\n" + atomic_write::Win32ErrorMessage(e);
        }
        CleanupImportTempFile(nullptr, tmp);
        return false;
    }
    tmpHandle = INVALID_HANDLE_VALUE;

    std::wstring replaceErr;
    if (!atomic_write::AtomicReplaceFile(dest, tmp, dest.parent_path(), &replaceErr)) {
        if (outErr) *outErr = replaceErr.empty()
            ? (IsEnglishUi() ? L"Failed to replace imported file." : L"取り込み先ファイルの置換に失敗しました。")
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
        const wchar_t* msg = IsEnglishUi()
            ? L"UNC/device paths are not supported."
            : L"UNC/デバイスパスは使用できません。";
        ShowSoftNotice(owner, msg, SoftNoticeKind::Warning);
        return false;
    }
    bool isReparse = false;
    if (TryIsReparsePointNoFollow(src, isReparse) && isReparse) {
        const wchar_t* msg = IsEnglishUi()
            ? L"Reparse point folders (junction/symlink) are not supported."
            : L"ジャンクション/シンボリックリンク等（reparse point）のフォルダは使用できません。";
        ShowSoftNotice(owner, msg, SoftNoticeKind::Warning);
        return false;
    }
    if (IsWorkspaceReservedImportDirectoryName(src)) {
        const wchar_t* msg = IsEnglishUi()
            ? L"Directories named __resource__ cannot be imported because they are reserved by this app."
            : L"__resource__ という名前のフォルダはアプリ予約領域のため取り込めません。";
        ShowSoftNotice(owner, msg, SoftNoticeKind::Warning);
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(src, ec) || ec ||
        !std::filesystem::is_directory(src, ec) || ec) {
        std::wstring msg = IsEnglishUi()
            ? L"Source directory was not found:\n\n"
            : L"取り込み元フォルダが見つかりません:\n\n";
        msg += src.wstring();
        ShowSilentMessageDialog(owner, title, msg, SoftNoticeKind::Warning);
        return false;
    }
    std::wstring readErr;
    if (!TryOpenDirForList(src, &readErr)) {
        std::wstring msg = IsEnglishUi()
            ? L"Source directory is not accessible:\n\n"
            : L"取り込み元フォルダにアクセスできません:\n\n";
        msg += src.wstring();
        if (!readErr.empty()) msg += L"\n\n" + readErr;
        ShowSilentMessageDialog(owner, title, msg, SoftNoticeKind::Warning);
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
        if (outErr) *outErr = IsEnglishUi() ? L"Invalid import path." : L"取り込みパスが不正です。";
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
                *outErr = IsEnglishUi()
                    ? L"Reparse point entries cannot be imported:\n\n"
                    : L"ジャンクション/シンボリックリンク等（reparse point）は取り込めません:\n\n";
                *outErr += p.wstring();
            }
            return false;
        }

        std::error_code relEc;
        std::filesystem::path rel = std::filesystem::relative(p, src, relEc);
        const bool relEscapesRoot = !rel.empty() && rel.begin()->wstring() == L"..";
        if (relEc || rel.empty() || relEscapesRoot) {
            if (outErr) {
                *outErr = IsEnglishUi()
                    ? L"Failed to resolve an import path safely:\n\n"
                    : L"取り込みパスを安全に解決できません:\n\n";
                *outErr += p.wstring();
            }
            return false;
        }

        std::error_code stEc;
        if (it->is_directory(stEc) && !stEc) {
            if (IsWorkspaceReservedImportDirectoryName(p)) {
                if (outErr) {
                    *outErr = IsEnglishUi()
                        ? L"Directories named __resource__ cannot be imported because they are reserved by this app:\n\n"
                        : L"__resource__ という名前のフォルダはアプリ予約領域のため取り込めません:\n\n";
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
            *outErr = IsEnglishUi()
                ? L"Import rollback was skipped because the destination path is outside the allowed root."
                : L"取り込み先が許可された範囲外のため、ロールバック削除を行いませんでした。";
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
            errors += IsEnglishUi()
                ? L"Skipped rollback for an out-of-scope imported file:\n" + file.wstring()
                : L"範囲外の取り込みファイルはロールバック削除しませんでした:\n" + file.wstring();
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
            errors += IsEnglishUi()
                ? L"Skipped rollback for an out-of-scope imported folder:\n" + dir.wstring()
                : L"範囲外の取り込みフォルダはロールバック削除しませんでした:\n" + dir.wstring();
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
        if (outErr) *outErr = IsEnglishUi() ? L"Invalid import destination." : L"取り込み先が不正です。";
        return false;
    }
    if (IsPathUnderRoot(plan.destRoot, plan.sourceRoot) ||
        IsPathUnderRoot(plan.sourceRoot, plan.destRoot)) {
        if (outErr) {
            *outErr = IsEnglishUi()
                ? L"Source and destination overlap. Import was canceled."
                : L"取り込み元と取り込み先が重複するため中止しました。";
        }
        return false;
    }

    std::error_code ec;
    if (std::filesystem::exists(plan.destRoot, ec) && !ec) {
        if (outErr) {
            *outErr = (IsEnglishUi() ? L"Destination already exists:\n\n" : L"取り込み先が既に存在します:\n\n") +
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
            *outErr = (IsEnglishUi() ? L"Failed to create destination folder:\n" : L"取り込み先フォルダを作成できません:\n") +
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
            std::wstring err = (IsEnglishUi() ? L"Failed to create imported subfolder:\n" : L"取り込み先サブフォルダを作成できません:\n") +
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
                ? (IsEnglishUi() ? L"Failed to copy an imported file." : L"取り込みファイルのコピーに失敗しました。")
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
                                (IsEnglishUi() ? L"Failed to create lecture destination folder:\n"
                                               : (g_config.studentMode ? L"授業取り込み先フォルダを作成できません:\n"
                                                                       : L"上位項目取り込み先フォルダを作成できません:\n")) +
                                    classesPath.wstring(),
                                SoftNoticeKind::Error);
        return false;
    }
    if (!VerifyDirReadableWritableForEditing(hWnd, classesPath,
                                             g_config.studentMode ? L"授業取り込み先フォルダ" : L"上位項目取り込み先フォルダ",
                                             g_config.studentMode ? L"Lecture import destination" : L"Parent item import destination")) {
        return false;
    }
    auto src = PickDirectoryImportSource(hWnd, title, DialogDownloadsInitialFolder());
    if (!src) return false;

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
    ShowSoftNotice(hWnd,
                   IsEnglishUi() ? L"Imported the directory as a lecture (copied)."
                                  : (g_config.studentMode ? L"フォルダを授業として取り込みました（コピーを実行しました）。"
                                                          : L"フォルダを上位項目として取り込みました（コピーを実行しました）。"),
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
                                             g_config.studentMode ? L"回次取り込み先授業フォルダ" : L"下位項目取り込み先上位項目フォルダ",
                                             g_config.studentMode ? L"Session import destination" : L"Child item import destination")) {
        return false;
    }
    auto src = PickDirectoryImportSource(hWnd, title, DialogDownloadsInitialFolder());
    if (!src) return false;

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
                   IsEnglishUi() ? L"Imported the directory as a session (copied)."
                                  : (g_config.studentMode ? L"フォルダを回次として取り込みました（コピーを実行しました）。"
                                                          : L"フォルダを下位項目として取り込みました（コピーを実行しました）。"),
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
            *outErr = (IsEnglishUi() ? L"Failed to open converted PDF:\n" : L"変換後PDFを開けません:\n") +
                      pdfPath.wstring() + L"\n\n" + atomic_write::Win32ErrorMessage(e);
        }
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 5 ||
        size.QuadPart > static_cast<LONGLONG>(std::numeric_limits<unsigned long>::max())) {
        CloseHandle(h);
        if (outErr) *outErr = IsEnglishUi()
            ? L"Converted PDF is empty or too large."
            : L"変換後PDFが空、または大きすぎます。";
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
        if (outErr) *outErr = IsEnglishUi()
            ? L"Converted output is not a PDF."
            : L"変換結果がPDFではありません。";
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
        if (outErr) *outErr = IsEnglishUi()
            ? L"PDFium rejected the converted PDF."
            : L"PDFiumで変換後PDFを検証できませんでした。";
        return false;
    }
    const int pageCount = FPDF_GetPageCount(doc);
    FPDF_CloseDocument(doc);
    CloseHandle(h);
    if (pageCount <= 0) {
        if (outErr) *outErr = IsEnglishUi()
            ? L"Converted PDF has no pages."
            : L"変換後PDFにページがありません。";
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
        if (outErr) *outErr = IsEnglishUi()
            ? L"Failed to prepare backup folder."
            : L"バックアップフォルダを準備できません。";
        return false;
    }

    std::filesystem::path backupDir = EscapeRootPath() / L"import_overwrite" / NowTimestampString();
    std::filesystem::create_directories(backupDir, ec);
    if (ec) {
        if (outErr) *outErr = (IsEnglishUi() ? L"Failed to create backup folder:\n" : L"バックアップフォルダを作成できません:\n") +
                              backupDir.wstring();
        return false;
    }

    std::filesystem::path backupPath = atomic_write::MakeUniqueDestInDir(backupDir, dest.filename());
    std::wstring copyErr;
    if (!CopyFileForImportSafely(dest, backupPath, &copyErr)) {
        if (outErr) *outErr = copyErr.empty()
            ? (IsEnglishUi() ? L"Failed to back up existing file." : L"既存ファイルのバックアップに失敗しました。")
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
        if (outFailure) *outFailure = IsEnglishUi() ? L"Invalid import path." : L"取り込みパスが不正です。";
        return ImportOneResult::Failed;
    }
    if (IsSameImportPath(copySource, dest)) {
        return ImportOneResult::Skipped;
    }

    std::error_code ec;
    const bool destExists = std::filesystem::exists(dest, ec);
    if (ec) {
        if (outFailure) *outFailure = IsEnglishUi()
            ? L"Failed to check destination file."
            : L"取り込み先ファイルを確認できません。";
        return ImportOneResult::Failed;
    }
    if (destExists) {
        if (IsCurrentOpenImportDestination(dest)) {
            if (outFailure) *outFailure = IsEnglishUi()
                ? L"Cannot overwrite the file that is currently open. Switch away from it first."
                : L"現在開いているファイルは取り込みで上書きできません。別のファイルへ切り替えてから実行してください。";
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
                ? (IsEnglishUi() ? L"Failed to back up existing destination. Import was canceled."
                                 : L"既存の取り込み先をバックアップできないため、取り込みを中止しました。")
                : backupErr;
            return ImportOneResult::Failed;
        }
    }

    std::wstring copyErr;
    if (!CopyFileForImportSafely(copySource, dest, &copyErr)) {
        if (outFailure) *outFailure = copyErr.empty()
            ? (IsEnglishUi() ? L"Failed to copy file." : L"ファイルのコピーに失敗しました。")
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

std::wstring FileUrlFromLocalPath(const std::filesystem::path& path) {
    std::wstring w = std::filesystem::absolute(path).wstring();
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
    return L"file://" + UTF8ToWide(encoded);
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
        if (outErr) *outErr = (IsEnglishUi() ? L"Failed to create LibreOffice local path folders:\n"
                                            : L"LibreOffice用ローカルパスフォルダを作成できません:\n") +
                              officeTempDir.wstring();
        return false;
    }

    auto urlUtf8 = [](const std::filesystem::path& path) {
        return WideToUTF8(FileUrlFromLocalPath(path));
    };
    const std::string workUrl = urlUtf8(workDir);
    const std::string backupUrl = urlUtf8(backupDir);
    const std::string tempUrl = urlUtf8(tempDir);

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
    std::ofstream out(configPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (outErr) *outErr = (IsEnglishUi() ? L"Failed to write LibreOffice profile configuration:\n"
                                            : L"LibreOfficeプロファイル設定を書き込めません:\n") +
                              configPath.wstring();
        return false;
    }
    const std::string data = xml.str();
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out) {
        if (outErr) *outErr = (IsEnglishUi() ? L"Failed to finish LibreOffice profile configuration:\n"
                                            : L"LibreOfficeプロファイル設定を確定できません:\n") +
                              configPath.wstring();
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
    wchar_t tempPath[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH, tempPath);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(tempPath) / L"PDFNoteWorkspace" / L"office_import";
}

std::filesystem::path MakeOfficeImportTempRoot(std::wstring* outErr) {
    if (outErr) outErr->clear();
    std::filesystem::path root = OfficeImportTempRootPath();
    if (root.empty()) {
        if (outErr) *outErr = IsEnglishUi()
            ? L"Failed to resolve the local conversion temp folder."
            : L"ローカル変換用一時フォルダを解決できません。";
        return {};
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        if (outErr) *outErr = (IsEnglishUi() ? L"Failed to create conversion temp folder:\n"
                                            : L"変換用一時フォルダを作成できません:\n") +
                              root.wstring();
        return {};
    }
    bool isReparse = false;
    if (!TryIsReparsePointNoFollow(root, isReparse) || isReparse) {
        if (outErr) *outErr = IsEnglishUi()
            ? L"The local conversion temp folder is a reparse point."
            : L"ローカル変換用一時フォルダが reparse point です。";
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
    if (outErr) *outErr = IsEnglishUi()
        ? L"Failed to create a unique conversion temp folder."
        : L"一意な変換用一時フォルダを作成できません。";
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
    std::wstring message = L"output_dir=" + outputDir.wstring();
    std::error_code ec;
    size_t count = 0;
    for (std::filesystem::directory_iterator it(outputDir, ec), end;
         !ec && it != end && count < 32; it.increment(ec), ++count) {
        const std::filesystem::path path = it->path();
        std::error_code fileEc;
        const bool regular = it->is_regular_file(fileEc) && !fileEc;
        message += L" | entry=" + path.filename().wstring();
        if (regular) {
            const uintmax_t size = std::filesystem::file_size(path, fileEc);
            if (!fileEc) message += L" size=" + std::to_wstring(size);
        }
    }
    if (ec) message += L" | enumerate_error=" + UTF8ToWide(ec.message());
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

bool RunLibreOfficePdfConversion(const std::filesystem::path& sourceOfficeCopy,
                                        const std::filesystem::path& outDir,
                                        const std::filesystem::path& profileDir,
                                        std::wstring* outErr,
                                        bool* outCanceled) {
    if (outErr) outErr->clear();
    if (outCanceled) *outCanceled = false;
    std::filesystem::path soffice = FindLibreOfficeSoffice();
    if (soffice.empty()) {
        if (outErr) *outErr = IsEnglishUi()
            ? L"The LibreOffice conversion runtime required by this standard edition was not found. Restore the complete standard release folder."
            : L"通常版に必要なLibreOffice変換ランタイムが見つかりません。通常版の配布フォルダ一式を復元してください。";
        return false;
    }
    CleanupLibreOfficePythonCacheBestEffort(soffice);

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        if (outErr) *outErr = (IsEnglishUi() ? L"Failed to create PDF output folder:\n"
                                            : L"PDF出力フォルダを作成できません:\n") +
                              outDir.wstring();
        return false;
    }
    std::filesystem::create_directories(profileDir, ec);
    if (ec) {
        if (outErr) *outErr = (IsEnglishUi() ? L"Failed to create LibreOffice profile folder:\n"
                                            : L"LibreOfficeプロファイルフォルダを作成できません:\n") +
                              profileDir.wstring();
        return false;
    }
    std::filesystem::path officeLocalDir = profileDir.parent_path() / L"local";
    if (!WriteLibreOfficeProfilePathConfig(profileDir, officeLocalDir, outErr)) {
        return false;
    }

    std::wstring cmd = QuoteWindowsCommandLineArg(soffice.wstring()) +
        L" --headless --nologo --nodefault --nolockcheck --nofirststartwizard --norestore" +
        L" " + QuoteWindowsCommandLineArg(L"-env:UserInstallation=" + FileUrlFromLocalPath(profileDir)) +
        L" --convert-to pdf" +
        L" --outdir " + QuoteWindowsCommandLineArg(outDir.wstring()) +
        L" " + QuoteWindowsCommandLineArg(sourceOfficeCopy.wstring());
    AppendOfficeConversionDiagnostic(L"start source=" + sourceOfficeCopy.wstring() +
                                     L" soffice=" + soffice.wstring() +
                                     L" profile=" + profileDir.wstring() +
                                     L" output=" + outDir.wstring() +
                                     L" command=" + cmd);

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
        if (outErr) *outErr = (IsEnglishUi() ? L"Failed to start LibreOffice:\n"
                                            : L"LibreOfficeを起動できません:\n") +
                              soffice.wstring() + L"\n\n" + atomic_write::Win32ErrorMessage(createErr);
        return false;
    }
    AppendOfficeConversionDiagnostic(L"process_started pid=" + std::to_wstring(pi.dwProcessId));

    constexpr DWORD kConvertTimeoutMs = 5 * 60 * 1000;
    ShowSoftNotice(g_hMainWnd,
                   IsEnglishUi() ? L"Converting Office file to PDF with LibreOffice..."
                                 : L"LibreOfficeでOfficeファイルをPDFに変換中...",
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
            if (outErr) *outErr = IsEnglishUi()
                ? L"LibreOffice conversion was canceled."
                : L"LibreOffice変換を中止しました。";
        } else if (wait == OfficeConversionWaitResult::TimedOut) {
            if (outErr) *outErr = IsEnglishUi()
                ? L"LibreOffice conversion timed out."
                : L"LibreOffice変換がタイムアウトしました。";
        } else if (outErr) {
            *outErr = IsEnglishUi()
                ? L"Failed while waiting for the LibreOffice process tree to exit."
                : L"LibreOfficeプロセスツリーの終了待機に失敗しました。";
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
        if (outErr) *outErr = (IsEnglishUi() ? L"LibreOffice conversion failed. Exit code: "
                                            : L"LibreOffice変換に失敗しました。終了コード: ") +
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
                                     L" output_wait_canceled=" + std::to_wstring(outputWaitCanceled ? 1 : 0) +
                                     L" expected=" + expectedPdf.wstring());
    if (!outputReady) AppendOfficeConversionOutputSnapshot(outDir);
    if (outputWaitCanceled) {
        CleanupLibreOfficePythonCacheBestEffort(soffice);
        if (outCanceled) *outCanceled = true;
        if (outErr) *outErr = IsEnglishUi()
            ? L"LibreOffice conversion was canceled."
            : L"LibreOffice変換を中止しました。";
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
        *outErr = IsEnglishUi()
            ? L"LibreOffice did not produce an identifiable PDF."
            : L"LibreOfficeが変換結果のPDFを特定できませんでした。";
        *outErr += IsEnglishUi() ? L"\nExpected:\n" : L"\n想定した出力:\n";
        *outErr += expectedPdf.wstring();
        if (ec) {
            *outErr += IsEnglishUi() ? L"\n\nFailed to inspect output folder:\n"
                                      : L"\n\n出力フォルダを確認できません:\n";
            *outErr += atomic_write::Win32ErrorMessage(ec.value());
        } else if (!candidates.empty()) {
            *outErr += IsEnglishUi() ? L"\n\nPDF files found:\n" : L"\n\n出力されたPDF:\n";
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
            *outErr = IsEnglishUi()
                ? L"Office conversion was not started because its temporary path is too long. Choose a shorter local temp path."
                : L"Office変換用の一時パスが長すぎるため、変換を開始しませんでした。短いローカル一時パスを使用してください。";
            *outErr += L"\n" + path.wstring() + L"\nlength=" + std::to_wstring(path.wstring().size());
        }
        return false;
    }
    return true;
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
        if (outFailure) *outFailure = IsEnglishUi()
            ? L"Failed to create Office conversion input folder."
            : L"Office変換用の入力フォルダを作成できません。";
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
            ? (IsEnglishUi() ? L"Failed to stage Office file for conversion."
                             : L"Officeファイルを変換用にコピーできません。")
            : copyErr;
        RemoveOfficeImportTempDirBestEffort(tempDir);
        return ImportOneResult::Failed;
    }

    std::wstring packageSafetyErr;
    if (!office::ValidateOfficePackageForOfflineConversion(stagedOffice, &packageSafetyErr)) {
        if (outFailure) *outFailure = packageSafetyErr.empty()
            ? (IsEnglishUi() ? L"The Office file did not pass the offline safety check."
                             : L"Officeファイルがオフライン安全性検査を通過しませんでした。")
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
        AppendOfficeConversionDiagnostic(L"output_identification_failed detail=" + outputErr);
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

    std::filesystem::path dest = sessionRoot / L"pdf" / (src.stem().wstring() + L".pdf");
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
                                                     std::wstring* outFailure) {
    if (outFailure) outFailure->clear();

    if (IsUnsupportedImportSourcePath(src)) {
        if (outFailure) *outFailure = IsEnglishUi()
            ? L"UNC/device paths are not supported."
            : L"UNC/デバイスパスは使用できません。";
        return ImportOneResult::Failed;
    }

    bool isReparse = false;
    if (TryIsReparsePointNoFollow(src, isReparse) && isReparse) {
        if (outFailure) *outFailure = IsEnglishUi()
            ? L"Reparse point files are not supported."
            : L"ジャンクション/シンボリックリンク等（reparse point）のファイルは使用できません。";
        return ImportOneResult::Failed;
    }

    std::error_code ec;
    if (!std::filesystem::exists(src, ec) || ec || !std::filesystem::is_regular_file(src, ec) || ec) {
        if (outFailure) *outFailure = IsEnglishUi()
            ? L"Source file was not found."
            : L"取り込み元ファイルが見つかりません。";
        return ImportOneResult::Failed;
    }

    if (IsOfficeImportSourcePath(src)) {
        if (!kOfficePdfConversionApprovedForUse) {
            if (outFailure) *outFailure = IsEnglishUi()
                ? L"Office-to-PDF conversion is disabled until a communication-free conversion engine is verified."
                : L"外部通信を持たない変換エンジンの検証が完了するまで、Office PDF変換は無効です。";
            return ImportOneResult::Failed;
        }
        if (!HasOfficeConversionFeature()) {
            if (outFailure) *outFailure = IsEnglishUi()
                ? L"This version (Lite) does not have the Office-to-PDF conversion feature."
                : L"このバージョン（Lite版）にはOfficeファイルのPDF変換機能がありません。";
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
        std::wstring msg = IsEnglishUi() ? L"Import was canceled." : L"取り込みを中止しました。";
        msg += (IsEnglishUi() ? L"\nImported (copied): " : L"\n取り込み（コピーを実行しました）: ") + std::to_wstring(stats.imported);
        msg += (IsEnglishUi() ? L"\nNot processed: " : L"\n未処理: ") + std::to_wstring(stats.skipped);
        ShowSoftNotice(hWnd, msg, SoftNoticeKind::Info);
        return;
    }
    if (stats.failed > 0) {
        std::wstring msg = (stats.imported > 0)
            ? (IsEnglishUi() ? L"Import finished with warnings." : L"取り込みを完了しました（一部失敗あり）。")
            : ui.errImportFile;
        msg += (IsEnglishUi() ? L"\nImported (copied): " : L"\n取り込み（コピーを実行しました）: ") + std::to_wstring(stats.imported);
        msg += (IsEnglishUi() ? L"\nSkipped: " : L"\nスキップ: ") + std::to_wstring(stats.skipped);
        msg += (IsEnglishUi() ? L"\nFailed: " : L"\n失敗: ") + std::to_wstring(stats.failed);
        if (!stats.failures.empty()) {
            msg += IsEnglishUi() ? L"\n\nDetails:\n" : L"\n\n詳細:\n";
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
        std::wstring msg = (IsEnglishUi() ? L"Imported files (copied): " : L"取り込んだファイル（コピーを実行しました）: ") +
                           std::to_wstring(stats.imported);
        if (stats.skipped > 0) {
            msg += (IsEnglishUi() ? L"\nSkipped: " : L"\nスキップ: ") + std::to_wstring(stats.skipped);
        }
        ShowSoftNotice(hWnd, msg);
    }
}

std::wstring OfficeOpenConversionTitle() {
    return IsEnglishUi() ? L"LibreOffice: Office to PDF conversion (Experimental)"
                         : L"LibreOfficeによるOfficeファイルのPDF変換（試験的）";
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
        std::filesystem::path dest = sessionRoot / L"pdf" / (src.stem().wstring() + L".pdf");
        std::wstring destKey = OfficeOpenPathKey(dest);
        if (destKey.empty() || !seenDest.insert(destKey).second) continue;
        out.push_back(src);
    }
}

static void ShowOfficeOpenConversionResult(HWND hWnd, const ImportBatchStats& stats, size_t totalCount) {
    const std::wstring title = OfficeOpenConversionTitle();
    if (stats.canceled) {
        std::wstring msg = IsEnglishUi() ? L"PDF conversion was canceled." : L"PDF変換を中止しました。";
        msg += (IsEnglishUi() ? L"\nConverted: " : L"\n変換: ") + std::to_wstring(stats.imported);
        msg += (IsEnglishUi() ? L"\nNot processed: " : L"\n未処理: ") + std::to_wstring(stats.skipped);
        ShowSoftNotice(hWnd, msg, SoftNoticeKind::Info);
        return;
    }
    if (stats.failed > 0) {
        std::wstring msg = (stats.imported > 0)
            ? (IsEnglishUi() ? L"PDF conversion finished with warnings."
                             : L"PDF変換を完了しました（一部失敗あり）。")
            : (IsEnglishUi() ? L"Failed to convert Office files to PDF."
                             : L"OfficeファイルをPDFに変換できませんでした。");
        msg += (IsEnglishUi() ? L"\nConverted: " : L"\n変換: ") + std::to_wstring(stats.imported);
        msg += (IsEnglishUi() ? L"\nSkipped: " : L"\nスキップ: ") + std::to_wstring(stats.skipped);
        msg += (IsEnglishUi() ? L"\nFailed: " : L"\n失敗: ") + std::to_wstring(stats.failed);
        if (!stats.failures.empty()) {
            msg += IsEnglishUi() ? L"\n\nDetails:\n" : L"\n\n詳細:\n";
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
        std::wstring msg = (IsEnglishUi() ? L"Converted to PDF: " : L"PDFに変換しました: ") +
                           std::to_wstring(stats.imported);
        if (stats.skipped > 0) {
            msg += (IsEnglishUi() ? L"\nSkipped: " : L"\nスキップ: ") + std::to_wstring(stats.skipped);
        }
        ShowSoftNotice(hWnd, msg);
    }
}

bool ConvertMissingOfficeFilesUnderDirectory(HWND hWnd, const std::filesystem::path& sessionRoot) {
    if (!kOfficePdfConversionApprovedForUse) {
        ShowSoftNotice(hWnd,
                       IsEnglishUi()
                           ? L"Office-to-PDF conversion is disabled until a communication-free conversion engine is verified."
                           : L"外部通信を持たない変換エンジンの検証が完了するまで、Office PDF変換は無効です。",
                       SoftNoticeKind::Warning);
        return false;
    }
    if (!HasOfficeConversionFeature()) {
        ShowSoftNotice(hWnd,
                       IsEnglishUi()
                           ? L"This version (Lite) does not have the Office-to-PDF conversion feature."
                           : L"このバージョン（Lite版）にはOfficeファイルのPDF変換機能がありません。",
                       SoftNoticeKind::Info);
        return false;
    }

    std::error_code ec;
    if (sessionRoot.empty() ||
        !std::filesystem::exists(sessionRoot, ec) || ec ||
        !std::filesystem::is_directory(sessionRoot, ec) || ec) {
        std::wstring msg = (IsEnglishUi() ? L"Target folder is not accessible:\n"
                                          : L"対象フォルダにアクセスできません:\n") +
                           sessionRoot.wstring();
        ShowSilentMessageDialog(hWnd, OfficeOpenConversionTitle(), msg, SoftNoticeKind::Error);
        return false;
    }

    std::vector<std::filesystem::path> pending;
    CollectOfficeFilesMissingPdfForOpen(sessionRoot, pending);
    if (pending.empty()) {
        ShowSoftNotice(hWnd,
                       IsEnglishUi()
                           ? L"There are no Office files to convert in this folder."
                           : L"このフォルダで変換対象のOfficeファイルはありません。",
                       SoftNoticeKind::Info);
        return false;
    }

    ImportBatchStats stats;
    for (size_t i = 0; i < pending.size(); ++i) {
        const auto& src = pending[i];
        if (HasSameStemPdfForOfficeOpen(sessionRoot, src)) {
            ++stats.skipped;
            continue;
        }
        UpdateOfficeConversionProgress(hWnd, i + 1, pending.size(), src);
        std::wstring failure;
        ImportOneResult result = ImportOfficeFileAsPdfToCurrentSession(hWnd, sessionRoot, src, &failure);
        switch (result) {
        case ImportOneResult::Imported:
            ++stats.imported;
            break;
        case ImportOneResult::Skipped:
            ++stats.skipped;
            break;
        case ImportOneResult::Canceled:
            stats.canceled = true;
            stats.skipped += static_cast<int>(pending.size() - i);
            break;
        case ImportOneResult::Failed:
            AddImportFailure(stats, src, failure);
            break;
        }
        if (stats.canceled) break;
    }
    EndOfficeConversionProgress(hWnd);

    if (stats.imported > 0 &&
        OfficeOpenPathKey(std::filesystem::path(g_currentSessionPath)) == OfficeOpenPathKey(sessionRoot)) {
        RefreshCurrentSessionFiles();
    }
    ShowOfficeOpenConversionResult(hWnd, stats, pending.size());
    return stats.imported > 0;
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
        std::wstring msg = IsEnglishUi() ? L"Conversion was canceled." : L"変換を中止しました。";
        msg += (IsEnglishUi() ? L"\nConverted: " : L"\n変換: ") + std::to_wstring(stats.imported);
        msg += (IsEnglishUi() ? L"\nNot processed: " : L"\n未処理: ") + std::to_wstring(stats.skipped);
        ShowSoftNotice(hWnd, msg, SoftNoticeKind::Info);
        return;
    }
    if (stats.failed > 0) {
        std::wstring msg = (stats.imported > 0)
            ? (IsEnglishUi() ? L"Conversion finished with warnings." : L"変換を完了しました（一部失敗あり）。")
            : (IsEnglishUi() ? L"Failed to convert Office files to PDF."
                             : L"OfficeファイルをPDFに変換できませんでした。");
        msg += (IsEnglishUi() ? L"\nConverted: " : L"\n変換: ") + std::to_wstring(stats.imported);
        msg += (IsEnglishUi() ? L"\nSkipped: " : L"\nスキップ: ") + std::to_wstring(stats.skipped);
        msg += (IsEnglishUi() ? L"\nFailed: " : L"\n失敗: ") + std::to_wstring(stats.failed);
        if (!stats.failures.empty()) {
            msg += IsEnglishUi() ? L"\n\nDetails:\n" : L"\n\n詳細:\n";
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
        ? ((IsEnglishUi() ? L"Converted to PDF: " : L"PDFに変換しました: ") +
           std::to_wstring(stats.imported))
        : (IsEnglishUi() ? L"No files were converted." : L"変換したファイルはありません。");
    if (stats.skipped > 0 || selectedCount > 1) {
        msg += (IsEnglishUi() ? L"\nSkipped: " : L"\nスキップ: ") + std::to_wstring(stats.skipped);
    }
    ShowSoftNotice(hWnd, msg, stats.imported > 0 ? SoftNoticeKind::Info : SoftNoticeKind::Warning);
}

bool ConvertOfficeFilesToCurrentSession(HWND hWnd) {
    const auto& ui = GetUiText();
    if (!kOfficePdfConversionApprovedForUse) {
        ShowSoftNotice(hWnd,
                       IsEnglishUi()
                           ? L"Office-to-PDF conversion is disabled until a communication-free conversion engine is verified."
                           : L"外部通信を持たない変換エンジンの検証が完了するまで、Office PDF変換は無効です。",
                       SoftNoticeKind::Warning);
        return false;
    }
    if (!HasOfficeConversionFeature()) {
        ShowSoftNotice(hWnd,
                       IsEnglishUi()
                           ? L"This version (Lite) does not have the Office-to-PDF conversion feature."
                           : L"このバージョン（Lite版）にはOfficeファイルのPDF変換機能がありません。",
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
        std::wstring msg = (IsEnglishUi() ? L"Session folder is not accessible:\n"
                                          : (g_config.studentMode ? L"回次フォルダにアクセスできません:\n"
                                                                  : L"下位項目フォルダにアクセスできません:\n")) +
                           sessionRoot.wstring();
        ShowSilentMessageDialog(hWnd, ui.menuConvertOfficeToPdf, msg, SoftNoticeKind::Error);
        return false;
    }

    auto picked = PickOfficeFilesUnder(hWnd, OfficeConversionInitialDirectory(), ui.menuConvertOfficeToPdf);
    if (picked.empty()) return false;

    ImportBatchStats stats;
    for (size_t i = 0; i < picked.size(); ++i) {
        const auto& path = picked[i];
        std::filesystem::path src(path);
        std::wstring failure;

        if (!IsOfficeImportSourcePath(src)) {
            failure = IsEnglishUi() ? L"Only .docx and .pptx files can be converted."
                                    : L"変換できるのは .docx / .pptx だけです。";
            AddImportFailure(stats, src, failure);
            continue;
        }
        if (IsUnsupportedImportSourcePath(src)) {
            failure = IsEnglishUi() ? L"UNC/device paths are not supported."
                                    : L"UNC/デバイスパスは使用できません。";
            AddImportFailure(stats, src, failure);
            continue;
        }
        bool isReparse = false;
        if (TryIsReparsePointNoFollow(src, isReparse) && isReparse) {
            failure = IsEnglishUi() ? L"Reparse point files are not supported."
                                    : L"ジャンクション/シンボリックリンク等（reparse point）のファイルは使用できません。";
            AddImportFailure(stats, src, failure);
            continue;
        }
        ec.clear();
        if (!std::filesystem::exists(src, ec) || ec ||
            !std::filesystem::is_regular_file(src, ec) || ec) {
            ec.clear();
            failure = IsEnglishUi() ? L"Source file was not found."
                                    : L"変換元ファイルが見つかりません。";
            AddImportFailure(stats, src, failure);
            continue;
        }

        UpdateOfficeConversionProgress(hWnd, i + 1, picked.size(), src);
        ImportOneResult result = ImportOfficeFileAsPdfToCurrentSession(hWnd, sessionRoot, src, &failure);
        switch (result) {
        case ImportOneResult::Imported:
            ++stats.imported;
            break;
        case ImportOneResult::Skipped:
            ++stats.skipped;
            break;
        case ImportOneResult::Canceled:
            stats.canceled = true;
            stats.skipped += static_cast<int>(picked.size() - i);
            break;
        case ImportOneResult::Failed:
            AddImportFailure(stats, src, failure);
            break;
        }
        if (stats.canceled) break;
    }
    EndOfficeConversionProgress(hWnd);

    if (stats.imported > 0) {
        RefreshCurrentSessionFiles();
    }
    ShowOfficeConversionBatchResult(hWnd, stats, picked.size());
    return stats.imported > 0;
}

bool ConvertOfficeFileToCurrentSession(HWND hWnd, const std::filesystem::path& src) {
    const auto& ui = GetUiText();
    if (!kOfficePdfConversionApprovedForUse) {
        ShowSoftNotice(hWnd,
                       IsEnglishUi()
                           ? L"Office-to-PDF conversion is disabled until a communication-free conversion engine is verified."
                           : L"外部通信を持たない変換エンジンの検証が完了するまで、Office PDF変換は無効です。",
                       SoftNoticeKind::Warning);
        return false;
    }
    if (!HasOfficeConversionFeature()) {
        ShowSoftNotice(hWnd,
                       IsEnglishUi()
                           ? L"This version (Lite) does not have the Office-to-PDF conversion feature."
                           : L"このバージョン（Lite版）にはOfficeファイルのPDF変換機能がありません。",
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
        std::wstring msg = (IsEnglishUi() ? L"Session folder is not accessible:\n"
                                          : (g_config.studentMode ? L"回次フォルダにアクセスできません:\n"
                                                                  : L"下位項目フォルダにアクセスできません:\n")) +
                           sessionRoot.wstring();
        ShowSilentMessageDialog(hWnd, ui.menuConvertOfficeToPdf, msg, SoftNoticeKind::Error);
        return false;
    }

    std::wstring failure;
    if (!IsOfficeImportSourcePath(src)) {
        failure = IsEnglishUi() ? L"Only .docx and .pptx files can be converted."
                                : L"変換できるのは .docx / .pptx だけです。";
        ShowSilentMessageDialog(hWnd, ui.menuConvertOfficeToPdf, failure, SoftNoticeKind::Warning);
        return false;
    }
    if (IsUnsupportedImportSourcePath(src)) {
        failure = IsEnglishUi() ? L"UNC/device paths are not supported."
                                : L"UNC/デバイスパスは使用できません。";
        ShowSilentMessageDialog(hWnd, ui.menuConvertOfficeToPdf, failure, SoftNoticeKind::Warning);
        return false;
    }
    bool isReparse = false;
    if (TryIsReparsePointNoFollow(src, isReparse) && isReparse) {
        failure = IsEnglishUi() ? L"Reparse point files are not supported."
                                : L"ジャンクション/シンボリックリンク等（reparse point）のファイルは使用できません。";
        ShowSilentMessageDialog(hWnd, ui.menuConvertOfficeToPdf, failure, SoftNoticeKind::Warning);
        return false;
    }
    ec.clear();
    if (!std::filesystem::exists(src, ec) || ec ||
        !std::filesystem::is_regular_file(src, ec) || ec) {
        failure = IsEnglishUi() ? L"Source file was not found."
                                : L"変換元ファイルが見つかりません。";
        ShowSilentMessageDialog(hWnd, ui.menuConvertOfficeToPdf, failure, SoftNoticeKind::Warning);
        return false;
    }

    ImportOneResult result = ImportOfficeFileAsPdfToCurrentSession(hWnd, sessionRoot, src, &failure);
    switch (result) {
    case ImportOneResult::Imported:
        RefreshCurrentSessionFiles();
        ShowSoftNotice(hWnd,
                       (IsEnglishUi() ? L"Converted to PDF: " : L"PDFに変換しました: ") + src.filename().wstring(),
                       SoftNoticeKind::Info);
        return true;
    case ImportOneResult::Skipped:
        ShowSoftNotice(hWnd,
                       IsEnglishUi() ? L"Conversion was skipped." : L"変換をスキップしました。",
                       SoftNoticeKind::Info);
        return false;
    case ImportOneResult::Canceled:
        ShowSoftNotice(hWnd,
                       IsEnglishUi() ? L"Conversion was canceled." : L"変換を中止しました。",
                       SoftNoticeKind::Info);
        return false;
    case ImportOneResult::Failed:
        ShowSilentMessageDialog(hWnd,
                                ui.menuConvertOfficeToPdf,
                                failure.empty()
                                    ? (IsEnglishUi() ? L"Failed to convert the Office file to PDF."
                                                     : L"OfficeファイルをPDFに変換できませんでした。")
                                    : failure,
                                SoftNoticeKind::Error);
        return false;
    }
    return false;
}

bool ConfirmDroppedOfficeConversion(HWND hWnd,
                                           const std::filesystem::path& sessionRoot,
                                           const std::vector<std::filesystem::path>& officeFiles) {
    if (officeFiles.empty()) return false;

    const bool english = IsEnglishUi();
    std::wstring msg = english
        ? L"Dropped Office files will be converted to PDF with LibreOffice and placed in the PDF area.\nThis conversion is experimental.\nConvert them now?"
        : L"ドロップされたOfficeファイルはLibreOfficeでPDFへ変換してPDF欄に取り込まれます。\nこの変換は試験的です。\n変換しますか？";
    msg += english ? L"\n\nFiles:\n" : L"\n\n対象:\n";
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
    confirm.buttons = SilentDialogButtons::YesNo;
    confirm.yesLabel = english ? L"Convert" : L"変換する";
    confirm.noLabel = english ? L"Not Now" : L"今回はしない";
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
        std::wstring msg = ui.errImportFile + L"\n" + sessionRoot.wstring();
        ShowSilentMessageDialog(hWnd, ui.menuImportFile, msg, SoftNoticeKind::Error);
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
    auto importOne = [&](const std::filesystem::path& src) {
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
    };

    for (const auto& src : regularFiles) {
        importOne(src);
    }

    if (!officeFiles.empty()) {
        if (!HasOfficeConversionFeature()) {
            ShowSoftNotice(hWnd,
                           IsEnglishUi()
                               ? L"This version (Lite) does not have the Office-to-PDF conversion feature."
                               : L"このバージョン（Lite版）にはOfficeファイルのPDF変換機能がありません。",
                           SoftNoticeKind::Info);
            stats.skipped += static_cast<int>(officeFiles.size());
        } else if (ConfirmDroppedOfficeConversion(hWnd, sessionRoot, officeFiles)) {
            for (size_t i = 0; i < officeFiles.size(); ++i) {
                UpdateOfficeConversionProgress(hWnd, i + 1, officeFiles.size(), officeFiles[i]);
                importOne(officeFiles[i]);
                if (stats.canceled) {
                    stats.skipped += static_cast<int>(officeFiles.size() - i);
                    break;
                }
            }
            EndOfficeConversionProgress(hWnd);
        } else {
            stats.skipped += static_cast<int>(officeFiles.size());
        }
    }

    if (stats.imported > 0) {
        RefreshCurrentSessionFiles();
    }
    if (stats.imported == 0 && stats.failed == 0 && stats.skipped > 0 && paths.size() == 1) {
        ShowSoftNotice(hWnd,
                       IsEnglishUi() ? L"Dropped file was not imported." : L"ドロップされたファイルは取り込みませんでした。",
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
        std::wstring msg = ui.errImportFile + L"\n" + sessionRoot.wstring();
        ShowSilentMessageDialog(hWnd, ui.menuImportFile, msg, SoftNoticeKind::Error);
        return false;
    }
    auto initial = DialogDownloadsInitialFolder();

    auto picked = PickFilesUnder(hWnd, initial, ui.menuImportFile);
    if (picked.empty()) return false;

    ImportBatchStats stats;
    for (size_t i = 0; i < picked.size(); ++i) {
        const auto& path = picked[i];
        std::filesystem::path src(path);
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
        if (stats.canceled) {
            stats.skipped += static_cast<int>(picked.size() - i);
            break;
        }
    }

    if (stats.imported > 0) {
        RefreshCurrentSessionFiles();
    }
    ShowImportBatchResult(hWnd, stats, picked.size());
    return stats.imported > 0;
}
