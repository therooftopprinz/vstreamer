#ifndef VSTREAMER_CORE_STREAM_AIR_LIMITS_HPP
#define VSTREAMER_CORE_STREAM_AIR_LIMITS_HPP

#include "core/stream_header.hpp"

#include <cstddef>

namespace vstreamer
{

/* Match winject-manager stream caps (1476 WiFi MPDU − stream_header_s). */
inline constexpr size_t k_wifi_payload_max = 1476;
inline constexpr size_t k_stream_payload_max = k_wifi_payload_max - k_stream_header_len;

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_STREAM_AIR_LIMITS_HPP
