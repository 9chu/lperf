file(GLOB_RECURSE SOURCE_FILES ./*.c)
add_library(procmapsparser STATIC ${SOURCE_FILES})
target_include_directories(procmapsparser PUBLIC ./)
