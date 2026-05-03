cmake_minimum_required(VERSION 3.16)
project(audio-meet-server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_ZLIB OFF CACHE BOOL "" FORCE)

set(SOURCES
    src/main.cpp
    src/server.cpp
    src/meeting_manager.cpp
    src/meeting.cpp
    src/participant.cpp
    src/rtp_packet.cpp
    src/port_allocator.cpp
    src/pjmedia_ctx.cpp
)

add_executable(audio-meet-server ${SOURCES})

target_include_directories(audio-meet-server PRIVATE
    src
    third_party/include
    /usr/local/include
)

target_link_directories(audio-meet-server PRIVATE /usr/local/lib)

# PJSIP static libraries (target-triple suffix: x86_64-pc-linux-gnu)
set(PJSIP_LIBS
    pjmedia-audiodev-x86_64-pc-linux-gnu
    pjmedia-x86_64-pc-linux-gnu
    pjlib-util-x86_64-pc-linux-gnu
    pj-x86_64-pc-linux-gnu
    # Private PJSIP dependencies
    srtp-x86_64-pc-linux-gnu
    speex-x86_64-pc-linux-gnu
    webrtc-x86_64-pc-linux-gnu
    resample-x86_64-pc-linux-gnu
    gsmcodec-x86_64-pc-linux-gnu
    ilbccodec-x86_64-pc-linux-gnu
    g7221codec-x86_64-pc-linux-gnu
    yuv-x86_64-pc-linux-gnu
)

target_link_libraries(audio-meet-server
    PRIVATE
    ${PJSIP_LIBS}
    pthread
    m
    rt
    asound
    ssl
    crypto
    uuid
)

target_compile_definitions(audio-meet-server PRIVATE
    PJ_AUTOCONF=1
    PJ_IS_BIG_ENDIAN=0
    PJ_IS_LITTLE_ENDIAN=1
)

target_compile_options(audio-meet-server PRIVATE -Wall -Wextra)

target_compile_options(audio-meet-server PRIVATE -Wall -Wextra -Wpedantic)
