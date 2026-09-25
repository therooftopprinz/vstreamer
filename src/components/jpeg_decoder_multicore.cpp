#include "components/jpeg_decoder_multicore.hpp"

#include "core/key_util.hpp"
#include "core/output_opts.hpp"
#include "core/pix_convert.hpp"
#include "core/thread_affinity.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

namespace vstreamer
{
namespace
{

int parse_size(std::string_view s, int *w, int *h)
{
    if (nullptr == w || nullptr == h || s.empty())
    {
        return -EINVAL;
    }
    char buf[64];
    if (s.size() >= sizeof(buf))
    {
        return -EINVAL;
    }
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    char *x = std::strchr(buf, 'x');
    if (nullptr == x)
    {
        x = std::strchr(buf, 'X');
    }
    if (nullptr == x || x == buf || x[1] == '\0')
    {
        return -EINVAL;
    }
    *x = '\0';
    int64_t ww = 0;
    int64_t hh = 0;
    if (key_parse_i64(buf, &ww) < 0 || key_parse_i64(x + 1, &hh) < 0)
    {
        return -EINVAL;
    }
    /* Width ×32 for Cedar-bound NV12. */
    if (ww < 2 || hh < 2 || (ww % 32) != 0 || (hh % 2) != 0 || ww > 7680 || hh > 4320)
    {
        return -EINVAL;
    }
    *w = static_cast<int>(ww);
    *h = static_cast<int>(hh);
    return 0;
}

void log_unsupported_pix_fmt_once(int fmt)
{
    static std::atomic<bool> logged {false};
    if (!logged.exchange(true))
    {
        std::fprintf(stderr, "jpeg_decoder_multicore: unsupported decoded pix_fmt %s (%d)\n",
                     av_get_pix_fmt_name(static_cast<AVPixelFormat>(fmt)), fmt);
    }
}

}  // namespace

jpeg_decoder_multicore::jpeg_decoder_multicore() = default;

jpeg_decoder_multicore::~jpeg_decoder_multicore()
{
    close();
}

std::string jpeg_decoder_multicore::name() const
{
    return "jpeg_decoder_multicore";
}

media_kind_e jpeg_decoder_multicore::input_kind() const
{
    return media_kind_e::MJPEG;
}

media_kind_e jpeg_decoder_multicore::output_kind() const
{
    std::lock_guard<std::mutex> lock(cfg_mu);
    return output_format;
}

int jpeg_decoder_multicore::decode_one(void *dec_v, void *avframe_v, void *pkt_v, const job &j,
                                       frame *out) const
{
    auto *ctx = static_cast<AVCodecContext *>(dec_v);
    auto *avf = static_cast<AVFrame *>(avframe_v);
    auto *packet = static_cast<AVPacket *>(pkt_v);

    int          dw = 0;
    int          dh = 0;
    media_kind_e fmt = media_kind_e::NV12;
    {
        std::lock_guard<std::mutex> lock(cfg_mu);
        dw = width;
        dh = height;
        fmt = output_format;
    }
    if (fmt != media_kind_e::NV12)
    {
        return -ENOTSUP;
    }

    av_packet_unref(packet);
    packet->data = j.data;
    packet->size = static_cast<int>(j.size);
    packet->pts = j.pts;

    if (avcodec_send_packet(ctx, packet) < 0)
    {
        return -EIO;
    }
    if (avcodec_receive_frame(ctx, avf) < 0)
    {
        return -EIO;
    }

    {
        const char *pix_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(avf->format));
        std::lock_guard<std::mutex> lock(cfg_mu);
        decoded_pix_fmt = (nullptr != pix_name && pix_name[0] != '\0') ? pix_name : "unknown";
    }

    size_t nv12_sz = static_cast<size_t>(dw) * static_cast<size_t>(dh) * 3ULL / 2ULL;
    auto  *buf = static_cast<uint8_t *>(std::malloc(nv12_sz));
    if (nullptr == buf)
    {
        return -ENOMEM;
    }
    std::memset(buf, 0, nv12_sz);

    int r = -ENOTSUP;
    switch (avf->format)
    {
        case AV_PIX_FMT_YUVJ422P:
        case AV_PIX_FMT_YUV422P:
            r = pack_yuv422p_to_nv12(avf->data[0], avf->linesize[0], avf->data[1], avf->linesize[1],
                                     avf->data[2], avf->linesize[2], avf->width, avf->height, buf,
                                     dw, dh);
            break;
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_YUV420P:
            r = pack_yuv420p_to_nv12(avf->data[0], avf->linesize[0], avf->data[1], avf->linesize[1],
                                     avf->data[2], avf->linesize[2], avf->width, avf->height, buf,
                                     dw, dh);
            break;
        case AV_PIX_FMT_NV12:
            r = copy_nv12_planes_to_packed(avf->data[0], avf->linesize[0], avf->data[1],
                                           avf->linesize[1], avf->width, avf->height, false, buf,
                                           dw, dh);
            break;
        case AV_PIX_FMT_NV21:
            r = copy_nv12_planes_to_packed(avf->data[0], avf->linesize[0], avf->data[1],
                                           avf->linesize[1], avf->width, avf->height, true, buf,
                                           dw, dh);
            break;
        default:
            log_unsupported_pix_fmt_once(avf->format);
            std::free(buf);
            return -ENOTSUP;
    }
    if (r < 0)
    {
        std::free(buf);
        return r;
    }

    out->reset(media_kind_e::NV12, dw, dh, j.pts, true, buf, nv12_sz,
               [](uint8_t *p) { std::free(p); }, j.capture_mono_ns);
    return 0;
}

void jpeg_decoder_multicore::worker_main()
{
    {
        int cpu = -1;
        {
            std::lock_guard<std::mutex> lock(cfg_mu);
            cpu = worker_cpu;
        }
        pin_current_thread_to_cpu(cpu);
    }

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (nullptr == codec)
    {
        return;
    }
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (nullptr == dec)
    {
        return;
    }
    dec->thread_count = 1;
    if (avcodec_open2(dec, codec, nullptr) < 0)
    {
        avcodec_free_context(&dec);
        return;
    }
    AVFrame  *avf = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    if (nullptr == avf || nullptr == pkt)
    {
        av_frame_free(&avf);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
        return;
    }

    for (;;)
    {
        job j;
        {
            std::unique_lock<std::mutex> lock(job_mu);
            job_cv.wait(lock, [this] { return stop || job_count > 0; });
            if (stop && job_count == 0)
            {
                break;
            }
            j = jobs[job_head];
            jobs[job_head] = job{};
            job_head = (job_head + 1) % k_queue_depth;
            job_count--;
            job_cv.notify_all();
        }

        frame out;
        int   status = decode_one(dec, avf, pkt, j, &out);
        std::free(j.data);
        j.data = nullptr;

        {
            std::unique_lock<std::mutex> lock(res_mu);
            int                         slot = static_cast<int>(j.seq % k_queue_depth);
            res_cv.wait(lock, [this, slot] { return stop || !results[slot].ready; });
            if (stop)
            {
                out.release();
                break;
            }
            results[slot].seq = j.seq;
            results[slot].status = status;
            results[slot].out = std::move(out);
            results[slot].ready = true;
            res_cv.notify_all();
        }
    }

    av_frame_free(&avf);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
}

int jpeg_decoder_multicore::start_workers()
{
    stop = false;
    next_in_seq = 0;
    next_out_seq = 0;
    job_head = 0;
    job_tail = 0;
    job_count = 0;
    for (int i = 0; i < k_queue_depth; i++)
    {
        results[i].ready = false;
        results[i].status = 0;
        results[i].seq = 0;
        results[i].out.release();
    }

    int n = 0;
    {
        std::lock_guard<std::mutex> lock(cfg_mu);
        n = workers;
    }
    if (n < 1)
    {
        n = 1;
    }
    if (n > k_max_workers)
    {
        n = k_max_workers;
    }

    threads.clear();
    threads.reserve(static_cast<size_t>(n));
    try
    {
        for (int i = 0; i < n; i++)
        {
            threads.emplace_back([this] { worker_main(); });
        }
    }
    catch (...)
    {
        stop_workers();
        return -EAGAIN;
    }
    return 0;
}

void jpeg_decoder_multicore::stop_workers()
{
    {
        std::lock_guard<std::mutex> lock(job_mu);
        stop = true;
    }
    job_cv.notify_all();
    res_cv.notify_all();

    for (auto &t : threads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
    threads.clear();

    {
        std::lock_guard<std::mutex> lock(job_mu);
        while (job_count > 0)
        {
            std::free(jobs[job_head].data);
            jobs[job_head] = job{};
            job_head = (job_head + 1) % k_queue_depth;
            job_count--;
        }
    }
    {
        std::lock_guard<std::mutex> lock(res_mu);
        for (int i = 0; i < k_queue_depth; i++)
        {
            results[i].out.release();
            results[i].ready = false;
        }
    }
    stop = false;
}

int jpeg_decoder_multicore::open()
{
    std::lock_guard<std::mutex> lock(life_mu);
    if (opened)
    {
        return 0;
    }
    int r = start_workers();
    if (r < 0)
    {
        return r;
    }
    opened = true;
    return 0;
}

void jpeg_decoder_multicore::close()
{
    std::lock_guard<std::mutex> lock(life_mu);
    if (!opened && threads.empty())
    {
        return;
    }
    stop_workers();
    opened = false;
}

int jpeg_decoder_multicore::input(uint8_t /*port*/, const data_packet &in)
{
    const frame_data &f = data_packet::cast<frame_data>(in);
    if (f.kind != media_kind_e::MJPEG || f.buf.size > k_max_jpeg)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> life(life_mu);
    if (!opened)
    {
        return -EBADF;
    }

    auto *copy = static_cast<uint8_t *>(std::malloc(f.buf.size));
    if (nullptr == copy)
    {
        return -ENOMEM;
    }
    std::memcpy(copy, f.buf.data, f.buf.size);

    std::unique_lock<std::mutex> lock(job_mu);
    if (job_count == k_queue_depth)
    {
        lock.unlock();
        std::free(copy);
        return -EAGAIN;
    }

    jobs[job_tail].seq = next_in_seq++;
    jobs[job_tail].data = copy;
    jobs[job_tail].size = f.buf.size;
    jobs[job_tail].pts = f.pts;
    jobs[job_tail].capture_mono_ns = f.capture_mono_ns;
    job_tail = (job_tail + 1) % k_queue_depth;
    job_count++;
    job_cv.notify_one();
    return 0;
}

int jpeg_decoder_multicore::output(uint8_t /*port*/, data_packet &out, int timeout_ms)
{
    std::unique_lock<std::mutex> life(life_mu);
    if (!opened)
    {
        return -EBADF;
    }
    life.unlock();

    std::unique_lock<std::mutex> lock(res_mu);
    auto                         ready = [this] {
        int slot = static_cast<int>(next_out_seq % k_queue_depth);
        return results[slot].ready && results[slot].seq == next_out_seq;
    };

    if (timeout_ms == 0)
    {
        if (!ready())
        {
            return -EAGAIN;
        }
    }
    else if (timeout_ms < 0)
    {
        res_cv.wait(lock, [&] { return stop || ready(); });
        if (!ready())
        {
            return -EAGAIN;
        }
    }
    else
    {
        if (!res_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [&] { return stop || ready(); }))
        {
            return -EAGAIN;
        }
        if (!ready())
        {
            return -EAGAIN;
        }
    }

    int slot = static_cast<int>(next_out_seq % k_queue_depth);
    int status = results[slot].status;
    if (status == 0)
    {
        out.adopt_frame(std::move(results[slot].out));
    }
    else
    {
        results[slot].out.release();
    }
    results[slot].ready = false;
    results[slot].status = 0;
    next_out_seq++;
    res_cv.notify_all();
    return status == 0 ? 0 : status;
}

int jpeg_decoder_multicore::configure(uint64_t /*key*/, int64_t /*value*/)
{
    return -EINVAL;
}

int jpeg_decoder_multicore::query(uint64_t /*key*/, int64_t * /*value*/) const
{
    return -EINVAL;
}

int jpeg_decoder_multicore::configure(std::string_view key, std::string_view *value)
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    std::string_view v = *value;

    if (key == "workers")
    {
        int64_t     n = 0;
        std::string tmp(v);
        int         r = key_parse_i64(tmp.c_str(), &n);
        if (r < 0 || n < 1 || n > k_max_workers)
        {
            return -EINVAL;
        }
        std::lock_guard<std::mutex> life(life_mu);
        if (opened)
        {
            return -EBUSY;
        }
        std::lock_guard<std::mutex> lock(cfg_mu);
        workers = static_cast<int>(n);
        return 0;
    }
    if (key == "worker_cpu")
    {
        int64_t     n = 0;
        std::string tmp(v);
        int         r = key_parse_i64(tmp.c_str(), &n);
        if (r < 0 || n < -1 || n > 255)
        {
            return -EINVAL;
        }
        std::lock_guard<std::mutex> life(life_mu);
        if (opened)
        {
            return -EBUSY;
        }
        std::lock_guard<std::mutex> lock(cfg_mu);
        worker_cpu = static_cast<int>(n);
        return 0;
    }

    std::lock_guard<std::mutex> lock(cfg_mu);
    if (key == "size")
    {
        int w = 0;
        int h = 0;
        int r = parse_size(v, &w, &h);
        if (r < 0)
        {
            return r;
        }
        width = w;
        height = h;
        return 0;
    }
    if (key == "fps")
    {
        int64_t     n = 0;
        std::string tmp(v);
        int         r = key_parse_i64(tmp.c_str(), &n);
        if (r < 0 || n <= 0 || n > 240)
        {
            return -EINVAL;
        }
        fps = static_cast<int>(n);
        return 0;
    }
    if (key == "output_mode")
    {
        output_mode_e mode = output_mode_e::filter;
        int           r = parse_output_mode(v, &mode);
        if (r < 0)
        {
            return r;
        }
        output_mode = mode;
        return 0;
    }
    if (key == "output_format")
    {
        media_kind_e kind = media_kind_e::UNKNOWN;
        int          r = parse_output_format(v, &kind);
        if (r < 0)
        {
            return r;
        }
        output_format = kind;
        return 0;
    }
    return -EINVAL;
}

int jpeg_decoder_multicore::query(std::string_view key, std::string_view *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }

    if (key == "status")
    {
        bool is_open = false;
        {
            std::lock_guard<std::mutex> life(life_mu);
            is_open = opened;
        }
        std::lock_guard<std::mutex> lock(cfg_mu);
        query_buf = is_open ? "open" : "closed";
        *value = query_buf;
        return 0;
    }

    std::lock_guard<std::mutex> lock(cfg_mu);
    if (key == "size")
    {
        char buf[64];
        if (std::snprintf(buf, sizeof(buf), "%dx%d", width, height) < 0)
        {
            return -EINVAL;
        }
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if (key == "fps")
    {
        char buf[32];
        if (key_format_i64(fps, buf, sizeof(buf)) < 0)
        {
            return -EINVAL;
        }
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if (key == "workers")
    {
        char buf[32];
        if (key_format_i64(workers, buf, sizeof(buf)) < 0)
        {
            return -EINVAL;
        }
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if (key == "format" || key == "output_format")
    {
        const char *name = output_format_name(output_format);
        if (nullptr == name || name[0] == '\0')
        {
            return -EINVAL;
        }
        query_buf = name;
        *value = query_buf;
        return 0;
    }
    if (key == "output_mode")
    {
        query_buf = output_mode_name(output_mode);
        *value = query_buf;
        return 0;
    }
    if (key == "decoded_pix_fmt")
    {
        query_buf = decoded_pix_fmt;
        *value = query_buf;
        return 0;
    }
    return -EINVAL;
}

}  // namespace vstreamer
