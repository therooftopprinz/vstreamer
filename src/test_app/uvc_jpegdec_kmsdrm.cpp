/*
 * UVC → JPEG decode → H.264 encode → loopback channel → decode → SDL kmsdrm display.
 * Same pipeline as stream_sdl.cpp with VSTREAMER_APP_UVC_JPEGDEC_KMSDRM (defaults: V4L2, kmsdrm).
 * Encoder: --gop N (default fps), VSTREAMER_ENC_GOP; CBR/QP/GOP via UDP console or cbr_controller.py.
 */
#define VSTREAMER_APP_UVC_JPEGDEC_KMSDRM 1
#include "stream_sdl.cpp"
