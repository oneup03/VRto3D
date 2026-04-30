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
    src/presenter/presenter_factory.cpp
    src/presenter/window_presenter.cpp
)

if(WIN32)
    list(APPEND DRIVER_SOURCES
        src/platform/platform_win32.cpp
        src/presenter/leiasr_presenter.cpp
        src/presenter/nvstereo_dx9_presenter.cpp
        src/presenter/display_timing_helper.cpp
    )
else()
    list(APPEND DRIVER_SOURCES
        src/platform/platform_linux.cpp
    )
endif()

add_library(driver_vrto3d SHARED ${DRIVER_SOURCES})

target_include_directories(driver_vrto3d PRIVATE
    src
    ${ROOT}/external/VRto3DLib/include
    ${ROOT}/external/json/include
    ${ROOT}/external/openvr/headers
    ${ROOT}/external/LeiaSR/include
    ${ROOT}/external/LeiaSR/third_party/OpenCV/include
    ${ROOT}/external/nvapi
    ${ROOT}/utils/vrmath
)

# VRto3DLib submodule — compile its sources directly. No transitive target yet.
target_sources(driver_vrto3d PRIVATE
    ${ROOT}/external/VRto3DLib/src/json_manager.cpp
)

if(WIN32)
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
    )
    target_compile_definitions(driver_vrto3d PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
else()
    # Linux build — Win32-only subsystems are stubbed.
    target_compile_definitions(driver_vrto3d PRIVATE
        VRTO3D_NO_XINPUT
        VRTO3D_NO_WIN32_FOCUS
        VRTO3D_NO_UEVR_IPC
    )
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
        if(SDL2_FOUND)
            target_link_libraries(driver_vrto3d PRIVATE PkgConfig::SDL2)
        endif()
    endif()
    # openvr_api: prefer prebuilt .so under external/openvr/lib/linux64; otherwise find_library.
    set(OPENVR_LINUX_SO "${ROOT}/external/openvr/lib/linux64/libopenvr_api.so")
    if(EXISTS ${OPENVR_LINUX_SO})
        target_link_libraries(driver_vrto3d PRIVATE ${OPENVR_LINUX_SO})
    else()
        find_library(OPENVR_API_LIB openvr_api)
        if(OPENVR_API_LIB)
            target_link_libraries(driver_vrto3d PRIVATE ${OPENVR_API_LIB})
        endif()
    endif()
    # DXVK linkage is expected at runtime via LD_PRELOAD or the app's linker rpath;
    # headers come from DXVK's install. Bring-up for full Linux support is v1.5.
endif()

# Post-build: copy manifest + settings next to the binary and drop the driver
# into SteamVR's drivers/ dir for quick iteration.
set(OUT_DIR "${CMAKE_BINARY_DIR}/output/drivers/vrto3d")
if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(BIN_SUBDIR "bin/win64")
    else()
        set(BIN_SUBDIR "bin/win32")
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
