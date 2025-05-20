#include "overlay_mgr.h"
#include <string>
#include <gdiplus.h>
#pragma comment (lib, "Gdiplus.lib")

using namespace Gdiplus;

static ULONG_PTR g_gdiplusToken = 0;

void InitGDIPlus() {
    if (g_gdiplusToken != 0) return;  // Already initialized

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr);
}

void DrawOverlayText(HWND hwnd, const std::string& text, int x, int y) {
    if (!g_gdiplusToken) return;

    HDC hdc = GetDC(hwnd);
    if (!hdc) return;

    Graphics graphics(hdc);

    FontFamily fontFamily(L"Segoe UI");
    Font font(&fontFamily, 18, FontStyleRegular, UnitPixel);
    SolidBrush brush(Color(255, 255, 255, 255));

    graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);
    std::wstring wtext = std::wstring(text.begin(), text.end());;
    graphics.DrawString(wtext.c_str(), -1, &font, PointF((REAL)x, (REAL)y), &brush);

    ReleaseDC(hwnd, hdc);
}