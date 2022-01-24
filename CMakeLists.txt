cmake_minimum_required (VERSION 3.11)

#set(CMAKE_BUILD_TYPE Release)
# transcoder sources
project(AvCppSamples LANGUAGES CXX VERSION 2.0.99)

# Add path to find FFmpeg
set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake ${CMAKE_MODULE_PATH})

set(FFMPEG_PKG_CONFIG_SUFFIX "" CACHE STRING "This suffix uses for FFmpeg component names searches by pkg-config")
set(AV_ENABLE_STATIC On CACHE BOOL "Enable static library build (On)")
set(AV_ENABLE_SHARED On CACHE BOOL "Enable shared library build (On)")
set(AV_BUILD_EXAMPLES On CACHE BOOL "Build example applications (On)")

# Compiler-specific C++11 activation.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED yes)

find_package(Threads)
find_package(FFmpeg
        COMPONENTS AVCODEC AVFORMAT AVUTIL AVDEVICE AVFILTER SWSCALE SWRESAMPLE REQUIRED)
find_package(OpenCV REQUIRED)

add_executable(${PROJECT_NAME} main.cpp
        src/video_reader.cpp
        src/curl_reader.cpp
        include/data_types.h
        include/video_reader.h
        include/curl_reader.h)

include_directories(${OpenCV_INCLUDE_DIRS}, ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(${PROJECT_NAME} PUBLIC
        Threads::Threads
        FFmpeg
        -lcurl
        ${OpenCV_LIBS})
