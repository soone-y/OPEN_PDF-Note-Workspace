#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace pdf_keyboard_scroll {

struct Delta {
    double x = 0.0;
    double y = 0.0;
};

// Pure key/time state. The window owns focus, document validity and its timer.
class Motion {
public:
    [[nodiscard]] static int KeyIndex(unsigned key) {
        switch (key) {
        case 'W': return 0;
        case 'A': return 1;
        case 'S': return 2;
        case 'D': return 3;
        default: return -1;
        }
    }

    void SetKey(unsigned key, bool down) {
        const int index = KeyIndex(key);
        if (index < 0) return;
        keys_[static_cast<size_t>(index)] = down;
        // Released/opposing axes stop immediately, without a drift into text editing.
        if (keys_[1] == keys_[3]) velocity_.x = 0.0;
        if (keys_[0] == keys_[2]) velocity_.y = 0.0;
    }

    [[nodiscard]] bool HasKeys() const {
        return std::any_of(keys_.begin(), keys_.end(), [](bool down) { return down; });
    }

    void Reset() { keys_ = {}; velocity_ = {}; }

    [[nodiscard]] Delta Advance(double elapsedSeconds) {
        if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0) return {};
        // A stalled UI must not jump by the entire unobserved interval.
        const double dt = std::min(elapsedSeconds, 0.05);
        double x = static_cast<double>(keys_[3]) - static_cast<double>(keys_[1]);
        double y = static_cast<double>(keys_[2]) - static_cast<double>(keys_[0]);
        const double length = std::hypot(x, y);
        if (length == 0.0) return {};
        constexpr double speed = 600.0; // screen pixels per second, including diagonals
        constexpr double rampSeconds = 0.06;
        x *= speed / length;
        y *= speed / length;
        const double decay = std::exp(-dt / rampSeconds);
        const Delta movement{
            x * dt + (velocity_.x - x) * rampSeconds * (1.0 - decay),
            y * dt + (velocity_.y - y) * rampSeconds * (1.0 - decay)};
        velocity_.x = x + (velocity_.x - x) * decay;
        velocity_.y = y + (velocity_.y - y) * decay;
        return movement;
    }

private:
    std::array<bool, 4> keys_{};
    Delta velocity_{};
};

} // namespace pdf_keyboard_scroll
