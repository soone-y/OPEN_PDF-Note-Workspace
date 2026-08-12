#pragma once

#include <filesystem>

struct EscapeBackupPresence {
    bool hasPdfPositionBackup = false;
    bool hasLectureLastOpenBackup = false;
    bool hasSessionLastOpenBackup = false;
    bool hasSavedFileBackup = false;
};

std::filesystem::path EscapeRootPath();
EscapeBackupPresence ScanEscapeBackupPresence();
