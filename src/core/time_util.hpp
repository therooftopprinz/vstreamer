#ifndef VSTREAMER_CORE_TIME_UTIL_HPP
#define VSTREAMER_CORE_TIME_UTIL_HPP

#include <chrono>
#include <cstdint>

namespace vstreamer
{

/* Monotonic ns from steady_clock (same epoch on one process; use for latency deltas). */
[[nodiscard]] inline int64_t steady_mono_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_TIME_UTIL_HPP
