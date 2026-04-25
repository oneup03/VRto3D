/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#ifdef _WIN32

#include "presenter/output_presenter.h"

namespace vrto3d {

// Alternate presenter: forwards the SBS texture to SR::IDX11Weaver1.
// v1 skeleton — bodies marked // TODO.
class LeiaSrPresenter : public IOutputPresenter {
public:
    bool Init(Dx11Renderer& renderer,
              const StereoDisplayDriverConfiguration& cfg,
              const FocusContext& focus) override;
    void PresentFrame(ID3D11Texture2D* sbs_input) override;
    void Shutdown() override;
};

}  // namespace vrto3d

#endif  // _WIN32
