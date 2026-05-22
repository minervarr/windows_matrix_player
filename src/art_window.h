#pragma once
#include <windows.h>
#include <string>

// Separate fullscreen artwork window — mirrors Android's ArtworkActivity.
// Works great on dual-monitor setups: show art on one screen, controls on the other.
// TODO(style): Add keep-screen-on (SetThreadExecutionState) like ArtworkActivity.
// TODO(style): Add click-to-close or press Escape to dismiss.

class ArtWindow {
public:
    bool create(HINSTANCE hInst);
    void show(const std::string& imagePath, HMONITOR preferMonitor = nullptr);
    void hide();
    bool isVisible() const;

    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    void onPaint();

    HWND    hwnd_      = nullptr;
    HBITMAP artBitmap_ = nullptr;
    std::string currentPath_;
};
