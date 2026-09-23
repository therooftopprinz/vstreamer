#include "components/encoder_cbr_logic.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{

bool near_kbps(int v, int target, int tol)
{
    return std::abs(v - target) <= tol;
}

void step_loss(vstreamer::encoder_cbr_logic &cbr, float loss, float deliver_kbps, int repeats)
{
    vstreamer::stream_telemetry tel;
    tel.channel_loss = loss;
    tel.deliverable_kbps = deliver_kbps;
    tel.egress_kbps = static_cast<float>(cbr.pid_runtime_state().nominal_kbps);
    for (int i = 0; i < repeats; ++i)
    {
        cbr.feed_telemetry_for_pid(tel);
    }
}

}  // namespace

int main()
{
    vstreamer::encoder_cbr_logic cbr;
    cbr.set_target_kbps(20'000);

    step_loss(cbr, 0.f, 20'000.f, 30);
    const int baseline = cbr.pid_runtime_state().pending_kbps;
    if (!near_kbps(baseline, 20'000, 500))
    {
        std::fprintf(stderr, "cbr_pid_test: baseline pending=%d expected ~20000\n", baseline);
        return 1;
    }

    step_loss(cbr, 0.12f, 3'500.f, 80);
    const int stressed = cbr.pid_runtime_state().pending_kbps;
    if (stressed > 8'000)
    {
        std::fprintf(stderr, "cbr_pid_test: stressed pending=%d expected well below 8000\n",
                     stressed);
        return 1;
    }

    step_loss(cbr, 0.f, 19'500.f, 200);
    const int recovered = cbr.pid_runtime_state().pending_kbps;
    if (recovered < 15'000)
    {
        std::fprintf(stderr, "cbr_pid_test: recovered pending=%d expected >= 15000\n", recovered);
        return 1;
    }

    char reply[512];
    cbr.handle_pid_console_line("set_cbr_pid kp=0.12 ki=0.02", reply, sizeof(reply));
    if (0 != std::strncmp(reply, "ok", 2))
    {
        std::fprintf(stderr, "cbr_pid_test: console set failed: %s", reply);
        return 1;
    }
    cbr.handle_pid_console_line("cbr_pid", reply, sizeof(reply));
    if (nullptr == std::strstr(reply, "kp=0.1200"))
    {
        std::fprintf(stderr, "cbr_pid_test: cbr_pid query missing kp: %s", reply);
        return 1;
    }

    std::fprintf(stderr,
                 "cbr_pid_test: ok baseline=%d stressed=%d recovered=%d\n", baseline, stressed,
                 recovered);
    return 0;
}
