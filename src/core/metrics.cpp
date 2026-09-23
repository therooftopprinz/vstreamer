#include "core/metrics.hpp"

#include <cinttypes>
#include <cstdio>
#include <unordered_map>

namespace vstreamer
{

void metric_store(metric &m, uint64_t v)
{
    std::lock_guard<std::mutex> lock(m.mutex);
    m.value = v;
}

void metric_store(metric &m, int64_t v)
{
    std::lock_guard<std::mutex> lock(m.mutex);
    m.value = v;
}

void metric_store(metric &m, double v)
{
    std::lock_guard<std::mutex> lock(m.mutex);
    m.value = v;
}

void metric_store(metric &m, const std::string &v)
{
    std::lock_guard<std::mutex> lock(m.mutex);
    m.value = v;
}

void metric_store(metric &m, const char *v)
{
    metric_store(m, std::string(v != nullptr ? v : ""));
}

metrics::metrics() = default;

metrics::~metrics() = default;

std::shared_ptr<metric> metrics::get_metric(const std::string &name, metric &seed)
{
    std::lock_guard<std::mutex> lock(mu);
    for (const auto &entry : entries)
    {
        if (entry.first == name)
        {
            return entry.second;
        }
    }

    auto ptr = std::make_shared<metric>();
    {
        std::lock_guard<std::mutex> slock(seed.mutex);
        ptr->value = seed.value;
    }
    order.push_back(name);
    entries.emplace_back(name, ptr);
    return ptr;
}

namespace
{

[[nodiscard]] std::string strip_trailing_angle_comment(std::string s)
{
    const auto pos = s.rfind(" <");
    if (pos != std::string::npos)
    {
        s.erase(pos);
    }
    return s;
}

[[nodiscard]] std::string format_metric_value(metric &m, const std::string &key)
{
    std::lock_guard<std::mutex> lock(m.mutex);
    if (std::holds_alternative<uint64_t>(m.value))
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, std::get<uint64_t>(m.value));
        return buf;
    }
    if (std::holds_alternative<int64_t>(m.value))
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRId64, std::get<int64_t>(m.value));
        return buf;
    }
    if (std::holds_alternative<double>(m.value))
    {
        char        buf[32];
        const char *fmt = "%.0f";
        if (key.size() >= 3 && key.compare(key.size() - 3, 3, "_ms") == 0)
        {
            fmt = "%.1f";
        }
        std::snprintf(buf, sizeof(buf), fmt, std::get<double>(m.value));
        return buf;
    }
    return strip_trailing_angle_comment(std::get<std::string>(m.value));
}

[[nodiscard]] bool split_section_key(const std::string &name, std::string &section, std::string &key)
{
    const auto dot = name.find('.');
    if (dot == std::string::npos)
    {
        section = name;
        key.clear();
        return false;
    }
    section = name.substr(0, dot);
    key = name.substr(dot + 1);
    return !key.empty();
}

}  // namespace

std::string metrics::to_string() const
{
    std::lock_guard<std::mutex> lock(mu);

    std::unordered_map<std::string, std::vector<std::pair<std::string, std::shared_ptr<metric>>>> by_section;
    std::vector<std::string> section_order;

    for (const std::string &name : order)
    {
        std::shared_ptr<metric> ptr;
        for (const auto &entry : entries)
        {
            if (entry.first == name)
            {
                ptr = entry.second;
                break;
            }
        }
        if (nullptr == ptr)
        {
            continue;
        }

        std::string section;
        std::string key;
        if (!split_section_key(name, section, key))
        {
            continue;
        }

        auto &bucket = by_section[section];
        if (bucket.empty())
        {
            section_order.push_back(section);
        }
        bucket.emplace_back(key, ptr);
    }

    std::string out;
    for (size_t si = 0; si < section_order.size(); si++)
    {
        const std::string &section = section_order[si];
        out += '[';
        out += section;
        out += "]\n";

        const auto it = by_section.find(section);
        if (it == by_section.end())
        {
            continue;
        }

        for (const auto &kv : it->second)
        {
            out += kv.first;
            out += " = ";
            out += strip_trailing_angle_comment(format_metric_value(*kv.second, kv.first));
            out += '\n';
        }

        if (si + 1 < section_order.size())
        {
            out += '\n';
        }
    }

    return out;
}

}  // namespace vstreamer
