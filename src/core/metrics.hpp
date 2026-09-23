#ifndef VSTREAMER_CORE_METRICS_HPP
#define VSTREAMER_CORE_METRICS_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

namespace vstreamer
{

struct metric
{
    std::variant<uint64_t, int64_t, double, std::string> value;
    std::mutex mutex;
};

void metric_store(metric &m, uint64_t v);
void metric_store(metric &m, int64_t v);
void metric_store(metric &m, double v);
void metric_store(metric &m, const std::string &v);
void metric_store(metric &m, const char *v);

class metrics
{
public:
    metrics();
    ~metrics();

    std::shared_ptr<metric> get_metric(const std::string &name, metric &metric);

    [[nodiscard]] std::string to_string() const;

private:
    mutable std::mutex mu;

    std::vector<std::string> order;
    std::vector<std::pair<std::string, std::shared_ptr<metric>>> entries;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_METRICS_HPP
