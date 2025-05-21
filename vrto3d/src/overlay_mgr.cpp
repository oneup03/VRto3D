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
#include "overlay_mgr.h"
#include <string>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment (lib, "Gdiplus.lib")

using namespace Gdiplus;

static ULONG_PTR g_gdiplusToken = 0;


//-----------------------------------------------------------------------------
// Purpose: Initialize GDI library
//-----------------------------------------------------------------------------
void InitGDIPlus() {
    if (g_gdiplusToken != 0) return;  // Already initialized

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr);
}


//-----------------------------------------------------------------------------
// Purpose: Draw text in the lower left corner of the VR window
//-----------------------------------------------------------------------------
void DrawOverlayText(HWND hwnd, const std::string& text, int height) {
    if (!g_gdiplusToken) return;

    HDC hdc = GetDC(hwnd);
    if (!hdc) return;

    Graphics graphics(hdc);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);

    FontFamily fontFamily(L"Segoe UI");
    Font font(&fontFamily, 30, FontStyleRegular, UnitPixel);
    SolidBrush brush(Color(255, 0, 255, 0));  // Bright green

    std::wstring wtext(text.begin(), text.end());

    graphics.DrawString(wtext.c_str(), -1, &font, PointF(50, height - 70), &brush);

    ReleaseDC(hwnd, hdc);
}
