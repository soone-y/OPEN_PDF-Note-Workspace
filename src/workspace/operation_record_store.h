#pragma once

// Moving a whole operation-transaction record out of the scanned store.
//
// Owner: the session recovery scan in file_ops, after the user explicitly
// chose to stop recovering one record. The record directory (manifest and
// every backup) is moved, never copied or deleted, and nothing outside the
// record directory and the destination store is touched. The user's files are
// not involved at all.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>

namespace operation_record_store {

struct SetAsideResult {
    bool ok = false;
    std::filesystem::path destination;  // set when ok
    DWORD win32Error = ERROR_SUCCESS;    // set when the move itself failed
};

// Moves `recordDir` into `asideRoot` (created when missing) under its own
// name, or "<name>_<n>" when that name is taken. Fails without changing
// anything when either side is a reparse point, `recordDir` is not a
// directory, or no free name exists. An existing entry is never overwritten.
SetAsideResult MoveRecordAside(const std::filesystem::path& recordDir,
                               const std::filesystem::path& asideRoot);

}  // namespace operation_record_store
