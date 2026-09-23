#ifndef VSTREAMER_CORE_PACKET_ERROR_HPP
#define VSTREAMER_CORE_PACKET_ERROR_HPP

#include <stdexcept>
#include <string>

namespace vstreamer
{

class packet_type_error : public std::runtime_error
{
public:
    explicit packet_type_error(const std::string &what) : std::runtime_error(what) {}
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_PACKET_ERROR_HPP
