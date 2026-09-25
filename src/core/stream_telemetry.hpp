#ifndef VSTREAMER_CORE_STREAM_TELEMETRY_HPP
#define VSTREAMER_CORE_STREAM_TELEMETRY_HPP

#include <cstdint>

namespace vstreamer
{

/* Receiver RX and gap counters (cached on stream_sender from reverse telemetry). */
struct stream_receiver_counters
{
    uint64_t udp_packet_received = 0;
    /* App payloads delivered past RS (post-FEC output), like packets after "remove FEC". */
    uint64_t fec_packet_received = 0;
    /* stream_sequence forward gaps on wire (pre-FEC datagrams). */
    uint64_t udp_gap_count = 0;
    /* Undelivered post-FEC output packets (RS block failure / missing data slots). */
    uint64_t fec_gap_count = 0;
    /* Valid stream air shards received (wire, pre-FEC). */
    uint64_t fec_air_shard_received = 0;
    /* 5-sample moving average of interval delta loss (percent). */
    double loss_udp_pct = 0.;
    double loss_fec_pct = 0.;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_STREAM_TELEMETRY_HPP
