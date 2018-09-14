cmake_minimum_required(VERSION 3.1)
project(lperf)

set(CMAKE_CXX_STANDARD 11)

add_subdirectory(3rd/Moe.Core)
add_subdirectory(3rd/libelfin)
add_subdirectory(3rd/proc_maps_parser)

include_directories(./include)
file(GLOB_RECURSE SOURCE_FILES src/*.cpp include/*.hpp)
add_executable(lperf ${SOURCE_FILES})
target_link_libraries(lperf MoeCore elfin procmapsparser rt)
