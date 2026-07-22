#pragma once
struct BlankPdfSpec {
    double widthPt = 595.0;
    double heightPt = 842.0;
    int pageCount = 1;
};

struct ImportBatchStats {
    int imported = 0;
    int skipped = 0;
    int failed = 0;
    bool canceled = false;
    std::vector<std::wstring> failures;
};

#include <windows.h>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <unordered_set>

void ShowNewLectureDialog(HWND owner);
bool CreateLectureFolder(const std::wstring& baseName, std::wstring& outPath);
std::wstring TodayDateForName();
void PushUniqueSuggestion(std::vector<std::wstring>& suggestions, const std::wstring& value);
std::wstring ToJapaneseNumeralForName(int value);
std::vector<std::wstring> NewSessionNameSuggestions();
void ShowNewSessionDialog(HWND owner);
std::filesystem::path ExeDirPath();
void AddTempExternalLectures(HWND owner, const std::vector<std::wstring>& lectureDirs);
std::optional<std::wstring> PickWorkspaceFolder(HWND parent);
bool IsSupportedNewNoteExtension(const std::wstring& ext);
std::wstring DefaultNewNoteStem();
bool ValidateNewNoteFileName(HWND owner, const std::wstring& name);
bool TryCreateEmptyNoteFile(const std::filesystem::path& target, DWORD* outError);
void CreateNewClroInSession(HWND hWnd);
std::filesystem::path CurrentNoteDirectory();
std::wstring DefaultBlankPdfFileName();
std::filesystem::path CurrentPdfDirectory();
bool TryParsePositiveDoubleToken(const std::wstring& token, double* out);
bool TryParsePositiveIntToken(const std::wstring& token, int minValue, int maxValue, int* out);
bool TryResolveBlankPdfPreset(const std::wstring& token, double* outWPt, double* outHPt);
bool TryParseBlankPdfSizeToken(std::wstring token, double* outWPt, double* outHPt);
bool TryParseBlankPdfSpec(const std::wstring& input, BlankPdfSpec* out);
void CreateBlankPdfInCurrentSession(HWND hWnd);
std::vector<std::wstring> NewNoteNameSuggestions(const std::wstring& ext);
bool ShowNewNoteButtonContextMenu(HWND hWnd, LPARAM lParam);
bool IsUnsupportedImportSourcePath(const std::filesystem::path& path);
bool IsOfficeImportSourcePath(const std::filesystem::path& path);
bool IsDocxImportSourcePath(const std::filesystem::path& path);
void CleanupImportTempFile(HANDLE* handle, const std::filesystem::path& tmp);
bool IsWorkspaceReservedImportDirectoryName(const std::filesystem::path& path);
bool ImportDirectoryAsLecture(HWND hWnd);
bool ImportDirectoryAsSession(HWND hWnd);
bool ValidateImportPdfFile(const std::filesystem::path& pdfPath, std::wstring* outErr);
bool IsSameImportPath(const std::filesystem::path& lhs, const std::filesystem::path& rhs);
bool IsCurrentOpenImportDestination(const std::filesystem::path& dest);
std::wstring QuoteWindowsCommandLineArg(const std::wstring& arg);
std::wstring FileUrlFromLocalPath(const std::filesystem::path& path);
bool IsUsableLibreOfficeSofficeCandidate(const std::filesystem::path& cand);
std::filesystem::path FindLibreOfficeSoffice();
bool HasOfficeConversionFeature();
void CleanupLibreOfficePythonCacheBestEffort(const std::filesystem::path& soffice);
bool CreateOfficeImportTempMarker(const std::filesystem::path& dir);
bool HasOfficeImportTempMarker(const std::filesystem::path& dir);
std::filesystem::path MakeOfficeImportTempRoot(std::wstring* outErr);
std::filesystem::path MakeUniqueOfficeImportTempDir(std::wstring* outErr);
void RemoveOfficeImportTempDirBestEffort(const std::filesystem::path& dir);
bool IsOfficeConversionWaitDispatchMessage(const MSG& msg);
void PumpOfficeConversionWaitMessages();
void ShowImportBatchResult(HWND hWnd, const ImportBatchStats& stats, size_t selectedCount);
std::filesystem::path OfficeConversionInitialDirectory();
bool ConvertOfficeFilesToCurrentSession(HWND hWnd);
bool ConvertOfficeFileToCurrentSession(HWND hWnd, const std::filesystem::path& src);
bool ConvertMissingOfficeFilesUnderDirectory(HWND hWnd, const std::filesystem::path& directoryRoot);
bool ImportDroppedFilesToCurrentSession(HWND hWnd, const std::vector<std::wstring>& paths);
bool ImportFileToCurrentSession(HWND hWnd);
std::vector<std::wstring> PickFoldersWithInitial(HWND parent, const std::filesystem::path& initialDir, const std::wstring& title);
std::optional<std::wstring> PickFolderWithInitial(HWND parent, const std::filesystem::path& initialDir, const std::wstring& title);
