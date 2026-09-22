#ifndef VSTREAMER_CORE_COMPONENT_FACTORY_HPP
#define VSTREAMER_CORE_COMPONENT_FACTORY_HPP

#include <memory>
#include <string>

namespace vstreamer
{

class component_coder;
class component_sink;
class component_source;

class component_factory
{
public:
    [[nodiscard]] static std::unique_ptr<component_source> create_source(std::string name);
    [[nodiscard]] static std::unique_ptr<component_coder>  create_coder(std::string name);
    [[nodiscard]] static std::unique_ptr<component_sink>   create_sink(std::string name);
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_FACTORY_HPP
