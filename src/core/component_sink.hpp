#ifndef VSTREAMER_CORE_COMPONENT_SINK_HPP
#define VSTREAMER_CORE_COMPONENT_SINK_HPP

#include <string>

#include "core/component.hpp"
#include "core/component_input.hpp"
#include "core/frame.hpp"
#include "core/packet_kind.hpp"

namespace vstreamer
{

class component_sink : public component, public component_input
{
public:
    ~component_sink() override = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual media_kind_e input_kind() const = 0;

    [[nodiscard]] virtual uint8_t input_pad_count() const { return 1; }

    [[nodiscard]] virtual packet_kind_e input_packet_kind(uint8_t port) const
    {
        if (0 != port)
        {
            return packet_kind_e::UNKNOWN;
        }
        return packet_kind_e::FRAME;
    }

    virtual int  open() = 0;
    virtual void close() = 0;

    virtual int set_enabled(bool on, int timeout_ms);
    [[nodiscard]] virtual bool enabled() const;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_SINK_HPP
