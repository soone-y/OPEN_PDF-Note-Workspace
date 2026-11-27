#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "core/app_core.h"
#include "core/atomic_write.h"
#include "core/workspace_layout.h"
#include "core/workspace_relocation.h"
#include "workspace/operation_record_store.h"

namespace {

namespace fs = std::filesystem;

int g_failed = 0;

void Expect(bool condition, const char* message) {
    if (condition) {
        std::cout << "[PASS] " << message << "\n";
        return;
    }
    std::cout << "[FAIL] " << message << "\n";
    ++g_failed;
}

fs::path MakeTestRoot() {
    fs::path root = fs::temp_directory_path() / L"pdf_note_path_safety_tests";
    root /= std::to_wstring(GetCurrentProcessId()) + L"_" +
            std::to_wstring(static_cast<unsigned long long>(GetTickCount64()));
    std::error_code ec;
    fs::create_directories(root, ec);
    return root;
}

bool WriteRaw(const fs::path& path, const std::string& contents) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return static_cast<bool>(ofs);
}

// Records written before the folder holding the workspace was renamed keep
// absolute paths of the old location. These cases pin how the old root is
// derived from a record's own location and how members are mapped.
void TestWorkspaceRelocation() {
    namespace wr = workspace_relocation;
    const fs::path oldBackup =
        L"C:\\Users\\u\\Downloads\\release_1\\sample_workspace\\__pdf_note_workspace__"
        L"\\__operations__\\op_1\\move_0.bak";
    const fs::path newBackup =
        L"C:\\Users\\u\\Downloads\\試験利用_release_1\\sample_workspace\\__pdf_note_workspace__"
        L"\\__operations__\\op_1\\move_0.bak";

    const auto moved = wr::DetectFromSelfPath(oldBackup, newBackup);
    Expect(moved && moved->moved, "renamed exe folder is detected from a record's own location");
    Expect(moved && wr::SameLocation(moved->recordedRoot,
                                     L"C:\\Users\\u\\Downloads\\release_1\\sample_workspace"),
           "recorded workspace root is the parent of the managed root");
    Expect(moved && wr::SameLocation(moved->currentRoot,
                                     L"C:\\Users\\u\\Downloads\\試験利用_release_1\\sample_workspace"),
           "current workspace root is taken from the record's actual location");

    const auto same = wr::DetectFromSelfPath(
        L"\\\\?\\C:\\WS\\__PDF_NOTE_WORKSPACE__\\__operations__\\op_1\\move_0.bak",
        L"c:\\ws\\__pdf_note_workspace__\\__operations__\\op_1\\move_0.bak");
    Expect(same && !same->moved, "case and extended prefix differences are not a relocation");

    Expect(!wr::DetectFromSelfPath(
               L"C:\\old\\ws\\__pdf_note_workspace__\\__operations__\\op_2\\move_0.bak",
               newBackup),
           "a record naming another transaction directory is not reinterpreted");
    Expect(!wr::DetectFromSelfPath(L"C:\\elsewhere\\move_0.bak", newBackup),
           "a record outside any managed root is not reinterpreted");
    Expect(!wr::DetectFromSelfPath(L"move_0.bak", newBackup),
           "a relative recorded path is not reinterpreted");

    const fs::path oldRoot = L"C:\\old\\ws";
    const fs::path newRoot = L"D:\\new\\ws";
    const auto inside = wr::Rebase(L"C:\\OLD\\ws\\講義\\note\\a.clro", oldRoot, newRoot);
    Expect(inside && wr::SameLocation(*inside, L"D:\\new\\ws\\講義\\note\\a.clro"),
           "a member below the old workspace root maps below the new root");
    Expect(!wr::Rebase(L"C:\\Users\\u\\Documents\\授業\\a.clro", oldRoot, newRoot),
           "a member in a registered external folder is left as recorded");
    Expect(!wr::Rebase(L"C:\\old\\ws2\\a.clro", oldRoot, newRoot),
           "a sibling with a common name prefix is not treated as inside the root");

    Expect(!wr::DetectFromSelfPath(
               L"\\\\server\\share\\ws\\__pdf_note_workspace__\\__operations__\\op_1\\move_0.bak",
               newBackup),
           "a UNC record location is never mapped");

    const fs::path ws = L"C:\\apps\\pdf\\Sample_Workspace";
    const std::wstring stored = wr::ToRecordPath(L"c:\\APPS\\pdf\\sample_workspace\\講義\\A.pdf", ws);
    Expect(stored == L"講義\\A.pdf",
           "a workspace member is stored relative to the workspace with its own case");
    Expect(wr::ToRecordPath(L"C:\\Users\\u\\a.pdf", ws) == L"C:\\Users\\u\\a.pdf",
           "a member outside every known root stays absolute");
    const std::wstring external =
        wr::ToRecordPath(L"C:\\Users\\u\\授業\\a.pdf", ws, {L"C:\\Users\\u\\授業"});
    Expect(external == L"@external:C:\\Users\\u\\授業\\a.pdf",
           "a member in a registered external folder uses the identity external form");
    const auto externalLoaded = wr::FromRecordPath(external, ws);
    Expect(externalLoaded && externalLoaded->wstring() == L"C:\\Users\\u\\授業\\a.pdf",
           "an external record resolves to its absolute path");
    Expect(!wr::FromRecordPath(L"@external:relative\\a.pdf", ws),
           "an external record without a drive path is rejected");
    const auto loaded = wr::FromRecordPath(stored, L"E:\\renamed\\sample_workspace");
    Expect(loaded && loaded->wstring() == L"E:\\renamed\\sample_workspace\\講義\\A.pdf",
           "a relative record resolves against the current workspace root");
    const auto legacy = wr::FromRecordPath(L"C:\\old\\a.pdf", ws);
    Expect(legacy && legacy->wstring() == L"C:\\old\\a.pdf", "a legacy absolute record is kept");
    Expect(!wr::FromRecordPath(L"..\\outside.pdf", ws),
           "a relative record cannot climb out of the workspace");
    Expect(!wr::FromRecordPath(L"\\rooted.pdf", ws), "a drive-relative record is rejected");
    Expect(!wr::FromRecordPath(L"a.pdf:stream", ws), "an alternate data stream record is rejected");

    const fs::path txDir = L"C:\\ws\\__pdf_note_workspace__\\__operations__\\op_1";
    const auto own = wr::FromOwnFileName(L"move_0.bak", txDir);
    Expect(own && own->wstring() == (txDir / L"move_0.bak").wstring(),
           "a record's own file name resolves inside the record directory");
    Expect(!wr::FromOwnFileName(L"sub\\move_0.bak", txDir) &&
               !wr::FromOwnFileName(L"..\\move_0.bak", txDir) &&
               !wr::FromOwnFileName(L"C:\\move_0.bak", txDir),
           "a record's own file must be a single name inside its directory");
}

// Setting a record aside moves the whole record directory and never deletes
// or overwrites anything.
void TestWorkspaceLayout() {
    namespace wl = workspace_layout;
    const fs::path ws = L"C:\\ws";
    const fs::path managed = ws / L"__pdf_note_workspace__";
    Expect(wl::Root(fs::path{}).empty() && wl::Unsaved(fs::path{}).empty() && wl::DeleteGuide(fs::path{}).empty(),
           "layout paths stay empty without a workspace");
    Expect(wl::Unsaved(ws) == managed / L"__unsaved__" && wl::Recent(ws) == managed / L"__recent__" &&
               wl::Backup(ws) == managed / L"__backup__" && wl::Failed(ws) == managed / L"__failed__" &&
               wl::Operations(ws) == managed / L"__operations__" &&
               wl::OperationsSetAside(ws) == managed / L"__operations_set_aside__" &&
               wl::Tmp(ws) == managed / L"__tmp__" && wl::Test(ws) == managed / L"__test__",
           "layout folders sit directly below the management root");
    Expect(wl::DeleteGuide(ws) == managed / L"DELETE_OR_KEEP.txt", "the delete guide sits in the management root");
    bool wellFormed = true;
    for (size_t i = 0; i < wl::kFolders.size(); ++i) {
        const std::wstring name = wl::kFolders[i].name;
        wellFormed = wellFormed && name.size() > 4 && name.rfind(L"__", 0) == 0 &&
                     name.compare(name.size() - 2, 2, L"__") == 0 && name.find(L'\\') == std::wstring::npos &&
                     name.find(L'/') == std::wstring::npos;
        for (size_t j = i + 1; j < wl::kFolders.size(); ++j)
            wellFormed = wellFormed && _wcsicmp(wl::kFolders[i].name, wl::kFolders[j].name) != 0;
    }
    Expect(wellFormed, "layout folder names are distinct single __name__ components");
    // The operation scan treats every directory below __operations__ as a record,
    // so the set-aside store must not be nested inside it.
    Expect(wl::OperationsSetAside(ws).parent_path() == managed,
           "set-aside operation records are not scanned as live records");
}

void TestMoveRecordAside(const fs::path& root) {
    const fs::path store = root / L"operation_transactions";
    const fs::path aside = root / L"operation_transactions_set_aside";
    const fs::path record = store / L"op_1";
    std::error_code ec;
    fs::create_directories(record, ec);
    Expect(WriteRaw(record / L"manifest.tsv", "m") && WriteRaw(record / L"move_0.bak", "b"),
           "set-aside fixture is created");

    const auto first = operation_record_store::MoveRecordAside(record, aside);
    Expect(first.ok && !fs::exists(record) && fs::exists(aside / L"op_1" / L"move_0.bak"),
           "a record directory is moved aside with its backups");

    fs::create_directories(record, ec);
    Expect(WriteRaw(record / L"manifest.tsv", "second"), "second record with the same name is created");
    const auto second = operation_record_store::MoveRecordAside(record, aside);
    std::string kept;
    Expect(second.ok && second.destination.filename() == L"op_1_1" &&
               ReadFileBytesWin32(aside / L"op_1" / L"manifest.tsv", kept) && kept == "m",
           "a name collision gets a new name and leaves the earlier record unchanged");

    const fs::path file = store / L"not_a_dir";
    Expect(WriteRaw(file, "x"), "plain file fixture is created");
    const auto notDir = operation_record_store::MoveRecordAside(file, aside);
    Expect(!notDir.ok && fs::exists(file), "a plain file is refused and left in place");
    const auto missing = operation_record_store::MoveRecordAside(store / L"missing", aside);
    Expect(!missing.ok, "a missing record directory is refused");
}

} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    TestWorkspaceRelocation();
    TestWorkspaceLayout();

    const fs::path root = MakeTestRoot();
    TestMoveRecordAside(root / L"set_aside");

    Expect(IsUncPathString(L"\\\\server\\share\\doc.pdf"),
           "plain UNC path is classified as UNC");
    Expect(IsUncPathString(L"\\\\?\\UNC\\server\\share\\doc.pdf"),
           "extended UNC path is classified as UNC");
    Expect(!IsUncPathString(L"\\\\?\\C:\\local\\doc.pdf"),
           "extended local drive path is not classified as UNC");
    Expect(!IsUncPathString(L"\\\\.\\C:\\local\\doc.pdf"),
           "device-style local path is not classified as UNC");

    const fs::path localFile = root / L"日本語 path & space.txt";
    Expect(WriteRaw(localFile, "ok"), "special-character local file fixture is written");
    Expect(PathExistsWin32(localFile), "PathExistsWin32 handles special-character local paths");
    Expect(RegularFileExistsWin32(localFile), "RegularFileExistsWin32 recognizes local file");
    Expect(!DirectoryExistsWin32(localFile), "DirectoryExistsWin32 rejects local file");

    const std::wstring extended = ToExtendedWin32PathIfAbsoluteLocal(localFile);
    Expect(extended.rfind(L"\\\\?\\", 0) == 0,
           "absolute local path is converted to extended Win32 path");
    Expect(PathExistsWin32(fs::path(extended)),
           "extended Win32 path still resolves to the local file");
    Expect(ToExtendedWin32PathIfAbsoluteLocal(fs::path(extended)) == extended,
           "already-extended local path is not rewritten");
    Expect(ToExtendedWin32PathIfAbsoluteLocal(fs::path(L"\\\\server\\share\\doc.pdf")) ==
               L"\\\\?\\UNC\\server\\share\\doc.pdf",
           "absolute UNC path is converted to extended UNC form");
    Expect(ToExtendedWin32PathIfAbsoluteLocal(fs::path(L"C:relative\\doc.pdf")) ==
               L"C:relative\\doc.pdf",
           "drive-relative path is not incorrectly converted to an absolute extended path");

    fs::path deepDirectory = root;
    for (int index = 0; index < 30; ++index) {
        deepDirectory /= L"long_path_segment_" + std::to_wstring(index);
    }
    const fs::path deepFile = deepDirectory / L"path_safety.txt";
    Expect(deepFile.wstring().size() > MAX_PATH,
           "long-path fixture exceeds MAX_PATH");
    Expect(atomic_write::EnsureDirectoryExists(deepDirectory),
           "long-path parent directory is created through extended Win32 APIs");
    const std::wstring deepOpenPath = ToExtendedWin32PathIfAbsoluteLocal(deepFile);
    HANDLE deepHandle = CreateFileW(deepOpenPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    const char deepPayload[] = "deep";
    DWORD deepWritten = 0;
    const bool deepWrittenOk = deepHandle != INVALID_HANDLE_VALUE &&
                               WriteFile(deepHandle, deepPayload, sizeof(deepPayload) - 1, &deepWritten, nullptr) &&
                               deepWritten == sizeof(deepPayload) - 1;
    if (deepHandle != INVALID_HANDLE_VALUE) CloseHandle(deepHandle);
    Expect(deepWrittenOk, "extended Win32 write succeeds beyond MAX_PATH");
    Expect(RegularFileExistsWin32(deepFile),
           "path-safety helpers inspect a file beyond MAX_PATH");
    std::string deepBytes;
    Expect(ReadFileBytesWin32(deepFile, deepBytes) && deepBytes == "deep",
           "path-safety helpers read a file beyond MAX_PATH");
    DeleteFileW(deepOpenPath.c_str());
    for (fs::path current = deepDirectory; current != root; current = current.parent_path()) {
        RemoveDirectoryW(ToExtendedWin32PathIfAbsoluteLocal(current).c_str());
    }

    bool isReparse = true;
    Expect(TryIsReparsePointNoFollow(localFile, isReparse) && !isReparse,
           "regular local file is not reported as a reparse point");

    std::uint64_t size = 0;
    std::int64_t mtimeMs = 0;
    Expect(TryGetFileMetaWin32(localFile, &size, &mtimeMs) && size == 2,
           "TryGetFileMetaWin32 reads local file metadata without following network paths");

    std::string bytes;
    Expect(ReadFileBytesWin32(localFile, bytes) && bytes == "ok",
           "ReadFileBytesWin32 reads special-character local paths");
    Expect(ReadFileBytesNoFollowLimitedWin32(localFile, 2, bytes) && bytes == "ok",
           "bounded no-follow reader accepts a regular file within its limit");
    Expect(!ReadFileBytesNoFollowLimitedWin32(localFile, 1, bytes) && bytes.empty(),
           "bounded no-follow reader rejects a file beyond its limit");

    const fs::path missing = root / L"missing.txt";
    Expect(!PathExistsWin32(missing), "missing local path is reported absent");
    Expect(!RegularFileExistsWin32(missing), "missing local file is not reported regular");

    std::error_code ec;
    fs::remove_all(root, ec);

    std::cout << "Summary: failed=" << g_failed << "\n";
    return g_failed == 0 ? 0 : 1;
}
