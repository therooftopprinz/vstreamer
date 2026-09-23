#ifndef VSTREAMER_COMPONENTS_MKV_SINK_HPP
#define VSTREAMER_COMPONENTS_MKV_SINK_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_MKV_SINK
#error "mkv_sink requires -DENABLE_MKV_SINK=ON"
#endif

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include "core/component_sink.hpp"
#include "core/frame.hpp"

namespace vstreamer
{

/* MJPEG → Matroska via libavformat; incremental cluster writes for live-ish playback. */
class mkv_sink : public component_sink
{
public:
    mkv_sink();
    ~mkv_sink() override;

    mkv_sink(const mkv_sink &) = delete;
    mkv_sink &operator=(const mkv_sink &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e input_kind() const override;

    int  open() override;
    void close() override;

    int input(uint8_t port, const data_packet &in) override;

    int configure(uint64_t key, int64_t value) override;
    int query(uint64_t key, int64_t *value) const override;

    int configure(std::string_view key, std::string_view *value) override;
    int query(std::string_view key, std::string_view *value) const override;

private:
    void stop_locked();
    int  start_locked(int w, int h);
    int  ensure_session_locked(int w, int h);
    int  write_frame_locked(const data_packet &in);

    mutable std::mutex mu;

    bool opened = false;

    std::string output_path;
    int         cfg_width = 0;
    int         cfg_height = 0;
    int         fps = 30;

    bool recording = false;
    int  live_w = 0;
    int  live_h = 0;
    int64_t pts = 0;
    double  t0 = 0.0;
    uint64_t frames_out = 0;

    /* Opaque libav handles; typed in the .cpp. */
    void *fmt = nullptr;
    void *stream = nullptr;

    mutable std::string query_buf;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_MKV_SINK_HPP
