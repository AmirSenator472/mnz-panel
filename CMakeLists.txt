cmake_minimum_required(VERSION 3.15)
project(MNZPanel CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/imgui)

add_library(MNZPanel SHARED
    src/main.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx9.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)

target_include_directories(MNZPanel PRIVATE
    src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(MNZPanel PRIVATE d3d9)

set_target_properties(MNZPanel PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "MNZPanel"
)

if(MSVC)
    target_compile_options(MNZPanel PRIVATE /MT /O2)
endif()
