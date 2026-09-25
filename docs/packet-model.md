# Packet model

Pipeline wires carry `data_packet` (`core/data_packet.hpp`): owned
`packet_body` subclasses (`frame_data`, `audio_data`, `sock_data`).

Receiver RX/gap counters are cached on **stream_sender** (`peer_*` via `query()`) — not
`data_packet`. See [pipeline-flow.md](pipeline-flow.md).

## Stream path

```text
TX: … → rtp_h264_pay → stream_sender.0

RX: stream_receiver.0 → rtp_h264_depay → h264_decoder → sdl_sink
```
