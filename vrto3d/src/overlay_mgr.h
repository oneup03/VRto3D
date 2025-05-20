#pragma once

#include <windows.h>
#include <string>

// Initialize GDI+ (must be called once before any drawing)
void InitGDIPlus();

// Draw text at (x, y) in the given HWND
void DrawOverlayText(HWND hwnd, const std::string& text, int x, int y);
