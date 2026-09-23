#ifndef VSTREAMER_CORE_COMPONENT_STREAM_TELEMETRY_HPP
#define VSTREAMER_CORE_COMPONENT_STREAM_TELEMETRY_HPP

#include <cstdint>

#include "core/stream_telemetry.hpp"

namespace vstreamer
{

/*
 * Pad 1 on stream_sender: link metrics for rate / CBR logic (not data_packet).
 * The pipeline core polls telemetry_snapshot() and forwards to feedback nodes.
 */
class component_stream_telemetry
{
public:
    static constexpr uint8_t k_pad = 1;

    virtual ~component_stream_telemetry() = default;

    [[nodiscard]] virtual stream_telemetry telemetry_snapshot() const = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_STREAM_TELEMETRY_HPP
