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

#ifdef ENABLE_H264_ENCODER_CEDAR
#include "components/h264_encoder_cedar.hpp"
#endif

#ifdef ENABLE_H264_ENCODER_INTEL
#include "components/h264_encoder_intel.hpp"
#endif

#ifdef ENABLE_MKV_SINK
#include "components/mkv_sink.hpp"
#endif

#endif  // VSTREAMER_COMPONENTS_HPP
