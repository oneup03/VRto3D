cmake_minimum_required(VERSION 3.21)
project(vrto3d_driver LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Sources shared by both platforms.
set(DRIVER_SOURCES
    src/hmd_device_driver.cpp
    src/hmd_driver_factory.cpp
    src/device_provider.cpp
    src/vr_recenter.cpp
    src/osd/osd_menu.cpp
)

set(IMGUI_DIR "${ROOT}/external/imgui")

if(WIN32)
    list(APPEND DRIVER_SOURCES
        src/dx11_renderer.cpp
        src/screenshot.cpp
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
        src/osd/osd_input.cpp
    )
else()
    list(APPEND DRIVER_SOURCES
        src/platform_linux.cpp
        src/vk/vk_context.cpp
        src/vk/vk_renderer.cpp
        src/vk/direct_mode_component_vk.cpp
        src/presenter/vk_swapchain_util.cpp
        src/presenter/vk_presenter_factory.cpp
        src/presenter/wayland_presenter.cpp
        src/presenter/x11_presenter.cpp
        src/presenter/x11_modeline.cpp
        src/presenter/wayland_protocols/xdg-shell-client-protocol.c
        src/presenter/wayland_protocols/wlr-layer-shell-client-protocol.c
        src/osd/osd_renderer_vk.cpp
        src/osd/osd_input_linux.cpp
    )
endif()

# Dear ImGui (vendored as submodule at external/imgui).
# Compiled directly into the driver — no separate target.
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)
if(WIN32)
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
else()
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
endif()
list(APPEND DRIVER_SOURCES ${IMGUI_SOURCES})

# MinHook (vendored as submodule at external/minhook). Pure-C library compiled
# directly into the driver — same pattern as imgui. Used to detour the OSD
# warning dispatcher inside nvd3dumx.dll so the 3D Vision "non-stereo display
# mode" red overlay can be suppressed on panels that aren't NV-certified.
# HDE disassembler source is arch-specific. Windows-only.
if(WIN32)
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
endif()

add_library(driver_vrto3d SHARED ${DRIVER_SOURCES})

target_include_directories(driver_vrto3d PRIVATE
    src
    ${ROOT}/external/VRto3DLib/include
    ${ROOT}/external/json/include
    ${ROOT}/external/openvr/headers
    ${ROOT}/utils/vrmath
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# VRto3DLib submodule — compile its sources directly. No transitive target yet.
target_sources(driver_vrto3d PRIVATE
    ${ROOT}/external/VRto3DLib/src/json_manager.cpp
    ${ROOT}/external/VRto3DLib/src/key_names.cpp
    ${ROOT}/external/VRto3DLib/src/app_id_mgr.cpp
)

if(WIN32)
    target_include_directories(driver_vrto3d PRIVATE
        ${ROOT}/external/SR-lib/SR-SDK-1.34.10/x64/include
        ${ROOT}/external/SR-lib/SR-SDK-1.34.10/x64/third_party/OpenCV/include
        ${ROOT}/external/nvapi
        ${ROOT}/external/adl_sdk/include
        ${MINHOOK_DIR}/include
    )

    # Pre-built openvr_api import lib ships with the submodule.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(OPENVR_LIB_DIR "${ROOT}/external/openvr/lib/win64")
    else()
        set(OPENVR_LIB_DIR "${ROOT}/external/openvr/lib/win32")
    endif()
    target_link_libraries(driver_vrto3d PRIVATE
        "${OPENVR_LIB_DIR}/openvr_api.lib"
        "${ROOT}/external/SR-lib/SR-SDK-1.34.10/x64/lib/simulatedreality.lib"
        "${ROOT}/external/SR-lib/SR-SDK-1.34.10/x64/lib/SimulatedRealityCore.lib"
        "${ROOT}/external/SR-lib/SR-SDK-1.34.10/x64/lib/SimulatedRealityDirectX.lib"
        "${ROOT}/external/SR-lib/SR-SDK-1.34.10/x64/lib/DimencoWeaving.lib"
        "${ROOT}/external/SR-lib/SR-SDK-1.34.10/x64/third_party/OpenCV/lib/x64/opencv_core343.lib"
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
else()
    # Linux: the driver links no openvr import lib (the driver context comes
    # in through HmdDriverFactory); everything else is system libraries.
    target_sources(driver_vrto3d PRIVATE
        ${ROOT}/external/VRto3DLib/src/linux_input.cpp
    )
    target_include_directories(driver_vrto3d PRIVATE
        ${ROOT}/external/stb
        src/presenter/wayland_protocols
    )
    find_package(Threads REQUIRED)
    target_link_libraries(driver_vrto3d PRIVATE
        vulkan
        X11
        Xrandr
        wayland-client
        xkbcommon
        Threads::Threads
    )
    target_compile_options(driver_vrto3d PRIVATE -fvisibility=hidden -fPIC)
    set_target_properties(driver_vrto3d PROPERTIES
        PREFIX ""
        VISIBILITY_INLINES_HIDDEN ON
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

# Post-build: copy manifest + settings next to the binary and drop the driver
# into SteamVR's drivers/ dir for quick iteration.
set(OUT_DIR "${CMAKE_BINARY_DIR}/output/drivers/vrto3d")
if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(BIN_SUBDIR "bin/win64")
        set(OPENVR_DLL "${ROOT}/external/openvr/bin/win64/openvr_api.dll")
    else()
        set(BIN_SUBDIR "bin/win32")
        set(OPENVR_DLL "${ROOT}/external/openvr/bin/win32/openvr_api.dll")
    endif()
else()
    set(BIN_SUBDIR "bin/linux64")
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

if(WIN32)
    add_custom_command(TARGET driver_vrto3d POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENVR_DLL}"
                "${OUT_DIR}/${BIN_SUBDIR}/openvr_api.dll"
        COMMENT "Staging openvr_api.dll next to driver_vrto3d"
    )
endif()
