#include "search/search_groups.h"
#include "search/search_wire.h"
#include "core/atomic_write.h"
#include "diagnostics/write_checks.h"
#include "workspace/workspace_write_lock.h"
#include <set>

namespace search {
namespace {
constexpr size_t kLimit = 2 * 1024 * 1024;
bool Read(const std::filesystem::path& path, std::string& bytes, bool& exists) {
    if (!write_checks::IsSafeLocalPath(path)) return false;
    DWORD attributes = GetFileAttributesW(ToExtendedWin32PathIfAbsoluteLocal(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        bytes.clear(); exists = false; return true;
    }
    exists = true;
    return ReadFileBytesNoFollowLimitedWin32(path, kLimit, bytes);
}
}
std::string EncodeGroups(const std::vector<Group>& groups) {
    if (groups.size() > 128) throw std::length_error("search groups");
    Writer writer; writer.Put<uint32_t>(0x31474e50); writer.Put<uint32_t>(static_cast<uint32_t>(groups.size()));
    std::set<std::wstring> names;
    for (const auto& group : groups) {
        if (group.name.empty() || group.name.size() > 128 || group.name.find(L'\0') != std::wstring::npos ||
            !names.insert(group.name).second || group.folders.size() > 256 ||
            group.range < 0 || group.range > 3 || !group.targets || group.targets > 7)
            throw std::runtime_error("invalid search group");
        writer.Text(group.name); writer.Put<int32_t>(group.range); writer.Put<uint32_t>(group.targets);
        writer.Put<uint8_t>(group.external); writer.Put<uint8_t>(group.normalize); writer.Put<uint8_t>(group.ignoreCase);
        writer.Put<uint32_t>(static_cast<uint32_t>(group.folders.size()));
        for (const auto& folder : group.folders) {
            const std::filesystem::path path(folder.path);
            if (folder.path.size() > 32760 || !path.is_absolute() || IsUncPath(path) ||
                folder.path.find(L'\0') != std::wstring::npos) throw std::runtime_error("invalid search folder");
            // Preserve missing folders without probing them during serialization.
            writer.Text(folder.path); writer.Put<uint8_t>(folder.enabled);
            if (writer.bytes.size() > kLimit) throw std::length_error("search groups size");
        }
    }
    if (writer.bytes.size() > kLimit) throw std::length_error("search groups size");
    return writer.bytes;
}
std::vector<Group> DecodeGroups(std::string_view bytes) {
    if (bytes.size() > kLimit) throw std::length_error("search groups size");
    Reader reader(bytes);
    if (reader.Get<uint32_t>() != 0x31474e50) throw std::runtime_error("search groups version");
    const auto count = reader.Get<uint32_t>();
    if (count > 128) throw std::runtime_error("search groups count");
    std::vector<Group> groups;
    for (uint32_t i = 0; i < count; ++i) {
        Group group; group.name = reader.Text(128); group.range = reader.Get<int32_t>();
        group.targets = reader.Get<uint32_t>();
        auto flag = [&reader]() { const auto v = reader.Get<uint8_t>(); if (v > 1) throw std::runtime_error("search flag"); return v != 0; };
        group.external = flag(); group.normalize = flag(); group.ignoreCase = flag();
        const auto folders = reader.Get<uint32_t>();
        if (folders > 256) throw std::runtime_error("search folders count");
        for (uint32_t j = 0; j < folders; ++j) group.folders.push_back({reader.Text(32760), flag()});
        groups.push_back(std::move(group));
    }
    reader.End();
    if (EncodeGroups(groups) != bytes) throw std::runtime_error("invalid search groups encoding");
    return groups;
}
bool Groups::Load(const std::filesystem::path& workspace) {
    ready_ = false;
    try {
        workspace_ = workspace;
        path_ = workspace / L"__pdf_note_workspace__" / L"__settings__" / L"search_groups.bin";
        if (!Read(path_, baseline_, existed_)) return false;
        auto groups = existed_ ? DecodeGroups(baseline_) : std::vector<Group>{};
        groups_ = std::move(groups); ready_ = true; return true;
    } catch (...) { return false; }
}
bool Groups::Save(const std::vector<Group>& groups) {
    if (!ready_) return false;
    try {
        const auto bytes = EncodeGroups(groups);
        std::wstring error;
        WorkspaceOperationLock lock(workspace_.wstring(), &error);
        if (!lock.acquired()) return false;
        std::string current; bool exists = false;
        if (!Read(path_, current, exists) || exists != existed_ || current != baseline_) return false;
        const auto managed = workspace_ / L"__pdf_note_workspace__";
        const auto temporary = managed / L"__tmp__";
        const auto backup = managed / L"__escape__";
        if (!write_checks::IsSafeLocalPath(temporary) || !write_checks::IsSafeLocalPath(backup)) return false;
        if (exists && current != bytes) {
            const auto old = backup / (L"search_groups_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                                      std::to_wstring(GetTickCount64()) + L".bin");
            if (PathExistsWin32(old) || !atomic_write::AtomicWriteUtf8(old, current, temporary, backup, &error)) return false;
        }
        if (!atomic_write::AtomicWriteUtf8(path_, bytes, temporary, backup, &error)) return false;
        std::string verify; bool installed = false;
        if (!Read(path_, verify, installed) || !installed || verify != bytes) return false;
        groups_ = groups; baseline_ = bytes; existed_ = true; return true;
    } catch (...) { return false; }
}
}
