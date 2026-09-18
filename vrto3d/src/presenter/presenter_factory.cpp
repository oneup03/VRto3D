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

#include "output_presenter.h"
#include "window_presenter.h"
#include "leiasr_presenter.h"
#include "nvstereo_dx9_presenter.h"
#include "wibblewobble_presenter.h"

#include <NV3D.hpp>

#include "vrto3dlib/debug_log.hpp"

namespace vrto3d {

std::unique_ptr<IOutputPresenter> MakePresenter(OutputMode mode)
{
    // Fast Sync is wanted by exactly one output path. NvidiaDX9 is the only
    // presenter that runs a fullscreen-EXCLUSIVE device, so it is the only one
    // where DWM is bypassed and something else has to show the newest completed
    // frame at each vblank; every other mode is a DWM-composited FLIP_DISCARD
    // window that already gets that from DWM and would gain nothing.
    //
    // The profile is persistent driver state that outlives the process (by
    // design — the driver samples profiles at process start), so switching to
    // any other mode has to actively take it back down. Changing output mode
    // already requires a restart, which makes this the natural place: it runs
    // once per session with the selected mode, before any presenter Init.
    // NvStereoDx9Presenter::Init owns the enable side, because it also needs
    // the result to decide whether it may ask for unthrottled presents.
    if (mode != OutputMode::NvidiaDX9) {
        NV3D::SetFastSyncProfile(false, nullptr);
    }

    switch (mode) {
        case OutputMode::SbS:
        case OutputMode::TaB:
        case OutputMode::RowInterlaced:
        case OutputMode::ColInterlaced:
        case OutputMode::Checkerboard:
        case OutputMode::VirtualDesktop:
        case OutputMode::FramePacked720p60:
        case OutputMode::FramePacked1080p24:
        case OutputMode::FramePacked1080p60:
        case OutputMode::FramePacked1080p60CVT:
        case OutputMode::DualDisplay:
        case OutputMode::DualDisplayFlip:
        case OutputMode::AnaglyphRedCyan:
        case OutputMode::AnaglyphRedCyanDubois:
        case OutputMode::AnaglyphRedCyanDeghosted:
        case OutputMode::AnaglyphRedCyanCompromise:
        case OutputMode::AnaglyphGreenMagenta:
        case OutputMode::AnaglyphGreenMagentaDubois:
        case OutputMode::AnaglyphGreenMagentaDeghosted:
        case OutputMode::AnaglyphBlueAmber:
        case OutputMode::Mono:
            return std::make_unique<WindowPresenter>();

        case OutputMode::LeiaSR:
            return std::make_unique<LeiaSrPresenter>();

        case OutputMode::NvidiaDX9:
            return std::make_unique<NvStereoDx9Presenter>();

        case OutputMode::WibbleWobble:
            return std::make_unique<WibbleWobblePresenter>();
    }
    return std::make_unique<WindowPresenter>();
}


void ApplyOutputModeSideEffects(OutputMode mode)
{
    // Fast Sync, armed at selection time rather than at first use. The driver
    // samples profiles when a process starts, so a profile written while
    // vrserver is already running does not apply to it — writing it here, when
    // the user picks the mode, means the restart they have to do anyway is the
    // one that brings it up. Without this the profile would only be written on
    // the first NvidiaDX9 run and not take effect until the second.
    const NV3D::FastSyncResult r =
        NV3D::SetFastSyncProfile(mode == OutputMode::NvidiaDX9, nullptr);
    LOG() << "ApplyOutputModeSideEffects: " << OutputModeToString(mode)
          << " fastsync=" << static_cast<int>(r);
}

}  // namespace vrto3d
