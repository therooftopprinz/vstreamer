#ifndef VSTREAMER_COMPONENTS_H264_ENCODER_INTEL_HPP
#define VSTREAMER_COMPONENTS_H264_ENCODER_INTEL_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_H264_ENCODER_INTEL
#error "h264_encoder_intel requires -DENABLE_H264_ENCODER_INTEL=ON"
#endif

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

#include "core/component_coder.hpp"
#include "core/frame.hpp"

namespace vstreamer
{

/* NV12 → H.264 via libavcodec h264_vaapi (Intel/VA-API). */
class h264_encoder_intel : public component_coder
{
public:
    h264_encoder_intel();
    ~h264_encoder_intel() override;

    h264_encoder_intel(const h264_encoder_intel &) = delete;
    h264_encoder_intel &operator=(const h264_encoder_intel &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e input_kind() const override;
    [[nodiscard]] media_kind_e output_kind() const override;

    int  open() override;
    void close() override;

    int input(uint8_t port, const frame &in) override;
    int output(uint8_t port, frame &out, int timeout_ms) override;

    int configure(uint64_t key, int64_t value) override;
    int query(uint64_t key, int64_t *value) const override;

    int configure(std::string_view key, std::string_view *value) override;
    int query(std::string_view key, std::string_view *value) const override;

private:
    [[nodiscard]] int nv12_size_locked() const;
    int  codec_open_locked();
    void codec_close_locked();
    int  reopen_if_needed_locked();
    int  drain_packets_locked();
    void clear_out_locked();

    mutable std::mutex      mu;
    std::condition_variable cv;

    bool opened = false;
    bool reopen_req = false;

    int width = 1280;
    int height = 720;
    int fps = 30;
    int qp = 36;
    int gop = 30;
    std::string device;

    void *hw_device = nullptr;
    void *ctx = nullptr;
    void *swframe = nullptr;
    void *hwframe = nullptr;
    void *pkt = nullptr;
    int   live_w = 0;
    int   live_h = 0;
    int   live_fps = 0;
    int   live_qp = 0;
    int   live_gop = 0;

    std::deque<frame> out_q;

    mutable std::string query_buf;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_H264_ENCODER_INTEL_HPP
