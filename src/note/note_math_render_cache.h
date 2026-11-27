#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mathrender { class Formula; }

namespace note {
// Owned by one view. Published placements retain their own formula references,
// so eviction and view rebinding cannot revoke a completed frame's resources.
class NoteMathRenderCache final {
public:
    [[nodiscard]] std::shared_ptr<const mathrender::Formula> Find(std::wstring_view source) noexcept;
    void Clear() noexcept;
private:
    std::unordered_map<std::wstring, std::shared_ptr<const mathrender::Formula>> entries_;
};
} // namespace note
