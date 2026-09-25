#ifndef VSTREAMER_COMPONENTS_SDL_NV12_PRESENTER_HPP
#define VSTREAMER_COMPONENTS_SDL_NV12_PRESENTER_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_SDL_SINK
#error "sdl_nv12_presenter requires -DENABLE_SDL_SINK=ON"
#endif

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "core/packet_types.hpp"

namespace vstreamer
{

/* Shared SDL NV12 window/texture path for sdl_sink and sdl_kmsdrm_sink. */
class sdl_nv12_presenter
{
public:
    sdl_nv12_presenter(const char *log_tag, const char *video_driver);

    int  open();
    void close();

    /* Create window/texture before first frame (black until present). */
    int prepare(int w, int h, bool &session_open);

    int present(const frame_data &f, bool &session_open);

    void set_title(std::string_view title);
    void stats_string(char *buf, size_t buflen, uint64_t frames_in) const;

    [[nodiscard]] int live_width() const { return live_w; }
    [[nodiscard]] int live_height() const { return live_h; }

private:
    void destroy_video_locked();
    void destroy_texture_locked();
    void pump_events_locked(bool &session_open);
    int  ensure_video_locked(int w, int h);
    int  present_nv12_locked(const frame_data &f, bool &session_open);

    const char *log_tag;
    const char *video_driver;

    mutable std::mutex mu;

    bool sdl_ready = false;

    std::string title = "vstreamer";

    int live_w = 0;
    int live_h = 0;
    int tex_w = 0;
    int tex_h = 0;

    void *window = nullptr;
    void *renderer = nullptr;
    void *texture = nullptr;

    int  console_tty_fd = -1;
    int  console_vt = 0;
    bool console_kd_graphics = false;

    std::thread::id render_thread_id {};
    bool            render_thread_bound = false;

    uint64_t present_ok_count = 0;
    uint64_t present_fail_count = 0;
    double   last_latency_ms = 0.0;
    mutable std::string last_err;

    void note_present_failure(const char *op);
    void clear_sdl_error();
    bool check_sdl_error_after(const char *op);
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_SDL_NV12_PRESENTER_HPP
