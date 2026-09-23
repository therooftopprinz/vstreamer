#ifndef VSTREAMER_SOURCE_NOISE_SOURCE_HPP
#define VSTREAMER_SOURCE_NOISE_SOURCE_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_NOISE_SOURCE
#error "noise_source requires -DENABLE_NOISE_SOURCE=ON"
#endif

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include "core/component_source.hpp"

namespace vstreamer
{

/* Synthetic MJPEG or NV12 snow (rover camera noise path). Default 416x240 MJPEG. */
class noise_source : public component_source
{
public:
    noise_source();
    ~noise_source() override;

    noise_source(const noise_source &) = delete;
    noise_source &operator=(const noise_source &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e output_kind() const override;

    int  open() override;
    void close() override;

    int output(uint8_t port, data_packet &out, int timeout_ms) override;

    int configure(uint64_t key, int64_t value) override;
    int query(uint64_t key, int64_t *value) const override;

    int configure(std::string_view key, std::string_view *value) override;
    int query(std::string_view key, std::string_view *value) const override;

private:
    int  ensure_encoder_locked();
    void free_encoder_locked();
    int  make_jpeg_locked(int64_t frame_pts, uint8_t **out, size_t *out_sz);
    int  make_nv12_locked(uint8_t **out, size_t *out_sz);
    void pace_locked(int timeout_ms);

    mutable std::mutex mu;
    bool               opened = false;

    int width = 416;
    int height = 240;
    int fps = 30;
    bool output_nv12 = false;

    int64_t pts = 0;
    double  due_sec = 0;

    /* Opaque libav handles; typed in the .cpp. */
    void *enc = nullptr;
    void *avframe = nullptr;
    void *pkt = nullptr;
    int   enc_w = 0;
    int   enc_h = 0;
    uint32_t rng = 1;

    mutable std::string query_buf;
};

}  // namespace vstreamer

#endif  // VSTREAMER_SOURCE_NOISE_SOURCE_HPP
