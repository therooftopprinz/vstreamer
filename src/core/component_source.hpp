#ifndef VSTREAMER_CORE_COMPONENT_SOURCE_HPP
#define VSTREAMER_CORE_COMPONENT_SOURCE_HPP

#include <string>

#include "core/component.hpp"
#include "core/component_output.hpp"
#include "core/frame.hpp"
#include "core/packet_kind.hpp"

namespace vstreamer
{

class component_source : public component, public component_output
{
public:
    ~component_source() override = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual media_kind_e output_kind() const = 0;

    [[nodiscard]] virtual uint8_t output_pad_count() const { return 1; }

    [[nodiscard]] virtual packet_kind_e output_packet_kind(uint8_t port) const
    {
        if (0 != port)
        {
            return packet_kind_e::UNKNOWN;
        }
        return packet_kind_e::FRAME;
    }

    virtual int  open() = 0;
    virtual void close() = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_SOURCE_HPP
