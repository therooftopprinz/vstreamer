#ifndef VSTREAMER_CORE_FEC_STREAM_HEADER_HPP
#define VSTREAMER_CORE_FEC_STREAM_HEADER_HPP

#include <cstddef>
#include <cstdint>

namespace vstreamer
{

/* Prefix on each app payload after RS (before rtp depay strips it). */
struct fec_stream_header_s
{
    uint16_t fec_payload_sequence; /* big-endian on wire */
};

inline constexpr size_t k_fec_stream_header_len = sizeof(uint16_t);

inline void fec_stream_header_store_be16(uint8_t *wire, uint16_t seq)
{
    wire[0] = static_cast<uint8_t>((seq >> 8) & 0xFF);
    wire[1] = static_cast<uint8_t>(seq & 0xFF);
}

[[nodiscard]] inline uint16_t fec_stream_header_sequence_be16(const uint8_t *wire)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(wire[0]) << 8) |
                                 static_cast<uint16_t>(wire[1]));
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_FEC_STREAM_HEADER_HPP
