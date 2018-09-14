set(CMAKE_CXX_STANDARD 11)

file(GLOB_RECURSE SOURCE_FILES dwarf/*.cc elf/*.cc)
add_library(elfin STATIC ${SOURCE_FILES})
target_include_directories(elfin PUBLIC dwarf)
target_include_directories(elfin PUBLIC elf)
