#pragma once
#include <cstdint>

namespace pdf_text_edit {

// UI-thread owned. Keeps the edit alive while a viewing gesture temporarily
// owns the viewport. No timer, document mutation or deferred focus change.
class ViewFollow {
public:
    void Reset() noexcept { *this = {}; }
    void Input(std::uint64_t now) noexcept {
        lastInput_ = now;
        active_ = true;
        suspended_ = false;
    }
    void Suspend() noexcept { suspended_ = true; }
    [[nodiscard]] bool Following(std::uint64_t now) const noexcept {
        return active_ && !suspended_ && now - lastInput_ < 10000;
    }
private:
    std::uint64_t lastInput_ = 0;
    bool active_ = false;
    bool suspended_ = false;
};

} // namespace pdf_text_edit
