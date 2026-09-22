#ifndef VSTREAMER_CORE_COMPONENT_HPP
#define VSTREAMER_CORE_COMPONENT_HPP

#include <cstdint>
#include <string_view>

namespace vstreamer
{

class component
{
public:
    virtual ~component() = default;

    virtual int configure(uint64_t key, int64_t value) = 0;
    virtual int query(uint64_t key, int64_t *value) const = 0;

    virtual int configure(std::string_view key, std::string_view *value) = 0;
    virtual int query(std::string_view key, std::string_view *value) const = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_HPP
