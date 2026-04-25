/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace vrto3d {

// Flags observed by the presenter's window z-order thread. Owned by
// MockControllerDeviceDriver; passed by non-owning pointers to avoid
// ref-counting concerns.
struct FocusContext {
    std::atomic<bool>*     is_on_top     = nullptr;
    std::atomic<bool>*     man_on_top    = nullptr;
    std::atomic<bool>*     ue3d_on_top   = nullptr;
    std::atomic<uint32_t>* app_pid       = nullptr;
};

}  // namespace vrto3d
