#pragma once

#include <cstdint>

namespace main_window_liveness {
// No I/O or application data is inspected. A transient hide resets on
// recovery; a sustained absence emits at most three spaced requests and
// one notification per episode. No missed intervals produce a burst.
class Policy {
public:
    explicit Policy(std::uint64_t start) noexcept : missingSince_(start) {}
    [[nodiscard]] bool Observe(bool visible, std::uint64_t now) noexcept {
        if (visible) {
            missing_ = false;
            requests_ = 0;
            notified_ = false;
            return false;
        }
        if (!missing_) {
            missing_ = true;
            missingSince_ = now;
        }
        if (requests_ >= 3 || now < missingSince_ || now - missingSince_ < 15000 ||
            (requests_ != 0 && (now < lastRequest_ || now - lastRequest_ < 5000))) return false;
        ++requests_;
        lastRequest_ = now;
        return true;
    }
    [[nodiscard]] bool TakeNotification() noexcept {
        if (requests_ < 3 || notified_) return false;
        notified_ = true;
        return true;
    }
private:
    std::uint64_t missingSince_ = 0;
    std::uint64_t lastRequest_ = 0;
    bool missing_ = true;
    unsigned requests_ = 0;
    bool notified_ = false;
};
} // namespace main_window_liveness
