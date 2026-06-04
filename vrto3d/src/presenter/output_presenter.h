/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VRto3D is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with VRto3D. If not, see <http://www.gnu.org/licenses/>.
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

    // Hand the freshly-copied 2W x H SbS input texture to the presenter, record
    // all GPU work for this frame (compose-shader draw, weaver, packed-surface
    // pipeline, etc.) onto the immediate context, and Flush so the commands are
    // in the driver queue. Caller MUST hold Dx11Renderer::context_mutex_ across
    // this call.
    virtual void RecordComposite(ID3D11Texture2D* sbs_input) = 0;

    // Issue the swap-chain Present for the frame recorded by RecordComposite.
    // Caller MUST NOT hold Dx11Renderer::context_mutex_ — Present blocks on
    // display pacing (waitable handle / vsync interval), and holding the mutex
    // would stall the compositor thread's next OnDirectModeFrame.
    virtual void Present() = 0;

    virtual void Shutdown() = 0;

    // Optional: snap the current head pose as the neutral zero. Default
    // no-op — only the LeiaSR presenter implements head-pose calibration.
    // Called from the menu-callback thread; implementations MUST be
    // thread-safe (typically just an atomic flag the render/track thread
    // reads on the next iteration).
    virtual void RequestCalibrate() {}
};

std::unique_ptr<IOutputPresenter> MakePresenter(OutputMode mode);

}  // namespace vrto3d
