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

// Fullscreen triangle from gl_VertexIndex — no vertex buffer.
//
// UV convention: Vulkan clip space is Y-down (NDC y = -1 is the TOP of the
// viewport), so vertex 0 at (-1,-1) is the top-left corner of the output
// window and uv = (pos + 1) / 2 yields uv == (0,0) there. uv.y therefore
// increases downward — uv (0,0) = top-left of the OUTPUT window, matching
// both the D3D11 presenter's texcoord convention and the sbs texture's
// row order (v = 0 samples the top row). Do NOT flip uv.y.
//
//   gl_VertexIndex 0 -> pos (-1,-1)  uv (0,0)
//   gl_VertexIndex 1 -> pos ( 3,-1)  uv (2,0)
//   gl_VertexIndex 2 -> pos (-1, 3)  uv (0,2)

layout(location = 0) out vec2 uv;

void main() {
    vec2 pos = vec2((gl_VertexIndex == 1) ? 3.0 : -1.0,
                    (gl_VertexIndex == 2) ? 3.0 : -1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
    uv = (pos + 1.0) * 0.5;
}
