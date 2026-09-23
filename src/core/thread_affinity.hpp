#ifndef VSTREAMER_CORE_THREAD_AFFINITY_HPP
#define VSTREAMER_CORE_THREAD_AFFINITY_HPP

#if defined(__linux__)
#include <pthread.h>
#endif

namespace vstreamer
{

/* Pin calling thread to one CPU; no-op when cpu < 0 or unsupported. */
inline void pin_current_thread_to_cpu(int cpu)
{
#if defined(__linux__)
    if (cpu < 0)
    {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<size_t>(cpu), &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_THREAD_AFFINITY_HPP
