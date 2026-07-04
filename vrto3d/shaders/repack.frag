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
#version 450

// GLSL port of the Windows presenter's repack pixel shader (kPsSource in
// vrto3d/src/presenter/window_presenter.cpp) plus the display-correction
// pass (kAdjustPsHlsl in vrto3d/src/dx11_renderer.cpp) folded in as an
// optional post-step gated by pc.correction_enabled.
//
// The input `sbs` is the canonical 2W x H side-by-side texture — left eye in
// the left half (before eye_swap). uv (0,0) is the top-left of the OUTPUT
// window (see fullscreen.vert), and v = 0 samples the top row of `sbs`.
//
// `pc.mode` carries the raw OutputMode enum value (vrto3dlib/stereo_config.h),
// NOT the Windows presenter's remapped shader enum. The FramePacked* variants
// derive their blank-gap row count from the mode itself (30 px for 720p60,
// 45 px for the 1080p variants — mirrors FramePackTimingSpec::gap_pixels,
// which is what the Windows presenter feeds as `framepack_offset`).
//
// C++ mirror of the push-constant block (std430 layout, 96 bytes total):
//
//     struct RepackPushConstants {          // offset
//         int32_t out_size[2];              //   0  swapchain extent (w, h)
//         int32_t eye_size[2];              //   8  per-eye pixel size of sbs
//         int32_t mode;                     //  16  OutputMode int value
//         int32_t eye_swap;                 //  20  0/1
//         int32_t correction_enabled;       //  24  0/1 display-correction post-step
//         float   curve;                    //  28  shader_curve
//         float   lift[4];                  //  32  rgb + pad
//         float   gamma[4];                 //  48  rgb + pad
//         float   gain[4];                  //  64  rgb + pad
//         float   curve_offsets[4];         //  80  x=off_low y=off_high z=off_both w=unused
//     };                                    //  96 bytes (<= 128 push-constant min-spec)
//     static_assert(sizeof(RepackPushConstants) == 96);

layout(set = 0, binding = 0) uniform sampler2D sbs;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Pc {
    ivec2 out_size;           // swapchain extent
    ivec2 eye_size;           // per-eye pixel size of sbs (sbs is 2*eye_size.x wide); reserved
    int   mode;               // OutputMode int value
    int   eye_swap;           // 0/1
    int   correction_enabled; // display-correction post-step
    float curve;              // shader_curve
    vec4  lift;               // rgb + pad
    vec4  gamma_;             // rgb + pad
    vec4  gain;               // rgb + pad
    vec4  curve_offsets;      // x=off_low y=off_high z=off_both w=unused
} pc;

// OutputMode enum values — keep in sync with vrto3dlib/stereo_config.h.
const int kSbS                           = 0;
const int kTaB                           = 1;
const int kRowInterlaced                 = 2;
const int kColInterlaced                 = 3;
const int kCheckerboard                  = 4;
const int kLeiaSR                        = 5;   // alternate presenter — SbS fallback
const int kNvidiaDX9                     = 6;   // alternate presenter — SbS fallback
const int kWibbleWobble                  = 7;   // alternate presenter — SbS fallback
const int kVirtualDesktop                = 8;
const int kFramePacked720p60             = 9;
const int kFramePacked1080p24            = 10;
const int kFramePacked1080p60            = 11;
const int kFramePacked1080p60CVT         = 12;
const int kDualDisplay                   = 13;
const int kDualDisplayFlip               = 14;
const int kAnaglyphRedCyan               = 15;
const int kAnaglyphRedCyanDubois         = 16;
const int kAnaglyphRedCyanDeghosted      = 17;
const int kAnaglyphRedCyanCompromise     = 18;
const int kAnaglyphGreenMagenta          = 19;
const int kAnaglyphGreenMagentaDubois    = 20;
const int kAnaglyphGreenMagentaDeghosted = 21;
const int kAnaglyphBlueAmber             = 22;
const int kMono                          = 23;

// Given a half (0=left, 1=right), sample the input SbS at (u_half, v).
// u_half in [0,1] = position within the eye's half of the input.
// eye_swap flips which half-source we read (mirrors HLSL SampleEye).
vec4 SampleEye(int half_idx, float u_half, float v) {
    int eye = (pc.eye_swap != 0) ? (1 - half_idx) : half_idx;
    float u_src = 0.5 * u_half + 0.5 * float(eye);
    return texture(sbs, vec2(u_src, v));
}

// Display-correction post-process — exact port of kAdjustPsHlsl
// (dx11_renderer.cpp): extended SCurve.fx followed by LiftGammaGain.fx.
// Operates in the same byte space the swapchain stores (defaults are all
// pass-through, so black stays black in framepack gap rows).
vec3 ApplyCorrection(vec3 col) {
    float off_low  = pc.curve_offsets.x;
    float off_high = pc.curve_offsets.y;
    float off_both = pc.curve_offsets.z;

    // Extended SCurve.fx — at curve=1.0 the mix degenerates to pass-through.
    vec3 low  = pow(abs(col), vec3(pc.curve))       + off_low;
    vec3 high = pow(abs(col), vec3(1.0 / pc.curve)) + off_high;
    vec3 t    = clamp(col + off_both, 0.0, 1.0);
    col = mix(low, high, t);

    // LiftGammaGain.fx (operates in sRGB byte space — matches ReShade default).
    col = col * (1.5 - 0.5 * pc.lift.rgb) + 0.5 * pc.lift.rgb - 0.5;
    col = clamp(col, 0.0, 1.0);
    col *= pc.gain.rgb;
    col = pow(abs(col), 1.0 / pc.gamma_.rgb);

    return clamp(col, 0.0, 1.0);
}

vec4 Repack() {
    int m = pc.mode;

    // SbS / DualDisplay (+ alternate-presenter fallbacks): straight
    // horizontal split. For DualDisplay the output window spans two
    // monitors, so the left half (= left monitor) shows the left eye
    // fullscreen and the right half the right eye.
    if (m == kSbS || m == kDualDisplay
        || m == kLeiaSR || m == kNvidiaDX9 || m == kWibbleWobble) {
        int   half_idx = (uv.x < 0.5) ? 0 : 1;
        float u_half   = (uv.x < 0.5) ? (uv.x * 2.0) : ((uv.x - 0.5) * 2.0);
        return SampleEye(half_idx, u_half, uv.y);
    }

    // TaB / FramePacked*: left eye on the top rows, blank gap rows black,
    // right eye below. Plain TaB has no gap; the FramePacked variants derive
    // the gap row count from the HDMI 1.4 timing spec.
    if (m == kTaB || m == kFramePacked720p60 || m == kFramePacked1080p24
        || m == kFramePacked1080p60 || m == kFramePacked1080p60CVT) {
        float gap_px = 0.0;
        if (m == kFramePacked720p60)  gap_px = 30.0;
        else if (m != kTaB)           gap_px = 45.0;   // 1080p24/60/60CVT
        float gap = (gap_px > 0.0) ? (gap_px / float(pc.out_size.y)) : 0.0;
        float half_height = (1.0 - gap) * 0.5;
        if (uv.y < half_height) {
            // Top half -> left eye
            return SampleEye(0, uv.x, uv.y / half_height);
        } else if (uv.y >= half_height + gap) {
            // Bottom half -> right eye
            float v_rel = (uv.y - (half_height + gap)) / half_height;
            return SampleEye(1, uv.x, v_rel);
        } else {
            return vec4(0.0, 0.0, 0.0, 1.0);   // framepack gap
        }
    }

    // Row-interlaced: alternating rows; odd output rows = right eye.
    if (m == kRowInterlaced) {
        int row = int(gl_FragCoord.y);
        int half_idx = row & 1;
        return SampleEye(half_idx, uv.x, uv.y);
    }

    // Col-interlaced
    if (m == kColInterlaced) {
        int col = int(gl_FragCoord.x);
        int half_idx = col & 1;
        return SampleEye(half_idx, uv.x, uv.y);
    }

    // Checkerboard
    if (m == kCheckerboard) {
        int row = int(gl_FragCoord.y);
        int col = int(gl_FragCoord.x);
        int half_idx = (row + col) & 1;
        return SampleEye(half_idx, uv.x, uv.y);
    }

    // VirtualDesktop: SbS in center band of a 2W x 2H window (rows [0.25, 0.75)).
    if (m == kVirtualDesktop) {
        if (uv.y < 0.25 || uv.y >= 0.75) return vec4(0.0, 0.0, 0.0, 1.0);
        float v = (uv.y - 0.25) * 2.0;
        int   half_idx = (uv.x < 0.5) ? 0 : 1;
        float u_half   = (uv.x < 0.5) ? (uv.x * 2.0) : ((uv.x - 0.5) * 2.0);
        return SampleEye(half_idx, u_half, v);
    }

    // DualDisplayFlip: same horizontal split as SbS/DualDisplay, but the LEFT
    // half is sampled with v inverted (vertical flip).
    if (m == kDualDisplayFlip) {
        int   half_idx = (uv.x < 0.5) ? 0 : 1;
        float u_half   = (uv.x < 0.5) ? (uv.x * 2.0) : ((uv.x - 0.5) * 2.0);
        float v_src    = (half_idx == 0) ? (1.0 - uv.y) : uv.y;
        return SampleEye(half_idx, u_half, v_src);
    }

    // Mono — single-eye view. eye_swap selects right (1) instead of left (0).
    if (m == kMono) {
        return SampleEye(0, uv.x, uv.y);
    }

    // ----- Anaglyph variants (3DToElse / iaian7+vectorform formulas) -----
    // Sample full image of each eye; cA = left, cB = right.
    vec4 cA = SampleEye(0, uv.x, uv.y);
    vec4 cB = SampleEye(1, uv.x, uv.y);

    // Anaglyph red-cyan (simple)
    if (m == kAnaglyphRedCyan) {
        return vec4(cA.r, cB.g, cB.b, 1.0);
    }

    // Anaglyph red-cyan Dubois
    if (m == kAnaglyphRedCyanDubois) {
        float r = clamp( 0.437 * cA.r + 0.449 * cA.g + 0.164 * cA.b
                        - 0.011 * cB.r - 0.032 * cB.g - 0.007 * cB.b, 0.0, 1.0);
        float g = clamp(-0.062 * cA.r - 0.062 * cA.g - 0.024 * cA.b
                        + 0.377 * cB.r + 0.761 * cB.g + 0.009 * cB.b, 0.0, 1.0);
        float b = clamp(-0.048 * cA.r - 0.050 * cA.g - 0.017 * cA.b
                        - 0.026 * cB.r - 0.093 * cB.g + 1.234 * cB.b, 0.0, 1.0);
        return vec4(r, g, b, 1.0);
    }

    // Anaglyph red-cyan deghosted (iaian7 / vectorform)
    if (m == kAnaglyphRedCyanDeghosted) {
        float Contrast = 1.0;
        float DeGhost  = 0.06 * 0.1;
        float contrast = (Contrast * 0.5) + 0.5;
        float LOne = contrast * 0.45;
        float ROne = contrast;

        vec4 image = vec4(0.0, 0.0, 0.0, 1.0);
        vec4 accum;
        accum = clamp(cA * vec4(LOne, (1.0 - LOne) * 0.5, (1.0 - LOne) * 0.5, 1.0), 0.0, 1.0);
        image.r = pow(accum.r + accum.g + accum.b, 1.00);
        accum = clamp(cB * vec4(1.0 - ROne, ROne, 0.0, 1.0), 0.0, 1.0);
        image.g = pow(accum.r + accum.g + accum.b, 1.15);
        accum = clamp(cB * vec4(1.0 - ROne, 0.0, ROne, 1.0), 0.0, 1.0);
        image.b = pow(accum.r + accum.g + accum.b, 1.15);

        accum = image;
        image.r = accum.r + (accum.r * DeGhost)           + (accum.g * (DeGhost * -0.5))  + (accum.b * (DeGhost * -0.5));
        image.g = accum.g + (accum.r * (DeGhost * -0.25)) + (accum.g * (DeGhost * 0.5))   + (accum.b * (DeGhost * -0.25));
        image.b = accum.b + (accum.r * (DeGhost * -0.25)) + (accum.g * (DeGhost * -0.25)) + (accum.b * (DeGhost * 0.5));
        image.a = 1.0;
        return image;
    }

    // Anaglyph red-cyan compromise. The HLSL uses mul(row_matrix, rgb) —
    // written out here as explicit row dot products to avoid GLSL's
    // column-major mat3 constructor changing the meaning.
    if (m == kAnaglyphRedCyanCompromise) {
        float r = dot(vec3( 0.439,  0.447,  0.148), cA.rgb);
        float g = dot(vec3( 0.095,  0.934, -0.005), cB.rgb);
        float b = dot(vec3(-0.018, -0.028,  1.057), cB.rgb);
        return vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
    }

    // Anaglyph green-magenta (simple)
    if (m == kAnaglyphGreenMagenta) {
        return vec4(cB.r, cA.g, cB.b, 1.0);
    }

    // Anaglyph green-magenta Dubois
    if (m == kAnaglyphGreenMagentaDubois) {
        float r = clamp(-0.062 * cA.r - 0.158 * cA.g - 0.039 * cA.b
                        + 0.529 * cB.r + 0.705 * cB.g + 0.024 * cB.b, 0.0, 1.0);
        float g = clamp( 0.284 * cA.r + 0.668 * cA.g + 0.143 * cA.b
                        - 0.016 * cB.r - 0.015 * cB.g + 0.065 * cB.b, 0.0, 1.0);
        float b = clamp(-0.015 * cA.r - 0.027 * cA.g + 0.021 * cA.b
                        + 0.009 * cB.r + 0.075 * cB.g + 0.937 * cB.b, 0.0, 1.0);
        return vec4(r, g, b, 1.0);
    }

    // Anaglyph green-magenta deghosted
    if (m == kAnaglyphGreenMagentaDeghosted) {
        float Contrast = 1.0;
        float DeGhost  = 0.06 * 0.275;
        float contrast = (Contrast * 0.5) + 0.5;
        float LOne = contrast * 0.45;
        float ROne = contrast * 0.8;

        vec4 image = vec4(0.0, 0.0, 0.0, 1.0);
        vec4 accum;
        accum = clamp(cB * vec4(ROne, 1.0 - ROne, 0.0, 1.0), 0.0, 1.0);
        image.r = pow(accum.r + accum.g + accum.b, 1.15);
        accum = clamp(cA * vec4((1.0 - LOne) * 0.5, LOne, (1.0 - LOne) * 0.5, 1.0), 0.0, 1.0);
        image.g = pow(accum.r + accum.g + accum.b, 1.05);
        accum = clamp(cB * vec4(0.0, 1.0 - ROne, ROne, 1.0), 0.0, 1.0);
        image.b = pow(accum.r + accum.g + accum.b, 1.15);

        accum = image;
        image.r = accum.r + (accum.r * (DeGhost * 0.5))   + (accum.g * (DeGhost * -0.25)) + (accum.b * (DeGhost * -0.25));
        image.g = accum.g + (accum.r * (DeGhost * -0.5))  + (accum.g * (DeGhost * 0.25))  + (accum.b * (DeGhost * -0.5));
        image.b = accum.b + (accum.r * (DeGhost * -0.25)) + (accum.g * (DeGhost * -0.25)) + (accum.b * (DeGhost * 0.5));
        image.a = 1.0;
        return image;
    }

    // Anaglyph blue-amber (ColorCode style)
    if (m == kAnaglyphBlueAmber) {
        float Contrast = 1.0;
        float DeGhost  = 0.06 * 0.275;
        float contrast = (Contrast * 0.5) + 0.5;
        float LOne = contrast * 0.45;
        float ROne = contrast;

        vec4 image = vec4(0.0, 0.0, 0.0, 1.0);
        vec4 accum;
        accum = clamp(cA * vec4(ROne, 0.0, 1.0 - ROne, 1.0), 0.0, 1.0);
        image.r = pow(accum.r + accum.g + accum.b, 1.05);
        accum = clamp(cA * vec4(0.0, ROne, 1.0 - ROne, 1.0), 0.0, 1.0);
        image.g = pow(accum.r + accum.g + accum.b, 1.10);
        accum = clamp(cB * vec4((1.0 - LOne) * 0.5, (1.0 - LOne) * 0.5, LOne, 1.0), 0.0, 1.0);
        image.b = pow(accum.r + accum.g + accum.b, 1.0);
        image.b = mix(pow(image.b, (DeGhost * 0.15) + 1.0),
                      1.0 - pow(abs(1.0 - image.b), (DeGhost * 0.15) + 1.0),
                      image.b);

        accum = image;
        image.r = accum.r + (accum.r * (DeGhost * 1.5))   + (accum.g * (DeGhost * -0.75)) + (accum.b * (DeGhost * -0.75));
        image.g = accum.g + (accum.r * (DeGhost * -0.75)) + (accum.g * (DeGhost * 1.5))   + (accum.b * (DeGhost * -0.75));
        image.b = accum.b + (accum.r * (DeGhost * -1.5))  + (accum.g * (DeGhost * -1.5))  + (accum.b * (DeGhost * 3.0));
        return clamp(image, 0.0, 1.0);
    }

    // Fallback (mirrors the HLSL debug output)
    return vec4(uv, 0.0, 1.0);
}

void main() {
    vec4 c = Repack();
    if (pc.correction_enabled != 0) {
        c.rgb = ApplyCorrection(c.rgb);
    }
    out_color = c;
}
