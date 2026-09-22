#ifndef VSTREAMER_CORE_COMPONENT_INPUT_HPP
#define VSTREAMER_CORE_COMPONENT_INPUT_HPP

#include <cstdint>

#include "core/frame.hpp"

namespace vstreamer
{

class component_input
{
public:
    virtual ~component_input() = default;

    virtual int input(uint8_t port, const frame &in) = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_INPUT_HPP
