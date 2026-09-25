cmake_minimum_required(VERSION 3.15)
project(MNZPanel CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(MNZPanel SHARED src/main.cpp)

target_link_libraries(MNZPanel PRIVATE d3d9)

set_target_properties(MNZPanel PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "MNZPanel"
)

if(MSVC)
    target_compile_options(MNZPanel PRIVATE /MT /O2)
endif()
