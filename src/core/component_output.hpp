#ifndef VSTREAMER_CORE_COMPONENT_OUTPUT_HPP
#define VSTREAMER_CORE_COMPONENT_OUTPUT_HPP

#include <cstdint>

#include "core/frame.hpp"

namespace vstreamer
{

class component_output
{
public:
    virtual ~component_output() = default;

    virtual int output(uint8_t port, frame &out, int timeout_ms) = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_OUTPUT_HPP
