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
        src/presenter/wibblewobble_presenter_linux.cpp
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
    # first symbol is actually called (i.e., when LeiaSrPresenter::Init runs,
    # or when NV3D-Lib calls NvAPI_Initialize inside CreateInterfaceDX11 —
    # the lib SEH-guards that call so a missing nvapi64.dll fails cleanly).
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

    # --- NV3D-Lib (external/NV3D-Lib, MSBuild-only upstream) -----------------
    # Compiled here as a separate static target so its own vendored nvapi /
    # json / minhook headers can't shadow VRto3D's copies (the driver uses
    # external/nvapi and external/json/include directly). Only the DX11
    # backend TU is built: the DX12/OGL/Vulkan backends are separate TUs whose
    # CreateInterface* factories nothing references, and skipping them avoids
    # a Vulkan-Headers dependency (only vulkan_backend.cpp includes it).
    # MinHook is compiled INTO this target (from NV3D-Lib's own nested
    # submodule) — it powers the lib's 3D Vision suppressor; VRto3D no longer
    # carries a minhook copy of its own.
    set(NV3DLIB_DIR "${ROOT}/external/NV3D-Lib/NV3DLib")
    set(NV3DLIB_EXT "${ROOT}/external/NV3D-Lib/external")

    # nvtimings_embedded.h is gitignored upstream and regenerated at build
    # time. Generate into the submodule's src/ (the same location NV3D-Lib's
    # own MSBuild pre-build writes): lightboost.cpp includes it with quotes,
    # so the sibling-directory lookup always wins over include paths — a
    # binary-dir copy could be shadowed by a stale sibling.
    set(NV3D_EMBED_HDR "${NV3DLIB_DIR}/src/nvtimings_embedded.h")
    add_custom_command(
        OUTPUT "${NV3D_EMBED_HDR}"
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                -File "${ROOT}/external/NV3D-Lib/tools/embed_nvtimings.ps1"
                -InputJson "${ROOT}/external/NV3D-Lib/nvtimings.json"
                -OutputHeader "${NV3D_EMBED_HDR}"
        DEPENDS "${ROOT}/external/NV3D-Lib/nvtimings.json"
                "${ROOT}/external/NV3D-Lib/tools/embed_nvtimings.ps1"
        COMMENT "Generating NV3D-Lib nvtimings_embedded.h"
    )

    set(NV3DLIB_SOURCES
        ${NV3DLIB_DIR}/src/NV3D.cpp
        ${NV3DLIB_DIR}/src/d3d9_presenter.cpp
        ${NV3DLIB_DIR}/src/lightboost.cpp
        ${NV3DLIB_DIR}/src/log.cpp
        ${NV3DLIB_DIR}/src/nv_3dvision_suppressor.cpp
        ${NV3DLIB_DIR}/src/present_window.cpp
        ${NV3DLIB_DIR}/src/backends/dx11_backend.cpp
        ${NV3D_EMBED_HDR}
        ${NV3DLIB_EXT}/minhook/src/buffer.c
        ${NV3DLIB_EXT}/minhook/src/hook.c
        ${NV3DLIB_EXT}/minhook/src/trampoline.c
    )
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        list(APPEND NV3DLIB_SOURCES ${NV3DLIB_EXT}/minhook/src/hde/hde64.c)
    else()
        list(APPEND NV3DLIB_SOURCES ${NV3DLIB_EXT}/minhook/src/hde/hde32.c)
    endif()

    add_library(nv3dlib STATIC ${NV3DLIB_SOURCES})
    target_include_directories(nv3dlib
        PUBLIC  ${NV3DLIB_DIR}/include
        PRIVATE ${NV3DLIB_DIR}/src
                ${NV3DLIB_EXT}/nvapi
                ${NV3DLIB_EXT}/minhook/include
                ${NV3DLIB_EXT}/json/single_include
    )
    # Parity with NV3DLib.vcxproj (note: no UNICODE here — the lib uses
    # explicit W-suffixed Win32 APIs).
    target_compile_definitions(nv3dlib PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    set_property(TARGET nv3dlib PROPERTY CXX_STANDARD 17)
    set_property(TARGET nv3dlib PROPERTY CXX_STANDARD_REQUIRED ON)
    # nvapi64.lib / d3d9 / d3d11 / dxgi already on the driver's link line
    # satisfy the archive's externals.
    target_link_libraries(driver_vrto3d PRIVATE nv3dlib)
else()
    # Linux: the driver links no openvr import lib (the driver context comes
    # in through HmdDriverFactory); everything else is system libraries.
    target_sources(driver_vrto3d PRIVATE
        ${ROOT}/external/VRto3DLib/src/linux_input.cpp
    )
    target_include_directories(driver_vrto3d PRIVATE
        ${ROOT}/external/stb
        src/presenter/wayland_protocols
        /usr/include/libdrm
    )
    find_package(Threads REQUIRED)
    # libopenvr_api.so is needed for the OpenVR *client* API used by
    # vr_recenter.cpp (VR_Init/VR_GetGenericInterface); the driver-side
    # interfaces come in through HmdDriverFactory and need no library.
    target_link_libraries(driver_vrto3d PRIVATE
        "${ROOT}/external/openvr/lib/linux64/libopenvr_api.so"
        vulkan
        X11
        Xext
        Xrandr
        wayland-client
        xkbcommon
        dl
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
else()
    # RPATH $ORIGIN resolves this staged copy at dlopen time.
    add_custom_command(TARGET driver_vrto3d POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ROOT}/external/openvr/bin/linux64/libopenvr_api.so"
                "${OUT_DIR}/${BIN_SUBDIR}/libopenvr_api.so"
        COMMENT "Staging libopenvr_api.so next to driver_vrto3d"
    )
endif()
