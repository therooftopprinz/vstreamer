#ifndef VSTREAMER_CORE_COMPONENT_CODER_HPP
#define VSTREAMER_CORE_COMPONENT_CODER_HPP

#include <string>

#include "core/component.hpp"
#include "core/component_input.hpp"
#include "core/component_output.hpp"
#include "core/frame.hpp"

namespace vstreamer
{

/* Push-in / pull-out transform (encode or decode). */
class component_coder : public component, public component_input, public component_output
{
public:
    ~component_coder() override = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual media_kind_e input_kind() const = 0;
    [[nodiscard]] virtual media_kind_e output_kind() const = 0;

    virtual int  open() = 0;
    virtual void close() = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_CODER_HPP
