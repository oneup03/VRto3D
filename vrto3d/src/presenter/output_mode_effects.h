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

#include "vrto3dlib/stereo_config.h"

// Deliberately standalone rather than part of output_presenter.h: this is
// reached from hmd_device_driver.cpp, which is built on BOTH platforms, while
// output_presenter.h includes <d3d11.h> and presenter_factory.cpp is in the
// WIN32-only source list. Pulling the declaration in through either would break
// the Linux build at compile time and again at link time.

namespace vrto3d {

// Side effects of SELECTING an output mode, as distinct from instantiating its
// presenter: persistent machine state that has to already be in place when the
// process NEXT starts, and so cannot wait for a presenter to exist.
//
// Call this the moment the user picks a mode in the OSD (changing output mode
// needs a restart anyway, so that restart is exactly what arms it). Safe to
// call on any GPU vendor and from any thread.
//
// Deliberately NOT called by MakePresenter: at startup the enable direction
// would write the very state the NvidiaDX9 presenter inspects to decide whether
// its Fast Sync profile is LIVE in this process, and a just-written profile is
// not. MakePresenter therefore only ever takes the profile DOWN. See the
// comment there.
#ifdef _WIN32
void ApplyOutputModeSideEffects(OutputMode mode);
#else
// Nothing to arm: the only mode with persistent driver state behind it
// (NvidiaDX9, via an NVIDIA Fast Sync profile) is Windows-only.
inline void ApplyOutputModeSideEffects(OutputMode) {}
#endif

}  // namespace vrto3d
