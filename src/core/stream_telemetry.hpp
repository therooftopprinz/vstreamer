#ifndef VSTREAMER_CORE_STREAM_TELEMETRY_HPP
#define VSTREAMER_CORE_STREAM_TELEMETRY_HPP

namespace vstreamer
{

/* Link metrics on stream_sender pad 1; not carried on data_packet. */
struct stream_telemetry
{
    float channel_loss = 0.f;
    float flow = 0.f;
    float rssi = 0.f;
    /* Recent UDP egress bitrate (kb/s), stream_sender pad 1. */
    float egress_kbps = 0.f;
    /* Measured forward-path goodput (kb/s), from channel / receiver counters. */
    float deliverable_kbps = 0.f;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_STREAM_TELEMETRY_HPP
