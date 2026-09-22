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

#ifdef ENABLE_H264_ENCODER_CEDAR
#include "components/h264_encoder_cedar.hpp"
#endif

#ifdef ENABLE_H264_ENCODER_INTEL
#include "components/h264_encoder_intel.hpp"
#endif

#ifdef ENABLE_MKV_SINK
#include "components/mkv_sink.hpp"
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

    return nullptr;
}

std::unique_ptr<component_coder> component_factory::create_coder(std::string name)
{
#ifdef ENABLE_JPEG_DECODER_MULTICORE
    if ("jpeg_decoder_multicore" == name)
    {
        return std::make_unique<jpeg_decoder_multicore>();
    }
#endif

#ifdef ENABLE_H264_DECODER_MPP
    if ("h264_decoder_mpp" == name)
    {
        return std::make_unique<h264_decoder_mpp>();
    }
#endif

#ifdef ENABLE_H264_ENCODER_CEDAR
    if ("h264_encoder_cedar" == name)
    {
        return std::make_unique<h264_encoder_cedar>();
    }
#endif

#ifdef ENABLE_H264_ENCODER_INTEL
    if ("h264_encoder_intel" == name)
    {
        return std::make_unique<h264_encoder_intel>();
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

    return nullptr;
}

}  // namespace vstreamer
