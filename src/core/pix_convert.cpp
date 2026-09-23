#include "core/pix_convert.hpp"

#include <cerrno>
#include <cstring>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define VSTREAMER_HAVE_NEON 1
#elif defined(__SSE2__)
#include <emmintrin.h>
#define VSTREAMER_HAVE_SSE2 1
#endif

namespace vstreamer
{
namespace
{

void pad_nv12_tail(uint8_t *dy, uint8_t *duv, int dst_w, int dst_h, int h)
{
    for (int y = h; y < dst_h; y++)
    {
        std::memcpy(dy + static_cast<size_t>(y) * static_cast<size_t>(dst_w),
                    dy + static_cast<size_t>(h - 1) * static_cast<size_t>(dst_w),
                    static_cast<size_t>(dst_w));
    }
    int ch = h / 2;
    int dst_ch = dst_h / 2;
    if (ch > 0)
    {
        for (int y = ch; y < dst_ch; y++)
        {
            std::memcpy(duv + static_cast<size_t>(y) * static_cast<size_t>(dst_w),
                        duv + static_cast<size_t>(ch - 1) * static_cast<size_t>(dst_w),
                        static_cast<size_t>(dst_w));
        }
    }
}

/* Planar U/V → interleaved UV (NV12 chroma). NEON (aarch64/H3) or SSE2 (x86). */
void merge_uv_row(uint8_t *uv, const uint8_t *cb, const uint8_t *cr, int cw)
{
    int x = 0;
#if defined(VSTREAMER_HAVE_NEON)
    for (; x + 16 <= cw; x += 16)
    {
        uint8x16x2_t uv_pair;
        uv_pair.val[0] = vld1q_u8(cb + x);
        uv_pair.val[1] = vld1q_u8(cr + x);
        vst2q_u8(uv + x * 2, uv_pair);
    }
#elif defined(VSTREAMER_HAVE_SSE2)
    for (; x + 16 <= cw; x += 16)
    {
        __m128i u = _mm_loadu_si128(reinterpret_cast<const __m128i *>(cb + x));
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(cr + x));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(uv + x * 2), _mm_unpacklo_epi8(u, v));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(uv + x * 2 + 16), _mm_unpackhi_epi8(u, v));
    }
#endif
    for (; x < cw; x++)
    {
        uv[x * 2] = cb[x];
        uv[x * 2 + 1] = cr[x];
    }
}

}  // namespace

int pack_yuv420sp_to_nv12(const uint8_t *base, int src_w, int src_h, int hor_stride,
                          int ver_stride, uint8_t *dst, int dst_w, int dst_h)
{
    int w = src_w < dst_w ? src_w : dst_w;
    int h = src_h < dst_h ? src_h : dst_h;
    w &= ~1;
    h &= ~1;
    if (w < 2 || h < 2 || nullptr == base || nullptr == dst || hor_stride < w || ver_stride < h)
    {
        return -EINVAL;
    }

    const uint8_t *src_y = base;
    const uint8_t *src_c = base + static_cast<size_t>(hor_stride) * static_cast<size_t>(ver_stride);
    uint8_t       *dy = dst;
    uint8_t       *duv = dst + static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h);

    for (int y = 0; y < h; y++)
    {
        std::memcpy(dy + static_cast<size_t>(y) * static_cast<size_t>(dst_w),
                    src_y + static_cast<size_t>(y) * static_cast<size_t>(hor_stride),
                    static_cast<size_t>(w));
    }
    for (int y = 0; y < h / 2; y++)
    {
        std::memcpy(duv + static_cast<size_t>(y) * static_cast<size_t>(dst_w),
                    src_c + static_cast<size_t>(y) * static_cast<size_t>(hor_stride),
                    static_cast<size_t>(w));
    }

    pad_nv12_tail(dy, duv, dst_w, dst_h, h);
    return 0;
}

int pack_yuv422sp_to_nv12(const uint8_t *base, int src_w, int src_h, int hor_stride,
                          int ver_stride, uint8_t *dst, int dst_w, int dst_h)
{
    int w = src_w < dst_w ? src_w : dst_w;
    int h = src_h < dst_h ? src_h : dst_h;
    w &= ~1;
    h &= ~1;
    if (w < 2 || h < 2 || nullptr == base || nullptr == dst || hor_stride < w || ver_stride < h)
    {
        return -EINVAL;
    }

    const uint8_t *src_y = base;
    const uint8_t *src_c = base + static_cast<size_t>(hor_stride) * static_cast<size_t>(ver_stride);
    uint8_t       *dy = dst;
    uint8_t       *duv = dst + static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h);

    for (int y = 0; y < h; y++)
    {
        std::memcpy(dy + static_cast<size_t>(y) * static_cast<size_t>(dst_w),
                    src_y + static_cast<size_t>(y) * static_cast<size_t>(hor_stride),
                    static_cast<size_t>(w));
    }
    for (int y = 0; y < h / 2; y++)
    {
        std::memcpy(duv + static_cast<size_t>(y) * static_cast<size_t>(dst_w),
                    src_c + static_cast<size_t>(y * 2) * static_cast<size_t>(hor_stride),
                    static_cast<size_t>(w));
    }

    pad_nv12_tail(dy, duv, dst_w, dst_h, h);
    return 0;
}

int pack_yuv420p_to_nv12(const uint8_t *y, int y_stride, const uint8_t *u, int u_stride,
                         const uint8_t *v, int v_stride, int src_w, int src_h, uint8_t *dst,
                         int dst_w, int dst_h)
{
    int w = src_w < dst_w ? src_w : dst_w;
    int h = src_h < dst_h ? src_h : dst_h;
    w &= ~1;
    h &= ~1;
    if (w < 2 || h < 2 || nullptr == y || nullptr == u || nullptr == v || nullptr == dst ||
        y_stride < w || u_stride < w / 2 || v_stride < w / 2)
    {
        return -EINVAL;
    }

    uint8_t *dy = dst;
    uint8_t *duv = dst + static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h);

    for (int row = 0; row < h; row++)
    {
        std::memcpy(dy + static_cast<size_t>(row) * static_cast<size_t>(dst_w),
                    y + static_cast<size_t>(row) * static_cast<size_t>(y_stride),
                    static_cast<size_t>(w));
    }

    for (int row = 0; row < h / 2; row++)
    {
        const uint8_t *cb = u + static_cast<size_t>(row) * static_cast<size_t>(u_stride);
        const uint8_t *cr = v + static_cast<size_t>(row) * static_cast<size_t>(v_stride);
        uint8_t       *uv = duv + static_cast<size_t>(row) * static_cast<size_t>(dst_w);
        merge_uv_row(uv, cb, cr, w / 2);
    }

    pad_nv12_tail(dy, duv, dst_w, dst_h, h);
    return 0;
}

int pack_yuv422p_to_nv12(const uint8_t *y, int y_stride, const uint8_t *u, int u_stride,
                         const uint8_t *v, int v_stride, int src_w, int src_h, uint8_t *dst,
                         int dst_w, int dst_h)
{
    int w = src_w < dst_w ? src_w : dst_w;
    int h = src_h < dst_h ? src_h : dst_h;
    w &= ~1;
    h &= ~1;
    if (w < 2 || h < 2 || nullptr == y || nullptr == u || nullptr == v || nullptr == dst ||
        y_stride < w || u_stride < w / 2 || v_stride < w / 2)
    {
        return -EINVAL;
    }

    uint8_t *dy = dst;
    uint8_t *duv = dst + static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h);

    for (int row = 0; row < h; row++)
    {
        std::memcpy(dy + static_cast<size_t>(row) * static_cast<size_t>(dst_w),
                    y + static_cast<size_t>(row) * static_cast<size_t>(y_stride),
                    static_cast<size_t>(w));
    }

    for (int row = 0; row < h; row += 2)
    {
        const uint8_t *cb = u + static_cast<size_t>(row) * static_cast<size_t>(u_stride);
        const uint8_t *cr = v + static_cast<size_t>(row) * static_cast<size_t>(v_stride);
        uint8_t       *uv = duv + static_cast<size_t>(row / 2) * static_cast<size_t>(dst_w);
        merge_uv_row(uv, cb, cr, w / 2);
    }

    pad_nv12_tail(dy, duv, dst_w, dst_h, h);
    return 0;
}

int copy_nv12_planes_to_packed(const uint8_t *y_plane, int y_stride, const uint8_t *uv_plane,
                               int uv_stride, int src_w, int src_h, bool swap_chroma, uint8_t *dst,
                               int dst_w, int dst_h)
{
    int w = src_w < dst_w ? src_w : dst_w;
    int h = src_h < dst_h ? src_h : dst_h;
    w &= ~1;
    h &= ~1;
    if (w < 2 || h < 2 || nullptr == y_plane || nullptr == uv_plane || nullptr == dst ||
        y_stride < w || uv_stride < w)
    {
        return -EINVAL;
    }

    uint8_t *dy = dst;
    uint8_t *duv = dst + static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h);

    for (int row = 0; row < h; row++)
    {
        std::memcpy(dy + static_cast<size_t>(row) * static_cast<size_t>(dst_w),
                    y_plane + static_cast<size_t>(row) * static_cast<size_t>(y_stride),
                    static_cast<size_t>(w));
    }
    for (int row = 0; row < h / 2; row++)
    {
        const uint8_t *src = uv_plane + static_cast<size_t>(row) * static_cast<size_t>(uv_stride);
        uint8_t       *out = duv + static_cast<size_t>(row) * static_cast<size_t>(dst_w);
        if (!swap_chroma)
        {
            std::memcpy(out, src, static_cast<size_t>(w));
            continue;
        }
        for (int x = 0; x < w; x += 2)
        {
            out[x] = src[x + 1];
            out[x + 1] = src[x];
        }
    }

    pad_nv12_tail(dy, duv, dst_w, dst_h, h);
    return 0;
}

}  // namespace vstreamer
