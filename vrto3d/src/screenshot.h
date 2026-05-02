/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <cstdint>
#include <string>

#include <dxgiformat.h>

namespace vrto3d::screenshot {

// Result of writing a stereo screenshot pair.
struct SaveResult {
    bool         ok = false;     // both files written successfully
    int          index = -1;     // selected counter (filename suffix)
    std::string  dir;             // destination directory (UTF-8)
    uint32_t     out_width = 0;   // final SBS width  after aspect-ratio scaling
    uint32_t     out_height = 0;  // final SBS height after aspect-ratio scaling
};

// Save a stereo SBS screenshot pair to <Steam>\steamapps\common\SteamVR\screenshots.
//
// `data`/`row_pitch` describe the raw mapped staging texture. `dxgi_format`
// must be one of DXGI_FORMAT_{R,B}8G8B8A8_UNORM[_SRGB] — others are rejected.
//
// `target_eye_aspect` (>0) corrects the per-eye aspect by *enlarging* the
// smaller axis only. Pass 0 to skip scaling.
//
// Two files are written next to each other:
//   <app_name>_NNNN.png           (parallel-view, as-is)
//   <app_name>_NNNN_crossview.png (eyes swapped)
// NNNN starts at 0001 and increments until both filenames are free, so
// existing screenshots are never overwritten.
SaveResult SaveStereoPair(const std::string& app_name,
                          uint32_t            sbs_width,
                          uint32_t            sbs_height,
                          DXGI_FORMAT         dxgi_format,
                          const void*         data,
                          uint32_t            row_pitch,
                          float               target_eye_aspect);

}  // namespace vrto3d::screenshot
