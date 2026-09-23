#include "components/h264_decoder_mpp.hpp"

#include "core/key_util.hpp"
#include "core/output_opts.hpp"
#include "core/pix_convert.hpp"
#include "core/time_util.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#define MODULE_TAG "vstreamer_mpp_h264"

extern "C"
{
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_mpi_cmd.h>
#include <rockchip/rk_vdec_cfg.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
}

namespace vstreamer
{
namespace
{

constexpr size_t k_max_au = 8ULL * 1024ULL * 1024ULL;
constexpr int    k_frm_grp_count = 24;

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
    if (ww < 2 || hh < 2 || (ww % 2) || (hh % 2) || ww > 7680 || hh > 4320)
    {
        return -EINVAL;
    }
    *w = static_cast<int>(ww);
    *h = static_cast<int>(hh);
    return 0;
}

}  // namespace

h264_decoder_mpp::h264_decoder_mpp() = default;

h264_decoder_mpp::~h264_decoder_mpp()
{
    close();
}

std::string h264_decoder_mpp::name() const
{
    return "h264_decoder_mpp";
}

media_kind_e h264_decoder_mpp::input_kind() const
{
    return media_kind_e::H264;
}

media_kind_e h264_decoder_mpp::output_kind() const
{
    return output_format;
}

int h264_decoder_mpp::ensure_decoder_locked()
{
    if (nullptr != ctx)
    {
        return 0;
    }

    MppCtx  mpp_ctx = nullptr;
    MppApi *mpp_mpi = nullptr;
    MPP_RET ret = mpp_create(&mpp_ctx, &mpp_mpi);
    if (ret != MPP_OK || nullptr == mpp_ctx || nullptr == mpp_mpi)
    {
        return -EIO;
    }

    ret = mpp_init(mpp_ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK)
    {
        mpp_destroy(mpp_ctx);
        return -EIO;
    }

    MppDecCfg cfg = nullptr;
    mpp_dec_cfg_init(&cfg);
    if (nullptr == cfg)
    {
        mpp_destroy(mpp_ctx);
        return -ENOMEM;
    }

    ret = mpp_mpi->control(mpp_ctx, MPP_DEC_GET_CFG, cfg);
    if (ret != MPP_OK)
    {
        mpp_dec_cfg_deinit(cfg);
        mpp_destroy(mpp_ctx);
        return -EIO;
    }

    /* Split Annex-B byte stream into access units inside MPP. */
    ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1);
    if (ret != MPP_OK)
    {
        mpp_dec_cfg_deinit(cfg);
        mpp_destroy(mpp_ctx);
        return -EIO;
    }

    ret = mpp_mpi->control(mpp_ctx, MPP_DEC_SET_CFG, cfg);
    mpp_dec_cfg_deinit(cfg);
    if (ret != MPP_OK)
    {
        mpp_destroy(mpp_ctx);
        return -EIO;
    }

    ctx = mpp_ctx;
    mpi = mpp_mpi;
    return 0;
}

void h264_decoder_mpp::free_decoder_locked()
{
    if (ctx)
    {
        auto *mpp_ctx = static_cast<MppCtx>(ctx);
        auto *mpp_mpi = static_cast<MppApi *>(mpi);
        if (mpp_mpi)
        {
            for (int i = 0; i < 64; i++)
            {
                MppFrame frm = nullptr;
                const MPP_RET gr = mpp_mpi->decode_get_frame(mpp_ctx, &frm);
                if (gr != MPP_OK || nullptr == frm)
                {
                    break;
                }
                mpp_frame_deinit(&frm);
            }
            mpp_mpi->reset(mpp_ctx);
        }
        mpp_destroy(mpp_ctx);
        ctx = nullptr;
        mpi = nullptr;
    }
    if (frm_grp)
    {
        MppBufferGroup grp = static_cast<MppBufferGroup>(frm_grp);
        mpp_buffer_group_put(grp);
        frm_grp = nullptr;
    }
}

void h264_decoder_mpp::clear_pending_locked()
{
    for (vstreamer::frame &f : ready_frames)
    {
        f.release();
    }
    ready_frames.clear();
    pending_capture_mono_ns = 0;
}

int h264_decoder_mpp::handle_info_change_locked(void *mpp_frame)
{
    auto *frame = static_cast<MppFrame>(mpp_frame);
    auto *mpp_ctx = static_cast<MppCtx>(ctx);
    auto *mpp_mpi = static_cast<MppApi *>(mpi);

    RK_U32 buf_size = mpp_frame_get_buf_size(frame);
    if (buf_size == 0)
    {
        return -EIO;
    }

    if (frm_grp)
    {
        MppBufferGroup old = static_cast<MppBufferGroup>(frm_grp);
        mpp_buffer_group_put(old);
        frm_grp = nullptr;
    }

    MppBufferGroup grp = nullptr;
    MPP_RET        ret = mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_ION);
    if (ret != MPP_OK || nullptr == grp)
    {
        return -ENOMEM;
    }
    ret = mpp_buffer_group_limit_config(grp, buf_size, k_frm_grp_count);
    if (ret != MPP_OK)
    {
        mpp_buffer_group_put(grp);
        return -EIO;
    }

    ret = mpp_mpi->control(mpp_ctx, MPP_DEC_SET_EXT_BUF_GROUP, grp);
    if (ret != MPP_OK)
    {
        mpp_buffer_group_put(grp);
        return -EIO;
    }
    frm_grp = grp;

    ret = mpp_mpi->control(mpp_ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
    if (ret != MPP_OK)
    {
        return -EIO;
    }
    return 0;
}

int h264_decoder_mpp::pack_mpp_to_ready_locked(void *mpp_frame)
{
    auto *frame = static_cast<MppFrame>(mpp_frame);
    if (mpp_frame_get_errinfo(frame) || mpp_frame_get_discard(frame))
    {
        static std::atomic<unsigned> n_errinfo {0};
        if (n_errinfo.fetch_add(1) < 5)
        {
            std::fprintf(stderr,
                         "h264_decoder_mpp: frame errinfo=%u discard=%u\n",
                         mpp_frame_get_errinfo(frame), mpp_frame_get_discard(frame));
        }
        return -EIO;
    }

    MppBuffer buffer = mpp_frame_get_buffer(frame);
    if (nullptr == buffer)
    {
        return -EIO;
    }

    if (output_format != media_kind_e::NV12)
    {
        return -ENOTSUP;
    }

    MppFrameFormat fmt = mpp_frame_get_fmt(frame);
    MppFrameFormat raw = static_cast<MppFrameFormat>(fmt & MPP_FRAME_FMT_MASK);
    if (MPP_FRAME_FMT_IS_FBC(fmt) || MPP_FRAME_FMT_IS_TILE(fmt))
    {
        static std::atomic<unsigned> n_fbc {0};
        if (n_fbc.fetch_add(1) < 5)
        {
            std::fprintf(stderr, "h264_decoder_mpp: unsupported MPP fmt=0x%x (fbc/tile)\n",
                         static_cast<unsigned>(fmt));
        }
        return -ENOTSUP;
    }

    const bool is_420 = (raw == MPP_FMT_YUV420SP || raw == MPP_FMT_YUV420SP_VU);
    const bool is_422 = (raw == MPP_FMT_YUV422SP || raw == MPP_FMT_YUV422SP_VU);
    if (!is_420 && !(is_422 && output_mode == output_mode_e::convert))
    {
        static std::atomic<unsigned> n_pix {0};
        if (n_pix.fetch_add(1) < 5)
        {
            std::fprintf(stderr, "h264_decoder_mpp: unsupported pixel raw=0x%x mode=%d\n",
                         static_cast<unsigned>(raw), static_cast<int>(output_mode));
        }
        return -ENOTSUP;
    }

    auto *base = static_cast<const uint8_t *>(mpp_buffer_get_ptr(buffer));
    if (nullptr == base)
    {
        return -EIO;
    }

    int src_w = static_cast<int>(mpp_frame_get_width(frame));
    int src_h = static_cast<int>(mpp_frame_get_height(frame));
    int hor = static_cast<int>(mpp_frame_get_hor_stride(frame));
    int ver = static_cast<int>(mpp_frame_get_ver_stride(frame));

    size_t nv12_sz =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 3ULL / 2ULL;
    auto *out = static_cast<uint8_t *>(std::malloc(nv12_sz));
    if (nullptr == out)
    {
        return -ENOMEM;
    }
    std::memset(out, 0, nv12_sz);

    int r = 0;
    if (is_420)
    {
        r = pack_yuv420sp_to_nv12(base, src_w, src_h, hor, ver, out, width, height);
    }
    else
    {
        r = pack_yuv422sp_to_nv12(base, src_w, src_h, hor, ver, out, width, height);
    }
    if (r < 0)
    {
        std::free(out);
        return r;
    }

    int64_t pts = mpp_frame_get_pts(frame);
    vstreamer::frame packed;
    packed.reset(media_kind_e::NV12, width, height, pts, false, out, nv12_sz,
                 [](uint8_t *p) { std::free(p); }, pending_capture_mono_ns);
    if (pending_capture_mono_ns > 0)
    {
        const int64_t now_ns = steady_mono_ns();
        const double  ms =
            static_cast<double>(now_ns - pending_capture_mono_ns) / 1e6;
        if (ms >= 0.0)
        {
            last_latency_ms = ms;
        }
    }
    ready_frames.push_back(std::move(packed));
    return 0;
}

void h264_decoder_mpp::drain_mpp_to_ready_locked(int timeout_ms)
{
    while (ready_frames.size() < k_max_ready_frames)
    {
        const int r = fetch_one_mpp_frame_locked(timeout_ms);
        if (r < 0)
        {
            break;
        }
    }
}

int h264_decoder_mpp::fetch_one_mpp_frame_locked(int timeout_ms)
{
    if (ready_frames.size() >= k_max_ready_frames)
    {
        return -EAGAIN;
    }

    auto *mpp_ctx = static_cast<MppCtx>(ctx);
    auto *mpp_mpi = static_cast<MppApi *>(mpi);

    using clock = std::chrono::steady_clock;
    auto deadline = clock::now();
    if (timeout_ms > 0)
    {
        deadline += std::chrono::milliseconds(timeout_ms);
    }
    else if (timeout_ms < 0)
    {
        deadline = clock::time_point::max();
    }

    for (;;)
    {
        if (cancel_io.load())
        {
            return -ECANCELED;
        }
        MppFrame frame = nullptr;
        MPP_RET  ret = mpp_mpi->decode_get_frame(mpp_ctx, &frame);
        if (ret == MPP_ERR_TIMEOUT || (ret == MPP_OK && nullptr == frame))
        {
            if (timeout_ms == 0)
            {
                return -EAGAIN;
            }
            if (timeout_ms > 0 && clock::now() >= deadline)
            {
                return -EAGAIN;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (ret != MPP_OK)
        {
            return -EIO;
        }

        if (mpp_frame_get_info_change(frame))
        {
            int r = handle_info_change_locked(frame);
            mpp_frame_deinit(&frame);
            if (r < 0)
            {
                return r;
            }
            continue;
        }

        int r = pack_mpp_to_ready_locked(frame);
        mpp_frame_deinit(&frame);
        return r;
    }
}

int h264_decoder_mpp::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return 0;
    }
    int r = ensure_decoder_locked();
    if (r < 0)
    {
        return r;
    }
    opened = true;
    cancel_io = false;
    return 0;
}

void h264_decoder_mpp::cancel_pending_io()
{
    cancel_io = true;
}

void h264_decoder_mpp::close()
{
    cancel_pending_io();
    std::lock_guard<std::mutex> lock(mu);
    clear_pending_locked();
    free_decoder_locked();
    opened = false;
    cancel_io = false;
}

int h264_decoder_mpp::input(uint8_t /*port*/, const data_packet &in)
{
    const frame_data &f = data_packet::cast<frame_data>(in);
    if (f.kind != media_kind_e::H264 || f.buf.size > k_max_au)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (!opened || nullptr == ctx || nullptr == mpi)
    {
        return -EBADF;
    }

    drain_mpp_to_ready_locked(0);
    if (ready_frames.size() >= k_max_ready_frames)
    {
        return -EAGAIN;
    }

    MppPacket packet = nullptr;
    MPP_RET ret = mpp_packet_init(&packet, const_cast<uint8_t *>(f.buf.data), f.buf.size);
    if (ret != MPP_OK || nullptr == packet)
    {
        return -ENOMEM;
    }
    mpp_packet_set_pts(packet, f.pts);
    mpp_packet_set_length(packet, f.buf.size);

    auto *mpp_ctx = static_cast<MppCtx>(ctx);
    auto *mpp_mpi = static_cast<MppApi *>(mpi);
    pending_capture_mono_ns = f.capture_mono_ns;

    ret = mpp_mpi->decode_put_packet(mpp_ctx, packet);
    mpp_packet_deinit(&packet);

    if (ret == MPP_ERR_BUFFER_FULL)
    {
        drain_mpp_to_ready_locked(0);
        return -EAGAIN;
    }
    if (ret != MPP_OK)
    {
        return -EIO;
    }

    drain_mpp_to_ready_locked(0);
    return 0;
}

int h264_decoder_mpp::output(uint8_t /*port*/, data_packet &out, int timeout_ms)
{
    std::lock_guard<std::mutex> lock(mu);
    if (!opened || nullptr == ctx || nullptr == mpi)
    {
        return -EBADF;
    }

    drain_mpp_to_ready_locked(timeout_ms);
    if (ready_frames.empty())
    {
        return -EAGAIN;
    }

    out.adopt_frame(std::move(ready_frames.front()));
    ready_frames.pop_front();
    return 0;
}

int h264_decoder_mpp::configure(uint64_t /*key*/, int64_t /*value*/)
{
    return -EINVAL;
}

int h264_decoder_mpp::query(uint64_t /*key*/, int64_t * /*value*/) const
{
    return -EINVAL;
}

int h264_decoder_mpp::configure(std::string_view key, std::string_view *value)
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    std::string_view v = *value;

    std::lock_guard<std::mutex> lock(mu);
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

int h264_decoder_mpp::query(std::string_view key, std::string_view *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (key == "status")
    {
        query_buf = opened ? "open" : "closed";
        *value = query_buf;
        return 0;
    }
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
    if (key == "latency_ms")
    {
        char buf[32];
        if (std::snprintf(buf, sizeof(buf), "%.2f", last_latency_ms) < 0)
        {
            return -EINVAL;
        }
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    return -EINVAL;
}

}  // namespace vstreamer
