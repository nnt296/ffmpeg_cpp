cmake_minimum_required(VERSION 3.11)

set(CMAKE_BUILD_TYPE Release)
if (POLICY CMP0011)
    cmake_policy(SET CMP0011 NEW)
endif ()

#**************************************************************************************************
# General CMake settings
#**************************************************************************************************
project(av_pybind11)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED yes)

#**************************************************************************************************
# Find Package **************************************************************************************************
# Add path to find FFmpeg
set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/../cmake ${CMAKE_MODULE_PATH})

set(FFMPEG_PKG_CONFIG_SUFFIX "" CACHE STRING "This suffix uses for FFmpeg component names searches by pkg-config")
set(AV_ENABLE_STATIC On CACHE BOOL "Enable static library build (On)")
set(AV_ENABLE_SHARED On CACHE BOOL "Enable shared library build (On)")
set(AV_BUILD_EXAMPLES On CACHE BOOL "Build example applications (On)")

find_package(Threads)
find_package(FFmpeg
        COMPONENTS AVCODEC AVFORMAT AVUTIL AVDEVICE AVFILTER SWSCALE SWRESAMPLE REQUIRED)
find_package(OpenCV REQUIRED)

#find_package(Python COMPONENTS Interpreter Development REQUIRED)
#find_package(pybind11 CONFIG REQUIRED)
add_subdirectory(pybind11)
MESSAGE(" *** PYTHON_INCLUDE_DIRS : " ${PYTHON_INCLUDE_DIRS})
MESSAGE(" *** PYTHON_LIBRARIES : " ${PYTHON_LIBRARIES})
MESSAGE(" *** pybind11_INCLUDE_DIR : " ${pybind11_INCLUDE_DIR})

set(NUMPY_INCLUDE_DIR "/home/local/miniconda3/envs/gen_thumbnail/lib/python3.8/site-packages/numpy/core/include")
include_directories(${NUMPY_INCLUDE_DIR})

#**************************************************************************************************
# Include **************************************************************************************************
include_directories(${pybind11_INCLUDE_DIR})
include_directories(${PYTHON_INCLUDE_DIRS})
include_directories(${OpenCV_INCLUDE_DIRS})
include_directories(${OpenCV_INCLUDE_DIRS}/opencv4)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)

#**************************************************************************************************
# Set variable **************************************************************************************************
SET(SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/ndarray_converter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/curl_reader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/video_reader.cpp
        )

#**************************************************************************************************
# Make configuration
#**************************************************************************************************
pybind11_add_module(av_reader_module MODULE ${SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
target_link_libraries(av_reader_module PRIVATE
        Threads::Threads
        FFmpeg
        -lcurl
        ${OpenCV_LIBS})
