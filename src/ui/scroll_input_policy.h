#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace scroll_input {

// UI-thread owned, with no timers or deferred movement. A precision gesture
// stays precision across both axes, including packets that happen to be 120.
// Reset at document/focus/navigation boundaries. No input is ever replayed
// into another document. Classification does not depend on stabilization.
class WheelInput {
public:
    void Reset() noexcept { *this = {}; }

    [[nodiscard]] bool IsPrecise(int delta, uint32_t time, bool touchpad = false) noexcept {
        if (delta == 0) return precise_;
        if (!active_ || static_cast<uint32_t>(time - time_) > 250) Reset();
        active_ = true;
        time_ = time;
        precise_ = precise_ || touchpad || delta % 120 != 0;
        return precise_;
    }

    // Buffer only a small reversal during an established direction. Continuing
    // the old direction discards that jitter; a deliberate reversal releases
    // its full buffered distance at the threshold. No added inertia/latency
    // applies to motion in the established direction. OFF bypasses the filter.
    [[nodiscard]] double Filter(bool horizontal, double pixels, uint32_t time,
                                bool enabled, double threshold = 2.0) noexcept {
        auto& axis = axes_[horizontal ? 1 : 0];
        if (!std::isfinite(pixels)) { axis = {}; return 0.0; }
        if (!enabled) { axis = {}; return pixels; }
        if (pixels == 0.0) return 0.0;
        if (!axis.active || static_cast<uint32_t>(time - axis.time) > 150) axis = {};
        axis.active = true;
        axis.time = time;
        const int direction = pixels > 0.0 ? 1 : -1;
        if (axis.direction == 0 || axis.direction == direction) {
            axis.direction = direction;
            axis.pending = 0.0;
            return pixels;
        }
        axis.pending += pixels;
        if (std::abs(axis.pending) < threshold) return 0.0;
        const double result = axis.pending;
        axis.pending = 0.0;
        axis.direction = direction;
        return result;
    }

private:
    struct Axis {
        bool active = false;
        uint32_t time = 0;
        int direction = 0;
        double pending = 0.0;
    };
    bool active_ = false;
    bool precise_ = false;
    uint32_t time_ = 0;
    Axis axes_[2]{};
};

// Integer-coordinate controls must retain fractional pixels per view and axis.
class PixelRemainder {
public:
    void Reset() noexcept { remainder_ = 0.0; }
    [[nodiscard]] int Take(double pixels) noexcept {
        if (!std::isfinite(pixels)) { Reset(); return 0; }
        const double sum = pixels + remainder_;
        const double nearest = std::round(sum);
        const double whole = std::abs(sum - nearest) < 1e-9 ? nearest : std::trunc(sum);
        if (whole > std::numeric_limits<int>::max()) { Reset(); return std::numeric_limits<int>::max(); }
        if (whole < std::numeric_limits<int>::min()) { Reset(); return std::numeric_limits<int>::min(); }
        remainder_ = std::abs(sum - whole) < 1e-9 ? 0.0 : sum - whole;
        return static_cast<int>(whole);
    }
private:
    double remainder_ = 0.0;
};

// Native LISTBOX wheel accumulation is shared across controls on Windows.
// Own exact fractional rows per list, independently of the PDF/note filter.
// No timer, inertia, acceleration or deferred movement is introduced here.
class ListWheelInput {
public:
    void Reset() noexcept { *this = {}; }

    [[nodiscard]] int Take(int delta, uint32_t rowsPerNotch) noexcept {
        if (rowsPerNotch != rows_) Reset();
        rows_ = rowsPerNotch;
        const int64_t total = remainder_ - static_cast<int64_t>(delta) * rowsPerNotch;
        const int64_t whole = total / 120;
        remainder_ = total % 120;
        if (whole > std::numeric_limits<int>::max()) { Reset(); return std::numeric_limits<int>::max(); }
        if (whole < std::numeric_limits<int>::min()) { Reset(); return std::numeric_limits<int>::min(); }
        return static_cast<int>(whole);
    }

private:
    uint32_t rows_ = 0;
    int64_t remainder_ = 0;
};

} // namespace scroll_input
