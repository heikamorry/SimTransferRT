#pragma once
#include <cstdint>

namespace rt {
// A simulation monotonic host clock, NOT an external device's sampling counter.
// Replace its use with a hardware snapshot provider for real acquisition.
std::uint64_t monotonicUs() noexcept;
void waitBrieflyUntil(std::uint64_t deadlineUs);
} // namespace rt
