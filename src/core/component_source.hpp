#ifndef VSTREAMER_CORE_COMPONENT_SOURCE_HPP
#define VSTREAMER_CORE_COMPONENT_SOURCE_HPP

#include <string>

#include "core/component.hpp"
#include "core/component_output.hpp"
#include "core/frame.hpp"

namespace vstreamer
{

class component_source : public component, public component_output
{
public:
    ~component_source() override = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual media_kind_e output_kind() const = 0;

    virtual int  open() = 0;
    virtual void close() = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_SOURCE_HPP
