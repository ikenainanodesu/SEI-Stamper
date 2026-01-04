cmake_minimum_required(VERSION 3.20)
project(obs-sei-stamper VERSION 1.1.1)

# 设置C/C++标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# 查找OBS Studio库
# 需要设置OBS的安装路径
if(NOT DEFINED OBS_DIR)
    set(OBS_DIR "${CMAKE_SOURCE_DIR}/obs-studio-master/build/rundir/Release" CACHE PATH "OBS installation directory")
endif()

# 添加OBS的CMake模块路径
list(APPEND CMAKE_MODULE_PATH "${OBS_DIR}/../cmake")
list(APPEND CMAKE_PREFIX_PATH "${OBS_DIR}")

# 查找libobs
find_package(libobs QUIET)
if(NOT libobs_FOUND)
    # 手动设置libobs路径(如果find_package失败)
    set(LIBOBS_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/obs-studio-master/libobs")
    set(LIBOBS_LIB_DIR "${CMAKE_SOURCE_DIR}/obs-studio-master/build/libobs/Release")
    set(LIBOBS_BIN_DIR "${OBS_DIR}/bin/64bit")
    
    add_library(OBS::libobs SHARED IMPORTED)
    set_target_properties(OBS::libobs PROPERTIES
        IMPORTED_LOCATION "${LIBOBS_BIN_DIR}/obs.dll"
        IMPORTED_IMPLIB "${LIBOBS_LIB_DIR}/obs.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBOBS_INCLUDE_DIR}"
    )
    
    message(STATUS "Using manual libobs configuration")
    message(STATUS "  Include: ${LIBOBS_INCLUDE_DIR}")
    message(STATUS "  Lib: ${LIBOBS_LIB_DIR}/obs.lib")
    message(STATUS "  DLL: ${LIBOBS_BIN_DIR}/obs.dll")
endif()

# 源文件列表
set(PLUGIN_SOURCES
    src/obs-sei-stamper-plugin.c
    src/ntp-client.c
    src/sei-handler.c
    src/sei-stamper-encoder.c
    src/qsv-encoder.c          # Intel VPL Encoder
    src/nvenc-encoder.c        # NVIDIA NVENC Encoder
    src/amd-encoder.c          # AMD AMF Encoder
    src/sei-receiver-source.c  # 重新启用
)

# 创建插件模块
add_library(obs-sei-stamper MODULE
    ${PLUGIN_SOURCES}
)

# 查找pthread库（Windows）
find_library(PTHREAD_LIBRARY NAMES w32-pthreads
    PATHS
    "${CMAKE_SOURCE_DIR}/obs-studio-master/build/deps/w32-pthreads/Release"
    "${CMAKE_SOURCE_DIR}/obs-studio-master/build/deps/w32-pthreads/Debug"
)

if(PTHREAD_LIBRARY)
    message(STATUS "Found pthread library: ${PTHREAD_LIBRARY}")
else()
    message(WARNING "pthread library not found")
endif()

# 查找FFmpeg库
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(FFMPEG QUIET libavcodec libavformat libavutil libswscale)
endif()

# 如果pkg-config找不到，手动设置FFmpeg路径
if(NOT FFMPEG_FOUND)
    # OBS的FFmpeg依赖位于.deps目录
    set(FFMPEG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/obs-studio-master/.deps/obs-deps-2025-08-23-x64/include")
    set(FFMPEG_LIBRARY_DIRS "${CMAKE_SOURCE_DIR}/obs-studio-master/.deps/obs-deps-2025-08-23-x64/bin")
    set(FFMPEG_LIBRARIES avcodec avformat avutil swscale)
    message(STATUS "Using OBS deps FFmpeg configuration")
    message(STATUS "  FFmpeg include: ${FFMPEG_INCLUDE_DIRS}")
endif()

# 查找x264库（用于编码器）
set(X264_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/obs-studio-master/.deps/obs-deps-2025-08-23-x64/include")
set(X264_LIBRARY "${CMAKE_SOURCE_DIR}/obs-studio-master/.deps/obs-deps-2025-08-23-x64/lib/libx264.lib")

if(EXISTS "${X264_LIBRARY}")
    message(STATUS "Found x264 library: ${X264_LIBRARY}")
else()
    message(WARNING "x264 library not found, encoder may not work")
endif()

# 包含目录（在所有变量设置后）
target_include_directories(obs-sei-stamper PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${LIBOBS_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/obs-studio-master/build/config
    ${CMAKE_SOURCE_DIR}/obs-studio-master/deps/w32-pthreads  # pthread头文件
    ${FFMPEG_INCLUDE_DIRS}
)

# 查找SRT库
find_library(SRT_LIBRARY NAMES srt libsrt
    PATHS 
    "${CMAKE_SOURCE_DIR}/srt/_build/Release"
    "${CMAKE_SOURCE_DIR}/obs-studio-master/build/rundir/Release/bin/64bit"
    "${CMAKE_SOURCE_DIR}/obs-studio-master/plugins/obs-outputs"
)

if(SRT_LIBRARY)
    message(STATUS "Found SRT library: ${SRT_LIBRARY}")
    set(SRT_LIBRARIES ${SRT_LIBRARY})
    # SRT头文件
    set(SRT_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/srt/srtcore")
    # 添加SRT头文件路径
    target_include_directories(obs-sei-stamper PRIVATE ${SRT_INCLUDE_DIRS})
else()
    message(WARNING "SRT library not found, receiver functionality will be limited")
    set(SRT_LIBRARIES "")  # 设为空而不是NOTFOUND
    set(SRT_INCLUDE_DIRS "")
endif()

# 链接库
target_link_libraries(obs-sei-stamper
    OBS::libobs
    ws2_32  # Windows Socket库(NTP客户端需要)
    ${PTHREAD_LIBRARY}  # pthread库
)

# 添加FFmpeg库（使用完整路径）
set(FFMPEG_LIB_DIR "${CMAKE_SOURCE_DIR}/obs-studio-master/.deps/obs-deps-2025-08-23-x64/lib")
target_link_libraries(obs-sei-stamper
    "${FFMPEG_LIB_DIR}/avcodec.lib"
    "${FFMPEG_LIB_DIR}/avformat.lib"
    "${FFMPEG_LIB_DIR}/avutil.lib"
    "${FFMPEG_LIB_DIR}/swscale.lib"
)

# 添加x264库
target_link_libraries(obs-sei-stamper
    "${X264_LIBRARY}"
)

# 添加Intel VPL库
set(VPL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/obs-studio-master/.deps/obs-deps-2025-08-23-x64/include")
set(VPL_LIBRARY "${CMAKE_SOURCE_DIR}/obs-studio-master/.deps/obs-deps-2025-08-23-x64/lib/vpl.lib")

if(EXISTS "${VPL_LIBRARY}")
    message(STATUS "Found VPL library: ${VPL_LIBRARY}")
    target_include_directories(obs-sei-stamper PRIVATE "${VPL_INCLUDE_DIR}")
    target_link_libraries(obs-sei-stamper "${VPL_LIBRARY}")
    add_definitions(-DENABLE_VPL)
else()
    message(WARNING "VPL library not found, Intel QuickSync (Native) will be disabled")
endif()

# 启用 NVENC 和 AMD 编码器 (通过 FFmpeg，无需额外库)
add_definitions(-DENABLE_NVENC)
add_definitions(-DENABLE_AMD)
message(STATUS "Enabled NVENC and AMD encoders (via FFmpeg)")


# 如果找到SRT库，才链接
if(SRT_LIBRARY)
    target_link_libraries(obs-sei-stamper ${SRT_LIBRARIES})
endif()

# 设置输出目录
set_target_properties(obs-sei-stamper PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin"
)

# 安装规则
install(TARGETS obs-sei-stamper
    LIBRARY DESTINATION obs-plugins/64bit
    RUNTIME DESTINATION obs-plugins/64bit
)

# 安装数据文件(本地化等)
install(DIRECTORY data/
    DESTINATION data/obs-plugins/obs-sei-stamper
)

message(STATUS "=================================================")
message(STATUS "OBS SEI Stamper Plugin Configuration")
message(STATUS "=================================================")
message(STATUS "OBS Directory: ${OBS_DIR}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "=================================================")
