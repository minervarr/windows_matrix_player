#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#include "icons.h"
#include <cstring>
#include <cstdlib>

// ── SVG icon definitions ─────────────────────────────────────────────────────
// Edit the path `d=` attributes or rect coordinates to reshape icons.
// viewBox is 36x36 for transport buttons, 24x24 for close.
// All shapes use fill='white' — the actual color is overridden at rasterize time.

const char* SVG_PLAY =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 36 36'>"
    "<path d='M13,7 L13,29 L28,18 Z' fill='white'/>"
    "</svg>";

const char* SVG_PAUSE =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 36 36'>"
    "<rect x='9' y='8' width='6' height='20' rx='1' fill='white'/>"
    "<rect x='21' y='8' width='6' height='20' rx='1' fill='white'/>"
    "</svg>";

const char* SVG_STOP =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 36 36'>"
    "<rect x='10' y='10' width='16' height='16' rx='2' fill='white'/>"
    "</svg>";

const char* SVG_PREV =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 36 36'>"
    "<rect x='6' y='10' width='3' height='16' rx='1' fill='white'/>"
    "<path d='M27,8 L14,18 L27,28 Z' fill='white'/>"
    "</svg>";

const char* SVG_NEXT =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 36 36'>"
    "<path d='M9,8 L22,18 L9,28 Z' fill='white'/>"
    "<rect x='27' y='10' width='3' height='16' rx='1' fill='white'/>"
    "</svg>";

const char* SVG_CLOSE =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
    "<path d='M6,6 L18,18 M18,6 L6,18' fill='none' stroke='white' stroke-width='2.5' stroke-linecap='round'/>"
    "</svg>";


// ── Rasterization ────────────────────────────────────────────────────────────

HBITMAP rasterizeSvgIcon(const char* svgStr, int size, COLORREF color) {
    if (!svgStr || size <= 0) return nullptr;

    // nsvgParse modifies the input string, so we need a mutable copy
    size_t len = strlen(svgStr);
    char* buf = (char*)malloc(len + 1);
    if (!buf) return nullptr;
    memcpy(buf, svgStr, len + 1);

    NSVGimage* image = nsvgParse(buf, "px", 96.0f);
    free(buf);
    if (!image) return nullptr;

    // Override all shape colors to the requested COLORREF
    unsigned char r = GetRValue(color);
    unsigned char g = GetGValue(color);
    unsigned char b = GetBValue(color);
    unsigned int nsvgColor = (r) | (g << 8) | (b << 16) | (0xFF << 24);

    for (NSVGshape* shape = image->shapes; shape; shape = shape->next) {
        if (shape->fill.type != NSVG_PAINT_NONE) {
            shape->fill.type = NSVG_PAINT_COLOR;
            shape->fill.color = nsvgColor;
        }
        if (shape->stroke.type != NSVG_PAINT_NONE) {
            shape->stroke.type = NSVG_PAINT_COLOR;
            shape->stroke.color = nsvgColor;
        }
    }

    // Rasterize to RGBA buffer
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(image); return nullptr; }

    unsigned char* rgba = (unsigned char*)malloc(size * size * 4);
    if (!rgba) { nsvgDeleteRasterizer(rast); nsvgDelete(image); return nullptr; }

    float scale = (float)size / (image->width > image->height ? image->width : image->height);
    float offsetX = (size - image->width * scale) / 2.0f;
    float offsetY = (size - image->height * scale) / 2.0f;

    nsvgRasterize(rast, image, offsetX, offsetY, scale, rgba, size, size, size * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    // Convert RGBA to premultiplied BGRA for AlphaBlend
    for (int i = 0; i < size * size; i++) {
        unsigned char* px = rgba + i * 4;
        unsigned char a = px[3];
        unsigned char sr = px[0], sg = px[1], sb = px[2];
        // Premultiply and swap R/B for BGRA
        px[0] = (unsigned char)(sb * a / 255);  // B
        px[1] = (unsigned char)(sg * a / 255);  // G
        px[2] = (unsigned char)(sr * a / 255);  // R
        px[3] = a;
    }

    // Create a 32-bit DIB section HBITMAP
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = size;
    bmi.bmiHeader.biHeight      = -size; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbm && bits) {
        memcpy(bits, rgba, size * size * 4);
    }

    free(rgba);
    return hbm;
}
