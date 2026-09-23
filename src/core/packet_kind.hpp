#ifndef VSTREAMER_CORE_PACKET_KIND_HPP
#define VSTREAMER_CORE_PACKET_KIND_HPP

#include <cstdint>

namespace vstreamer
{

enum class packet_kind_e : uint8_t
{
    UNKNOWN = 0,
    FRAME = 1,
    AUDIO = 2,
    SOCK = 3,
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_PACKET_KIND_HPP
