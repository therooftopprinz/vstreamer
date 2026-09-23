#include "core/component_sink.hpp"

namespace vstreamer
{

int component_sink::set_enabled(bool /*on*/, int /*timeout_ms*/)
{
    return 0;
}

bool component_sink::enabled() const
{
    return true;
}

}  // namespace vstreamer
