#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace search {
struct Folder { std::wstring path; bool enabled = true; };
struct Group {
    std::wstring name;
    std::vector<Folder> folders;
    int range = 3; // 0 lecture, 1 session, 2 workspace, 3 folders only
    unsigned targets = 7;
    bool external = false;
    bool normalize = true, ignoreCase = true;
};
// Workspace-pinned explicit storage. Missing is an empty collection; corrupt
// or externally changed data is never replaced. Saving retains a backup.
class Groups {
public:
    [[nodiscard]] bool Load(const std::filesystem::path& workspace);
    [[nodiscard]] bool Save(const std::vector<Group>& groups);
    const std::vector<Group>& items() const { return groups_; }
    bool ready() const { return ready_; }
private:
    std::filesystem::path workspace_, path_;
    std::string baseline_;
    std::vector<Group> groups_;
    bool existed_ = false, ready_ = false;
};
[[nodiscard]] std::string EncodeGroups(const std::vector<Group>& groups);
[[nodiscard]] std::vector<Group> DecodeGroups(std::string_view bytes);
}
