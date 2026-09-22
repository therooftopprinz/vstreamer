#ifndef VSTREAMER_CORE_OUTPUT_OPTS_HPP
#define VSTREAMER_CORE_OUTPUT_OPTS_HPP

#include <cerrno>
#include <cstdint>
#include <string_view>

#include "core/frame.hpp"

namespace vstreamer
{

enum class output_mode_e : uint8_t
{
    filter  = 0,
    convert = 1,
};

inline int parse_output_mode(std::string_view s, output_mode_e *out)
{
    if (nullptr == out)
    {
        return -EINVAL;
    }
    if (s == "filter")
    {
        *out = output_mode_e::filter;
        return 0;
    }
    if (s == "convert")
    {
        *out = output_mode_e::convert;
        return 0;
    }
    return -EINVAL;
}

inline const char *output_mode_name(output_mode_e mode)
{
    switch (mode)
    {
        case output_mode_e::filter:
            return "filter";
        case output_mode_e::convert:
            return "convert";
    }
    return "filter";
}

/* Exact lowercase; only nv12 for now. */
inline int parse_output_format(std::string_view s, media_kind_e *out)
{
    if (nullptr == out)
    {
        return -EINVAL;
    }
    if (s == "nv12")
    {
        *out = media_kind_e::NV12;
        return 0;
    }
    return -EINVAL;
}

inline const char *output_format_name(media_kind_e kind)
{
    if (kind == media_kind_e::NV12)
    {
        return "nv12";
    }
    return "";
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_OUTPUT_OPTS_HPP
