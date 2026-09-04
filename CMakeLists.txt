cmake_minimum_required(VERSION 3.20)
project(sei-stamper VERSION 1.3.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# --- OBS Studio SDK paths ---
# Set these via -D flags or environment variables of the same name:
#   OBS_SOURCE_DIR  = OBS Studio source tree (e.g. C:/obs-studio)
#   OBS_BUILD_DIR   = OBS build output (default: ${OBS_SOURCE_DIR}/build)
#   OBS_DEPS_DIR    = OBS deps bundle (default: the newest obs-deps-*/windows-deps-*
#                     under ${OBS_SOURCE_DIR}/.deps)

if(NOT DEFINED OBS_SOURCE_DIR)
    if(DEFINED ENV{OBS_SOURCE_DIR})
        set(OBS_SOURCE_DIR "$ENV{OBS_SOURCE_DIR}")
    else()
        message(FATAL_ERROR "OBS_SOURCE_DIR not set. Point it to the OBS Studio source tree.")
    endif()
endif()

if(NOT DEFINED OBS_BUILD_DIR)
    if(DEFINED ENV{OBS_BUILD_DIR})
        set(OBS_BUILD_DIR "$ENV{OBS_BUILD_DIR}")
    else()
        set(OBS_BUILD_DIR "${OBS_SOURCE_DIR}/build")
    endif()
endif()

if(NOT DEFINED OBS_DEPS_DIR)
    if(DEFINED ENV{OBS_DEPS_DIR})
        set(OBS_DEPS_DIR "$ENV{OBS_DEPS_DIR}")
    else()
        # The date-shaped glob cannot match the separate qt6 bundle (obs-deps-qt6-<date>).
        file(GLOB _OBS_DEPS_CANDIDATES "${OBS_SOURCE_DIR}/.deps/obs-deps-20??-??-??-x64"
                                       "${OBS_SOURCE_DIR}/.deps/windows-deps-20??-??-??-x64")
        list(SORT _OBS_DEPS_CANDIDATES)
        list(REVERSE _OBS_DEPS_CANDIDATES)
        if(_OBS_DEPS_CANDIDATES)
            list(GET _OBS_DEPS_CANDIDATES 0 OBS_DEPS_DIR)
        else()
            message(FATAL_ERROR "OBS_DEPS_DIR not set and no obs-deps-*/windows-deps-* bundle found under ${OBS_SOURCE_DIR}/.deps.")
        endif()
    endif()
endif()

message(STATUS "OBS_SOURCE_DIR: ${OBS_SOURCE_DIR}")
message(STATUS "OBS_BUILD_DIR:  ${OBS_BUILD_DIR}")
message(STATUS "OBS_DEPS_DIR:   ${OBS_DEPS_DIR}")

# --- Find libobs ---
set(LIBOBS_INCLUDE_DIR "${OBS_SOURCE_DIR}/libobs")
set(LIBOBS_LIB_DIR "${OBS_BUILD_DIR}/libobs/Release")
set(LIBOBS_BIN_DIR "${OBS_BUILD_DIR}/rundir/Release/bin/64bit")

add_library(OBS::libobs SHARED IMPORTED)
set_target_properties(OBS::libobs PROPERTIES
    IMPORTED_LOCATION "${LIBOBS_BIN_DIR}/obs.dll"
    IMPORTED_IMPLIB "${LIBOBS_LIB_DIR}/obs.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBOBS_INCLUDE_DIR}"
)

message(STATUS "libobs include: ${LIBOBS_INCLUDE_DIR}")
message(STATUS "libobs lib:     ${LIBOBS_LIB_DIR}/obs.lib")

# --- Source files ---
set(PLUGIN_SOURCES
    src/sei-stamper-plugin.c
    src/ntp-client.c
    src/sei-handler.c
    src/sei-stamper-encoder.c
    src/unified-encoder.c
    src/qsv-encoder.c
    src/nvenc-encoder.c
    src/amd-encoder.c
    src/sei-receiver-source.c
)

add_library(sei-stamper MODULE ${PLUGIN_SOURCES})

# --- Include directories ---
target_include_directories(sei-stamper PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${LIBOBS_INCLUDE_DIR}
    ${OBS_BUILD_DIR}/config
    ${OBS_SOURCE_DIR}/deps/w32-pthreads
    ${OBS_DEPS_DIR}/include
)

# --- pthread (Windows) ---
find_library(PTHREAD_LIBRARY NAMES w32-pthreads
    PATHS
    "${OBS_BUILD_DIR}/deps/w32-pthreads/Release"
    "${OBS_BUILD_DIR}/deps/w32-pthreads/Debug"
)
if(PTHREAD_LIBRARY)
    message(STATUS "Found pthread: ${PTHREAD_LIBRARY}")
else()
    message(WARNING "pthread library not found")
endif()

# --- FFmpeg (from OBS deps) ---
# The DLL hard-links the bundle's FFmpeg majors and loads only in an OBS that
# ships them: OBS 32.2 needs libavcodec 62 (FFmpeg 8, deps >= 2026-05-21).
set(AVCODEC_VERSION_HEADER "${OBS_DEPS_DIR}/include/libavcodec/version_major.h")
if(NOT EXISTS "${AVCODEC_VERSION_HEADER}")
    message(FATAL_ERROR "No libavcodec headers under ${OBS_DEPS_DIR}/include; OBS_DEPS_DIR must point at an OBS deps bundle.")
endif()
file(STRINGS "${AVCODEC_VERSION_HEADER}" _AVCODEC_MAJOR_LINE REGEX "^#define LIBAVCODEC_VERSION_MAJOR")
string(REGEX REPLACE "^#define LIBAVCODEC_VERSION_MAJOR +([0-9]+).*$" "\\1" AVCODEC_VERSION_MAJOR "${_AVCODEC_MAJOR_LINE}")
if(NOT AVCODEC_VERSION_MAJOR MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Could not read LIBAVCODEC_VERSION_MAJOR from ${AVCODEC_VERSION_HEADER}")
endif()
if(AVCODEC_VERSION_MAJOR LESS 62)
    message(FATAL_ERROR "OBS deps at ${OBS_DEPS_DIR} ship libavcodec ${AVCODEC_VERSION_MAJOR}; this plugin needs FFmpeg 8 (libavcodec 62). Use windows-deps-2026-05-21-x64 or newer, the OBS 32.2 bundles.")
endif()
message(STATUS "libavcodec:     ${AVCODEC_VERSION_MAJOR}")

set(FFMPEG_LIB_DIR "${OBS_DEPS_DIR}/lib")

target_link_libraries(sei-stamper
    OBS::libobs
    ws2_32
    ${PTHREAD_LIBRARY}
    "${FFMPEG_LIB_DIR}/avcodec.lib"
    "${FFMPEG_LIB_DIR}/avformat.lib"
    "${FFMPEG_LIB_DIR}/avutil.lib"
    "${FFMPEG_LIB_DIR}/swscale.lib"
)

# --- x264 ---
set(X264_LIBRARY "${OBS_DEPS_DIR}/lib/libx264.lib")
if(EXISTS "${X264_LIBRARY}")
    message(STATUS "Found x264: ${X264_LIBRARY}")
    target_link_libraries(sei-stamper "${X264_LIBRARY}")
else()
    message(WARNING "x264 not found, software encoder may not work")
endif()

# --- Intel VPL (QuickSync) ---
set(VPL_LIBRARY "${OBS_DEPS_DIR}/lib/vpl.lib")
if(EXISTS "${VPL_LIBRARY}")
    message(STATUS "Found VPL: ${VPL_LIBRARY}")
    target_link_libraries(sei-stamper "${VPL_LIBRARY}")
    add_definitions(-DENABLE_VPL)
else()
    message(WARNING "VPL not found, Intel QuickSync disabled")
endif()

# --- NVENC and AMD (via FFmpeg, no extra libs needed) ---
add_definitions(-DENABLE_NVENC)
add_definitions(-DENABLE_AMD)
message(STATUS "Enabled NVENC and AMD encoders (via FFmpeg)")

# --- SRT (optional, for the receiver) ---
find_library(SRT_LIBRARY NAMES srt libsrt
    PATHS
    "${CMAKE_SOURCE_DIR}/srt/_build/Release"
    "${OBS_BUILD_DIR}/rundir/Release/bin/64bit"
    "${OBS_SOURCE_DIR}/plugins/obs-outputs"
)
if(SRT_LIBRARY)
    message(STATUS "Found SRT: ${SRT_LIBRARY}")
    target_link_libraries(sei-stamper ${SRT_LIBRARY})
    if(EXISTS "${CMAKE_SOURCE_DIR}/srt/srtcore")
        target_include_directories(sei-stamper PRIVATE "${CMAKE_SOURCE_DIR}/srt/srtcore")
    endif()
else()
    message(WARNING "SRT library not found, receiver functionality will be limited")
endif()

# --- Output ---
set_target_properties(sei-stamper PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin"
)

install(TARGETS sei-stamper
    LIBRARY DESTINATION obs-plugins/64bit
    RUNTIME DESTINATION obs-plugins/64bit
)

install(DIRECTORY data/
    DESTINATION data/obs-plugins/sei-stamper
)

message(STATUS "=================================================")
message(STATUS "SEI Stamper Plugin v${PROJECT_VERSION}")
message(STATUS "=================================================")
