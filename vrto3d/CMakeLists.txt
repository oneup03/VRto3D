cmake_minimum_required(VERSION 3.21)
project(vrto3d_driver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

set(DRIVER_SOURCES
    src/hmd_device_driver.cpp
    src/hmd_driver_factory.cpp
    src/device_provider.cpp
    src/dx11_renderer.cpp
    src/screenshot.cpp
    src/vr_recenter.cpp
    src/auto_depth.cpp
    src/platform_win32.cpp
    src/presenter/presenter_factory.cpp
    src/presenter/window_presenter.cpp
    src/presenter/leiasr_presenter.cpp
    src/presenter/nvstereo_dx9_presenter.cpp
    src/presenter/nv_3dvision_suppressor.cpp
    src/presenter/wibblewobble_presenter.cpp
    src/presenter/display_timing_helper.cpp
    src/osd/osd_renderer.cpp
    src/osd/osd_menu.cpp
    src/osd/osd_input.cpp
)

# Dear ImGui (vendored as submodule at external/imgui).
# Compiled directly into the driver — no separate target.
set(IMGUI_DIR "${ROOT}/external/imgui")
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
list(APPEND DRIVER_SOURCES ${IMGUI_SOURCES})

# MinHook (vendored as submodule at external/minhook). Pure-C library compiled
# directly into the driver — same pattern as imgui. Used to detour the OSD
# warning dispatcher inside nvd3dumx.dll so the 3D Vision "non-stereo display
# mode" red overlay can be suppressed on panels that aren't NV-certified.
# HDE disassembler source is arch-specific.
set(MINHOOK_DIR "${ROOT}/external/minhook")
set(MINHOOK_SOURCES
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/trampoline.c
)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND MINHOOK_SOURCES ${MINHOOK_DIR}/src/hde/hde64.c)
else()
    list(APPEND MINHOOK_SOURCES ${MINHOOK_DIR}/src/hde/hde32.c)
endif()
list(APPEND DRIVER_SOURCES ${MINHOOK_SOURCES})

add_library(driver_vrto3d SHARED ${DRIVER_SOURCES})

target_include_directories(driver_vrto3d PRIVATE
    src
    ${ROOT}/external/VRto3DLib/include
    ${ROOT}/external/json/include
    ${ROOT}/external/openvr/headers
    ${ROOT}/external/LeiaSR/include
    ${ROOT}/external/LeiaSR/third_party/OpenCV/include
    ${ROOT}/external/nvapi
    ${ROOT}/external/adl_sdk/include
    ${ROOT}/utils/vrmath
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${MINHOOK_DIR}/include
)

# VRto3DLib submodule — compile its sources directly. No transitive target yet.
target_sources(driver_vrto3d PRIVATE
    ${ROOT}/external/VRto3DLib/src/json_manager.cpp
)

# Pre-built openvr_api import lib ships with the submodule.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(OPENVR_LIB_DIR "${ROOT}/external/openvr/lib/win64")
else()
    set(OPENVR_LIB_DIR "${ROOT}/external/openvr/lib/win32")
endif()
target_link_libraries(driver_vrto3d PRIVATE
    "${OPENVR_LIB_DIR}/openvr_api.lib"
    "${ROOT}/external/LeiaSR/lib/simulatedreality.lib"
    "${ROOT}/external/LeiaSR/lib/SimulatedRealityCore.lib"
    "${ROOT}/external/LeiaSR/lib/SimulatedRealityDirectX.lib"
    "${ROOT}/external/LeiaSR/lib/DimencoWeaving.lib"
    "${ROOT}/external/LeiaSR/third_party/OpenCV/lib/x64/opencv_core343.lib"
    "${ROOT}/external/nvapi/amd64/nvapi64.lib"
    delayimp
    d3d11
    dxgi
    dxguid
    d3dcompiler
    d3d9
    xinput9_1_0
    ws2_32
    user32
    gdi32
    urlmon
)
# Delay-load LeiaSR + OpenCV + NVAPI DLLs so the driver still loads when
# those runtimes aren't installed. The DLLs are only resolved when their
# first symbol is actually called (i.e., when LeiaSrPresenter::Init or
# NvStereoDx9Presenter::Init runs).
target_link_options(driver_vrto3d PRIVATE
    "/DELAYLOAD:simulatedreality.dll"
    "/DELAYLOAD:SimulatedRealityCore.dll"
    "/DELAYLOAD:SimulatedRealityDirectX.dll"
    "/DELAYLOAD:DimencoWeaving.dll"
    "/DELAYLOAD:opencv_core343.dll"
    "/DELAYLOAD:nvapi64.dll"
    "/DELAYLOAD:openvr_api.dll"
)
target_compile_definitions(driver_vrto3d PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

# Post-build: copy manifest + settings next to the binary and drop the driver
# into SteamVR's drivers/ dir for quick iteration.
set(OUT_DIR "${CMAKE_BINARY_DIR}/output/drivers/vrto3d")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(BIN_SUBDIR "bin/win64")
    set(OPENVR_DLL "${ROOT}/external/openvr/bin/win64/openvr_api.dll")
else()
    set(BIN_SUBDIR "bin/win32")
    set(OPENVR_DLL "${ROOT}/external/openvr/bin/win32/openvr_api.dll")
endif()

add_custom_command(TARGET driver_vrto3d POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OUT_DIR}/${BIN_SUBDIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:driver_vrto3d>
            "${OUT_DIR}/${BIN_SUBDIR}/$<TARGET_FILE_NAME:driver_vrto3d>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/vrto3d"
            "${OUT_DIR}"
    COMMENT "Staging driver_vrto3d into ${OUT_DIR}"
)

add_custom_command(TARGET driver_vrto3d POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENVR_DLL}"
            "${OUT_DIR}/${BIN_SUBDIR}/openvr_api.dll"
    COMMENT "Staging openvr_api.dll next to driver_vrto3d"
)
