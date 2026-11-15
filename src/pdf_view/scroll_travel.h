#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace pdf_scroll {

// Keep roughly 200 ms of absolute travel, measured in viewport lengths.
// Time buckets bound the work and storage even for bursts of input messages.
class RecentTravel {
public:
    void Reset() noexcept { *this = {}; }

    void Observe(std::uint64_t now, double position, int extent, double scale, int flow) noexcept {
        if (extent <= 0) {
            Reset();
            return;
        }
        if (extent != extent_ || scale != scale_ || flow != flow_) {
            Reset();
            extent_ = extent;
            scale_ = scale;
            flow_ = flow;
            position_ = position;
            return;
        }
        const double distance = std::abs(position - position_);
        position_ = position;
        if (distance == 0.0) return;
        const auto tick = now / kBucketMs;
        auto& bucket = buckets_[tick % buckets_.size()];
        if (bucket.tick != tick) bucket = {tick, 0.0};
        bucket.travel += distance / extent;
    }

    [[nodiscard]] double Viewports(std::uint64_t now) const noexcept {
        const auto tick = now / kBucketMs;
        double travel = 0.0;
        for (const auto& bucket : buckets_) {
            if (tick >= bucket.tick && tick - bucket.tick < buckets_.size()) {
                travel += bucket.travel;
            }
        }
        return travel;
    }

private:
    static constexpr std::uint64_t kBucketMs = 16;
    struct Bucket {
        std::uint64_t tick = 0;
        double travel = 0.0;
    };
    std::array<Bucket, 13> buckets_{};
    double position_ = 0.0;
    double scale_ = 0.0;
    int extent_ = 0;
    int flow_ = 0;
};

} // namespace pdf_scroll
