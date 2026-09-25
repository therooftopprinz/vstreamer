#ifndef VSTREAMER_COMPONENTS_JPEG_DECODER_MULTICORE_HPP
#define VSTREAMER_COMPONENTS_JPEG_DECODER_MULTICORE_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_JPEG_DECODER_MULTICORE
#error "jpeg_decoder_multicore requires -DENABLE_JPEG_DECODER_MULTICORE=ON"
#endif

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/component_coder.hpp"
#include "core/output_opts.hpp"

namespace vstreamer
{

/*
 * libav MJPEG → packed NV12 on CPU (H3 rover).
 * N worker threads, each with its own AVCodecContext (thread_count=1),
 * job queue + in-order result slots — same model as camera.c (not Cedar/VPU).
 */
class jpeg_decoder_multicore : public component_coder
{
public:
    jpeg_decoder_multicore();
    ~jpeg_decoder_multicore() override;

    jpeg_decoder_multicore(const jpeg_decoder_multicore &) = delete;
    jpeg_decoder_multicore &operator=(const jpeg_decoder_multicore &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e input_kind() const override;
    [[nodiscard]] media_kind_e output_kind() const override;

    int  open() override;
    void close() override;

    int input(uint8_t port, const data_packet &in) override;
    int output(uint8_t port, data_packet &out, int timeout_ms) override;

    int configure(uint64_t key, int64_t value) override;
    int query(uint64_t key, int64_t *value) const override;

    int configure(std::string_view key, std::string_view *value) override;
    int query(std::string_view key, std::string_view *value) const override;

private:
    static constexpr int k_max_workers = 8;
    static constexpr int k_queue_depth = 32;
    static constexpr size_t k_max_jpeg = 8ULL * 1024ULL * 1024ULL;

    struct job
    {
        uint64_t seq = 0;
        uint8_t *data = nullptr;
        size_t   size = 0;
        int64_t  pts = 0;
        int64_t  capture_mono_ns = 0;
    };

    struct result_slot
    {
        uint64_t seq = 0;
        int      status = 0;
        bool     ready = false;
        frame    out;
    };

    void worker_main();
    int  decode_one(void *dec, void *avframe, void *pkt, const job &j, frame *out) const;
    int  start_workers();
    void stop_workers();

    mutable std::mutex cfg_mu;
    int                width = 1280;
    int                height = 720;
    int                fps = 30;
    int                workers = 2;
    int                worker_cpu = -1;
    output_mode_e      output_mode = output_mode_e::filter;
    media_kind_e       output_format = media_kind_e::NV12;
    mutable std::string decoded_pix_fmt = "unknown";

    mutable std::mutex      life_mu;
    bool                    opened = false;
    bool                    stop = false;
    std::vector<std::thread> threads;

    std::mutex              job_mu;
    std::condition_variable job_cv;
    job                     jobs[k_queue_depth];
    int                     job_head = 0;
    int                     job_tail = 0;
    int                     job_count = 0;
    uint64_t                next_in_seq = 0;

    std::mutex              res_mu;
    std::condition_variable res_cv;
    result_slot             results[k_queue_depth];
    uint64_t                next_out_seq = 0;

    mutable std::string query_buf;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_JPEG_DECODER_MULTICORE_HPP
