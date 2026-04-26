/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "output_presenter.h"
#include "window_presenter.h"

#ifdef _WIN32
#  include "leiasr_presenter.h"
#  include "nvstereo_dx9_presenter.h"
#endif

#include "vrto3dlib/debug_log.hpp"

namespace vrto3d {

std::unique_ptr<IOutputPresenter> MakePresenter(OutputMode mode)
{
    switch (mode) {
        case OutputMode::SbS:
        case OutputMode::DualDisplay:
        case OutputMode::DualDisplayFlip:
        case OutputMode::TaB:
        case OutputMode::RowInterlaced:
        case OutputMode::ColInterlaced:
        case OutputMode::Checkerboard:
        case OutputMode::AnaglyphRedCyan:
        case OutputMode::AnaglyphRedCyanDubois:
        case OutputMode::AnaglyphRedCyanDeghosted:
        case OutputMode::AnaglyphGreenMagenta:
        case OutputMode::AnaglyphGreenMagentaDubois:
        case OutputMode::AnaglyphGreenMagentaDeghosted:
        case OutputMode::AnaglyphBlueAmber:
            return std::make_unique<WindowPresenter>();

        case OutputMode::LeiaSR:
#ifdef _WIN32
            return std::make_unique<LeiaSrPresenter>();
#else
            LOG() << "MakePresenter: LeiaSR not available on this platform; falling back to SbS";
            return std::make_unique<WindowPresenter>();
#endif

        case OutputMode::NvidiaDX9:
#ifdef _WIN32
            return std::make_unique<NvStereoDx9Presenter>();
#else
            LOG() << "MakePresenter: NvidiaDX9 not available on this platform; falling back to SbS";
            return std::make_unique<WindowPresenter>();
#endif
    }
    return std::make_unique<WindowPresenter>();
}

}  // namespace vrto3d
