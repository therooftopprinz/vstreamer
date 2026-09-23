#include "components/h264_encoder_mpp.hpp"

#include "core/key_util.hpp"
#include "core/time_util.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#define MODULE_TAG "vstreamer_mpp_h264_enc"

extern "C"
{
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_mpi_cmd.h>
#include <rockchip/rk_venc_cmd.h>
#include <rockchip/rk_venc_rc.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/mpp_packet.h>
}

namespace vstreamer
{
namespace
{

constexpr size_t k_max_au = 8ULL * 1024ULL * 1024ULL;
constexpr size_t k_out_q_max = 32;

int mpp_align(int x, int a)
{
    return (x + a - 1) & ~(a - 1);
}

size_t enc_mdinfo_bytes(int hor_stride, int ver_stride)
{
    const int w = mpp_align(hor_stride, 64);
    const int h = mpp_align(ver_stride, 64);
    return static_cast<size_t>((w >> 6) * (h >> 6) * 32);
}

int cfg_set_s32(MppEncCfg cfg, const char *key, RK_S32 val)
{
    const MPP_RET ret = mpp_enc_cfg_set_s32(cfg, key, val);
    if (ret != MPP_OK)
    {
        std::fprintf(stderr, "h264_encoder_mpp: cfg %s=%d failed %d\n", key, val, ret);
        return -EIO;
    }
    return 0;
}

int h264_level_for_size(int w, int h, int fps)
{
    const int pixels = w * h;
    const int f = fps > 0 ? fps : 30;
    if (pixels <= 416 * 240)
    {
        return 31;
    }
    if (pixels <= 1280 * 720)
    {
        return f > 30 ? 32 : 31;
    }
    return 40;
}

size_t enc_frame_buffer_bytes(int hor_stride, int ver_stride)
{
    const int w = mpp_align(hor_stride, 64);
    const int h = mpp_align(ver_stride, 64);
    return static_cast<size_t>(w) * static_cast<size_t>(h) * 3ULL / 2ULL;
}

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

bool buffer_looks_annexb(const uint8_t *data, size_t len)
{
    if (len < 4 || nullptr == data)
    {
        return false;
    }
    if (data[0] == 0 && data[1] == 0 && data[2] == 1)
    {
        return true;
    }
    return data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
}

int avcc_to_annexb(const uint8_t *data, size_t len, std::vector<uint8_t> *out)
{
    if (nullptr == data || nullptr == out)
    {
        return -EINVAL;
    }
    out->clear();
    size_t i = 0;
    while (i + 4 <= len)
    {
        uint32_t nal_len = (static_cast<uint32_t>(data[i]) << 24) |
                           (static_cast<uint32_t>(data[i + 1]) << 16) |
                           (static_cast<uint32_t>(data[i + 2]) << 8) |
                           static_cast<uint32_t>(data[i + 3]);
        i += 4;
        if (nal_len == 0 || i + nal_len > len)
        {
            return -EINVAL;
        }
        static const uint8_t sc[4] = {0, 0, 0, 1};
        out->insert(out->end(), sc, sc + 4);
        out->insert(out->end(), data + i, data + i + nal_len);
        i += nal_len;
    }
    return out->empty() ? -EINVAL : 0;
}

void copy_nv12_to_stride(const uint8_t *src, int w, int h, uint8_t *dst, int hor, int ver)
{
    for (int y = 0; y < h; y++)
    {
        std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(hor),
                    src + static_cast<size_t>(y) * static_cast<size_t>(w),
                    static_cast<size_t>(w));
    }
    const uint8_t *src_uv = src + static_cast<size_t>(w) * static_cast<size_t>(h);
    uint8_t       *dst_uv = dst + static_cast<size_t>(hor) * static_cast<size_t>(ver);
    const int      uv_h = h / 2;
    for (int y = 0; y < uv_h; y++)
    {
        std::memcpy(dst_uv + static_cast<size_t>(y) * static_cast<size_t>(hor),
                    src_uv + static_cast<size_t>(y) * static_cast<size_t>(w),
                    static_cast<size_t>(w));
    }
}

bool mpp_packet_keyframe(MppPacket packet)
{
    MppMeta meta = mpp_packet_get_meta(packet);
    if (nullptr != meta)
    {
        RK_S32 intra = 0;
        if (MPP_OK == mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &intra) && intra != 0)
        {
            return true;
        }
    }
    const uint8_t *data = static_cast<const uint8_t *>(mpp_packet_get_data(packet));
    const size_t   len = mpp_packet_get_length(packet);
    if (nullptr == data || len < 5)
    {
        return false;
    }
    const uint8_t *nal = data;
    size_t         off = 0;
    if (buffer_looks_annexb(data, len))
    {
        off = (data[2] == 1) ? 3 : 4;
        nal = data + off;
    }
    else if (len >= 5)
    {
        const uint32_t n = (static_cast<uint32_t>(data[0]) << 24) |
                           (static_cast<uint32_t>(data[1]) << 16) |
                           (static_cast<uint32_t>(data[2]) << 8) |
                           static_cast<uint32_t>(data[3]);
        if (n > 0 && 4 + n <= len)
        {
            nal = data + 4;
        }
    }
    const int nal_type = nal[0] & 0x1f;
    return nal_type == 5;
}

}  // namespace

h264_encoder_mpp::h264_encoder_mpp() = default;

h264_encoder_mpp::~h264_encoder_mpp()
{
    close();
}

std::string h264_encoder_mpp::name() const
{
    return "h264_encoder_mpp";
}

media_kind_e h264_encoder_mpp::input_kind() const
{
    return media_kind_e::NV12;
}

media_kind_e h264_encoder_mpp::output_kind() const
{
    return media_kind_e::H264;
}

void h264_encoder_mpp::clear_out_locked()
{
    out_q.clear();
}

int h264_encoder_mpp::apply_h264_cfg_locked()
{
    auto *cfg = static_cast<MppEncCfg>(enc_cfg);
    if (nullptr == cfg)
    {
        return -EBADF;
    }

    const int w = live_w > 0 ? live_w : width;
    const int h = live_h > 0 ? live_h : height;
    const int f = live_fps > 0 ? live_fps : fps;
    const int level = h264_level_for_size(w, h, f);

    if (cfg_set_s32(cfg, "h264:profile", 100) < 0)
    {
        return -EIO;
    }
    if (cfg_set_s32(cfg, "h264:level", level) < 0)
    {
        return -EIO;
    }
    if (cfg_set_s32(cfg, "h264:cabac_en", 1) < 0)
    {
        return -EIO;
    }
    if (cfg_set_s32(cfg, "h264:cabac_idc", 0) < 0)
    {
        return -EIO;
    }
    if (cfg_set_s32(cfg, "h264:trans8x8", 1) < 0)
    {
        return -EIO;
    }
    return 0;
}

int h264_encoder_mpp::apply_rc_cfg_locked()
{
    auto *cfg = static_cast<MppEncCfg>(enc_cfg);
    auto *mpp_ctx = static_cast<MppCtx>(ctx);
    auto *mpp_mpi = static_cast<MppApi *>(mpi);
    if (nullptr == cfg || nullptr == mpp_ctx || nullptr == mpp_mpi)
    {
        return -EBADF;
    }

    const int fps_val = live_fps > 0 ? live_fps : 1;
    const int bps_cfg = live_bps > 0 ? live_bps : bps_target;
    const int bps = (rc_mpp_cbr && bps_cfg > 0) ? bps_cfg : 0;

    if (bps > 0)
    {
        const RK_S32 wire_target = static_cast<RK_S32>(bps);
        const RK_S32 bps_max = wire_target;
        const RK_S32 bps_min = wire_target * 7 / 8;

        const int  w = live_w > 0 ? live_w : width;
        const int  h = live_h > 0 ? live_h : height;
        const bool large_frame = (w * h) >= (1920 * 1080);

        if (cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:priority", MPP_ENC_RC_BY_BITRATE_FIRST) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:bps_target", wire_target) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:bps_max", bps_max) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:bps_min", bps_min) < 0)
        {
            return -EIO;
        }

        const RK_S32 qp_init = live_qp > 0 ? live_qp : qp;
        const RK_S32 qp_min = large_frame ? 28 : 10;
        const RK_S32 qp_max = 51;
        if (cfg_set_s32(cfg, "rc:qp_init", qp_init) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:qp_min", qp_min) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:qp_max", qp_max) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:qp_min_i", qp_min) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:qp_max_i", qp_max) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:max_reenc_times", 8) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:stats_time", 1) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED) < 0)
        {
            return -EIO;
        }

        const RK_S32 avg_bytes = static_cast<RK_S32>(wire_target / 8 / fps_val);
        const RK_S32 super_p = large_frame ? avg_bytes : (avg_bytes * 3 / 2);
        const RK_S32 super_i = large_frame ? avg_bytes : (avg_bytes * 2);
        if (cfg_set_s32(cfg, "rc:super_mode", MPP_ENC_RC_SUPER_FRM_REENC) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:super_p_thd", super_p > 0 ? super_p : 1) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:super_i_thd", super_i > 0 ? super_i : 1) < 0)
        {
            return -EIO;
        }
    }
    else
    {
        if (cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_FIXQP) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:bps_target", 0) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:bps_max", 0) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:bps_min", 0) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:qp_init", live_qp) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:qp_max", live_qp) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:qp_min", live_qp) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:qp_max_i", live_qp) < 0)
        {
            return -EIO;
        }
        if (cfg_set_s32(cfg, "rc:qp_min_i", live_qp) < 0)
        {
            return -EIO;
        }
    }

    if (cfg_set_s32(cfg, "rc:gop", live_gop > 0 ? live_gop : 1) < 0)
    {
        return -EIO;
    }
    if (cfg_set_s32(cfg, "rc:fps_in_num", live_fps) < 0)
    {
        return -EIO;
    }
    if (cfg_set_s32(cfg, "rc:fps_in_denom", 1) < 0)
    {
        return -EIO;
    }
    if (cfg_set_s32(cfg, "rc:fps_out_num", live_fps) < 0)
    {
        return -EIO;
    }
    if (cfg_set_s32(cfg, "rc:fps_out_denom", 1) < 0)
    {
        return -EIO;
    }

    MPP_RET ret = MPP_OK;
    {
        std::lock_guard<std::mutex> api_lock(mpp_api_mu);
        ret = mpp_mpi->control(mpp_ctx, MPP_ENC_SET_CFG, cfg);
    }
    if (ret != MPP_OK)
    {
        std::fprintf(stderr, "h264_encoder_mpp: MPP_ENC_SET_CFG failed %d\n", ret);
        return -EIO;
    }
    return 0;
}

int h264_encoder_mpp::encoder_open_locked()
{
    MppCtx  mpp_ctx = nullptr;
    MppApi *mpp_mpi = nullptr;
    MPP_RET ret = mpp_create(&mpp_ctx, &mpp_mpi);
    if (ret != MPP_OK || nullptr == mpp_ctx || nullptr == mpp_mpi)
    {
        std::fprintf(stderr, "h264_encoder_mpp: mpp_create failed %d\n", ret);
        return -EIO;
    }

    ret = mpp_init(mpp_ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK)
    {
        std::fprintf(stderr, "h264_encoder_mpp: mpp_init enc failed %d\n", ret);
        mpp_destroy(mpp_ctx);
        return -EIO;
    }

    {
        MppPollType out_to = MPP_POLL_NON_BLOCK;
        MppPollType in_to = MPP_POLL_BLOCK;
        ret = mpp_mpi->control(mpp_ctx, MPP_SET_OUTPUT_TIMEOUT, &out_to);
        if (ret != MPP_OK)
        {
            std::fprintf(stderr, "h264_encoder_mpp: SET_OUTPUT_TIMEOUT failed %d\n", ret);
        }
        ret = mpp_mpi->control(mpp_ctx, MPP_SET_INPUT_TIMEOUT, &in_to);
        if (ret != MPP_OK)
        {
            std::fprintf(stderr, "h264_encoder_mpp: SET_INPUT_TIMEOUT failed %d\n", ret);
        }
    }

    MppEncCfg cfg = nullptr;
    ret = mpp_enc_cfg_init(&cfg);
    if (ret != MPP_OK || nullptr == cfg)
    {
        mpp_destroy(mpp_ctx);
        return -ENOMEM;
    }

    const int hor = mpp_align(width, 8);
    const int ver = mpp_align(height, 2);

    ret = mpp_enc_cfg_set_s32(cfg, "prep:width", width);
    ret = mpp_enc_cfg_set_s32(cfg, "prep:height", height);
    ret = mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", hor);
    ret = mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", ver);
    ret = mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
    ret = mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK)
    {
        mpp_enc_cfg_deinit(cfg);
        mpp_destroy(mpp_ctx);
        return -EIO;
    }

    live_w = width;
    live_h = height;
    live_hor = hor;
    live_ver = ver;
    live_fps = fps;
    live_qp = qp;
    live_gop = gop > 0 ? gop : 1;
    live_bps = bps_target;

    ctx = mpp_ctx;
    mpi = mpp_mpi;
    enc_cfg = cfg;

    int r = apply_h264_cfg_locked();
    if (r < 0)
    {
        encoder_close_locked();
        return r;
    }

    r = apply_rc_cfg_locked();
    if (r < 0)
    {
        encoder_close_locked();
        return r;
    }

    {
        MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
        ret = mpp_mpi->control(mpp_ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);
        if (ret != MPP_OK)
        {
            std::fprintf(stderr, "h264_encoder_mpp: SET_HEADER_MODE failed %d\n", ret);
        }
    }

    MppBufferGroup grp = nullptr;
    ret = mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE);
    if (ret != MPP_OK || nullptr == grp)
    {
        ret = mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_ION);
    }
    if (ret != MPP_OK || nullptr == grp)
    {
        encoder_close_locked();
        return -ENOMEM;
    }
    frm_grp = grp;

    const size_t frm_bytes = enc_frame_buffer_bytes(hor, ver);
    const size_t md_bytes = enc_mdinfo_bytes(hor, ver);

    const size_t pkt_bytes = frm_bytes * 2ULL;
    enc_free_slots.clear();
    enc_pending_slots.clear();
    for (int i = 0; i < enc_slot_count; i++)
    {
        MppBuffer in_buf = nullptr;
        MppBuffer out_buf = nullptr;
        ret = mpp_buffer_get(grp, &in_buf, frm_bytes);
        if (ret != MPP_OK || nullptr == in_buf)
        {
            encoder_close_locked();
            return -ENOMEM;
        }
        ret = mpp_buffer_get(grp, &out_buf, pkt_bytes);
        if (ret != MPP_OK || nullptr == out_buf)
        {
            mpp_buffer_put(in_buf);
            encoder_close_locked();
            return -ENOMEM;
        }
        enc_slots[i].frm = in_buf;
        enc_slots[i].pkt = out_buf;
        enc_free_slots.push_back(i);
    }
    MppBuffer md_buf = nullptr;
    if (md_bytes > 0)
    {
        ret = mpp_buffer_get(grp, &md_buf, md_bytes);
        if (ret != MPP_OK || nullptr == md_buf)
        {
            encoder_close_locked();
            return -ENOMEM;
        }
    }
    md_info = md_buf;

    reopen_req = false;
    std::fprintf(stderr, "h264_encoder_mpp: opened %dx%d@%d cbr=%d qp=%d gop=%d\n", live_w, live_h,
                 live_fps, live_bps, live_qp, live_gop);
    return 0;
}

void h264_encoder_mpp::encoder_close_locked()
{
    clear_out_locked();
    enc_au_accum.clear();
    enc_au_key = false;
    enc_au_pts = 0;
    enc_au_capture_mono_ns = 0;

    enc_free_slots.clear();
    enc_pending_slots.clear();
    for (enc_slot &slot : enc_slots)
    {
        if (slot.frm)
        {
            mpp_buffer_put(static_cast<MppBuffer>(slot.frm));
            slot.frm = nullptr;
        }
        if (slot.pkt)
        {
            mpp_buffer_put(static_cast<MppBuffer>(slot.pkt));
            slot.pkt = nullptr;
        }
    }
    if (md_info)
    {
        mpp_buffer_put(static_cast<MppBuffer>(md_info));
        md_info = nullptr;
    }

    if (frm_grp)
    {
        MppBufferGroup grp = static_cast<MppBufferGroup>(frm_grp);
        mpp_buffer_group_put(grp);
        frm_grp = nullptr;
    }

    if (ctx && mpi)
    {
        auto *mpp_ctx = static_cast<MppCtx>(ctx);
        auto *mpp_mpi = static_cast<MppApi *>(mpi);
        std::lock_guard<std::mutex> api_lock(mpp_api_mu);
        for (int i = 0; i < 64; i++)
        {
            MppPacket pkt = nullptr;
            const MPP_RET gr = mpp_mpi->encode_get_packet(mpp_ctx, &pkt);
            if (gr != MPP_OK || nullptr == pkt)
            {
                break;
            }
            mpp_packet_deinit(&pkt);
        }
        mpp_mpi->reset(mpp_ctx);
    }

    if (enc_cfg)
    {
        mpp_enc_cfg_deinit(static_cast<MppEncCfg>(enc_cfg));
        enc_cfg = nullptr;
    }

    if (ctx)
    {
        mpp_destroy(static_cast<MppCtx>(ctx));
        ctx = nullptr;
        mpi = nullptr;
    }

    live_w = 0;
    live_h = 0;
    live_hor = 0;
    live_ver = 0;
    live_fps = 0;
    live_qp = 0;
    live_gop = 0;
    live_bps = 0;
    enc_frames_in = 0;
}

int h264_encoder_mpp::reopen_if_needed_locked()
{
    if (!reopen_req && nullptr != ctx)
    {
        return 0;
    }
    encoder_close_locked();
    return encoder_open_locked();
}

int h264_encoder_mpp::drain_packets_locked(int timeout_ms)
{
    auto *mpp_ctx = static_cast<MppCtx>(ctx);
    auto *mpp_mpi = static_cast<MppApi *>(mpi);
    if (nullptr == mpp_ctx || nullptr == mpp_mpi)
    {
        return -EBADF;
    }

    std::lock_guard<std::mutex> api_lock(mpp_api_mu);

    using clock = std::chrono::steady_clock;
    auto deadline = clock::now();
    if (timeout_ms > 0)
    {
        deadline += std::chrono::milliseconds(timeout_ms);
    }

    for (;;)
    {
        MppPacket packet = nullptr;
        MPP_RET   ret = mpp_mpi->encode_get_packet(mpp_ctx, &packet);
        if (ret == MPP_ERR_TIMEOUT || (ret == MPP_OK && nullptr == packet))
        {
            if (timeout_ms == 0)
            {
                break;
            }
            if (timeout_ms > 0 && clock::now() >= deadline)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (ret != MPP_OK)
        {
            break;
        }

        ingest_enc_packet(packet);
    }

    return 0;
}

int h264_encoder_mpp::drain_packets_unlocked(int timeout_ms)
{
    return drain_packets_locked(timeout_ms);
}

bool h264_encoder_mpp::append_enc_packet_bytes(const uint8_t *data, size_t len)
{
    if (nullptr == data || len == 0)
    {
        return true;
    }
    if (!buffer_looks_annexb(data, len))
    {
        std::vector<uint8_t> annex;
        if (avcc_to_annexb(data, len, &annex) < 0)
        {
            return false;
        }
        enc_au_accum.insert(enc_au_accum.end(), annex.begin(), annex.end());
        return true;
    }
    enc_au_accum.insert(enc_au_accum.end(), data, data + len);
    return true;
}

void h264_encoder_mpp::flush_enc_au_to_out_locked()
{
    if (enc_au_accum.empty())
    {
        return;
    }
    if (enc_au_accum.size() > k_max_au)
    {
        enc_au_accum.clear();
        return;
    }

    auto *buf = static_cast<uint8_t *>(std::malloc(enc_au_accum.size()));
    if (nullptr == buf)
    {
        enc_au_accum.clear();
        return;
    }
    std::memcpy(buf, enc_au_accum.data(), enc_au_accum.size());
    const size_t out_len = enc_au_accum.size();
    enc_au_accum.clear();

    frame au;
    au.reset(media_kind_e::H264, live_w, live_h, enc_au_pts, enc_au_key, buf, out_len,
             [](uint8_t *p) { std::free(p); }, enc_au_capture_mono_ns);

    if (enc_au_capture_mono_ns > 0)
    {
        const int64_t now_ns = steady_mono_ns();
        const double  ms =
            static_cast<double>(now_ns - enc_au_capture_mono_ns) / 1e6;
        if (ms >= 0.0)
        {
            last_latency_ms = ms;
        }
    }

    while (out_q.size() >= k_out_q_max && !out_q.empty())
    {
        out_q.pop_front();
    }
    if (out_q.size() < k_out_q_max)
    {
        out_q.push_back(std::move(au));
        cv.notify_one();
    }
}

void h264_encoder_mpp::ingest_enc_packet(void *mpp_packet_opaque)
{
    MppPacket packet = static_cast<MppPacket>(mpp_packet_opaque);
    if (nullptr == packet)
    {
        return;
    }

    MppBuffer pkt_mem = mpp_packet_get_buffer(packet);
    if (nullptr != pkt_mem)
    {
        mpp_buffer_sync_ro_begin(pkt_mem);
        mpp_buffer_sync_ro_end(pkt_mem);
    }

    const uint8_t *data = static_cast<const uint8_t *>(mpp_packet_get_pos(packet));
    if (nullptr == data)
    {
        data = static_cast<const uint8_t *>(mpp_packet_get_data(packet));
    }
    const size_t  len = mpp_packet_get_length(packet);
    const int64_t pts = mpp_packet_get_pts(packet);
    const bool    key = mpp_packet_keyframe(packet);

    if (nullptr != data && len > 0)
    {
        (void)append_enc_packet_bytes(data, len);
    }

    RK_U32 eoi = 1;
    if (mpp_packet_is_partition(packet))
    {
        eoi = mpp_packet_is_eoi(packet);
    }
    mpp_packet_deinit(&packet);

    enc_au_pts = pts;
    if (key)
    {
        enc_au_key = true;
    }
    if (eoi)
    {
        if (!enc_pending_slots.empty())
        {
            const int slot_idx = enc_pending_slots.front();
            enc_au_capture_mono_ns = enc_slots[slot_idx].capture_mono_ns;
        }
        std::lock_guard<std::mutex> lock(mu);
        flush_enc_au_to_out_locked();
        enc_au_key = false;
        release_enc_slot_after_eoi();
    }
}

bool h264_encoder_mpp::push_mpp_packet_to_out_locked(void *packet_opaque)
{
    MppPacket packet = static_cast<MppPacket>(packet_opaque);
    if (nullptr == packet)
    {
        return false;
    }

    MppBuffer pkt_mem = mpp_packet_get_buffer(packet);
    if (nullptr != pkt_mem)
    {
        mpp_buffer_sync_ro_begin(pkt_mem);
        mpp_buffer_sync_ro_end(pkt_mem);
    }

    const uint8_t *data = static_cast<const uint8_t *>(mpp_packet_get_pos(packet));
    if (nullptr == data)
    {
        data = static_cast<const uint8_t *>(mpp_packet_get_data(packet));
    }
    size_t        len = mpp_packet_get_length(packet);
    const int64_t pts = mpp_packet_get_pts(packet);
    const bool    key = mpp_packet_keyframe(packet);

    if (nullptr == data || len == 0)
    {
        return false;
    }

    std::vector<uint8_t> annex;
    const uint8_t       *out_data = data;
    size_t               out_len = len;
    if (!buffer_looks_annexb(data, len))
    {
        if (avcc_to_annexb(data, len, &annex) < 0)
        {
            return false;
        }
        out_data = annex.data();
        out_len = annex.size();
    }

    if (out_len > k_max_au)
    {
        return false;
    }

    auto *buf = static_cast<uint8_t *>(std::malloc(out_len));
    if (nullptr == buf)
    {
        return false;
    }
    std::memcpy(buf, out_data, out_len);

    frame au;
    au.reset(media_kind_e::H264, live_w, live_h, pts, key, buf, out_len,
             [](uint8_t *p) { std::free(p); }, enc_au_capture_mono_ns);

    if (enc_au_capture_mono_ns > 0)
    {
        const int64_t now_ns = steady_mono_ns();
        const double  ms =
            static_cast<double>(now_ns - enc_au_capture_mono_ns) / 1e6;
        if (ms >= 0.0)
        {
            last_latency_ms = ms;
        }
    }

    while (out_q.size() >= k_out_q_max)
    {
        out_q.pop_front();
    }
    out_q.push_back(std::move(au));
    cv.notify_one();
    return true;
}

bool h264_encoder_mpp::push_mpp_packet_to_out(void *packet_opaque)
{
    std::lock_guard<std::mutex> lock(mu);
    return push_mpp_packet_to_out_locked(packet_opaque);
}

void h264_encoder_mpp::release_enc_slot_after_eoi()
{
    if (enc_pending_slots.empty())
    {
        return;
    }
    enc_free_slots.push_back(enc_pending_slots.front());
    enc_pending_slots.pop_front();
}

void h264_encoder_mpp::drain_enc_packets_nonblock(void *mpp_ctx_opaque, void *mpp_mpi_opaque)
{
    auto *mpp_ctx = static_cast<MppCtx>(mpp_ctx_opaque);
    auto *mpp_mpi = static_cast<MppApi *>(mpp_mpi_opaque);
    if (nullptr == mpp_ctx || nullptr == mpp_mpi)
    {
        return;
    }

    for (int poll = 0; poll < 64; poll++)
    {
        MppPacket packet = nullptr;
        MPP_RET   ret = mpp_mpi->encode_get_packet(mpp_ctx, &packet);
        if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT)
        {
            break;
        }
        if (nullptr == packet)
        {
            break;
        }

        ingest_enc_packet(packet);
    }
}

int h264_encoder_mpp::put_nv12_frame_unlocked(const frame_data &f)
{
    auto *mpp_ctx = static_cast<MppCtx>(ctx);
    auto *mpp_mpi = static_cast<MppApi *>(mpi);
    if (nullptr == mpp_ctx || nullptr == mpp_mpi)
    {
        return -EBADF;
    }

    if (enc_free_slots.empty())
    {
        std::lock_guard<std::mutex> api_lock(mpp_api_mu);
        drain_enc_packets_nonblock(mpp_ctx, mpp_mpi);
    }
    if (enc_free_slots.empty())
    {
        return -EAGAIN;
    }

    const int slot_idx = enc_free_slots.front();
    enc_free_slots.pop_front();
    enc_slots[slot_idx].capture_mono_ns = f.capture_mono_ns;
    auto in_buf = static_cast<MppBuffer>(enc_slots[slot_idx].frm);
    auto out_buf = static_cast<MppBuffer>(enc_slots[slot_idx].pkt);
    if (nullptr == in_buf || nullptr == out_buf)
    {
        enc_free_slots.push_back(slot_idx);
        return -EBADF;
    }

    const int hor = live_hor > 0 ? live_hor : mpp_align(live_w, 8);
    const int ver = live_ver > 0 ? live_ver : mpp_align(live_h, 2);

    uint8_t *dst = static_cast<uint8_t *>(mpp_buffer_get_ptr(in_buf));
    if (nullptr == dst)
    {
        return -EIO;
    }

    mpp_buffer_sync_begin(in_buf);
    copy_nv12_to_stride(f.buf.data, live_w, live_h, dst, hor, ver);
    mpp_buffer_sync_end(in_buf);

    MPP_RET ret = MPP_OK;
    {
        std::lock_guard<std::mutex> api_lock(mpp_api_mu);

        drain_enc_packets_nonblock(mpp_ctx, mpp_mpi);

        MppFrame  frame = nullptr;
        MppPacket packet = nullptr;
        ret = mpp_frame_init(&frame);
        if (ret != MPP_OK || nullptr == frame)
        {
            return -ENOMEM;
        }

        mpp_frame_set_width(frame, live_w);
        mpp_frame_set_height(frame, live_h);
        mpp_frame_set_hor_stride(frame, hor);
        mpp_frame_set_ver_stride(frame, ver);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(frame, in_buf);
        mpp_frame_set_pts(frame, f.pts);

        ret = mpp_packet_init_with_buffer(&packet, out_buf);
        if (ret != MPP_OK || nullptr == packet)
        {
            mpp_frame_deinit(&frame);
            return -ENOMEM;
        }
        mpp_packet_set_length(packet, 0);

        MppMeta meta = mpp_frame_get_meta(frame);
        if (nullptr == meta)
        {
            mpp_packet_deinit(&packet);
            mpp_frame_deinit(&frame);
            return -EIO;
        }
        mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, packet);
        if (nullptr != md_info)
        {
            mpp_meta_set_buffer(meta, KEY_MOTION_INFO, static_cast<MppBuffer>(md_info));
        }
        if (enc_frames_in == 0)
        {
            mpp_meta_set_s32(meta, KEY_INPUT_IDR_REQ, 1);
        }

        if (cancel_io.load())
        {
            mpp_packet_deinit(&packet);
            mpp_frame_deinit(&frame);
            return -ECANCELED;
        }

        ret = mpp_mpi->encode_put_frame(mpp_ctx, frame);
        mpp_frame_deinit(&frame);

        if (ret == MPP_ERR_BUFFER_FULL)
        {
            mpp_packet_deinit(&packet);
            enc_free_slots.push_back(slot_idx);
            return -EAGAIN;
        }
        if (ret != MPP_OK)
        {
            mpp_packet_deinit(&packet);
            enc_free_slots.push_back(slot_idx);
            std::fprintf(stderr, "h264_encoder_mpp: encode_put_frame failed %d\n", ret);
            return -EIO;
        }

        enc_pending_slots.push_back(slot_idx);
        drain_enc_packets_nonblock(mpp_ctx, mpp_mpi);
    }

    enc_frames_in++;
    return 0;
}

int h264_encoder_mpp::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return 0;
    }
    const int r = encoder_open_locked();
    if (r < 0)
    {
        return r;
    }
    opened = true;
    cancel_io = false;
    return 0;
}

void h264_encoder_mpp::cancel_pending_io()
{
    cancel_io = true;
    cv.notify_all();
}

void h264_encoder_mpp::close()
{
    cancel_pending_io();
    std::lock_guard<std::mutex> lock(mu);
    encoder_close_locked();
    opened = false;
    reopen_req = false;
    cancel_io = false;
    cv.notify_all();
}

int h264_encoder_mpp::input(uint8_t /*port*/, const data_packet &in)
{
    const frame_data &f = data_packet::cast<frame_data>(in);
    if (f.kind != media_kind_e::NV12)
    {
        return -EINVAL;
    }

    std::unique_lock<std::mutex> lock(mu);
    if (!opened)
    {
        return -EBADF;
    }

    const int r = reopen_if_needed_locked();
    if (r < 0)
    {
        return r;
    }

    if (f.width != live_w || f.height != live_h)
    {
        return -EINVAL;
    }

    const size_t want = static_cast<size_t>(f.width) * static_cast<size_t>(f.height) * 3ULL / 2ULL;
    if (f.buf.size != want || nullptr == f.buf.data)
    {
        return -EINVAL;
    }

    lock.unlock();
    const int enc_r = put_nv12_frame_unlocked(f);
    return enc_r;
}

int h264_encoder_mpp::output(uint8_t /*port*/, data_packet &out, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(mu);
    if (!opened && out_q.empty())
    {
        return -EBADF;
    }

    if (out_q.empty() && opened)
    {
        lock.unlock();
        (void)drain_packets_unlocked(timeout_ms > 0 ? timeout_ms : 0);
        lock.lock();
    }

    auto ready = [this]() { return !out_q.empty() || !opened || cancel_io.load(); };

    if (out_q.empty())
    {
        if (timeout_ms == 0)
        {
            return -EAGAIN;
        }
        if (timeout_ms < 0)
        {
            cv.wait(lock, ready);
        }
        else
        {
            cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
        }
    }

    if (out_q.empty())
    {
        return opened ? -EAGAIN : -EBADF;
    }

    out.adopt_frame(std::move(out_q.front()));
    out_q.pop_front();
    return 0;
}

int h264_encoder_mpp::configure(uint64_t /*key*/, int64_t /*value*/)
{
    return -EINVAL;
}

int h264_encoder_mpp::query(uint64_t /*key*/, int64_t * /*value*/) const
{
    return -EINVAL;
}

int h264_encoder_mpp::configure(std::string_view key, std::string_view *value)
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    std::string_view v = *value;
    std::string      tmp(v);

    std::unique_lock<std::mutex> lock(mu);

    if (key == "size")
    {
        int w = 0;
        int h = 0;
        const int r = parse_size(v, &w, &h);
        if (r < 0)
        {
            return r;
        }
        if (w != width || h != height)
        {
            width = w;
            height = h;
            if (opened)
            {
                reopen_req = true;
            }
        }
        return 0;
    }
    if (key == "fps")
    {
        int64_t n = 0;
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n < 1 || n > 120)
        {
            return -EINVAL;
        }
        const int nv = static_cast<int>(n);
        if (nv != fps)
        {
            fps = nv;
            if (opened)
            {
                reopen_req = true;
            }
        }
        return 0;
    }
    if (key == "qp")
    {
        int64_t n = 0;
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n < 2 || n > 51)
        {
            return -EINVAL;
        }
        const int nv = static_cast<int>(n);
        if (nv != qp)
        {
            qp = nv;
            if (opened)
            {
                live_qp = qp;
                return apply_rc_cfg_locked();
            }
        }
        return 0;
    }
    if (key == "gop")
    {
        int64_t n = 0;
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n < 1 || n > 255)
        {
            return -EINVAL;
        }
        const int nv = static_cast<int>(n);
        if (nv != gop)
        {
            gop = nv;
            if (opened)
            {
                live_gop = gop;
                return apply_rc_cfg_locked();
            }
        }
        return 0;
    }
    if (key == "cbr" || key == "bps")
    {
        int64_t n = 0;
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n < 0 || n > 200000000LL)
        {
            return -EINVAL;
        }
        const int nv = static_cast<int>(n);
        if (nv != bps_target)
        {
            bps_target = nv;
            if (opened && rc_mpp_cbr)
            {
                live_bps = bps_target;
                return apply_rc_cfg_locked();
            }
        }
        return 0;
    }
    if (key == "rc")
    {
        const bool want_cbr = (tmp == "cbr");
        const bool want_fix = (tmp == "fixqp");
        if (!want_cbr && !want_fix)
        {
            return -EINVAL;
        }
        rc_mpp_cbr = want_cbr;
        if (opened)
        {
            return apply_rc_cfg_locked();
        }
        return 0;
    }
    return -EINVAL;
}

int h264_encoder_mpp::query(std::string_view key, std::string_view *value) const
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
        const int w = live_w > 0 ? live_w : width;
        const int h = live_h > 0 ? live_h : height;
        if (std::snprintf(buf, sizeof(buf), "%dx%d", w, h) < 0)
        {
            return -EINVAL;
        }
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if (key == "fps" || key == "qp" || key == "gop")
    {
        int n = 0;
        if (key == "fps")
        {
            n = live_fps > 0 ? live_fps : fps;
        }
        else if (key == "qp")
        {
            n = live_qp > 0 ? live_qp : qp;
        }
        else
        {
            n = live_gop > 0 ? live_gop : gop;
        }
        char buf[32];
        if (key_format_i64(n, buf, sizeof(buf)) < 0)
        {
            return -EINVAL;
        }
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if (key == "rc")
    {
        query_buf = rc_mpp_cbr ? "cbr" : "fixqp";
        *value = query_buf;
        return 0;
    }
    if (key == "cbr" || key == "bps")
    {
        const int n = bps_target > 0 ? bps_target : live_bps;
        char buf[32];
        if (key_format_i64(n, buf, sizeof(buf)) < 0)
        {
            return -EINVAL;
        }
        query_buf = buf;
        *value = query_buf;
        return 0;
    }
    if (key == "backend" || key == "codec")
    {
        query_buf = "h264_mpp";
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
