#ifndef VSTREAMER_CORE_PIX_CONVERT_HPP
#define VSTREAMER_CORE_PIX_CONVERT_HPP

#include <cstdint>

namespace vstreamer
{

/* Semi-planar (NV12/NV21 layout) with hor/ver stride → packed NV12 (linesize == width). */
int pack_yuv420sp_to_nv12(const uint8_t *base, int src_w, int src_h, int hor_stride,
                          int ver_stride, uint8_t *dst, int dst_w, int dst_h);

/* Semi-planar NV16/NV61 (full-height UV) → packed NV12. */
int pack_yuv422sp_to_nv12(const uint8_t *base, int src_w, int src_h, int hor_stride,
                          int ver_stride, uint8_t *dst, int dst_w, int dst_h);

/* Planar I420 → packed NV12. */
int pack_yuv420p_to_nv12(const uint8_t *y, int y_stride, const uint8_t *u, int u_stride,
                         const uint8_t *v, int v_stride, int src_w, int src_h, uint8_t *dst,
                         int dst_w, int dst_h);

/* Planar YUV422P → packed NV12 (vertical chroma downsample). */
int pack_yuv422p_to_nv12(const uint8_t *y, int y_stride, const uint8_t *u, int u_stride,
                         const uint8_t *v, int v_stride, int src_w, int src_h, uint8_t *dst,
                         int dst_w, int dst_h);

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_PIX_CONVERT_HPP
