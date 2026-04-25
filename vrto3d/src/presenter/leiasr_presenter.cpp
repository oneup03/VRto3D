/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifdef _WIN32

#include "leiasr_presenter.h"

#include "dx11_renderer.h"
#include "vrto3dlib/debug_log.hpp"

namespace vrto3d {

bool LeiaSrPresenter::Init(Dx11Renderer& /*renderer*/,
                           const StereoDisplayDriverConfiguration& /*cfg*/,
                           const FocusContext& /*focus*/)
{
    // TODO: link against LeiaSR SDK. Minimum bring-up:
    //   1. SR::SRContext::create() (or equivalent constructor), hold in a member.
    //   2. platform::CreatePresentWindow(primary, secondary, 0, "VRto3D-LeiaSR").
    //   3. Create DXGI swapchain on the window at native resolution.
    //   4. SR::CreateDX11Weaver(ctx, renderer.Context(), hwnd, &weaver_).
    //   5. context_->initialize() after weaver is constructed.
    // Keep the HWND alive for the lifetime of the weaver — the weaver tracks
    // the eye positions per-window for late-latching.
    LOG() << "LeiaSrPresenter::Init: LeiaSR path is a v1 skeleton (not yet wired)";
    return false;
}

void LeiaSrPresenter::PresentFrame(ID3D11Texture2D* /*sbs_input*/)
{
    // TODO:
    //   1. CreateShaderResourceView(sbs_input) with dimensions (w/2, h) — weaver
    //      expects PER-EYE width (it treats the SBS texture as 2 side-by-side views).
    //   2. weaver_->setInputViewTexture(srv, w/2, h, format).
    //   3. ctx->OMSetRenderTargets(swapchain_rtv);
    //   4. weaver_->weave();
    //   5. swapchain_->Present(1, 0);
}

void LeiaSrPresenter::Shutdown()
{
    // TODO: destroy weaver, release SRContext, destroy swapchain + window.
}

}  // namespace vrto3d

#endif  // _WIN32
