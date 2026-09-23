#ifndef VSTREAMER_COMPONENTS_H264_DECODER_MPP_HPP
#define VSTREAMER_COMPONENTS_H264_DECODER_MPP_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_H264_DECODER_MPP
#error "h264_decoder_mpp requires -DENABLE_H264_DECODER_MPP=ON"
#endif

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

#include "core/component_coder.hpp"
#include "core/output_opts.hpp"

namespace vstreamer
{

/* Rockchip MPP H.264 (Annex-B) → packed NV12 (GS receive path). */
class h264_decoder_mpp : public component_coder
{
public:
    h264_decoder_mpp();
    ~h264_decoder_mpp() override;

    h264_decoder_mpp(const h264_decoder_mpp &) = delete;
    h264_decoder_mpp &operator=(const h264_decoder_mpp &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e input_kind() const override;
    [[nodiscard]] media_kind_e output_kind() const override;

    int  open() override;
    void close() override;

    void cancel_pending_io();

    int input(uint8_t port, const data_packet &in) override;
    int output(uint8_t port, data_packet &out, int timeout_ms) override;

    int configure(uint64_t key, int64_t value) override;
    int query(uint64_t key, int64_t *value) const override;

    int configure(std::string_view key, std::string_view *value) override;
    int query(std::string_view key, std::string_view *value) const override;

private:
    int  ensure_decoder_locked();
    void free_decoder_locked();
    void clear_pending_locked();
    int  handle_info_change_locked(void *mpp_frame);
    void drain_mpp_to_ready_locked(int timeout_ms);
    int  fetch_one_mpp_frame_locked(int timeout_ms);
    int  pack_mpp_to_ready_locked(void *mpp_frame);

    mutable std::mutex mu;
    bool               opened = false;
    std::atomic<bool>  cancel_io {false};

    int width = 1280;
    int height = 720;
    int fps = 30;

    output_mode_e output_mode = output_mode_e::filter;
    media_kind_e  output_format = media_kind_e::NV12;

    /* Opaque MPP handles; typed in the .cpp. */
    void *ctx = nullptr;
    void *mpi = nullptr;
    void *frm_grp = nullptr;

    static constexpr size_t k_max_ready_frames = 8;
    int64_t                 pending_capture_mono_ns = 0;
    std::deque<frame>       ready_frames;

    /* Capture-to-decoded-frame (ms); updated when output carries capture_mono_ns. */
    double last_latency_ms = 0.0;

    mutable std::string query_buf;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_H264_DECODER_MPP_HPP
