// Only linked by the stage persistence test host, never by the app build.
// Pause after one real Win32 write to an owned stage temp file. This gives the
// parent a deterministic process-termination boundary before flush/replace.
#include <windows.h>
#include <string>
BOOL WINAPI StageTestWriteFile(HANDLE file, LPCVOID bytes, DWORD count,
                                      LPDWORD written, LPOVERLAPPED overlapped) {
    const BOOL result = ::WriteFile(file, bytes, count, written, overlapped);
    if (!result || !written || *written == 0) return result;
    wchar_t signal[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"PDF_NOTE_STAGE_TEST_PAUSE_FILE", signal, 32768);
    if (!length || length >= 32768) return result;
    wchar_t target[32768]{};
    const DWORD targetLength = GetFinalPathNameByHandleW(file, target, 32768, FILE_NAME_NORMALIZED);
    if (!targetLength || targetLength >= 32768) return result;
    const std::wstring path(target, targetLength);
    if (path.find(L"\\__stage__\\clrop_journal\\") == std::wstring::npos ||
        path.size() < 4 || path.substr(path.size()-4) != L".tmp") return result;
    const HANDLE marker = CreateFileW(signal, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (marker == INVALID_HANDLE_VALUE) return FALSE;
    const std::string receipt = "after_real_WriteFile_before_flush_and_replace\nbytes=" + std::to_string(*written) + "\n";
    DWORD marked = 0;
    const BOOL markedOk = ::WriteFile(marker, receipt.data(), static_cast<DWORD>(receipt.size()), &marked, nullptr);
    const BOOL flushed = FlushFileBuffers(marker);
    CloseHandle(marker);
    if (!markedOk || marked != receipt.size() || !flushed) return FALSE;
    // An owned process is killed by the bounded parent watchdog; no other
    // process, disk, clock or production executable is changed.
    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return FALSE;
    WaitForSingleObject(event, INFINITE);
    CloseHandle(event);
    return result;
}
