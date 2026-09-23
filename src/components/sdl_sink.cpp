#include "components/sdl_sink.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>

namespace vstreamer
{

sdl_sink::sdl_sink() = default;

sdl_sink::~sdl_sink()
{
    close();
}

std::string sdl_sink::name() const
{
    return "sdl_sink";
}

media_kind_e sdl_sink::input_kind() const
{
    return media_kind_e::NV12;
}

int sdl_sink::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return 0;
    }
    int r = present.open();
    if (r < 0)
    {
        return r;
    }
    opened = true;
    return 0;
}

void sdl_sink::close()
{
    std::lock_guard<std::mutex> lock(mu);
    present.close();
    opened = false;
}

int sdl_sink::prepare(int width, int height)
{
    std::lock_guard<std::mutex> lock(mu);
    if (!opened)
    {
        return -EBADF;
    }
    return present.prepare(width, height, opened);
}

int sdl_sink::input(uint8_t port, const data_packet &in)
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

    int r = present.present(f, opened);
    if (0 == r)
    {
        frames_in++;
    }
    return r;
}

int sdl_sink::configure(uint64_t /*key*/, int64_t /*value*/)
{
    return -ENOTSUP;
}

int sdl_sink::query(uint64_t /*key*/, int64_t * /*value*/) const
{
    return -ENOTSUP;
}

int sdl_sink::configure(std::string_view key, std::string_view *value)
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

int sdl_sink::query(std::string_view key, std::string_view *value) const
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
