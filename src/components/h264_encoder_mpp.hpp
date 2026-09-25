#ifndef VSTREAMER_COMPONENTS_H264_ENCODER_MPP_HPP
#define VSTREAMER_COMPONENTS_H264_ENCODER_MPP_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_H264_ENCODER_MPP
#error "h264_encoder_mpp requires -DENABLE_H264_ENCODER_MPP=ON"
#endif

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>
#include <string>
#include <string_view>

#include "core/component_coder.hpp"
#include "core/frame.hpp"

namespace vstreamer
{

/* Packed NV12 → H.264 Annex-B (Rockchip MPP, RK3588 / OPI5). */
class h264_encoder_mpp : public component_coder
{
public:
    h264_encoder_mpp();
    ~h264_encoder_mpp() override;

    h264_encoder_mpp(const h264_encoder_mpp &) = delete;
    h264_encoder_mpp &operator=(const h264_encoder_mpp &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e input_kind() const override;
    [[nodiscard]] media_kind_e output_kind() const override;

    int  open() override;
    void close() override;

    /* Wake blocking encode drain / output waits (shutdown without joining a stuck thread). */
    void cancel_pending_io();

    int input(uint8_t port, const data_packet &in) override;
    int output(uint8_t port, data_packet &out, int timeout_ms) override;

    int configure(uint64_t key, int64_t value) override;
    int query(uint64_t key, int64_t *value) const override;

    int configure(std::string_view key, std::string_view *value) override;
    int query(std::string_view key, std::string_view *value) const override;

private:
    int  encoder_open_locked();
    void encoder_close_locked();
    int  reopen_if_needed_locked();
    int  apply_h264_cfg_locked();
    int  apply_rc_cfg_locked();
    int  drain_packets_locked(int timeout_ms);
    int  drain_packets_unlocked(int timeout_ms);
    int  put_nv12_frame_unlocked(const frame_data &f);
    bool push_mpp_packet_to_out(void *packet);
    bool push_mpp_packet_to_out_locked(void *packet);
    void clear_out_locked();
    void release_enc_slot_after_eoi();
    void drain_enc_packets_nonblock(void *mpp_ctx, void *mpp_mpi);
    bool append_enc_packet_bytes(const uint8_t *data, size_t len);
    void flush_enc_au_to_out_locked();
    void ingest_enc_packet(void *mpp_packet_opaque);

    mutable std::mutex      mu;
    std::mutex              mpp_api_mu;
    std::condition_variable cv;

    bool opened = false;
    bool reopen_req = false;
    std::atomic<bool> cancel_io {false};

    int width = 1920;
    int height = 1080;
    int fps = 30;
    int qp = 36;
    int gop = 30;
    /* Target wire bitrate (bit/s); metrics h264_encoder.cbr_kbps. Honored by MPP when rc_mpp_cbr. */
    int  bps_target = 20'000'000;
    bool rc_mpp_cbr = false;

    int live_w = 0;
    int live_h = 0;
    int live_hor = 0;
    int live_ver = 0;
    int live_fps = 0;
    int live_qp = 0;
    int live_gop = 0;
    int live_bps = 0;

    /* Opaque MPP handles; typed in the .cpp. */
    void *ctx = nullptr;
    void *mpi = nullptr;
    void *enc_cfg = nullptr;
    void *frm_grp = nullptr;
    void *md_info = nullptr;
    uint64_t enc_frames_in = 0;

    std::vector<uint8_t> enc_au_accum;
    int64_t              enc_au_pts = 0;
    int64_t              enc_au_capture_mono_ns = 0;
    bool                 enc_au_key = false;

    static constexpr int enc_slot_count = 4;
    struct enc_slot
    {
        void   *frm = nullptr;
        void   *pkt = nullptr;
        int64_t capture_mono_ns = 0;
    };
    std::array<enc_slot, enc_slot_count> enc_slots {};
    std::deque<int>                      enc_free_slots;
    std::deque<int>                      enc_pending_slots;

    std::deque<frame> out_q;

    /* Capture-to-encoded-AU (ms); updated when output carries capture_mono_ns. */
    double last_latency_ms = 0.0;

    mutable std::string query_buf;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_H264_ENCODER_MPP_HPP
