/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <memory>

#include <d3d11.h>

#include "focus_context.h"
#include "vrto3dlib/stereo_config.h"

class Dx11Renderer;

namespace vrto3d {

class IOutputPresenter {
public:
    virtual ~IOutputPresenter() = default;

    // Called once after Dx11Renderer has finished constructing the D3D11 device.
    virtual bool Init(Dx11Renderer& renderer,
                      const StereoDisplayDriverConfiguration& cfg,
                      const FocusContext& focus) = 0;

    // Hand the freshly-copied 2W x H SbS input texture to the presenter for display.
    // Presenter runs synchronously on the compositor's Present thread; keep it short.
    virtual void PresentFrame(ID3D11Texture2D* sbs_input) = 0;

    virtual void Shutdown() = 0;
};

std::unique_ptr<IOutputPresenter> MakePresenter(OutputMode mode);

}  // namespace vrto3d
