cmake_minimum_required(VERSION 3.16)
project(vstreamer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_NOISE_SOURCE "Build noise_source component" ON)
option(ENABLE_V4L2_SOURCE "Build v4l2_source component" ON)
option(ENABLE_JPEG_DECODER_MULTICORE "Build jpeg_decoder_multicore component" ON)
option(ENABLE_H264_DECODER_MPP "Build h264_decoder_mpp component" ON)
option(ENABLE_H264_ENCODER_MPP "Build h264_encoder_mpp component (Rockchip)" OFF)
option(ENABLE_H264_ENCODER_CEDAR "Build h264_encoder_cedar component" OFF)
option(ENABLE_H264_ENCODER_INTEL "Build h264_encoder_intel component" OFF)
option(ENABLE_MKV_SINK "Build mkv_sink component" ON)
option(ENABLE_SDL_SINK "Build sdl_sink display component" OFF)
option(ENABLE_STREAM_SENDER "Build stream_sender component" ON)
option(ENABLE_STREAM_RECEIVER "Build stream_receiver component" ON)
option(ENABLE_ENCODER_CBR_LOGIC "Build encoder_cbr_logic (stream_sender pad 1)" ON)
option(ENABLE_RTP_H264_PAY "Build rtp_h264_pay component" ON)
option(ENABLE_RTP_H264_DEPAY "Build rtp_h264_depay component" ON)

if(ENABLE_V4L2_SOURCE AND NOT ENABLE_NOISE_SOURCE)
    message(FATAL_ERROR "ENABLE_V4L2_SOURCE requires ENABLE_NOISE_SOURCE")
endif()

set(VSTREAMER_SOURCES
    src/core/frame.cpp
    src/core/metrics.cpp
    src/core/data_packet.cpp
    src/core/packet_types.cpp
    src/core/packet_pool.cpp
    src/core/component_sink.cpp
    src/core/rtp_h264.cpp
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
if(ENABLE_H264_ENCODER_MPP)
    list(APPEND VSTREAMER_SOURCES src/components/h264_encoder_mpp.cpp)
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
if(ENABLE_SDL_SINK)
    list(APPEND VSTREAMER_SOURCES
        src/components/sdl_nv12_presenter.cpp
        src/components/sdl_sink.cpp
        src/components/sdl_dmks_sink.cpp
    )
endif()
if(ENABLE_STREAM_SENDER)
    list(APPEND VSTREAMER_SOURCES src/components/stream_sender.cpp)
endif()
if(ENABLE_STREAM_RECEIVER)
    list(APPEND VSTREAMER_SOURCES src/components/stream_receiver.cpp)
endif()
if(ENABLE_ENCODER_CBR_LOGIC)
    list(APPEND VSTREAMER_SOURCES src/components/encoder_cbr_logic.cpp)
endif()
if(ENABLE_RTP_H264_PAY)
    list(APPEND VSTREAMER_SOURCES src/components/rtp_h264_pay.cpp)
endif()
if(ENABLE_RTP_H264_DEPAY)
    list(APPEND VSTREAMER_SOURCES src/components/rtp_h264_depay.cpp)
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
if(ENABLE_SDL_SINK)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    list(APPEND VSTREAMER_LIBS PkgConfig::SDL2)
endif()
if(ENABLE_H264_DECODER_MPP OR ENABLE_H264_ENCODER_MPP)
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

if(ENABLE_ENCODER_CBR_LOGIC)
    add_executable(cbr_pid_test src/test_app/cbr_pid_test.cpp)
    target_include_directories(cbr_pid_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    target_link_libraries(cbr_pid_test PRIVATE vstreamer_source)
    target_compile_options(cbr_pid_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

message(STATUS "vstreamer components:")
message(STATUS "  ENABLE_NOISE_SOURCE=${ENABLE_NOISE_SOURCE}")
message(STATUS "  ENABLE_V4L2_SOURCE=${ENABLE_V4L2_SOURCE}")
message(STATUS "  ENABLE_JPEG_DECODER_MULTICORE=${ENABLE_JPEG_DECODER_MULTICORE}")
message(STATUS "  ENABLE_H264_DECODER_MPP=${ENABLE_H264_DECODER_MPP}")
message(STATUS "  ENABLE_H264_ENCODER_MPP=${ENABLE_H264_ENCODER_MPP}")
message(STATUS "  ENABLE_H264_ENCODER_CEDAR=${ENABLE_H264_ENCODER_CEDAR}")
message(STATUS "  ENABLE_H264_ENCODER_INTEL=${ENABLE_H264_ENCODER_INTEL}")
message(STATUS "  ENABLE_MKV_SINK=${ENABLE_MKV_SINK}")
message(STATUS "  ENABLE_SDL_SINK=${ENABLE_SDL_SINK}")
message(STATUS "  ENABLE_STREAM_SENDER=${ENABLE_STREAM_SENDER}")
message(STATUS "  ENABLE_STREAM_RECEIVER=${ENABLE_STREAM_RECEIVER}")
message(STATUS "  ENABLE_ENCODER_CBR_LOGIC=${ENABLE_ENCODER_CBR_LOGIC}")
message(STATUS "  ENABLE_RTP_H264_PAY=${ENABLE_RTP_H264_PAY}")
message(STATUS "  ENABLE_RTP_H264_DEPAY=${ENABLE_RTP_H264_DEPAY}")

option(ENABLE_TEST_STREAM_SDL "Build stream_sdl loopback test" OFF)
option(ENABLE_TEST_NOISE_STREAM_SDL "Deprecated alias for ENABLE_TEST_STREAM_SDL" OFF)
if(ENABLE_TEST_NOISE_STREAM_SDL)
    set(ENABLE_TEST_STREAM_SDL ON)
endif()

if(ENABLE_TEST_STREAM_SDL)
    if(NOT ENABLE_NOISE_SOURCE OR NOT ENABLE_JPEG_DECODER_MULTICORE OR NOT ENABLE_SDL_SINK OR
       NOT ENABLE_STREAM_SENDER OR NOT ENABLE_STREAM_RECEIVER OR NOT ENABLE_RTP_H264_PAY OR
       NOT ENABLE_RTP_H264_DEPAY OR NOT ENABLE_H264_DECODER_MPP OR NOT ENABLE_ENCODER_CBR_LOGIC)
        message(FATAL_ERROR "ENABLE_TEST_STREAM_SDL requires noise, jpeg_decoder, stream_*, rtp, mpp decode, sdl, encoder_cbr_logic")
    endif()
    if(NOT ENABLE_H264_ENCODER_MPP AND NOT ENABLE_H264_ENCODER_CEDAR AND NOT ENABLE_H264_ENCODER_INTEL)
        message(FATAL_ERROR "ENABLE_TEST_STREAM_SDL requires ENABLE_H264_ENCODER_MPP, CEDAR, or INTEL")
    endif()
    add_executable(stream_sdl
        src/test_app/stream_sdl.cpp
        src/test_app/channel_controller.cpp
    )
    target_include_directories(stream_sdl PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    target_link_libraries(stream_sdl PRIVATE vstreamer_source)
    target_compile_options(stream_sdl PRIVATE -Wall -Wextra -Wpedantic)
endif()
