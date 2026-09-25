#ifndef VSTREAMER_CORE_SEQUENCE_GAP_HPP
#define VSTREAMER_CORE_SEQUENCE_GAP_HPP

#include <cstdint>

namespace vstreamer
{

/*
 * Forward gap count for a monotonic sequence in a finite ring (e.g. u16 wire
 * stream_sequence, u8 FEC block_id). Reordered / duplicate values (backward
 * more than half the ring) return 0 and leave last unchanged.
 */
template <typename T>
[[nodiscard]] inline uint64_t note_forward_gap(T seq, T &last, bool &have)
{
    if (!have)
    {
        have = true;
        last = seq;
        return 0;
    }
    const T next = static_cast<T>(last + static_cast<T>(1));
    if (seq == next)
    {
        last = seq;
        return 0;
    }
    const T forward = static_cast<T>(seq - next);
    constexpr T half_ring = static_cast<T>(1) << (sizeof(T) * 8 - 1);
    if (forward >= half_ring)
    {
        return 0;
    }
    last = seq;
    return static_cast<uint64_t>(forward);
}

[[nodiscard]] inline uint64_t note_u16_forward_gap(uint16_t seq, uint16_t &last, bool &have)
{
    return note_forward_gap(seq, last, have);
}

[[nodiscard]] inline uint64_t note_u8_forward_gap(uint8_t seq, uint8_t &last, bool &have)
{
    return note_forward_gap(seq, last, have);
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_SEQUENCE_GAP_HPP
