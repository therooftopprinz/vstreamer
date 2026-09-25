#ifndef VSTREAMER_CORE_STREAM_HEADER_HPP
#define VSTREAMER_CORE_STREAM_HEADER_HPP

#include "core/rs_block_erasure.hpp"

#include <cstddef>
#include <cstdint>

namespace vstreamer
{

/* Fixed 2-byte prefix on every stream datagram (UDP loss telemetry via stream_sequence). */
struct stream_header_s
{
    uint16_t stream_sequence; /* big-endian on wire */
};

inline constexpr size_t k_stream_header_len = sizeof(uint16_t);

/*
 * Wire layout (one UDP payload):
 *
 *   stream_header_s + FEC shard (4-byte hdr + body)
 *
 * stream_sequence is always present on vstreamer egress; receivers use it for gap loss.
 */

inline void stream_header_store_be16(uint8_t *wire, uint16_t seq)
{
    wire[0] = static_cast<uint8_t>((seq >> 8) & 0xFF);
    wire[1] = static_cast<uint8_t>(seq & 0xFF);
}

[[nodiscard]] inline uint16_t stream_header_sequence_be16(const uint8_t *wire)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(wire[0]) << 8) |
                                 static_cast<uint16_t>(wire[1]));
}

[[nodiscard]] inline bool stream_datagram_len_ok(size_t len)
{
    return len >= k_stream_header_len + rs_block_erasure::k_header_len;
}

[[nodiscard]] inline const uint8_t *stream_fec_shard(const uint8_t *payload, size_t len,
                                                     size_t *fec_len_out)
{
    if (fec_len_out == nullptr || !stream_datagram_len_ok(len))
    {
        return nullptr;
    }
    *fec_len_out = len - k_stream_header_len;
    return payload + k_stream_header_len;
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_STREAM_HEADER_HPP
