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

#include "vrto3dlib/debug_log.hpp"

namespace vrto3d {

std::unique_ptr<IOutputPresenter> MakePresenter(OutputMode mode)
{
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

}  // namespace vrto3d
