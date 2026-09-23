#include "components/sdl_nv12_presenter.hpp"

#include "core/time_util.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL.h>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace vstreamer
{
namespace
{

#if defined(__linux__)
[[nodiscard]] int parse_vt_from_sysfs_name(const char *name)
{
    if (nullptr == name || name[0] == '\0')
    {
        return 0;
    }
    if (0 != std::strncmp(name, "tty", 3))
    {
        return 0;
    }
    const char *num = name + 3;
    if (*num < '0' || *num > '9')
    {
        return 0;
    }
    char *end = nullptr;
    const long v = std::strtol(num, &end, 10);
    if (nullptr == end || *end != '\0' || v < 1 || v > 63)
    {
        return 0;
    }
    return static_cast<int>(v);
}

[[nodiscard]] int detect_display_vt()
{
    const char *env_vt = std::getenv("VSTREAMER_KMSDRM_VT");
    if (nullptr != env_vt && env_vt[0] != '\0')
    {
        if (env_vt[0] == 't')
        {
            const int v = parse_vt_from_sysfs_name(env_vt);
            if (v > 0)
            {
                return v;
            }
        }
        char *end = nullptr;
        const long n = std::strtol(env_vt, &end, 10);
        if (end != env_vt && n >= 1 && n <= 63)
        {
            return static_cast<int>(n);
        }
    }

    FILE *active = std::fopen("/sys/class/tty/console/active", "r");
    if (nullptr != active)
    {
        char buf[64] = {};
        if (std::fgets(buf, sizeof(buf), active) != nullptr)
        {
            char *nl = std::strchr(buf, '\n');
            if (nullptr != nl)
            {
                *nl = '\0';
            }
            const int v = parse_vt_from_sysfs_name(buf);
            if (v > 0)
            {
                std::fclose(active);
                return v;
            }
        }
        std::fclose(active);
    }

    const int tty0_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (tty0_fd >= 0)
    {
        struct vt_stat vt;
        if (ioctl(tty0_fd, VT_GETSTATE, &vt) == 0 && vt.v_active >= 1 && vt.v_active <= 63)
        {
            close(tty0_fd);
            return static_cast<int>(vt.v_active);
        }
        close(tty0_fd);
    }

    return 1;
}

bool enter_console_graphics(const char *log_tag, int &tty_fd, int &vt_num)
{
    vt_num = detect_display_vt();

    if (nullptr != std::getenv("SSH_CONNECTION") || nullptr != std::getenv("SSH_CLIENT"))
    {
        std::fprintf(stderr,
                     "%s: SSH session: targeting HDMI VT %d (/dev/tty%d), not the SSH pty\n",
                     log_tag, vt_num, vt_num);
    }

    const int tty0_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (tty0_fd >= 0)
    {
        if (ioctl(tty0_fd, VT_ACTIVATE, vt_num) < 0)
        {
            std::fprintf(stderr,
                         "%s: VT_ACTIVATE %d failed (%d; %s); try: sudo chvt %d (or run via "
                         "openvt -f -c %d -- ...)\n",
                         log_tag, vt_num, errno, std::strerror(errno), vt_num, vt_num);
        }
        else
        {
            (void)ioctl(tty0_fd, VT_WAITACTIVE, vt_num);
        }
        close(tty0_fd);
    }

    char vt_path[32];
    std::snprintf(vt_path, sizeof(vt_path), "/dev/tty%d", vt_num);
    tty_fd = open(vt_path, O_RDWR | O_CLOEXEC);
    if (tty_fd < 0 && vt_num != 1)
    {
        std::fprintf(stderr, "%s: open %s failed (%d; %s), trying /dev/tty1\n", log_tag, vt_path,
                     errno, std::strerror(errno));
        vt_num = 1;
        tty_fd = open("/dev/tty1", O_RDWR | O_CLOEXEC);
    }
    if (tty_fd < 0)
    {
        std::fprintf(stderr, "%s: cannot open display VT (%d; %s); need root or tty group\n",
                     log_tag, errno, std::strerror(errno));
        return false;
    }

    if (ioctl(tty_fd, KDSETMODE, KD_GRAPHICS) < 0)
    {
        std::fprintf(stderr, "%s: KDSETMODE KD_GRAPHICS on VT %d failed (%d; %s)\n", log_tag, vt_num,
                     errno, std::strerror(errno));
        close(tty_fd);
        tty_fd = -1;
        return false;
    }

    std::fprintf(stderr, "%s: VT %d graphics mode (getty hidden on monitor)\n", log_tag, vt_num);
    return true;
}

void leave_console_graphics(int &tty_fd, bool &active)
{
    if (!active || tty_fd < 0)
    {
        return;
    }
    (void)ioctl(tty_fd, KDSETMODE, KD_TEXT);
    close(tty_fd);
    tty_fd = -1;
    active = false;
}
#endif


[[nodiscard]] int nv12_byte_size(int w, int h)
{
    if (w <= 0 || h <= 0)
    {
        return -EINVAL;
    }
    return w * h + (w * h) / 2;
}

[[nodiscard]] bool driver_is_kmsdrm(const char *driver)
{
    return nullptr != driver && 0 == std::strcmp(driver, "kmsdrm");
}

[[nodiscard]] bool sdl_driver_is_kmsdrm(const char *active)
{
    if (nullptr == active)
    {
        return false;
    }
    return 0 == SDL_strcasecmp(active, "kmsdrm");
}

}  // namespace

sdl_nv12_presenter::sdl_nv12_presenter(const char *tag, const char *driver)
    : log_tag(tag), video_driver(driver)
{
}

void sdl_nv12_presenter::clear_sdl_error()
{
    SDL_ClearError();
}

void sdl_nv12_presenter::note_present_failure(const char *op)
{
    present_fail_count++;
    const char *sdl_err = SDL_GetError();
    if (nullptr != sdl_err && sdl_err[0] != '\0')
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s: %s", op, sdl_err);
        last_err = buf;
    }
    else
    {
        last_err = op;
    }
    if (present_fail_count <= 12 || (present_fail_count % 120) == 0)
    {
        std::fprintf(stderr, "%s: present failed (#%" PRIu64 ") %s\n", log_tag, present_fail_count,
                     last_err.c_str());
    }
}

bool sdl_nv12_presenter::check_sdl_error_after(const char *op)
{
    const char *sdl_err = SDL_GetError();
    if (nullptr != sdl_err && sdl_err[0] != '\0')
    {
        note_present_failure(op);
        return true;
    }
    return false;
}

void sdl_nv12_presenter::set_title(std::string_view t)
{
    std::lock_guard<std::mutex> lock(mu);
    title.assign(t.data(), t.size());
}

void sdl_nv12_presenter::destroy_video_locked()
{
    if (nullptr != texture)
    {
        SDL_DestroyTexture(static_cast<SDL_Texture *>(texture));
        texture = nullptr;
    }
    if (nullptr != renderer)
    {
        SDL_DestroyRenderer(static_cast<SDL_Renderer *>(renderer));
        renderer = nullptr;
    }
    if (nullptr != window)
    {
        SDL_DestroyWindow(static_cast<SDL_Window *>(window));
        window = nullptr;
    }
    live_w = 0;
    live_h = 0;
    tex_w = 0;
    tex_h = 0;
    render_thread_bound = false;
}

void sdl_nv12_presenter::destroy_texture_locked()
{
    if (nullptr != texture)
    {
        SDL_DestroyTexture(static_cast<SDL_Texture *>(texture));
        texture = nullptr;
    }
    tex_w = 0;
    tex_h = 0;
}

void sdl_nv12_presenter::pump_events_locked(bool &session_open)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
    {
        if (SDL_QUIT == ev.type)
        {
            session_open = false;
        }
    }
}

int sdl_nv12_presenter::ensure_video_locked(int w, int h)
{
    const bool kmsdrm = driver_is_kmsdrm(video_driver);
    const auto   here = std::this_thread::get_id();

    if (kmsdrm && render_thread_bound && render_thread_id != here &&
        (nullptr != window || nullptr != renderer))
    {
        std::fprintf(stderr,
                     "%s: kmsdrm video objects created on another thread; recreating on present "
                     "thread\n",
                     log_tag);
        destroy_video_locked();
    }

    if (w == tex_w && h == tex_h && nullptr != texture && nullptr != window && nullptr != renderer)
    {
        live_w = w;
        live_h = h;
        return 0;
    }

    if (w != tex_w || h != tex_h)
    {
        destroy_texture_locked();
    }

    int win_w = w;
    int win_h = h;
    if (kmsdrm)
    {
        SDL_DisplayMode mode;
        clear_sdl_error();
        if (SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.w > 0 && mode.h > 0)
        {
            win_w = mode.w;
            win_h = mode.h;
        }
        else
        {
            std::fprintf(stderr, "%s: SDL_GetCurrentDisplayMode failed: %s\n", log_tag,
                         SDL_GetError());
        }
    }

    if (nullptr == window)
    {
        Uint32 win_flags = SDL_WINDOW_RESIZABLE;
        if (kmsdrm)
        {
            win_flags = SDL_WINDOW_FULLSCREEN_DESKTOP;
        }

        auto *win = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     win_w, win_h, win_flags);
        if (nullptr == win)
        {
            std::fprintf(stderr, "%s: SDL_CreateWindow: %s\n", log_tag, SDL_GetError());
            return -EIO;
        }

        SDL_Renderer *ren = nullptr;
        if (kmsdrm)
        {
            /* GPU path: software full-screen scale on HDMI is too slow for 30 fps benches. */
            ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        }
        if (nullptr == ren)
        {
            ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        }
        if (nullptr == ren)
        {
            ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        }
        if (nullptr == ren)
        {
            SDL_DestroyWindow(win);
            std::fprintf(stderr, "%s: SDL_CreateRenderer: %s\n", log_tag, SDL_GetError());
            return -EIO;
        }

        SDL_RendererInfo info;
        if (SDL_GetRendererInfo(ren, &info) == 0)
        {
            std::fprintf(stderr, "%s: renderer %s (flags 0x%x) window %dx%d\n", log_tag, info.name,
                         info.flags, win_w, win_h);
        }

        window = win;
        renderer = ren;
        if (kmsdrm)
        {
            render_thread_id = here;
            render_thread_bound = true;
        }
    }

    auto *ren = static_cast<SDL_Renderer *>(renderer);
    auto *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (nullptr == tex)
    {
        std::fprintf(stderr, "%s: SDL_CreateTexture: %s\n", log_tag, SDL_GetError());
        return -EIO;
    }

    texture = tex;
    tex_w = w;
    tex_h = h;
    live_w = w;
    live_h = h;
    std::fprintf(stderr, "%s: stream texture %dx%d\n", log_tag, w, h);
    return 0;
}

int sdl_nv12_presenter::present_nv12_locked(const frame_data &f, bool &session_open)
{
    const int need = nv12_byte_size(f.width, f.height);
    if (need < 0 || static_cast<size_t>(need) != f.buf.size || nullptr == f.buf.data)
    {
        return -EINVAL;
    }
    if (f.capture_mono_ns > 0)
    {
        const int64_t now_ns = steady_mono_ns();
        last_latency_ms =
            static_cast<double>(now_ns - f.capture_mono_ns) / 1e6;
        if (last_latency_ms < 0.0)
        {
            last_latency_ms = 0.0;
        }
    }

    int r = ensure_video_locked(f.width, f.height);
    if (r < 0)
    {
        return r;
    }

    const uint8_t *y = f.buf.data;
    const uint8_t *uv = f.buf.data + static_cast<size_t>(f.width * f.height);

    auto *tex = static_cast<SDL_Texture *>(texture);
    clear_sdl_error();
    if (SDL_UpdateNVTexture(tex, nullptr, y, f.width, uv, f.width) != 0)
    {
        note_present_failure("SDL_UpdateNVTexture");
        return -EIO;
    }

    auto *ren = static_cast<SDL_Renderer *>(renderer);
    if (SDL_SetRenderDrawColor(ren, 0, 0, 0, 255) != 0)
    {
        note_present_failure("SDL_SetRenderDrawColor");
        return -EIO;
    }
    if (SDL_RenderClear(ren) != 0)
    {
        note_present_failure("SDL_RenderClear");
        return -EIO;
    }

    SDL_Rect dst;
    if (driver_is_kmsdrm(video_driver))
    {
        if (SDL_GetRendererOutputSize(ren, &dst.w, &dst.h) != 0)
        {
            std::fprintf(stderr, "%s: SDL_GetRendererOutputSize: %s\n", log_tag, SDL_GetError());
            dst.w = f.width;
            dst.h = f.height;
        }
        dst.x = 0;
        dst.y = 0;
    }
    else
    {
        dst.x = 0;
        dst.y = 0;
        dst.w = 0;
        dst.h = 0;
    }

    const SDL_Rect *dst_ptr = driver_is_kmsdrm(video_driver) ? &dst : nullptr;
    if (SDL_RenderCopy(ren, tex, nullptr, dst_ptr) != 0)
    {
        note_present_failure("SDL_RenderCopy");
        return -EIO;
    }
    clear_sdl_error();
    SDL_RenderPresent(ren);
    if (check_sdl_error_after("SDL_RenderPresent"))
    {
        return -EIO;
    }

    present_ok_count++;
    if (driver_is_kmsdrm(video_driver) && !console_kd_graphics && present_ok_count == 1)
    {
        std::fprintf(stderr,
                     "%s: warning: presenting without KD_GRAPHICS (getty may cover the image)\n",
                     log_tag);
    }

    pump_events_locked(session_open);
    return 0;
}

int sdl_nv12_presenter::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (sdl_ready)
    {
        return 0;
    }

    const bool kmsdrm = driver_is_kmsdrm(video_driver);
    if (kmsdrm)
    {
#if defined(__linux__)
        console_kd_graphics =
            enter_console_graphics(log_tag, console_tty_fd, console_vt);
#endif
        if (nullptr != std::getenv("DISPLAY"))
        {
            std::fprintf(stderr, "%s: unsetting DISPLAY for kmsdrm HDMI scanout\n", log_tag);
            unsetenv("DISPLAY");
        }
        (void)SDL_SetHint(SDL_HINT_VIDEO_DOUBLE_BUFFER, "1");

        const char *idx_env = std::getenv("SDL_KMSDRM_DEVICE_INDEX");
        if (nullptr != idx_env && idx_env[0] != '\0')
        {
            if (SDL_SetHint(SDL_HINT_VIDEODRIVER, "kmsdrm") == SDL_FALSE)
            {
                std::fprintf(stderr, "%s: SDL_SetHint VIDEODRIVER=kmsdrm failed\n", log_tag);
            }
            if (SDL_Init(SDL_INIT_VIDEO) != 0)
            {
                std::fprintf(stderr, "%s: SDL_Init: %s\n", log_tag, SDL_GetError());
#if defined(__linux__)
                leave_console_graphics(console_tty_fd, console_kd_graphics);
#endif
                return -EIO;
            }
        }
        else
        {
            constexpr int k_max_drm_index = 4;
            bool          inited = false;
            for (int drm_index = 0; drm_index < k_max_drm_index; drm_index++)
            {
                char idx_buf[8];
                std::snprintf(idx_buf, sizeof(idx_buf), "%d", drm_index);
                (void)SDL_SetHint(SDL_HINT_KMSDRM_DEVICE_INDEX, idx_buf);
                (void)SDL_SetHint(SDL_HINT_VIDEODRIVER, "kmsdrm");

                clear_sdl_error();
                if (SDL_Init(SDL_INIT_VIDEO) == 0)
                {
                    const char *active = SDL_GetCurrentVideoDriver();
                    if (sdl_driver_is_kmsdrm(active))
                    {
                        if (drm_index > 0)
                        {
                            std::fprintf(stderr, "%s: using SDL_KMSDRM_DEVICE_INDEX=%d\n", log_tag,
                                         drm_index);
                        }
                        inited = true;
                        break;
                    }
                    std::fprintf(stderr,
                                 "%s: SDL_Init ok on card %d but driver is '%s' (expected kmsdrm)\n",
                                 log_tag, drm_index, active ? active : "(null)");
                    SDL_QuitSubSystem(SDL_INIT_VIDEO);
                }
                else
                {
                    std::fprintf(stderr, "%s: SDL_Init card %d failed: %s\n", log_tag, drm_index,
                                 SDL_GetError());
                }
            }
            if (!inited)
            {
                std::fprintf(stderr,
                             "%s: SDL_Init kmsdrm failed on DRM card indices 0..%d (%s)\n", log_tag,
                             k_max_drm_index - 1, SDL_GetError());
                std::fprintf(stderr,
                             "%s: hint: SDL_KMSDRM_DEVICE_INDEX=0, stop lightdm, SSH: "
                             "VSTREAMER_KMSDRM_VT=1 sudo chvt 1\n",
                             log_tag);
#if defined(__linux__)
                leave_console_graphics(console_tty_fd, console_kd_graphics);
#endif
                return -EIO;
            }
        }
        std::fprintf(stderr,
                     "%s: kmsdrm video ready (driver %s, console_graphics=%s, ok=%" PRIu64
                     " fail=%" PRIu64 ")\n",
                     log_tag, SDL_GetCurrentVideoDriver(),
                     console_kd_graphics ? "yes" : "no", present_ok_count, present_fail_count);
    }
    else
    {
        if (nullptr != video_driver && video_driver[0] != '\0')
        {
            if (SDL_SetHint(SDL_HINT_VIDEODRIVER, video_driver) == SDL_FALSE)
            {
                std::fprintf(stderr, "%s: SDL_SetHint VIDEODRIVER=%s failed\n", log_tag,
                             video_driver);
            }
        }

        if (SDL_Init(SDL_INIT_VIDEO) != 0)
        {
            std::fprintf(stderr, "%s: SDL_Init: %s\n", log_tag, SDL_GetError());
            return -EIO;
        }
    }

    sdl_ready = true;
    return 0;
}

void sdl_nv12_presenter::close()
{
    std::lock_guard<std::mutex> lock(mu);
    destroy_video_locked();
    if (sdl_ready)
    {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        sdl_ready = false;
    }
#if defined(__linux__)
    if (driver_is_kmsdrm(video_driver))
    {
        leave_console_graphics(console_tty_fd, console_kd_graphics);
    }
#endif
}

int sdl_nv12_presenter::prepare(int w, int h, bool &session_open)
{
    std::lock_guard<std::mutex> lock(mu);
    if (!sdl_ready)
    {
        return -EBADF;
    }
    session_open = true;
    if (driver_is_kmsdrm(video_driver))
    {
        /* Window/renderer created on first present() (dedicated present thread in bench app). */
        return 0;
    }
    return ensure_video_locked(w, h);
}

int sdl_nv12_presenter::present(const frame_data &f, bool &session_open)
{
    std::lock_guard<std::mutex> lock(mu);
    if (!sdl_ready || !session_open)
    {
        return -EBADF;
    }
    return present_nv12_locked(f, session_open);
}

void sdl_nv12_presenter::stats_string(char *buf, size_t buflen, uint64_t frames_in) const
{
    std::lock_guard<std::mutex> lock(mu);
    if (driver_is_kmsdrm(video_driver))
    {
        std::snprintf(buf, buflen,
                      "frames=%" PRIu64 " %dx%d ok=%" PRIu64 " fail=%" PRIu64 " kd=%d "
                      "latency_ms=%.1f last=%s",
                      frames_in, live_w, live_h, present_ok_count, present_fail_count,
                      console_kd_graphics ? 1 : 0, last_latency_ms,
                      last_err.empty() ? "-" : last_err.c_str());
    }
    else
    {
        std::snprintf(buf, buflen, "frames=%" PRIu64 " %dx%d latency_ms=%.1f", frames_in, live_w,
                      live_h, last_latency_ms);
    }
}

}  // namespace vstreamer
