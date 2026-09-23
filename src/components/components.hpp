#ifndef VSTREAMER_COMPONENTS_HPP
#define VSTREAMER_COMPONENTS_HPP

#include "components/components_config.hpp"

#ifdef ENABLE_NOISE_SOURCE
#include "components/noise_source.hpp"
#endif

#ifdef ENABLE_V4L2_SOURCE
#include "components/v4l2_source.hpp"
#endif

#ifdef ENABLE_JPEG_DECODER_MULTICORE
#include "components/jpeg_decoder_multicore.hpp"
#endif

#ifdef ENABLE_H264_DECODER_MPP
#include "components/h264_decoder_mpp.hpp"
#endif

#ifdef ENABLE_H264_ENCODER_MPP
#include "components/h264_encoder_mpp.hpp"
#endif

#ifdef ENABLE_H264_ENCODER_CEDAR
#include "components/h264_encoder_cedar.hpp"
#endif

#ifdef ENABLE_H264_ENCODER_INTEL
#include "components/h264_encoder_intel.hpp"
#endif

#ifdef ENABLE_MKV_SINK
#include "components/mkv_sink.hpp"
#endif

#ifdef ENABLE_SDL_SINK
#include "components/sdl_dmks_sink.hpp"
#include "components/sdl_sink.hpp"
#endif

#ifdef ENABLE_STREAM_SENDER
#include "components/stream_sender.hpp"
#endif

#ifdef ENABLE_STREAM_RECEIVER
#include "components/stream_receiver.hpp"
#endif

#ifdef ENABLE_ENCODER_CBR_LOGIC
#include "components/encoder_cbr_logic.hpp"
#endif

#ifdef ENABLE_RTP_H264_PAY
#include "components/rtp_h264_pay.hpp"
#endif

#ifdef ENABLE_RTP_H264_DEPAY
#include "components/rtp_h264_depay.hpp"
#endif

#endif  // VSTREAMER_COMPONENTS_HPP
