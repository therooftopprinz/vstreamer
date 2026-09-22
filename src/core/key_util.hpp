#ifndef VSTREAMER_CORE_KEY_UTIL_HPP
#define VSTREAMER_CORE_KEY_UTIL_HPP

#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace vstreamer
{

inline int key_parse_i64(const char *s, int64_t *out)
{
    if (nullptr == s || nullptr == out)
    {
        return -EINVAL;
    }

    char *end = nullptr;
    errno = 0;
    int64_t v = std::strtoll(s, &end, 0);
    if (errno || end == s || *end != '\0')
    {
        return -EINVAL;
    }
    *out = v;
    return 0;
}

inline int key_format_i64(int64_t v, char *buf, size_t n)
{
    if (nullptr == buf || 0 == n)
    {
        return -EINVAL;
    }
    if (std::snprintf(buf, n, "%" PRId64, v) < 0)
    {
        return -EINVAL;
    }
    return 0;
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_KEY_UTIL_HPP
