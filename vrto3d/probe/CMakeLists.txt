# Standalone build for the Milestone-0 direct-mode probe driver.
#   cmake -B build -G Ninja && cmake --build build
# Produces build/drivers/vrto3d_probe/ ready for `vrpathreg adddriver`.
cmake_minimum_required(VERSION 3.16)
project(vrto3d_probe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE RelWithDebInfo)

set(OPENVR_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/../../external/openvr/headers")

add_library(driver_vrto3d_probe SHARED
    probe_main.cpp
    probe_direct_mode.cpp
)

target_include_directories(driver_vrto3d_probe PRIVATE "${OPENVR_HEADERS}")
target_compile_options(driver_vrto3d_probe PRIVATE -fvisibility=hidden -fPIC)
target_link_libraries(driver_vrto3d_probe PRIVATE vulkan pthread)
set_target_properties(driver_vrto3d_probe PROPERTIES PREFIX "")

# Stage the SteamVR driver folder layout.
set(STAGE "${CMAKE_BINARY_DIR}/drivers/vrto3d_probe")
add_custom_command(TARGET driver_vrto3d_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGE}/bin/linux64"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:driver_vrto3d_probe> "${STAGE}/bin/linux64/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/driver" "${STAGE}/"
)
