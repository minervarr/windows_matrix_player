#include "artwork.h"
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

static ULONG_PTR gdiplusToken = 0;

static void ensureGdiPlus() {
    if (gdiplusToken) return;
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&gdiplusToken, &in, nullptr);
}

void clearArtworkCache() {
    if (gdiplusToken) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
        gdiplusToken = 0;
    }
}

HBITMAP loadArtwork(const std::string& path, int targetW, int targetH) {
    if (path.empty()) return nullptr;
    ensureGdiPlus();

    // Convert to wide string for GDI+
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);

    Gdiplus::Bitmap* src = Gdiplus::Bitmap::FromFile(wpath.c_str());
    if (!src || src->GetLastStatus() != Gdiplus::Ok) {
        delete src;
        return nullptr;
    }

    // Scale preserving aspect ratio
    int srcW = (int)src->GetWidth();
    int srcH = (int)src->GetHeight();
    float scaleX = (float)targetW / srcW;
    float scaleY = (float)targetH / srcH;
    float scale  = scaleX < scaleY ? scaleX : scaleY;
    int dstW = (int)(srcW * scale);
    int dstH = (int)(srcH * scale);

    HDC hdc = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP hbm = CreateCompatibleBitmap(hdc, dstW, dstH);
    HBITMAP old = (HBITMAP)SelectObject(memDC, hbm);

    Gdiplus::Graphics g(memDC);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.DrawImage(src, 0, 0, dstW, dstH);

    SelectObject(memDC, old);
    DeleteDC(memDC);
    ReleaseDC(nullptr, hdc);
    delete src;

    return hbm;
}
