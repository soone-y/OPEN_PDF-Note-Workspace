#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace workspace_display {

[[nodiscard]] inline std::wstring LabelKey(std::wstring label) {
    std::transform(label.begin(), label.end(), label.begin(), ::towlower);
    return label;
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

private:
    std::vector<std::wstring> bases_;
    std::unordered_map<std::wstring, size_t> counts_;
};

} // namespace workspace_display
