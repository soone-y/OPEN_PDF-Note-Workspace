#pragma once

#include <filesystem>

struct EscapeBackupPresence {
    bool hasPdfPositionBackup = false;
    bool hasLectureLastOpenBackup = false;
    bool hasSessionLastOpenBackup = false;
    bool hasSavedFileBackup = false;
};

// __pdf_note_workspace__/__backup__: saved-file backups and state-file copies.
std::filesystem::path BackupRootPath();
EscapeBackupPresence ScanEscapeBackupPresence();
