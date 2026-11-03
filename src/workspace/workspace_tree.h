// file: main/workspace_tree.h
// Public API for the workspace tree (lecture / session / file lists).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <unordered_map>
#include <climits>

// ---------------------------------------------------------------------------
// Types and enums used by workspace_tree and its callers
// ---------------------------------------------------------------------------

enum class LectureSortMode { Recent, Name, Manual };
enum class SessionSortMode { NumericAsc, NumericDesc, Name };

// ---------------------------------------------------------------------------
// Public API: called from workspace_config_io.cpp, main.cpp, etc.
// ---------------------------------------------------------------------------

// Load the full lecture list into the left-pane listbox.
void LoadLectures();
// Refresh registered external rows from memory; no filesystem rescan or file selection.
void RefreshRegisteredExternalLectures(const std::vector<std::wstring>& previousPaths);

// Reload session list for lecturePath, then select and optionally open desiredName.
void ReloadSessionsAndSelect(const std::wstring& lecturePath,
                             const std::wstring& desiredName,
                             bool reopenFiles);

// Refresh the file lists for the currently open session.
bool RefreshCurrentSessionFiles();

// Rescan one displayed directory list without reopening the active documents.
[[nodiscard]] bool RefreshWorkspaceList(HWND list);

// Called once the workspace root has been validated and loaded.
void OnWorkspaceLoaded(HWND hWnd);

// Internal helpers also called directly from main.cpp event handlers
// (static in the original .cppinc; kept here so main.cpp can see them)
void LoadSessions(const std::wstring& lecturePath);
void LoadFiles(const struct SessionEntry& session,
               std::wstring& preferredPdf,
               std::wstring& preferredNote);

// ---------------------------------------------------------------------------
// Helpers defined in main.cpp that workspace_tree.cpp needs
// (de-staticified so that the separate TU can link to them)
// ---------------------------------------------------------------------------

// Index helpers
int CurrentLectureIndex();
int CurrentPdfIndex();
int CurrentNoteIndex();

// Session file management
void AutoOpenSingleSessionFiles(HWND hWnd);
bool SaveStartupLastOpenTarget();
void LoadSessionLastOpenMap();
void ApplySessionAutoOpenPreference(const struct SessionEntry& session,
                                    std::wstring& preferredPdf,
                                    std::wstring& preferredNote);
bool RecoverPendingFileOperationTransactionsForSession(
    HWND owner, const std::filesystem::path& sessionRoot);

// Display labels
[[nodiscard]] std::vector<std::wstring> LectureDisplayLabelsForPaths(const std::vector<std::wstring>& paths);
[[nodiscard]] std::vector<std::wstring> SessionDisplayLabelsForEntries(const std::vector<struct SessionEntry>& sessions,
                                                       const std::wstring& lecturePath);
[[nodiscard]] std::vector<std::wstring> FileDisplayLabelsForPaths(const std::vector<struct FileEntry>& files,
                                                  const std::filesystem::path& root);
std::wstring DirectFilesSessionLabel();
std::wstring LectureDisplayLabelForPath(const std::wstring& lecturePath,
                                        const std::vector<std::wstring>& allPaths);
std::wstring SessionDisplayLabelForEntry(const struct SessionEntry& session,
                                         const std::vector<struct SessionEntry>& sessions,
                                         const std::wstring& lecturePath);

// Sort / rank
LectureSortMode ParseLectureSortMode(const std::wstring& mode);
SessionSortMode ParseSessionSortMode(const std::wstring& mode);
void SortSessionsForDisplay(std::vector<struct SessionEntry>& sessions, SessionSortMode mode);

// Lecture last-open tracking
std::filesystem::path LectureLastOpenFilePath();
std::unordered_map<std::wstring, long long> LoadLectureOpenTimes(
    const std::filesystem::path& path);

// Filesystem helpers
std::optional<std::filesystem::file_time_type> TryLastWriteTime(
    const std::filesystem::path& p);

// File display labels (also used by main.cpp / workspace_actions)
std::wstring FileDisplayLabelForPath(const std::wstring& filePath,
                                     const std::vector<struct FileEntry>& files,
                                     const std::filesystem::path& root);

// Globals accessed by workspace_tree.cpp (defined in main.cpp)
extern const wchar_t kLectureSettingsDirName[];
extern std::unordered_map<std::wstring, std::wstring> g_lastNoteBySession;
extern std::unordered_map<std::wstring, std::wstring> g_lastPdfBySession;
extern std::wstring g_lastStartupLecturePathSaved;
extern std::wstring g_lastStartupSessionPathSaved;
