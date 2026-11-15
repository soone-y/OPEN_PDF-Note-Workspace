#pragma once

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace workspace_display {

[[nodiscard]] inline std::wstring LabelKey(std::wstring label) {
    std::transform(label.begin(), label.end(), label.begin(), ::towlower);
    return label;
}

// Compare whole Windows path components and preserve the original spelling of
// the remaining components. These paths are for display only.
class CommonParentPath {
public:
    void Include(const std::filesystem::path& itemPath) {
        auto parent = DisplayParent(itemPath);
        if (count_++ == 0) {
            first_ = std::move(parent);
            shared_ = static_cast<size_t>(std::distance(first_.begin(), first_.end()));
            return;
        }
        auto first = first_.begin();
        auto next = parent.begin();
        size_t matched = 0;
        while (matched < shared_ && next != parent.end() &&
               LabelKey(first->wstring()) == LabelKey(next->wstring())) {
            ++matched;
            ++first;
            ++next;
        }
        shared_ = matched;
    }

    [[nodiscard]] std::wstring Suffix(const std::filesystem::path& itemPath) const {
        const auto parent = DisplayParent(itemPath);
        if (shared_ == 0) return parent.wstring();
        auto component = parent.begin();
        for (size_t i = 0; i < shared_ && component != parent.end(); ++i) ++component;
        std::filesystem::path suffix;
        for (; component != parent.end(); ++component) suffix /= *component;
        // A file directly under the shared parent still needs a visible label.
        return suffix.empty() ? L"." : suffix.wstring();
    }

private:
    [[nodiscard]] static std::filesystem::path DisplayParent(const std::filesystem::path& itemPath) {
        auto path = itemPath.wstring();
        std::replace(path.begin(), path.end(), L'/', L'\\');
        return std::filesystem::path(path).parent_path();
    }

    std::filesystem::path first_;
    size_t count_ = 0;
    size_t shared_ = 0;
};

[[nodiscard]] inline std::wstring WithParentSuffix(std::wstring base, const std::wstring& suffix) {
    if (!suffix.empty()) base += L" [" + suffix + L"]";
    return base;
}

template<class Items, class MakeBase, class GetPath>
[[nodiscard]] std::wstring ParentSuffixForLabel(const std::filesystem::path& itemPath,
                                               const std::wstring& base, const Items& items,
                                               MakeBase makeBase, GetPath getPath) {
    const auto key = LabelKey(base);
    CommonParentPath group;
    group.Include(itemPath);
    size_t matches = 0;
    for (const auto& item : items) {
        if (LabelKey(makeBase(item)) != key) continue;
        ++matches;
        group.Include(getPath(item));
    }
    return matches > 1 ? group.Suffix(itemPath) : L"";
}

// A snapshot for one list rebuild, owned by its UI-thread caller. Base labels
// are computed once; duplicate detection never rescans the source collection.
class LabelBatch {
public:
    template<class Items, class MakeBase>
    LabelBatch(const Items& items, MakeBase makeBase) {
        bases_.reserve(items.size());
        counts_.reserve(items.size());
        for (const auto& item : items) {
            bases_.push_back(makeBase(item));
            ++counts_[LabelKey(bases_.back())];
        }
    }

    [[nodiscard]] const std::wstring& Base(size_t index) const { return bases_.at(index); }
    [[nodiscard]] bool Duplicated(const std::wstring& base) const {
        const auto found = counts_.find(LabelKey(base));
        return found != counts_.end() && found->second > 1;
    }

    template<class Items, class GetPath>
    [[nodiscard]] std::vector<std::wstring> ParentSuffixes(const Items& items, GetPath getPath) const {
        std::unordered_map<std::wstring, CommonParentPath> groups;
        // References to map values remain valid as more groups are inserted.
        std::vector<const CommonParentPath*> groupByIndex(items.size(), nullptr);
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& base = Base(i);
            auto key = LabelKey(base);
            if (counts_.at(key) < 2) continue;
            auto& group = groups[std::move(key)];
            group.Include(getPath(items[i]));
            groupByIndex[i] = &group;
        }
        std::vector<std::wstring> suffixes(items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            if (const auto* group = groupByIndex[i]) suffixes[i] = group->Suffix(getPath(items[i]));
        }
        return suffixes;
    }

private:
    std::vector<std::wstring> bases_;
    std::unordered_map<std::wstring, size_t> counts_;
};

} // namespace workspace_display
