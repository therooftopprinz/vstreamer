cmake_minimum_required(VERSION 3.16)
project(vstreamer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_NOISE_SOURCE "Build noise_source component" ON)
option(ENABLE_V4L2_SOURCE "Build v4l2_source component" ON)
option(ENABLE_JPEG_DECODER_MULTICORE "Build jpeg_decoder_multicore component" ON)
option(ENABLE_H264_DECODER_MPP "Build h264_decoder_mpp component" ON)
option(ENABLE_H264_ENCODER_CEDAR "Build h264_encoder_cedar component" OFF)
option(ENABLE_H264_ENCODER_INTEL "Build h264_encoder_intel component" OFF)
option(ENABLE_MKV_SINK "Build mkv_sink component" ON)

if(ENABLE_V4L2_SOURCE AND NOT ENABLE_NOISE_SOURCE)
    message(FATAL_ERROR "ENABLE_V4L2_SOURCE requires ENABLE_NOISE_SOURCE")
endif()

set(VSTREAMER_SOURCES
    src/core/frame.cpp
    src/core/pix_convert.cpp
    src/core/component_factory.cpp
)

if(ENABLE_NOISE_SOURCE)
    list(APPEND VSTREAMER_SOURCES src/components/noise_source.cpp)
endif()
if(ENABLE_V4L2_SOURCE)
    list(APPEND VSTREAMER_SOURCES src/components/v4l2_source.cpp)
endif()
if(ENABLE_JPEG_DECODER_MULTICORE)
    list(APPEND VSTREAMER_SOURCES src/components/jpeg_decoder_multicore.cpp)
endif()
if(ENABLE_H264_DECODER_MPP)
    list(APPEND VSTREAMER_SOURCES src/components/h264_decoder_mpp.cpp)
endif()
if(ENABLE_H264_ENCODER_CEDAR)
    list(APPEND VSTREAMER_SOURCES src/components/h264_encoder_cedar.cpp)
endif()
if(ENABLE_H264_ENCODER_INTEL)
    list(APPEND VSTREAMER_SOURCES src/components/h264_encoder_intel.cpp)
endif()
if(ENABLE_MKV_SINK)
    list(APPEND VSTREAMER_SOURCES src/components/mkv_sink.cpp)
endif()

find_package(PkgConfig REQUIRED)

set(VSTREAMER_LIBS)
if(ENABLE_NOISE_SOURCE OR ENABLE_JPEG_DECODER_MULTICORE OR ENABLE_H264_ENCODER_CEDAR OR ENABLE_H264_ENCODER_INTEL OR ENABLE_MKV_SINK)
    pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavcodec libavutil)
    list(APPEND VSTREAMER_LIBS PkgConfig::LIBAV)
endif()
if(ENABLE_MKV_SINK)
    pkg_check_modules(LIBAVFORMAT REQUIRED IMPORTED_TARGET libavformat)
    list(APPEND VSTREAMER_LIBS PkgConfig::LIBAVFORMAT)
endif()
if(ENABLE_H264_DECODER_MPP)
    pkg_check_modules(MPP REQUIRED IMPORTED_TARGET rockchip_mpp)
    list(APPEND VSTREAMER_LIBS PkgConfig::MPP)
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/components/components_config.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/components/components_config.hpp
    @ONLY
)

add_library(vstreamer_source ${VSTREAMER_SOURCES})
target_include_directories(vstreamer_source PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
if(VSTREAMER_LIBS)
    target_link_libraries(vstreamer_source PUBLIC ${VSTREAMER_LIBS})
endif()
target_compile_options(vstreamer_source PRIVATE -Wall -Wextra -Wpedantic)

message(STATUS "vstreamer components:")
message(STATUS "  ENABLE_NOISE_SOURCE=${ENABLE_NOISE_SOURCE}")
message(STATUS "  ENABLE_V4L2_SOURCE=${ENABLE_V4L2_SOURCE}")
message(STATUS "  ENABLE_JPEG_DECODER_MULTICORE=${ENABLE_JPEG_DECODER_MULTICORE}")
message(STATUS "  ENABLE_H264_DECODER_MPP=${ENABLE_H264_DECODER_MPP}")
message(STATUS "  ENABLE_H264_ENCODER_CEDAR=${ENABLE_H264_ENCODER_CEDAR}")
message(STATUS "  ENABLE_H264_ENCODER_INTEL=${ENABLE_H264_ENCODER_INTEL}")
message(STATUS "  ENABLE_MKV_SINK=${ENABLE_MKV_SINK}")
