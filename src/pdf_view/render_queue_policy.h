#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>

namespace pdf_render_queue {

// No UI, locks or allocation. The owner refreshes pending job priorities from
// its current viewport, and calls this only while holding the queue mutex.
inline constexpr unsigned kMaxVisibleBurst = 4;

template <class Jobs>
[[nodiscard]] std::optional<std::size_t> ChooseNext(
    const Jobs& jobs, unsigned visibleBurst) noexcept {
    std::optional<std::size_t> nearestVisible;
    std::optional<std::size_t> oldestOffscreen;
    double nearestDistance = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        if (!jobs[i].visible) {
            if (!oldestOffscreen) oldestOffscreen = i;
            continue;
        }
        const double distance = std::isfinite(jobs[i].distance) && jobs[i].distance >= 0.0
            ? jobs[i].distance : std::numeric_limits<double>::infinity();
        if (!nearestVisible || distance < nearestDistance) {
            nearestVisible = i;
            nearestDistance = distance;
        }
    }
    // Bound starvation without preempting any active PDFium render. Equal
    // visible distances and offscreen jobs retain enqueue order.
    if (oldestOffscreen && visibleBurst >= kMaxVisibleBurst) return oldestOffscreen;
    return nearestVisible ? nearestVisible : oldestOffscreen;
}

[[nodiscard]] inline unsigned AfterDispatch(unsigned visibleBurst, bool visible) noexcept {
    return visible ? (visibleBurst < kMaxVisibleBurst ? visibleBurst + 1 : kMaxVisibleBurst) : 0;
}

} // namespace pdf_render_queue
