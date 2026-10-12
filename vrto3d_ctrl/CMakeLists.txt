cmake_minimum_required(VERSION 3.21)
project(vrto3d_ctrl_driver LANGUAGES C CXX)

# Emulated motion-controller driver for SteamVR (driver_vrto3d_ctrl).
# Built standalone (cmake -S vrto3d_ctrl) or from vrto3d/CMakeLists.txt via
# add_subdirectory, which is how the Linux CI builds both drivers together.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CTRL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

set(CTRL_SOURCES
    src/ctrl_driver_factory.cpp
    src/ctrl_device_provider.cpp
    src/ctrl_controller_device.cpp
    src/ctrl_config.cpp
    src/ctrl_platform.cpp
    src/ctrl_plugin_host.cpp
    src/ctrl_plugin_guard.cpp
    src/ctrl_opentrack.cpp
    src/plugins/xinput_plugin.cpp
    src/plugins/remote_plugin.cpp
    ${CTRL_ROOT}/external/VRto3DLib/src/key_names.cpp
)
if(WIN32)
    list(APPEND CTRL_SOURCES src/plugins/kbm_plugin.cpp)
else()
    list(APPEND CTRL_SOURCES ${CTRL_ROOT}/external/VRto3DLib/src/linux_input.cpp)
endif()

add_library(driver_vrto3d_ctrl SHARED ${CTRL_SOURCES})

target_include_directories(driver_vrto3d_ctrl PRIVATE
    src
    include
    ${CTRL_ROOT}/external/VRto3DLib/include
    ${CTRL_ROOT}/external/json/include
    ${CTRL_ROOT}/external/openvr/headers
)

if(WIN32)
    target_compile_definitions(driver_vrto3d_ctrl PRIVATE NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(driver_vrto3d_ctrl PRIVATE ws2_32 winmm xinput user32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(driver_vrto3d_ctrl PRIVATE dl Threads::Threads)
    target_compile_options(driver_vrto3d_ctrl PRIVATE -fvisibility=hidden -fPIC)
    set_target_properties(driver_vrto3d_ctrl PROPERTIES
        PREFIX ""
        VISIBILITY_INLINES_HIDDEN ON
    )
endif()

# Stage manifest + resources + binary the way SteamVR expects them.
set(CTRL_OUT_DIR "${CMAKE_BINARY_DIR}/output/drivers/vrto3d_ctrl")
if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(CTRL_BIN_SUBDIR "bin/win64")
    else()
        set(CTRL_BIN_SUBDIR "bin/win32")
    endif()
else()
    set(CTRL_BIN_SUBDIR "bin/linux64")
endif()

add_custom_command(TARGET driver_vrto3d_ctrl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CTRL_OUT_DIR}/${CTRL_BIN_SUBDIR}/plugins"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:driver_vrto3d_ctrl>
            "${CTRL_OUT_DIR}/${CTRL_BIN_SUBDIR}/$<TARGET_FILE_NAME:driver_vrto3d_ctrl>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/vrto3d_ctrl"
            "${CTRL_OUT_DIR}"
    COMMENT "Staging driver_vrto3d_ctrl into ${CTRL_OUT_DIR}"
)

# Minimal third-party plugin, built to prove the external loader works.
option(VRTO3D_CTRL_BUILD_EXAMPLES "Build the example controller plugin" OFF)
if(VRTO3D_CTRL_BUILD_EXAMPLES)
    add_subdirectory(examples/example_plugin)
endif()
