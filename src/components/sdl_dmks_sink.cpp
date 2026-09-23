#include "components/sdl_dmks_sink.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace vstreamer
{

sdl_dmks_sink::sdl_dmks_sink() = default;

sdl_dmks_sink::~sdl_dmks_sink()
{
    close();
}

std::string sdl_dmks_sink::name() const
{
    return "sdl_dmks_sink";
}

media_kind_e sdl_dmks_sink::input_kind() const
{
    return media_kind_e::NV12;
}

int sdl_dmks_sink::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return 0;
    }
    int r = present.open();
    if (r < 0)
    {
        std::fprintf(stderr, "sdl_dmks_sink: open failed (%d", r);
        if (-r > 0 && -r < 4096)
        {
            std::fprintf(stderr, "; %s", std::strerror(-r));
        }
        std::fprintf(stderr, ")\n");
        return r;
    }
    opened = true;
    return 0;
}

void sdl_dmks_sink::close()
{
    std::lock_guard<std::mutex> lock(mu);
    present.close();
    opened = false;
}

int sdl_dmks_sink::prepare(int width, int height)
{
    std::lock_guard<std::mutex> lock(mu);
    if (!opened)
    {
        return -EBADF;
    }
    const int r = present.prepare(width, height, opened);
    if (r < 0)
    {
        std::fprintf(stderr, "sdl_dmks_sink: prepare(%d,%d) failed (%d", width, height, r);
        if (-r > 0 && -r < 4096)
        {
            std::fprintf(stderr, "; %s", std::strerror(-r));
        }
        std::fprintf(stderr, ")\n");
    }
    return r;
}

int sdl_dmks_sink::input(uint8_t port, const data_packet &in)
{
    if (0 != port)
    {
        return -EINVAL;
    }

    const frame_data &f = data_packet::cast<frame_data>(in);
    if (f.kind != media_kind_e::NV12)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (!opened)
    {
        return -EBADF;
    }

    const int r = present.present(f, opened);
    if (0 == r)
    {
        frames_in++;
    }
    else
    {
        const uint64_t n = frames_in + 1;
        if (n <= 12 || (n % 120) == 0)
        {
            char detail[192];
            present.stats_string(detail, sizeof(detail), frames_in);
            std::fprintf(stderr, "sdl_dmks_sink: present failed (%d", r);
            if (-r > 0 && -r < 4096)
            {
                std::fprintf(stderr, "; %s", std::strerror(-r));
            }
            std::fprintf(stderr, "; %s)\n", detail);
        }
    }
    return r;
}

int sdl_dmks_sink::configure(uint64_t /*key*/, int64_t /*value*/)
{
    return -ENOTSUP;
}

int sdl_dmks_sink::query(uint64_t /*key*/, int64_t * /*value*/) const
{
    return -ENOTSUP;
}

int sdl_dmks_sink::configure(std::string_view key, std::string_view *value)
{
    if (nullptr == value)
    {
        return -EINVAL;
    }

    if ("title" == key)
    {
        present.set_title(*value);
        return 0;
    }
    return -ENOTSUP;
}

int sdl_dmks_sink::query(std::string_view key, std::string_view *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }

    if ("stats" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[64];
        present.stats_string(buf, sizeof(buf), frames_in);
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
