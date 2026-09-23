#include "core/component_factory.hpp"

#include "components/components_config.hpp"

#include "core/component_coder.hpp"
#include "core/component_sink.hpp"
#include "core/component_source.hpp"

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

#ifdef ENABLE_RTP_H264_PAY
#include "components/rtp_h264_pay.hpp"
#endif

#ifdef ENABLE_RTP_H264_DEPAY
#include "components/rtp_h264_depay.hpp"
#endif

namespace vstreamer
{

std::unique_ptr<component_source> component_factory::create_source(std::string name)
{
#ifdef ENABLE_NOISE_SOURCE
    if ("noise" == name || "noise_source" == name)
    {
        return std::make_unique<noise_source>();
    }
#endif

#ifdef ENABLE_V4L2_SOURCE
    if ("v4l2" == name || "v4l2_source" == name)
    {
        return std::make_unique<v4l2_source>();
    }
#endif

#ifdef ENABLE_STREAM_RECEIVER
    if ("stream_receiver" == name || "stream" == name || "stream_source" == name)
    {
        return std::make_unique<stream_receiver>();
    }
#endif

    return nullptr;
}

std::unique_ptr<component_coder> component_factory::create_coder(std::string name)
{
#ifdef ENABLE_JPEG_DECODER_MULTICORE
    if ("jpeg_decoder_multicore" == name || "jpeg_decoder" == name)
    {
        return std::make_unique<jpeg_decoder_multicore>();
    }
#endif

#ifdef ENABLE_H264_DECODER_MPP
    if ("h264_decoder_mpp" == name || "h264_decoder" == name)
    {
        return std::make_unique<h264_decoder_mpp>();
    }
#endif

#ifdef ENABLE_H264_ENCODER_MPP
    if ("h264_encoder_mpp" == name || "h264_encoder" == name)
    {
        return std::make_unique<h264_encoder_mpp>();
    }
#endif

#ifdef ENABLE_H264_ENCODER_CEDAR
    if ("h264_encoder_cedar" == name
#ifndef ENABLE_H264_ENCODER_MPP
        || "h264_encoder" == name
#endif
    )
    {
        return std::make_unique<h264_encoder_cedar>();
    }
#endif

#ifdef ENABLE_H264_ENCODER_INTEL
    if ("h264_encoder_intel" == name
#ifndef ENABLE_H264_ENCODER_MPP
#ifndef ENABLE_H264_ENCODER_CEDAR
        || "h264_encoder" == name
#endif
#endif
    )
    {
        return std::make_unique<h264_encoder_intel>();
    }
#endif

#ifdef ENABLE_RTP_H264_PAY
    if ("rtp_h264_pay" == name || "rtp_pay" == name)
    {
        return std::make_unique<rtp_h264_pay>();
    }
#endif

#ifdef ENABLE_RTP_H264_DEPAY
    if ("rtp_h264_depay" == name || "rtp_depay" == name)
    {
        return std::make_unique<rtp_h264_depay>();
    }
#endif

    return nullptr;
}

std::unique_ptr<component_sink> component_factory::create_sink(std::string name)
{
#ifdef ENABLE_MKV_SINK
    if ("mkv" == name || "mkv_sink" == name)
    {
        return std::make_unique<mkv_sink>();
    }
#endif

#ifdef ENABLE_STREAM_SENDER
    if ("stream_sender" == name || "stream_sink" == name)
    {
        return std::make_unique<stream_sender>();
    }
#endif

#ifdef ENABLE_SDL_SINK
    if ("sdl" == name || "sdl_sink" == name || "display" == name)
    {
        return std::make_unique<sdl_sink>();
    }
    if ("sdl_dmks" == name || "sdl_dmks_sink" == name || "dmks" == name || "dmks_sink" == name)
    {
        return std::make_unique<sdl_dmks_sink>();
    }
#endif

    return nullptr;
}

}  // namespace vstreamer
