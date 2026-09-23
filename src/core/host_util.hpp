#ifndef VSTREAMER_CORE_HOST_UTIL_HPP
#define VSTREAMER_CORE_HOST_UTIL_HPP

#include <cerrno>
#include <cstring>
#include <string_view>

namespace vstreamer
{

inline int parse_host_port(std::string_view spec, char *host, size_t host_cap, int *port)
{
    if (nullptr == host || 0 == host_cap || nullptr == port || spec.empty())
    {
        return -EINVAL;
    }

    char buf[256];
    if (spec.size() >= sizeof(buf))
    {
        return -EINVAL;
    }
    std::memcpy(buf, spec.data(), spec.size());
    buf[spec.size()] = '\0';

    char *colon = std::strchr(buf, ':');
    if (nullptr == colon || colon == buf || colon[1] == '\0')
    {
        return -EINVAL;
    }
    *colon = '\0';

    char *end = nullptr;
    long p = std::strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || p <= 0 || p > 65535)
    {
        return -EINVAL;
    }

    if (std::strlen(buf) >= host_cap)
    {
        return -EINVAL;
    }
    std::memcpy(host, buf, std::strlen(buf) + 1);
    *port = static_cast<int>(p);
    return 0;
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_HOST_UTIL_HPP
