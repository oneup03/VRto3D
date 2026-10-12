cmake_minimum_required(VERSION 3.16)
project(vrto3d_ctrl_example_plugin LANGUAGES C)

# A plugin needs nothing but vrto3d_ctrl_plugin.h. Copy this folder and the
# header anywhere to start your own.

add_library(example_plugin SHARED example_plugin.c)
target_include_directories(example_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
set_target_properties(example_plugin PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
if(NOT WIN32)
    target_link_libraries(example_plugin PRIVATE m)
endif()
