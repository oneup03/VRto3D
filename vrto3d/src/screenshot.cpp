/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "screenshot.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <wincodec.h>
#  include <shlobj.h>
#  include <wrl/client.h>
#  pragma comment(lib, "windowscodecs.lib")
#  pragma comment(lib, "shell32.lib")
#endif

#include <cmath>
#include <cstdio>
#include <vector>

#include "vrto3dlib/debug_log.hpp"
#include "vrto3dlib/win32_helper.hpp"   // GetSteamInstallPath

namespace vrto3d::screenshot {

#ifdef _WIN32
namespace {

// Map an 8-bit DXGI format to its WIC pixel-format GUID. Returns nullptr for
// formats we don't handle (HDR, 10-bit, etc.) — caller logs and skips.
const GUID* WicGuidForDxgi(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return &GUID_WICPixelFormat32bppRGBA;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return &GUID_WICPixelFormat32bppBGRA;
        default:
            return nullptr;
    }
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring SanitizeFilename(std::wstring s) {
    for (auto& c : s) {
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    }
    if (s.empty()) s = L"vrto3d";
    return s;
}

void EnsureCoInitOnThread() {
    static thread_local bool inited = false;
    if (!inited) {
        // Either apartment works for WIC; ignore RPC_E_CHANGED_MODE / S_FALSE.
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        inited = true;
    }
}

// Build a tightly-packed (row_pitch == width*4) BGRA8 buffer from a source SBS
// image, scaled so the *per-eye* aspect ratio equals `target_eye_aspect`. Only
// enlarges: whichever dimension is "too small" gets stretched up; the other
// dimension is left at full source resolution. Returns false on failure.
//
// Output dims are written to *out_w / *out_h. The buffer layout stays SBS:
// the left half occupies cols [0, out_w/2) and the right half [out_w/2, out_w).
bool ScaleSbsToBgra(IWICImagingFactory* factory,
                    UINT src_w, UINT src_h, const GUID& src_format,
                    const BYTE* src_data, UINT src_row_pitch,
                    float target_eye_aspect,
                    std::vector<BYTE>& out_buf,
                    UINT* out_w, UINT* out_h) {
    if (src_w < 2 || (src_w & 1u) != 0 || src_h == 0) return false;

    const UINT src_eye_w = src_w / 2;
    const float src_eye_aspect = static_cast<float>(src_eye_w) / static_cast<float>(src_h);

    // Target SBS dims: enlarge the smaller axis only.
    UINT dst_eye_w = src_eye_w;
    UINT dst_h     = src_h;
    if (target_eye_aspect > 0.0f) {
        if (src_eye_aspect > target_eye_aspect) {
            // Source is wider than target → stretch height up.
            dst_h = static_cast<UINT>(std::lround(static_cast<float>(src_eye_w) / target_eye_aspect));
            if (dst_h < src_h) dst_h = src_h;  // never shrink
        } else if (src_eye_aspect < target_eye_aspect) {
            // Source is narrower than target → stretch per-eye width up.
            dst_eye_w = static_cast<UINT>(std::lround(static_cast<float>(src_h) * target_eye_aspect));
            if (dst_eye_w < src_eye_w) dst_eye_w = src_eye_w;
        }
    }
    const UINT dst_w = dst_eye_w * 2;

    Microsoft::WRL::ComPtr<IWICBitmap> src_bmp;
    HRESULT hr = factory->CreateBitmapFromMemory(src_w, src_h, src_format,
                                                 src_row_pitch,
                                                 src_row_pitch * src_h,
                                                 const_cast<BYTE*>(src_data),
                                                 &src_bmp);
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IWICBitmapSource> source = src_bmp;

    if (!IsEqualGUID(src_format, GUID_WICPixelFormat32bppBGRA)) {
        Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
        if (FAILED(factory->CreateFormatConverter(&conv))) return false;
        if (FAILED(conv->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeMedianCut))) return false;
        source = conv;
    }

    if (dst_w != src_w || dst_h != src_h) {
        Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler))) return false;
        if (FAILED(scaler->Initialize(source.Get(), dst_w, dst_h,
                                      WICBitmapInterpolationModeHighQualityCubic))) return false;
        source = scaler;
    }

    const UINT dst_pitch = dst_w * 4;
    out_buf.assign(static_cast<size_t>(dst_pitch) * dst_h, 0);
    if (FAILED(source->CopyPixels(nullptr, dst_pitch,
                                  static_cast<UINT>(out_buf.size()),
                                  out_buf.data()))) {
        return false;
    }
    *out_w = dst_w;
    *out_h = dst_h;
    return true;
}

bool EncodePngToFile(IWICImagingFactory* factory,
                     const std::wstring& path,
                     UINT width, UINT height,
                     const GUID& src_format,
                     const BYTE* data, UINT row_pitch) {
    Microsoft::WRL::ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return false;
    HRESULT hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        LOG() << "Screenshot: open '" << path.c_str() << "' hr=0x" << std::hex << hr;
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> options;
    if (FAILED(encoder->CreateNewFrame(&frame, &options))) return false;
    if (FAILED(frame->Initialize(options.Get()))) return false;
    if (FAILED(frame->SetSize(width, height))) return false;

    GUID requested = src_format;
    if (FAILED(frame->SetPixelFormat(&requested))) return false;

    if (IsEqualGUID(requested, src_format)) {
        if (FAILED(hr = frame->WritePixels(height, row_pitch,
                                           row_pitch * height,
                                           const_cast<BYTE*>(data)))) {
            LOG() << "Screenshot: WritePixels hr=0x" << std::hex << hr;
            return false;
        }
    } else {
        Microsoft::WRL::ComPtr<IWICBitmap> bmp;
        if (FAILED(factory->CreateBitmapFromMemory(width, height, src_format,
                                                   row_pitch, row_pitch * height,
                                                   const_cast<BYTE*>(data), &bmp))) return false;
        Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
        if (FAILED(factory->CreateFormatConverter(&conv))) return false;
        if (FAILED(conv->Initialize(bmp.Get(), requested,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeMedianCut))) return false;
        if (FAILED(frame->WriteSource(conv.Get(), nullptr))) return false;
    }

    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit());
}

// Pick the lowest 4-digit index N such that neither
// "<base>_NNNN.png" nor "<base>_NNNN_crossview.png" exists in `dir`.
int FindNextScreenshotIndex(const std::wstring& dir, const std::wstring& base) {
    for (int i = 1; i < 10000; ++i) {
        wchar_t suf[16];
        swprintf_s(suf, L"_%04d.png", i);
        std::wstring p1 = dir + L"\\" + base + suf;
        wchar_t xv_suf[32];
        swprintf_s(xv_suf, L"_%04d_crossview.png", i);
        std::wstring p2 = dir + L"\\" + base + xv_suf;
        if (GetFileAttributesW(p1.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(p2.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return i;
        }
    }
    return -1;
}

}  // namespace
#endif // _WIN32


SaveResult SaveStereoPair(const std::string& app_name,
                          uint32_t            sbs_width,
                          uint32_t            sbs_height,
                          DXGI_FORMAT         dxgi_format,
                          const void*         data,
                          uint32_t            row_pitch,
                          float               target_eye_aspect)
{
    SaveResult r;
#ifdef _WIN32
    if (!data || sbs_width < 2 || (sbs_width & 1u) != 0 || sbs_height == 0) {
        LOG() << "Screenshot: invalid input " << sbs_width << "x" << sbs_height;
        return r;
    }
    const GUID* wic_fmt = WicGuidForDxgi(dxgi_format);
    if (!wic_fmt) {
        LOG() << "Screenshot: unsupported texture format " << dxgi_format << " — skipping";
        return r;
    }

    std::string steam = GetSteamInstallPath();
    if (steam.empty()) {
        LOG() << "Screenshot: Steam install path not found";
        return r;
    }
    std::wstring wdir = Utf8ToWide(steam) + L"\\steamapps\\common\\SteamVR\\screenshots";
    SHCreateDirectoryExW(nullptr, wdir.c_str(), nullptr);
    r.dir = steam + "\\steamapps\\common\\SteamVR\\screenshots";

    std::wstring base = SanitizeFilename(Utf8ToWide(app_name));
    int idx = FindNextScreenshotIndex(wdir, base);
    if (idx < 0) {
        LOG() << "Screenshot: no free index available";
        return r;
    }
    r.index = idx;

    EnsureCoInitOnThread();

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        LOG() << "Screenshot: WIC factory create failed";
        return r;
    }

    std::vector<BYTE> bgra_buf;
    UINT out_w = 0, out_h = 0;
    if (!ScaleSbsToBgra(factory.Get(),
                        sbs_width, sbs_height, *wic_fmt,
                        static_cast<const BYTE*>(data), row_pitch,
                        target_eye_aspect,
                        bgra_buf, &out_w, &out_h)) {
        LOG() << "Screenshot: scale/convert failed";
        return r;
    }
    const UINT out_pitch  = out_w * 4;
    const UINT half_bytes = (out_w / 2) * 4;
    r.out_width  = out_w;
    r.out_height = out_h;

    // Normal (parallel-view).
    wchar_t suf[16];
    swprintf_s(suf, L"_%04d.png", idx);
    std::wstring p_normal = wdir + L"\\" + base + suf;
    bool ok1 = EncodePngToFile(factory.Get(), p_normal, out_w, out_h,
                               GUID_WICPixelFormat32bppBGRA,
                               bgra_buf.data(), out_pitch);

    // Cross-view: swap halves per row into a scratch buffer.
    std::vector<BYTE> swapped(bgra_buf.size());
    for (UINT y = 0; y < out_h; ++y) {
        const BYTE* src_row = bgra_buf.data() + y * out_pitch;
        BYTE*       dst_row = swapped.data() + y * out_pitch;
        memcpy(dst_row,              src_row + half_bytes, half_bytes);
        memcpy(dst_row + half_bytes, src_row,              half_bytes);
    }
    wchar_t xv_suf[32];
    swprintf_s(xv_suf, L"_%04d_crossview.png", idx);
    std::wstring p_cross = wdir + L"\\" + base + xv_suf;
    bool ok2 = EncodePngToFile(factory.Get(), p_cross, out_w, out_h,
                               GUID_WICPixelFormat32bppBGRA,
                               swapped.data(), out_pitch);

    r.ok = ok1 && ok2;
    if (r.ok) {
        LOG() << "Screenshot: saved index " << idx << " for '" << app_name << "' "
              << sbs_width << "x" << sbs_height << " -> " << out_w << "x" << out_h
              << " (eye aspect target=" << target_eye_aspect << ") to " << r.dir;
    } else {
        LOG() << "Screenshot: encode failed (normal=" << ok1 << " crossview=" << ok2 << ")";
    }
#else
    (void)app_name; (void)sbs_width; (void)sbs_height; (void)dxgi_format;
    (void)data; (void)row_pitch; (void)target_eye_aspect;
#endif
    return r;
}

}  // namespace vrto3d::screenshot
