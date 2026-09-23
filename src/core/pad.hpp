#ifndef VSTREAMER_CORE_PAD_HPP
#define VSTREAMER_CORE_PAD_HPP

#include <cstdint>

#include "core/frame.hpp"
#include "core/packet_kind.hpp"

namespace vstreamer
{

enum class pad_direction_e : uint8_t
{
    INPUT = 0,
    OUTPUT = 1,
};

struct pad_desc
{
    uint8_t          port = 0;
    pad_direction_e  direction = pad_direction_e::INPUT;
    packet_kind_e    packet_kind = packet_kind_e::UNKNOWN;
    media_kind_e     frame_kind = media_kind_e::UNKNOWN;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_PAD_HPP
