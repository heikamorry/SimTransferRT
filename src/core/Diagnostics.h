#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace rt {
struct Distribution {
    std::uint64_t count = 0;
    std::uint64_t maxUs = 0;
    std::uint64_t p50Us = 0;
    std::uint64_t p95Us = 0;
    std::uint64_t p99Us = 0;
    std::uint64_t overLimit = 0;
    std::uint64_t p999Us = 0;
};

// All observations; quantiles are 100us bucket upper bounds, capped at the
// exact maximum for the overflow bucket. Fixed storage, no hot-path allocations.
class TimingHistogram {
  public:
    void add(std::uint64_t us, std::uint64_t limit = UINT64_MAX) {
        ++count_;
        max_ = std::max(max_, us);
        if (us > limit)
            ++over_;
        ++bins_[std::min<std::uint64_t>(1001, (us + 99) / 100)];
    }

    Distribution summary() const {
        return {count_, max_, quantile(50), quantile(95), quantile(99), over_, quantile(999, 1000)};
    }

  private:
    std::array<std::uint64_t, 1002> bins_{};
    std::uint64_t count_ = 0;
    std::uint64_t max_ = 0;
    std::uint64_t over_ = 0;

    std::uint64_t quantile(unsigned p, unsigned denominator = 100) const {
        if (!count_)
            return 0;
        auto target = (count_ * p + denominator - 1) / denominator;
        auto sum = std::uint64_t(0);
        for (std::size_t i = 0; i < bins_.size(); ++i) {
            sum += bins_[i];
            if (sum >= target)
                return i == 1001 ? max_ : std::min(max_, std::uint64_t(i) * 100);
        }
        return max_;
    }
};

struct StageProfile {
    std::uint64_t calls = 0;
    std::uint64_t totalUs = 0;
    std::uint64_t maxUs = 0;

    void add(std::uint64_t us) {
        ++calls;
        totalUs += us;
        maxUs = std::max(maxUs, us);
    }
};
} // namespace rt
