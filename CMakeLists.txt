cmake_minimum_required (VERSION 3.11)

# transcoder sources
project(AvCppSamples LANGUAGES CXX VERSION 2.0.99)

find_package(avcpp PATHS /home/local/BM/cpplibs NO_DEFAULT_PATH)
find_package(OpenCV REQUIRED)

add_executable(${PROJECT_NAME} main.cpp)

include_directories(${OpenCV_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} avcpp::avcpp ${OpenCV_LIBS})
