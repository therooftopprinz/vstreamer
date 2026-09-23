# Packet model

Pipeline wires carry `data_packet` (`core/data_packet.hpp`): owned
`packet_body` subclasses (`frame_data`, `audio_data`, `sock_data`).

Link metrics use `stream_telemetry` on **stream_sender pad 1** only — not
`data_packet`. See [pipeline-flow.md](pipeline-flow.md).

## Stream path

```text
TX: … → rtp_h264_pay → stream_sender.0
    stream_sender.1 → encoder_cbr_logic → h264_encoder

RX: stream_receiver.0 → rtp_h264_depay → h264_decoder → sdl_sink
```
