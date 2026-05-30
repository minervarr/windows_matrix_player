#pragma once
#include <windows.h>

// Rasterize an SVG string to an HBITMAP with premultiplied alpha.
// The SVG is parsed, all fill/stroke colors are overridden to `color`,
// then rasterized at `size x size` pixels.
// Caller owns the returned HBITMAP (DeleteObject when done).
// Returns nullptr on failure.
HBITMAP rasterizeSvgIcon(const char* svgStr, int size, COLORREF color);

// Pre-defined SVG icon strings — edit the path data to reshape icons.
// Designed for a 36x36 viewBox (transport buttons) or 24x24 (close button).

extern const char* SVG_PLAY;
extern const char* SVG_PAUSE;
extern const char* SVG_STOP;
extern const char* SVG_PREV;
extern const char* SVG_NEXT;
extern const char* SVG_CLOSE;
