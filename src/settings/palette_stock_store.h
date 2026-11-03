#pragma once
#include "settings/palette_stock.h"
#include "diagnostics/normal_operations.h"
#include <string_view>

namespace palette_stock {
template<class Data> struct DocumentSnapshot {
    std::filesystem::path root;
    Data colors{};
    std::string bytes;
    bool exists = false;
    bool ready = false;
};
using Snapshot = DocumentSnapshot<std::vector<COLORREF>>;

class WriteLock {
public:
    explicit WriteLock(const std::filesystem::path& root) {
        auto key = root.lexically_normal().make_preferred().wstring();
        while (key.size() > 3 && key.back() == L'\\') key.pop_back();
        if (key.empty() || !CharUpperBuffW(key.data(), static_cast<DWORD>(key.size()))) return;
        std::uint64_t hash = 14695981039346656037ULL;
        for (wchar_t ch : key) { hash ^= static_cast<std::uint64_t>(ch); hash *= 1099511628211ULL; }
        const auto name = L"Local\\PDFNoteColorStock_" + std::to_wstring(hash);
        mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!mutex_) return;
        const DWORD wait = WaitForSingleObject(mutex_, 0);
        held_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    }
    ~WriteLock() { if (held_) ReleaseMutex(mutex_); if (mutex_) CloseHandle(mutex_); }
    WriteLock(const WriteLock&) = delete;
    WriteLock& operator=(const WriteLock&) = delete;
    [[nodiscard]] bool held() const { return held_; }
private:
    HANDLE mutex_ = nullptr;
    bool held_ = false;
};

[[nodiscard]] inline std::filesystem::path FilePath(const std::filesystem::path& root, const wchar_t* fileName = L"color_stock.json") {
    if (!fileName || (std::wstring_view(fileName) != L"color_stock.json" &&
        std::wstring_view(fileName) != L"palette_presets.json" &&
        std::wstring_view(fileName) != L"user_palette.json")) return {}; // Closed set, never an arbitrary path.
    return root.empty() ? std::filesystem::path{} : root / L"__pdf_note_workspace__" / L"__settings__" / fileName;
}

// Missing files start empty; read/parse failures never turn an existing file
// into an empty writable stock. The caller owns both the root and the snapshot.
template<class Data, class Parser>
[[nodiscard]] inline bool LoadDocument(const std::filesystem::path& root, DocumentSnapshot<Data>& out,
                                       const wchar_t* fileName, Parser parse) try {
    const auto path = FilePath(root, fileName);
    if (path.empty() || !root.is_absolute() || IsUncPath(root)) return false;
    const UINT drive = GetDriveTypeW(root.root_path().c_str());
    if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE && drive != DRIVE_RAMDISK && drive != DRIVE_CDROM) return false;
    // Inspect every existing ancestor; do not traverse a junction on reads or writes.
    for (auto ancestor = path; !ancestor.empty(); ancestor = ancestor.parent_path()) {
        DWORD attrs = 0;
        if (TryGetPathAttributesWin32(ancestor, &attrs)) {
            if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return false;
        } else {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        }
        if (ancestor == ancestor.parent_path()) break;
    }
    DocumentSnapshot<Data> next;
    next.root = root;
    DWORD attrs = 0;
    if (TryGetPathAttributesWin32(path, &attrs)) {
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) return false;
        next.exists = true;
        if (!ReadFileBytesNoFollowLimitedWin32(path, kMaxBytes, next.bytes) || !parse(next.bytes, next.colors)) return false;
    } else {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
    }
    next.ready = true;
    out = std::move(next);
    return true;
} catch (...) {
    return false;
}

// Caller-authorized commit only. Re-read before writing to reject stale dialogs. Original
// bytes are backed up inside the captured workspace before atomic replacement;
// failed saves keep both the original file and the caller's snapshot unchanged.
template<class Data, class Parser, class Serializer>
[[nodiscard]] inline bool SaveDocument(DocumentSnapshot<Data>& state, const Data& colors,
                                       const wchar_t* fileName, Parser parse, Serializer serialize) try {
    if (!state.ready) return false;
    WriteLock lock(state.root);
    if (!lock.held()) return false; // Never wait on another window/process in the UI thread.
    std::string bytes;
    if (!serialize(colors, bytes)) return false;
    DocumentSnapshot<Data> current;
    if (!LoadDocument(state.root, current, fileName, parse) || current.exists != state.exists || current.bytes != state.bytes) return false;
    // Allocate the committed snapshot before any write. Final publication is a move.
    DocumentSnapshot<Data> next = state;
    next.bytes = bytes;
    next.colors = colors;
    next.exists = true;
    const auto managed = state.root / L"__pdf_note_workspace__";
    const auto temp = managed / L"__tmp__";
    const auto escape = managed / L"__escape__";
    // The write helper's temporary/quarantine folders must obey the same boundary.
    for (const auto& dir : {temp, escape}) {
        bool reparse = false;
        if (TryIsReparsePointNoFollow(dir, reparse) && reparse) return false;
    }
    std::wstring error;
    if (state.exists) {
        const auto backup = atomic_write::MakeUniqueDestInDir(escape,
            std::filesystem::path(fileName).stem().wstring() + L"_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()) + L".json");
        if (!write_checks::ObservedWriteUtf8(state.root, backup, state.bytes, temp, escape, &error)) return false;
    }
    if (!LoadDocument(state.root, current, fileName, parse) || current.exists != state.exists || current.bytes != state.bytes) return false;
    if (!write_checks::ObservedWriteUtf8(state.root, FilePath(state.root, fileName), bytes, temp, escape, &error)) return false;
    state = std::move(next);
    return true;
} catch (...) {
    return false;
}

[[nodiscard]] inline bool Load(const std::filesystem::path& root, Snapshot& out) {
    return LoadDocument(root, out, L"color_stock.json", Parse);
}
[[nodiscard]] inline bool Save(Snapshot& state, const std::vector<COLORREF>& colors) {
    return SaveDocument(state, colors, L"color_stock.json", Parse, Serialize);
}
} // namespace palette_stock
