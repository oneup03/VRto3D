/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifdef _WIN32

#include "nvstereo_dx9_presenter.h"

#include "dx11_renderer.h"
#include "vrto3dlib/debug_log.hpp"

namespace vrto3d {

bool NvStereoDx9Presenter::Init(Dx11Renderer& /*renderer*/,
                                const StereoDisplayDriverConfiguration& /*cfg*/,
                                const FocusContext& /*focus*/)
{
    // TODO: mirror XR3DV/src/nvapi_stereo.cpp:335-410 pattern.
    //   1. Create a hidden D3D9Ex popup window via platform::CreatePresentWindow.
    //   2. Direct3DCreate9Ex + CreateDeviceEx (FSE).
    //   3. NvAPI_Stereo_CreateHandleFromIUnknown + NvAPI_Stereo_Activate.
    //   4. CreateOffscreenPlainSurface(2W, H+1, D3DFMT_X8R8G8B8, POOL_SYSTEMMEM) for packed source,
    //      and matching POOL_DEFAULT copy destination.
    //   5. Write NVSTEREOIMAGEHEADER sentinel into the last row of the sysmem surface.
    LOG() << "NvStereoDx9Presenter::Init: 3DVisionDX9 path is a v1 skeleton (not yet wired)";
    return false;
}

void NvStereoDx9Presenter::PresentFrame(ID3D11Texture2D* /*sbs_input*/)
{
    // TODO:
    //   1. CopyResource to a D3D11 staging texture (USAGE_STAGING, CPU_READ).
    //   2. Map, memcpy rows into the D3D9 sysmem surface.
    //   3. device9_->UpdateSurface(sysmem, default) to push to GPU.
    //   4. device9_->StretchRect(packed, &src, backbuffer, nullptr, LINEAR).
    //   5. device9_->PresentEx(...).
}

void NvStereoDx9Presenter::Shutdown()
{
    // TODO: NvAPI_Stereo_DestroyHandle, release D3D9Ex device, destroy window.
}

}  // namespace vrto3d

#endif  // _WIN32
