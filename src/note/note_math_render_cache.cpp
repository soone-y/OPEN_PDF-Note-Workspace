#include "note/note_math_render_cache.h"
#include "math/math_render.h"

namespace note {
std::shared_ptr<const mathrender::Formula> NoteMathRenderCache::Find(std::wstring_view source) noexcept {
    if (source.empty()) return {};
    try {
        const std::wstring key(source);
        const auto found = entries_.find(key);
        if (found != entries_.end()) return found->second;
        auto formula = std::shared_ptr<const mathrender::Formula>(mathrender::Parse(source));
        if (!formula) return {};
        // Preserve the existing view's 512-formula budget. Outstanding frame
        // references remain valid when the lookup cache is cleared.
        if (entries_.size() >= 512) entries_.clear();
        entries_.emplace(key, formula);
        return formula;
    } catch (...) {
        return {};
    }
}
void NoteMathRenderCache::Clear() noexcept { entries_.clear(); }
} // namespace note
