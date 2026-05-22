#include "art_window.h"
#include "artwork.h"
#include <cstring>

static const wchar_t* ART_CLASS = L"MatrixArtWindow";

bool ArtWindow::create(HINSTANCE hInst) {
    WNDCLASSEXW wc = {};
    wc.cbSize       = sizeof(wc);
    wc.lpfnWndProc  = wndProc;
    wc.hInstance    = hInst;
    wc.hbrBackground= (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName= ART_CLASS;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, ART_CLASS, L"Album Art",
        WS_POPUP, 0, 0, 800, 800, nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
    return true;
}

void ArtWindow::show(const std::string& imagePath, HMONITOR preferMonitor) {
    currentPath_ = imagePath;

    // Pick monitor — use preferMonitor or primary if null
    HMONITOR mon = preferMonitor;
    if (!mon) mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);

    int w = mi.rcMonitor.right  - mi.rcMonitor.left;
    int h = mi.rcMonitor.bottom - mi.rcMonitor.top;

    if (artBitmap_) { DeleteObject(artBitmap_); artBitmap_ = nullptr; }
    artBitmap_ = loadArtwork(imagePath, w, h);

    SetWindowPos(hwnd_, HWND_TOP,
        mi.rcMonitor.left, mi.rcMonitor.top, w, h, SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void ArtWindow::hide() {
    ShowWindow(hwnd_, SW_HIDE);
}

bool ArtWindow::isVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void ArtWindow::onPaint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc; GetClientRect(hwnd_, &rc);
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (artBitmap_) {
        HDC memDC = CreateCompatibleDC(hdc);
        BITMAP bm; GetObject(artBitmap_, sizeof(bm), &bm);
        HBITMAP old = (HBITMAP)SelectObject(memDC, artBitmap_);
        // Center on black background
        int x = (rc.right  - bm.bmWidth)  / 2;
        int y = (rc.bottom - bm.bmHeight) / 2;
        BitBlt(hdc, x, y, bm.bmWidth, bm.bmHeight, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, old);
        DeleteDC(memDC);
    }
    EndPaint(hwnd_, &ps);
}

LRESULT CALLBACK ArtWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ArtWindow* self = (ArtWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_PAINT:
            if (self) self->onPaint();
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && self) self->hide();
            return 0;
        case WM_LBUTTONDBLCLK:
            if (self) self->hide();
            return 0;
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
