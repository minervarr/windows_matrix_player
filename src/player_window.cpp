#include "player_window.h"
#include "log_util.h"
#include "artwork.h"
#include "icons.h"
#include <windowsx.h>
#include <commdlg.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "msimg32.lib")

#include <soxr.h>

static int pickOutputRate(int inRate, const std::vector<int>& supported) {
    for (int r : supported)
        if (r % inRate == 0) return r;
    for (int r : supported)
        if (r > inRate) return r;
    return supported.empty() ? 48000 : supported.back();
}

static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

static HFONT createFont(int size, int weight = FW_NORMAL) {
    return CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

static void drawCenteredText(HDC hdc, const RECT& rc, const wchar_t* text, HFONT font, COLORREF color) {
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, color);
    DrawTextW(hdc, text, -1, const_cast<RECT*>(&rc), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, old);
}

static void fillRect(HDC hdc, const RECT& rc, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    FillRect(hdc, &rc, br);
    DeleteObject(br);
}

static void drawIcon(HDC hdc, const RECT& rc, HBITMAP icon) {
    if (!icon) return;
    BITMAP bm; GetObject(icon, sizeof(bm), &bm);
    int x = rc.left + (rc.right - rc.left - bm.bmWidth) / 2;
    int y = rc.top  + (rc.bottom - rc.top - bm.bmHeight) / 2;
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP old = (HBITMAP)SelectObject(memDC, icon);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(hdc, x, y, bm.bmWidth, bm.bmHeight,
               memDC, 0, 0, bm.bmWidth, bm.bmHeight, bf);
    SelectObject(memDC, old);
    DeleteDC(memDC);
}

static const wchar_t* MAIN_CLASS = L"MatrixPlayerMain";

// ── Window creation ──────────────────────────────────────────────────────────

bool PlayerWindow::create(HINSTANCE hInst) {
    hInst_ = hInst;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = MAIN_CLASS;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(hInst, L"IDI_APPICON");
    wc.hIconSm       = LoadIconW(hInst, L"IDI_APPICON");
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(0, MAIN_CLASS, L"Matrix Player",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1200, 700,
        nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    // Create fonts
    hFontBrand_           = createFont(13, FW_BOLD);
    hFontSidebar_         = createFont(13);
    hFontSidebarActive_   = createFont(13, FW_SEMIBOLD);
    hFontAlbumTitle_      = createFont(13, FW_SEMIBOLD);
    hFontArtist_          = createFont(11);
    hFontTrackRow_        = createFont(13);
    hFontTrackNumber_     = createFont(12);
    hFontTransportTitle_  = createFont(14, FW_SEMIBOLD);
    hFontTransportArtist_ = createFont(12);
    hFontTime_            = createFont(12);
    hFontNowPlaying_      = createFont(15, FW_SEMIBOLD);
    hFontSettingsItem_    = createFont(14);
    hFontPlaceholder_     = createFont(32);

    // Rasterize SVG icons at button size
    iconPlay_       = rasterizeSvgIcon(SVG_PLAY,  36, CLR_TEXT_PRIMARY);
    iconPause_      = rasterizeSvgIcon(SVG_PAUSE, 36, CLR_TEXT_PRIMARY);
    iconStop_       = rasterizeSvgIcon(SVG_STOP,  36, CLR_TEXT_PRIMARY);
    iconPrev_       = rasterizeSvgIcon(SVG_PREV,  36, CLR_TEXT_PRIMARY);
    iconNext_       = rasterizeSvgIcon(SVG_NEXT,  36, CLR_TEXT_PRIMARY);
    iconClose_      = rasterizeSvgIcon(SVG_CLOSE, 24, CLR_TEXT_SECONDARY);
    iconPlayGreen_  = rasterizeSvgIcon(SVG_PLAY,  36, CLR_ACCENT);
    iconPauseGreen_ = rasterizeSvgIcon(SVG_PAUSE, 36, CLR_ACCENT);

    // Create brushes
    hbrMain_        = CreateSolidBrush(CLR_BG_MAIN);
    hbrSidebar_     = CreateSolidBrush(CLR_BG_SIDEBAR);
    hbrTransport_   = CreateSolidBrush(CLR_BG_TRANSPORT);
    hbrTrackPanel_  = CreateSolidBrush(CLR_BG_TRACKPANEL);
    hbrHover_       = CreateSolidBrush(CLR_HOVER);
    hbrPlaceholder_ = CreateSolidBrush(CLR_TILE_PLACEHOLDER);

    artWin_.create(hInst);
    recalcLayout();

    // Open DB
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dbPathW = exePath;
    dbPathW = dbPathW.substr(0, dbPathW.rfind(L'\\') + 1) + L"matrix_player.db";
    int dbLen = WideCharToMultiByte(CP_UTF8, 0, dbPathW.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string dbPath(dbLen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, dbPathW.c_str(), -1, dbPath.data(), dbLen, nullptr, nullptr);
    if (!dbPath.empty() && dbPath.back() == '\0') dbPath.pop_back();
    db_.open(dbPath);

    // Restore library from DB
    {
        auto raw = db_.loadAlbums();
        for (auto& a : raw) {
            bool dup = false;
            for (auto& ex : albums_)
                if (ex.name == a.name && ex.artist == a.artist) { dup = true; break; }
            if (!dup) albums_.push_back(std::move(a));
        }
        if (!albums_.empty()) {
            auto allTracks = db_.loadTracks();
            for (auto& t : allTracks)
                for (auto& a : albums_)
                    if (a.name == t.album) { a.tracks.push_back(t); break; }
        }
    }

    // Load EQ profiles
    {
        std::wstring eqPathW = exePath;
        eqPathW = eqPathW.substr(0, eqPathW.rfind(L'\\') + 1) + L"eq_profiles.json";
        int eqLen = WideCharToMultiByte(CP_UTF8, 0, eqPathW.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string eqPath(eqLen, '\0');
        WideCharToMultiByte(CP_UTF8, 0, eqPathW.c_str(), -1, eqPath.data(), eqLen, nullptr, nullptr);
        if (!eqPath.empty() && eqPath.back() == '\0') eqPath.pop_back();
        eqProfiles_.load(eqPath);
    }

    setupWatchers();
    startBackgroundScan();

    // Load audio mode
    bitperfectMode_.store(db_.loadSetting("audio_mode") == "bitperfect");

    // Load audio backend
    useWasapi_ = (db_.loadSetting("audio_backend") == "wasapi");
    wasapiMode_ = (db_.loadSetting("wasapi_mode") == "exclusive")
                  ? WasapiMode::Exclusive : WasapiMode::Shared;
    auto devIdUtf8 = db_.loadSetting("wasapi_device_id");
    wasapiDeviceId_ = utf8ToWide(devIdUtf8);

    if (useWasapi_) {
        output_ = std::make_unique<WasapiOutput>(wasapiDeviceId_, wasapiMode_);
        printf("[Audio] WASAPI backend selected (%s mode)\n",
               wasapiMode_ == WasapiMode::Exclusive ? "exclusive" : "shared");
    } else {
        auto vidStr = db_.loadSetting("usb_vid");
        auto pidStr = db_.loadSetting("usb_pid");
        uint16_t vid = vidStr.empty() ? (uint16_t)0x32BB : (uint16_t)strtoul(vidStr.c_str(), nullptr, 16);
        uint16_t pid = pidStr.empty() ? (uint16_t)0x0004 : (uint16_t)strtoul(pidStr.c_str(), nullptr, 16);

        usbOpen_ = usbDriver_.open(vid, pid);
        if (usbOpen_) {
            usbDriver_.parseDescriptors();
            auto rates = usbDriver_.getOutputRates();
            printf("[USB] DAC opened (VID=%04X PID=%04X). Supported rates:", vid, pid);
            for (int r : rates) printf(" %d", r);
            printf("\n");
        } else {
            printf("[USB] Failed to open DAC VID=%04X PID=%04X\n", vid, pid);
            wchar_t msgBuf[512];
            swprintf_s(msgBuf, sizeof(msgBuf)/sizeof(wchar_t),
                L"USB DAC not found (VID=%04X PID=%04X).\n\n"
                L"Steps to fix:\n"
                L"1. Open Zadig\n"
                L"2. Select your USB DAC interface MI_00\n"
                L"3. Install libusbK driver\n"
                L"4. Restart this app\n\n"
                L"Use Audio Settings to select a different device\n"
                L"or switch to WASAPI.", vid, pid);
            MessageBoxW(hwnd_, msgBuf, L"USB DAC not found", MB_OK | MB_ICONWARNING);
        }
        output_ = std::make_unique<UsbAudioOutput>(usbDriver_);
    }

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

void PlayerWindow::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ── Layout ───────────────────────────────────────────────────────────────────

void PlayerWindow::recalcLayout() {
    RECT rc; GetClientRect(hwnd_, &rc);
    int W = rc.right, H = rc.bottom;
    int transportH = 72;
    int sidebarW = 170;
    int trackPanelW = 300;

    rcTransport_ = { 0, H - transportH, W, H };
    rcSidebar_   = { 0, 0, sidebarW, H - transportH };

    if (trackPanelOpen_ && W > sidebarW + trackPanelW + 200) {
        rcTrackPanel_ = { W - trackPanelW, 0, W, H - transportH };
        rcGrid_       = { sidebarW, 0, W - trackPanelW, H - transportH };
    } else if (trackPanelOpen_) {
        trackPanelOpen_ = false;
        rcTrackPanel_ = { 0, 0, 0, 0 };
        rcGrid_       = { sidebarW, 0, W, H - transportH };
    } else {
        rcTrackPanel_ = { 0, 0, 0, 0 };
        rcGrid_       = { sidebarW, 0, W, H - transportH };
    }

    // Grid columns
    int gridW = rcGrid_.right - rcGrid_.left - gridPadX_ * 2;
    gridCols_ = std::max(1, gridW / gridTileSize_);
    int albumRows = ((int)albums_.size() + gridCols_ - 1) / gridCols_;
    gridTotalHeight_ = albumRows * (gridTileSize_ + 40) + gridPadY_;

    // Sidebar items
    rcBrand_       = { 0, 0, sidebarW, 50 };
    rcNavAlbums_   = { 0, 56, sidebarW, 96 };
    rcNavSettings_ = { 0, 96, sidebarW, 136 };

    // Transport sub-regions
    int tTop = rcTransport_.top;
    rcTransportArt_  = { 12, tTop + 11, 62, tTop + 61 };
    rcTransportInfo_ = { 72, tTop + 14, 260, tTop + 58 };

    // Center buttons
    int btnSize = 36;
    int btnGap = 10;
    int totalBtnW = btnSize * 4 + btnGap * 3;
    int btnX = W / 2 - totalBtnW / 2;
    int btnY = tTop + (transportH - btnSize) / 2;
    rcBtnPrev_ = { btnX, btnY, btnX + btnSize, btnY + btnSize };
    btnX += btnSize + btnGap;
    rcBtnPlay_ = { btnX, btnY, btnX + btnSize, btnY + btnSize };
    btnX += btnSize + btnGap;
    rcBtnStop_ = { btnX, btnY, btnX + btnSize, btnY + btnSize };
    btnX += btnSize + btnGap;
    rcBtnNext_ = { btnX, btnY, btnX + btnSize, btnY + btnSize };

    // Time display (text only, no seekbar)
    rcTimeDisplay_ = { rcBtnNext_.right + 20, tTop + 10, W - 16, tTop + transportH - 10 };

    // Track panel close button (chevron in header)
    if (trackPanelOpen_) {
        rcTrackClose_ = { rcTrackPanel_.right - 36, rcTrackPanel_.top + 6,
                          rcTrackPanel_.right - 8,  rcTrackPanel_.top + 34 };
    }

    // Settings page items
    int settCx = (rcGrid_.left + rcGrid_.right) / 2;
    int settTop = 80;
    rcSettingsAddFolder_ = { settCx - 200, settTop,       settCx + 200, settTop + 50 };
    rcSettingsManage_    = { settCx - 200, settTop + 60,  settCx + 200, settTop + 110 };
    rcSettingsAudio_     = { settCx - 200, settTop + 120, settCx + 200, settTop + 170 };
    rcSettingsEq_        = { settCx - 200, settTop + 180, settCx + 200, settTop + 230 };
    rcSettingsBitperfect_= { settCx - 200, settTop + 240, settCx + 200, settTop + 290 };

}

// ── Art cache ────────────────────────────────────────────────────────────────

HBITMAP PlayerWindow::getGridArt(int albumIdx) {
    auto it = gridArtCache_.find(albumIdx);
    if (it != gridArtCache_.end()) return it->second;
    if (albumIdx < 0 || albumIdx >= (int)albums_.size()) return nullptr;
    HBITMAP bmp = loadArtwork(albums_[albumIdx].artPath, gridArtSize_, gridArtSize_);
    gridArtCache_[albumIdx] = bmp;
    return bmp;
}

void PlayerWindow::clearGridArtCache() {
    for (auto& [idx, bmp] : gridArtCache_)
        if (bmp) DeleteObject(bmp);
    gridArtCache_.clear();
}

void PlayerWindow::loadTrackPanelArt(int albumIdx) {
    if (trackPanelArtAlbum_ == albumIdx && trackPanelArtBmp_) return;
    if (trackPanelArtBmp_) { DeleteObject(trackPanelArtBmp_); trackPanelArtBmp_ = nullptr; }
    trackPanelArtAlbum_ = albumIdx;
    if (albumIdx >= 0 && albumIdx < (int)albums_.size())
        trackPanelArtBmp_ = loadArtwork(albums_[albumIdx].artPath, trackPanelArtSize_, trackPanelArtSize_);
}

void PlayerWindow::loadTransportArt(const std::string& artPath) {
    if (transportArtPath_ == artPath && transportArtBmp_) return;
    if (transportArtBmp_) { DeleteObject(transportArtBmp_); transportArtBmp_ = nullptr; }
    transportArtPath_ = artPath;
    if (!artPath.empty())
        transportArtBmp_ = loadArtwork(artPath, 50, 50);
}

// ── Painting ─────────────────────────────────────────────────────────────────

void PlayerWindow::paintSidebar(HDC hdc) {
    FillRect(hdc, &rcSidebar_, hbrSidebar_);

    // Separator
    RECT sep = { rcSidebar_.right - 1, rcSidebar_.top, rcSidebar_.right, rcSidebar_.bottom };
    fillRect(hdc, sep, CLR_SEPARATOR);

    // Brand
    SetBkMode(hdc, TRANSPARENT);
    RECT brandText = { 16, 12, rcSidebar_.right - 8, 42 };
    HFONT oldF = (HFONT)SelectObject(hdc, hFontBrand_);
    SetTextColor(hdc, CLR_ACCENT);
    DrawTextW(hdc, L"MATRIX PLAYER", -1, &brandText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Nav items
    struct NavItem { const wchar_t* label; RECT rc; int idx; };
    NavItem items[] = {
        { L"Albums",   rcNavAlbums_,   0 },
        { L"Settings", rcNavSettings_, 1 },
    };

    for (auto& item : items) {
        bool active = (activeNavItem_ == item.idx);
        bool hovered = (hoverSidebarItem_ == item.idx && !active);

        if (hovered) FillRect(hdc, &item.rc, hbrHover_);

        if (active) {
            RECT accent = { item.rc.left, item.rc.top + 6, item.rc.left + 2, item.rc.bottom - 6 };
            fillRect(hdc, accent, CLR_ACCENT);
            SelectObject(hdc, hFontSidebarActive_);
            SetTextColor(hdc, CLR_TEXT_PRIMARY);
        } else {
            SelectObject(hdc, hFontSidebar_);
            SetTextColor(hdc, CLR_TEXT_SECONDARY);
        }

        RECT textRc = { item.rc.left + 20, item.rc.top, item.rc.right, item.rc.bottom };
        DrawTextW(hdc, item.label, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(hdc, oldF);
}

void PlayerWindow::paintGrid(HDC hdc) {
    FillRect(hdc, &rcGrid_, hbrMain_);

    if (albums_.empty()) {
        SetBkMode(hdc, TRANSPARENT);
        RECT msgRc = rcGrid_;
        msgRc.top += 100;
        drawCenteredText(hdc, msgRc, L"No albums yet. Go to Settings to add a music folder.",
                         hFontSidebar_, CLR_TEXT_DIM);
        return;
    }

    // Clip to grid area
    HRGN clipRgn = CreateRectRgnIndirect(&rcGrid_);
    SelectClipRgn(hdc, clipRgn);

    SetBkMode(hdc, TRANSPARENT);

    int gridW = rcGrid_.right - rcGrid_.left;
    int tileSpaceW = gridW - gridPadX_ * 2;
    int tileStepX = gridCols_ > 1 ? tileSpaceW / gridCols_ : gridTileSize_;
    int tileStepY = gridTileSize_ + 40;

    int firstRow = std::max(0, gridScrollY_ / tileStepY);
    int gridH = rcGrid_.bottom - rcGrid_.top;
    int lastRow = (gridScrollY_ + gridH) / tileStepY + 1;

    for (int row = firstRow; row <= lastRow; row++) {
        for (int col = 0; col < gridCols_; col++) {
            int idx = row * gridCols_ + col;
            if (idx >= (int)albums_.size()) break;

            int x = rcGrid_.left + gridPadX_ + col * tileStepX + (tileStepX - gridArtSize_) / 2;
            int y = rcGrid_.top + gridPadY_ + row * tileStepY - gridScrollY_;

            // Hover highlight
            if (hoverAlbumIdx_ == idx) {
                RECT hoverRc = { x - 6, y - 6, x + gridArtSize_ + 6, y + gridArtSize_ + 42 };
                FillRect(hdc, &hoverRc, hbrHover_);
            }

            // Selected border
            if (selectedAlbumIdx_ == idx) {
                RECT selRc = { x - 3, y - 3, x + gridArtSize_ + 3, y + gridArtSize_ + 3 };
                HPEN pen = CreatePen(PS_SOLID, 2, CLR_ACCENT);
                HPEN oldPen = (HPEN)SelectObject(hdc, pen);
                HBRUSH oldBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, selRc.left, selRc.top, selRc.right, selRc.bottom);
                SelectObject(hdc, oldPen);
                SelectObject(hdc, oldBr);
                DeleteObject(pen);
            }

            // Art
            HBITMAP art = getGridArt(idx);
            if (art) {
                HDC memDC = CreateCompatibleDC(hdc);
                BITMAP bm; GetObject(art, sizeof(bm), &bm);
                HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, art);
                int artX = x + (gridArtSize_ - bm.bmWidth) / 2;
                int artY = y + (gridArtSize_ - bm.bmHeight) / 2;
                BitBlt(hdc, artX, artY, bm.bmWidth, bm.bmHeight, memDC, 0, 0, SRCCOPY);
                SelectObject(memDC, oldBmp);
                DeleteDC(memDC);
            } else {
                RECT placeholder = { x, y, x + gridArtSize_, y + gridArtSize_ };
                FillRect(hdc, &placeholder, hbrPlaceholder_);
                drawCenteredText(hdc, placeholder, L"♫", hFontPlaceholder_, CLR_TEXT_DIM);
            }

            // Album name
            RECT nameRc = { x, y + gridArtSize_ + 8, x + gridArtSize_, y + gridArtSize_ + 23 };
            std::wstring nameW(utf8ToWide(albums_[idx].name));
            HFONT oldF = (HFONT)SelectObject(hdc, hFontAlbumTitle_);
            SetTextColor(hdc, CLR_TEXT_ALBUM_TITLE);
            DrawTextW(hdc, nameW.c_str(), -1, &nameRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            // Artist name
            RECT artistRc = { x, y + gridArtSize_ + 25, x + gridArtSize_, y + gridArtSize_ + 38 };
            std::wstring artistW(utf8ToWide(albums_[idx].artist));
            SelectObject(hdc, hFontArtist_);
            SetTextColor(hdc, CLR_TEXT_SECONDARY);
            DrawTextW(hdc, artistW.c_str(), -1, &artistRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            SelectObject(hdc, oldF);
        }
    }

    SelectClipRgn(hdc, nullptr);
    DeleteObject(clipRgn);
}

void PlayerWindow::paintTrackPanel(HDC hdc) {
    FillRect(hdc, &rcTrackPanel_, hbrTrackPanel_);

    // Left separator
    RECT sep = { rcTrackPanel_.left, rcTrackPanel_.top, rcTrackPanel_.left + 1, rcTrackPanel_.bottom };
    fillRect(hdc, sep, CLR_SEPARATOR);

    SetBkMode(hdc, TRANSPARENT);

    int panelW = rcTrackPanel_.right - rcTrackPanel_.left;
    int cx = rcTrackPanel_.left + panelW / 2;
    int y = rcTrackPanel_.top + 12;

    // Large album art
    int artDisplaySize = std::min(trackPanelArtSize_, panelW - 24);
    int artRight = cx + artDisplaySize / 2;
    int artTop = y;
    if (trackPanelArtBmp_) {
        HDC memDC = CreateCompatibleDC(hdc);
        BITMAP bm; GetObject(trackPanelArtBmp_, sizeof(bm), &bm);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, trackPanelArtBmp_);
        int artX = cx - bm.bmWidth / 2;
        BitBlt(hdc, artX, y, bm.bmWidth, bm.bmHeight, memDC, 0, 0, SRCCOPY);
        artRight = artX + bm.bmWidth;
        y += bm.bmHeight + 8;
        SelectObject(memDC, oldBmp);
        DeleteDC(memDC);
    } else {
        RECT placeholder = { cx - artDisplaySize/2, y, cx + artDisplaySize/2, y + artDisplaySize };
        FillRect(hdc, &placeholder, hbrPlaceholder_);
        drawCenteredText(hdc, placeholder, L"♫", hFontPlaceholder_, CLR_TEXT_DIM);
        y += artDisplaySize + 8;
    }

    // Close button — overlay on artwork top-right
    {
        int closeSize = 28;
        rcTrackClose_ = { artRight - closeSize - 4, artTop + 4,
                          artRight - 4, artTop + closeSize + 4 };
        // Dark circle background
        HBRUSH closeBg = CreateSolidBrush(RGB(20, 20, 20));
        HPEN closePen = (HPEN)GetStockObject(NULL_PEN);
        HPEN oldPen = (HPEN)SelectObject(hdc, closePen);
        HBRUSH oldBr = (HBRUSH)SelectObject(hdc, hoverTrackClose_ ? hbrHover_ : closeBg);
        Ellipse(hdc, rcTrackClose_.left, rcTrackClose_.top,
                rcTrackClose_.right, rcTrackClose_.bottom);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBr);
        DeleteObject(closeBg);
        drawIcon(hdc, rcTrackClose_, iconClose_);
    }

    if (selectedAlbumIdx_ < 0 || selectedAlbumIdx_ >= (int)albums_.size()) return;
    const Album& album = albums_[selectedAlbumIdx_];

    // Album name
    RECT nameRc = { rcTrackPanel_.left + 12, y, rcTrackPanel_.right - 12, y + 22 };
    std::wstring nameW(utf8ToWide(album.name));
    HFONT oldF = (HFONT)SelectObject(hdc, hFontNowPlaying_);
    SetTextColor(hdc, CLR_TEXT_PRIMARY);
    DrawTextW(hdc, nameW.c_str(), -1, &nameRc, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    y += 24;

    // Artist
    RECT artistRc = { rcTrackPanel_.left + 12, y, rcTrackPanel_.right - 12, y + 18 };
    std::wstring artistW(utf8ToWide(album.artist));
    SelectObject(hdc, hFontArtist_);
    SetTextColor(hdc, CLR_TEXT_SECONDARY);
    DrawTextW(hdc, artistW.c_str(), -1, &artistRc, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    y += 26;

    // Separator
    RECT lineSep = { rcTrackPanel_.left + 12, y, rcTrackPanel_.right - 12, y + 1 };
    fillRect(hdc, lineSep, CLR_SEPARATOR);
    y += 8;

    trackHeaderHeight_ = y - rcTrackPanel_.top;

    // Clip track rows
    RECT trackArea = { rcTrackPanel_.left, y, rcTrackPanel_.right, rcTrackPanel_.bottom };
    HRGN clipRgn = CreateRectRgnIndirect(&trackArea);
    SelectClipRgn(hdc, clipRgn);

    for (int i = 0; i < (int)album.tracks.size(); i++) {
        int rowY = y + i * trackRowHeight_ - trackScrollY_;
        if (rowY + trackRowHeight_ < trackArea.top) continue;
        if (rowY > trackArea.bottom) break;

        RECT rowRc = { rcTrackPanel_.left, rowY, rcTrackPanel_.right, rowY + trackRowHeight_ };
        bool isPlaying = (currentAlbum_ == selectedAlbumIdx_ && currentTrack_ == i && isPlaying_);

        if (hoverTrackIdx_ == i)
            FillRect(hdc, &rowRc, hbrHover_);

        // Track number
        wchar_t numBuf[8];
        swprintf_s(numBuf, L"%d", album.tracks[i].trackNumber > 0 ? album.tracks[i].trackNumber : i + 1);
        RECT numRc = { rcTrackPanel_.left + 12, rowY, rcTrackPanel_.left + 42, rowY + trackRowHeight_ };
        SelectObject(hdc, hFontTrackNumber_);
        SetTextColor(hdc, isPlaying ? CLR_ACCENT : CLR_TEXT_SECONDARY);
        DrawTextW(hdc, numBuf, -1, &numRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        // Title
        std::wstring titleW(utf8ToWide(album.tracks[i].title));
        RECT titleRc = { rcTrackPanel_.left + 50, rowY, rcTrackPanel_.right - 55, rowY + trackRowHeight_ };
        SelectObject(hdc, hFontTrackRow_);
        SetTextColor(hdc, isPlaying ? CLR_ACCENT : CLR_TEXT_PRIMARY);
        DrawTextW(hdc, titleW.c_str(), -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        // Duration
        int durMs = album.tracks[i].durationMs;
        if (durMs > 0) {
            wchar_t durBuf[16];
            swprintf_s(durBuf, L"%d:%02d", durMs / 60000, (durMs % 60000) / 1000);
            RECT durRc = { rcTrackPanel_.right - 55, rowY, rcTrackPanel_.right - 12, rowY + trackRowHeight_ };
            SelectObject(hdc, hFontTime_);
            SetTextColor(hdc, CLR_TEXT_SECONDARY);
            DrawTextW(hdc, durBuf, -1, &durRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    SelectClipRgn(hdc, nullptr);
    DeleteObject(clipRgn);
    SelectObject(hdc, oldF);
}

void PlayerWindow::paintTransport(HDC hdc) {
    FillRect(hdc, &rcTransport_, hbrTransport_);

    // Top separator
    RECT sep = { rcTransport_.left, rcTransport_.top, rcTransport_.right, rcTransport_.top + 1 };
    fillRect(hdc, sep, CLR_SEPARATOR);

    SetBkMode(hdc, TRANSPARENT);

    // Album art thumbnail
    if (transportArtBmp_) {
        HDC memDC = CreateCompatibleDC(hdc);
        BITMAP bm; GetObject(transportArtBmp_, sizeof(bm), &bm);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, transportArtBmp_);
        BitBlt(hdc, rcTransportArt_.left, rcTransportArt_.top,
               bm.bmWidth, bm.bmHeight, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteDC(memDC);
    }

    // Title + Artist
    HFONT oldF = (HFONT)SelectObject(hdc, hFontTransportTitle_);
    SetTextColor(hdc, CLR_TEXT_PRIMARY);
    RECT titleRc = { rcTransportInfo_.left, rcTransportInfo_.top,
                     rcTransportInfo_.right, rcTransportInfo_.top + 22 };
    DrawTextW(hdc, currentTitleW_.empty() ? L"No track" : currentTitleW_.c_str(),
              -1, &titleRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    SelectObject(hdc, hFontTransportArtist_);
    SetTextColor(hdc, CLR_TEXT_SECONDARY);
    RECT artRc = { rcTransportInfo_.left, rcTransportInfo_.top + 24,
                   rcTransportInfo_.right, rcTransportInfo_.bottom };
    DrawTextW(hdc, currentArtistW_.c_str(), -1, &artRc,
              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    // Transport buttons — SVG icons
    struct BtnDef { RECT rc; int idx; };
    BtnDef buttons[] = {
        { rcBtnPrev_, 0 },
        { rcBtnPlay_, 1 },
        { rcBtnStop_, 2 },
        { rcBtnNext_, 3 },
    };
    HBITMAP btnIcons[] = {
        iconPrev_,
        isPlaying_ ? iconPauseGreen_ : iconPlay_,
        iconStop_,
        iconNext_,
    };

    for (int i = 0; i < 4; i++) {
        if (hoverTransportBtn_ == buttons[i].idx) {
            HBRUSH roundBr = CreateSolidBrush(CLR_HOVER);
            HPEN nullPen2 = (HPEN)GetStockObject(NULL_PEN);
            HPEN oldPen2 = (HPEN)SelectObject(hdc, nullPen2);
            HBRUSH oldBrush2 = (HBRUSH)SelectObject(hdc, roundBr);
            RoundRect(hdc, buttons[i].rc.left, buttons[i].rc.top,
                      buttons[i].rc.right, buttons[i].rc.bottom, 8, 8);
            SelectObject(hdc, oldPen2);
            SelectObject(hdc, oldBrush2);
            DeleteObject(roundBr);
        }
        drawIcon(hdc, buttons[i].rc, btnIcons[i]);
    }

    // Time display (text only, no seekbar)
    wchar_t timeBuf[64];
    swprintf_s(timeBuf, L"%d:%02d / %d:%02d",
               seekPosMs_ / 60000, (seekPosMs_ % 60000) / 1000,
               seekTotalMs_ / 60000, (seekTotalMs_ % 60000) / 1000);
    SelectObject(hdc, hFontTime_);
    SetTextColor(hdc, CLR_TEXT_SECONDARY);
    DrawTextW(hdc, timeBuf, -1, const_cast<RECT*>(&rcTimeDisplay_),
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // Audio mode indicator
    {
        bool bp = bitperfectMode_.load();
        const wchar_t* modeText = bp ? L"BITPERFECT" : L"REF EQ";
        COLORREF modeClr = bp ? CLR_ACCENT : CLR_TEXT_DIM;
        RECT modeRc = { rcTimeDisplay_.right - 90, rcTransport_.top + 4,
                        rcTimeDisplay_.right, rcTransport_.top + 16 };
        SetTextColor(hdc, modeClr);
        DrawTextW(hdc, modeText, -1, &modeRc,
                  DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
    }

    SelectObject(hdc, oldF);
}

void PlayerWindow::paintSettingsPage(HDC hdc) {
    FillRect(hdc, &rcGrid_, hbrMain_);
    SetBkMode(hdc, TRANSPARENT);

    RECT headerRc = { rcGrid_.left, rcGrid_.top + 20, rcGrid_.right, rcGrid_.top + 55 };
    drawCenteredText(hdc, headerRc, L"Settings", hFontNowPlaying_, CLR_TEXT_PRIMARY);

    bool bp = bitperfectMode_.load();
    const wchar_t* modeLabel = bp
        ? L"Mode: Bitperfect  \x2014  click to switch to Reference EQ"
        : L"Mode: Reference EQ  \x2014  click to switch to Bitperfect";

    struct SettItem { RECT rc; const wchar_t* label; int idx; };
    SettItem items[] = {
        { rcSettingsAddFolder_, L"Add Music Folder",      0 },
        { rcSettingsManage_,    L"Manage Music Folders",  1 },
        { rcSettingsAudio_,     L"Audio Output Settings", 2 },
        { rcSettingsEq_,        L"EQ / AutoEQ Profiles",  3 },
        { rcSettingsBitperfect_, modeLabel,                4 },
    };

    for (auto& item : items) {
        if (hoverSettingsItem_ == item.idx) {
            HBRUSH br = CreateSolidBrush(CLR_HOVER);
            HPEN npen = (HPEN)GetStockObject(NULL_PEN);
            HPEN op = (HPEN)SelectObject(hdc, npen);
            HBRUSH ob = (HBRUSH)SelectObject(hdc, br);
            RoundRect(hdc, item.rc.left, item.rc.top, item.rc.right, item.rc.bottom, 8, 8);
            SelectObject(hdc, op);
            SelectObject(hdc, ob);
            DeleteObject(br);
        }

        COLORREF borderClr = (item.idx == 4 && bp) ? CLR_ACCENT : CLR_SEPARATOR;
        HPEN pen = CreatePen(PS_SOLID, 1, borderClr);
        HPEN op = (HPEN)SelectObject(hdc, pen);
        HBRUSH ob = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, item.rc.left, item.rc.top, item.rc.right, item.rc.bottom, 8, 8);
        SelectObject(hdc, op);
        SelectObject(hdc, ob);
        DeleteObject(pen);

        COLORREF textClr = (item.idx == 3 && bp) ? CLR_TEXT_DIM : CLR_TEXT_PRIMARY;
        if (item.idx == 4 && bp) textClr = CLR_ACCENT;
        drawCenteredText(hdc, item.rc, item.label, hFontSettingsItem_, textClr);
    }
}

// ── Hit testing ──────────────────────────────────────────────────────────────

int PlayerWindow::gridHitTest(int x, int y) const {
    if (x < rcGrid_.left || x >= rcGrid_.right || y < rcGrid_.top || y >= rcGrid_.bottom)
        return -1;
    int gridW = rcGrid_.right - rcGrid_.left;
    int tileSpaceW = gridW - gridPadX_ * 2;
    int tileStepX = gridCols_ > 1 ? tileSpaceW / gridCols_ : gridTileSize_;
    int tileStepY = gridTileSize_ + 40;

    int col = (x - rcGrid_.left - gridPadX_) / tileStepX;
    int row = (y - rcGrid_.top - gridPadY_ + gridScrollY_) / tileStepY;
    if (col < 0 || col >= gridCols_ || row < 0) return -1;
    int idx = row * gridCols_ + col;
    if (idx >= (int)albums_.size()) return -1;
    return idx;
}

int PlayerWindow::trackPanelHitTest(int x, int y) const {
    if (!trackPanelOpen_) return -1;
    if (x < rcTrackPanel_.left || x >= rcTrackPanel_.right) return -1;
    int trackAreaTop = rcTrackPanel_.top + trackHeaderHeight_;
    if (y < trackAreaTop || y >= rcTrackPanel_.bottom) return -1;
    int row = (y - trackAreaTop + trackScrollY_) / trackRowHeight_;
    if (selectedAlbumIdx_ < 0 || selectedAlbumIdx_ >= (int)albums_.size()) return -1;
    if (row < 0 || row >= (int)albums_[selectedAlbumIdx_].tracks.size()) return -1;
    return row;
}

int PlayerWindow::sidebarHitTest(int x, int y) const {
    POINT pt = { x, y };
    if (PtInRect(&rcNavAlbums_, pt)) return 0;
    if (PtInRect(&rcNavSettings_, pt)) return 1;
    return -1;
}

int PlayerWindow::transportBtnHitTest(int x, int y) const {
    POINT pt = { x, y };
    if (PtInRect(&rcBtnPrev_, pt)) return 0;
    if (PtInRect(&rcBtnPlay_, pt)) return 1;
    if (PtInRect(&rcBtnStop_, pt)) return 2;
    if (PtInRect(&rcBtnNext_, pt)) return 3;
    return -1;
}

int PlayerWindow::settingsHitTest(int x, int y) const {
    POINT pt = { x, y };
    if (PtInRect(&rcSettingsAddFolder_, pt)) return 0;
    if (PtInRect(&rcSettingsManage_, pt)) return 1;
    if (PtInRect(&rcSettingsAudio_, pt)) return 2;
    if (PtInRect(&rcSettingsEq_, pt)) return 3;
    if (PtInRect(&rcSettingsBitperfect_, pt)) return 4;
    return -1;
}

bool PlayerWindow::isInSeekbar(int x, int y) const {
    RECT expanded = { rcSeekbar_.left - 4, rcSeekbar_.top - 10,
                      rcSeekbar_.right + 4, rcSeekbar_.bottom + 10 };
    POINT pt = { x, y };
    return PtInRect(&expanded, pt) != 0;
}

int PlayerWindow::seekbarPosToMs(int x) const {
    int seekW = rcSeekbar_.right - rcSeekbar_.left;
    if (seekW <= 0 || seekTotalMs_ <= 0) return 0;
    int rel = std::clamp((int)(x - rcSeekbar_.left), 0, seekW);
    return (int)((int64_t)rel * seekTotalMs_ / seekW);
}

// ── Mouse handling ───────────────────────────────────────────────────────────

void PlayerWindow::onMouseMove(int x, int y) {
    if (!mouseTracking_) {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd_, 0 };
        TrackMouseEvent(&tme);
        mouseTracking_ = true;
    }

    int oldHoverAlbum = hoverAlbumIdx_;
    int oldHoverTrack = hoverTrackIdx_;
    int oldHoverSidebar = hoverSidebarItem_;
    int oldHoverTransBtn = hoverTransportBtn_;
    int oldHoverSettings = hoverSettingsItem_;
    bool oldHoverSeek = hoverSeekbar_;
    bool oldHoverClose = hoverTrackClose_;

    hoverAlbumIdx_ = -1;
    hoverTrackIdx_ = -1;
    hoverSidebarItem_ = -1;
    hoverTransportBtn_ = -1;
    hoverSettingsItem_ = -1;
    hoverSeekbar_ = false;
    hoverTrackClose_ = false;

    POINT pt = { x, y };

    if (seekDragging_) {
        seekPosMs_ = seekbarPosToMs(x);
        InvalidateRect(hwnd_, &rcTransport_, FALSE);
        return;
    }

    if (PtInRect(&rcSidebar_, pt)) {
        hoverSidebarItem_ = sidebarHitTest(x, y);
    } else if (PtInRect(&rcTransport_, pt)) {
        hoverTransportBtn_ = transportBtnHitTest(x, y);
        hoverSeekbar_ = isInSeekbar(x, y);
    } else if (trackPanelOpen_ && PtInRect(&rcTrackPanel_, pt)) {
        hoverTrackIdx_ = trackPanelHitTest(x, y);
        hoverTrackClose_ = PtInRect(&rcTrackClose_, pt) != 0;
    } else if (PtInRect(&rcGrid_, pt)) {
        if (activeNavItem_ == 0)
            hoverAlbumIdx_ = gridHitTest(x, y);
        else
            hoverSettingsItem_ = settingsHitTest(x, y);
    }

    bool changed = (hoverAlbumIdx_ != oldHoverAlbum ||
                    hoverTrackIdx_ != oldHoverTrack ||
                    hoverSidebarItem_ != oldHoverSidebar ||
                    hoverTransportBtn_ != oldHoverTransBtn ||
                    hoverSettingsItem_ != oldHoverSettings ||
                    hoverSeekbar_ != oldHoverSeek ||
                    hoverTrackClose_ != oldHoverClose);
    if (changed)
        InvalidateRect(hwnd_, nullptr, FALSE);
}

void PlayerWindow::onMouseLeave() {
    mouseTracking_ = false;
    hoverAlbumIdx_ = -1;
    hoverTrackIdx_ = -1;
    hoverSidebarItem_ = -1;
    hoverTransportBtn_ = -1;
    hoverSettingsItem_ = -1;
    hoverSeekbar_ = false;
    hoverTrackClose_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PlayerWindow::onLButtonDown(int x, int y) {
    POINT pt = { x, y };

    // Seekbar
    if (isInSeekbar(x, y)) {
        seekDragging_ = true;
        SetCapture(hwnd_);
        seekPosMs_ = seekbarPosToMs(x);
        InvalidateRect(hwnd_, &rcTransport_, FALSE);
        return;
    }

    // Transport buttons
    int btn = transportBtnHitTest(x, y);
    if (btn == 0) { onPrev(); return; }
    if (btn == 1) { onPlay(); return; }
    if (btn == 2) { onStop(); return; }
    if (btn == 3) { onNext(); return; }

    // Transport art -> fullscreen
    if (PtInRect(&rcTransportArt_, pt) && transportArtBmp_) {
        onArtClick();
        return;
    }

    // Sidebar
    if (PtInRect(&rcSidebar_, pt)) {
        int nav = sidebarHitTest(x, y);
        if (nav >= 0 && nav != activeNavItem_) {
            activeNavItem_ = nav;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    // Track panel close
    if (trackPanelOpen_ && PtInRect(&rcTrackClose_, pt)) {
        trackPanelOpen_ = false;
        recalcLayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // Track panel track click
    if (trackPanelOpen_ && PtInRect(&rcTrackPanel_, pt)) {
        int track = trackPanelHitTest(x, y);
        printf("[Click] Track panel click (%d,%d): hit=%d, trackHeaderHeight=%d, tracks=%d\n",
               x, y, track, trackHeaderHeight_,
               (selectedAlbumIdx_ >= 0 && selectedAlbumIdx_ < (int)albums_.size())
                   ? (int)albums_[selectedAlbumIdx_].tracks.size() : -1);
        fflush(stdout);
        if (track >= 0) {
            currentAlbum_ = selectedAlbumIdx_;
            currentTrack_ = track;
            onPlay();
        }
        return;
    }

    // Grid (albums view)
    if (activeNavItem_ == 0 && PtInRect(&rcGrid_, pt)) {
        int idx = gridHitTest(x, y);
        if (idx >= 0) {
            selectedAlbumIdx_ = idx;
            trackPanelOpen_ = true;
            trackScrollY_ = 0;
            loadTrackPanelArt(idx);
            recalcLayout();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    // Settings page
    if (activeNavItem_ == 1 && PtInRect(&rcGrid_, pt)) {
        int sett = settingsHitTest(x, y);
        if (sett == 0) onAddFolder();
        if (sett == 1) onManageFolders();
        if (sett == 2) onAudioSettings();
        if (sett == 3) onEqSettings();
        if (sett == 4) toggleBitperfectMode();
        return;
    }
}

void PlayerWindow::onLButtonUp(int x, int y) {
    if (seekDragging_) {
        seekDragging_ = false;
        ReleaseCapture();
        int posMs = seekbarPosToMs(x);
        onSeek(posMs);
        InvalidateRect(hwnd_, &rcTransport_, FALSE);
    }
}

void PlayerWindow::onLButtonDblClk(int x, int y) {
    POINT pt = { x, y };

    // Double-click on grid tile: play first track
    if (activeNavItem_ == 0 && PtInRect(&rcGrid_, pt)) {
        int idx = gridHitTest(x, y);
        if (idx >= 0) {
            selectedAlbumIdx_ = idx;
            trackPanelOpen_ = true;
            trackScrollY_ = 0;
            loadTrackPanelArt(idx);
            recalcLayout();
            currentAlbum_ = idx;
            currentTrack_ = 0;
            onPlay();
        }
        return;
    }

    // Double-click on track panel: play that track
    if (trackPanelOpen_ && PtInRect(&rcTrackPanel_, pt)) {
        int track = trackPanelHitTest(x, y);
        if (track >= 0) {
            currentAlbum_ = selectedAlbumIdx_;
            currentTrack_ = track;
            onPlay();
        }
        return;
    }
}

void PlayerWindow::onMouseWheel(int x, int y, int delta) {
    POINT pt = { x, y };
    ScreenToClient(hwnd_, &pt);

    if (trackPanelOpen_ && PtInRect(&rcTrackPanel_, pt)) {
        trackScrollY_ -= delta;
        int maxScroll = 0;
        if (selectedAlbumIdx_ >= 0 && selectedAlbumIdx_ < (int)albums_.size()) {
            maxScroll = (int)albums_[selectedAlbumIdx_].tracks.size() * trackRowHeight_
                      - (rcTrackPanel_.bottom - rcTrackPanel_.top - trackHeaderHeight_);
        }
        trackScrollY_ = std::clamp(trackScrollY_, 0, std::max(0, maxScroll));
        InvalidateRect(hwnd_, &rcTrackPanel_, FALSE);
        return;
    }

    if (PtInRect(&rcGrid_, pt)) {
        gridScrollY_ -= delta;
        int gridH = rcGrid_.bottom - rcGrid_.top;
        gridScrollY_ = std::clamp(gridScrollY_, 0, std::max(0, gridTotalHeight_ - gridH));
        InvalidateRect(hwnd_, &rcGrid_, FALSE);
    }
}

// ── Prev / Next ──────────────────────────────────────────────────────────────

void PlayerWindow::onNext() {
    if (currentAlbum_ < 0) return;
    int wanted = currentTrack_ + 1;
    if (wanted >= (int)albums_[currentAlbum_].tracks.size()) return;

    // Seamless path: if we're playing, the prepared nextDecoder_ matches the
    // requested track, and its (sampleRate, channels) match the running USB
    // output, hand off via the gapless coordinator. The output stream stays
    // alive — no stop()/configure()/start() cycle, no working-set re-lock
    // dance, no cold-start transient.
    //
    // Decoder::stop() does not fire the done callback (only natural EOF
    // does), so we signal gaplessSignal_ ourselves after stopping the
    // current decoder. flush() drops the ~3 s of stale tail still queued in
    // the ring so the user actually hears the next track promptly.
    if (isPlaying_ && active_ && output_ &&
        nextAlbum_ == currentAlbum_ && nextTrack_ == wanted) {
        Decoder* incoming = (active_ == &decoder_) ? &nextDecoder_ : &decoder_;
        if (incoming->sampleRate() == output_->getConfiguredRate() &&
            incoming->channels()   == output_->getConfiguredChannels()) {
            active_->stop();
            output_->flush();
            {
                std::lock_guard<std::mutex> lk(gaplessMu_);
                gaplessSignal_ = true;
            }
            gaplessCv_.notify_one();
            return;
        }
    }

    currentTrack_ = wanted;
    onPlay();
}

void PlayerWindow::onPrev() {
    if (currentAlbum_ < 0 || currentTrack_ < 0) return;
    if (seekPosMs_ > 3000) {
        onSeek(0);
        playedFrames_.store(0);
        return;
    }
    if (currentTrack_ > 0) {
        currentTrack_--;
        onPlay();
    }
}

// ── Manage Folders dialog ─────────────────────────────────────────────────────

#define ID_DLG_LIST   301
#define ID_DLG_REMOVE 302
#define ID_DLG_DONE   303

struct ManageDlgCtx {
    Db*   db;
    HWND  parent;
    HWND  list;
    bool  changed = false;
};

static LRESULT CALLBACK manageFoldersDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = (ManageDlgCtx*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        ctx = (ManageDlgCtx*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)ctx);
        HINSTANCE hi = ((CREATESTRUCTW*)lp)->hInstance;
        ctx->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            8, 8, 484, 200, hwnd, (HMENU)ID_DLG_LIST, hi, nullptr);
        for (auto& r : ctx->db->loadMusicRoots()) {
            SendMessageW(ctx->list, LB_ADDSTRING, 0, (LPARAM)utf8ToWide(r).c_str());
        }
        CreateWindowExW(0, L"BUTTON", L"Remove Selected",
            WS_CHILD | WS_VISIBLE, 8, 216, 150, 28, hwnd, (HMENU)ID_DLG_REMOVE, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Done",
            WS_CHILD | WS_VISIBLE, 422, 216, 70, 28, hwnd, (HMENU)ID_DLG_DONE, hi, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_DLG_REMOVE) {
            int sel = (int)SendMessageW(ctx->list, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR) break;
            wchar_t buf[MAX_PATH] = {};
            SendMessageW(ctx->list, LB_GETTEXT, sel, (LPARAM)buf);
            int pLen = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
            std::string path(pLen, '\0');
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, path.data(), pLen, nullptr, nullptr);
            if (!path.empty() && path.back() == '\0') path.pop_back();
            ctx->db->removeMusicRoot(path);
            SendMessageW(ctx->list, LB_DELETESTRING, sel, 0);
            ctx->changed = true;
        } else if (LOWORD(wp) == ID_DLG_DONE) {
            DestroyWindow(hwnd);
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        PostMessageW(ctx->parent, WM_NULL, 0, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void PlayerWindow::onManageFolders() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = manageFoldersDlgProc;
        wc.hInstance     = hInst_;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MatrixManageFolders";
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    ManageDlgCtx ctx{ &db_, hwnd_, nullptr, false };
    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"MatrixManageFolders", L"Music Folders",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, 510, 290, hwnd_, nullptr, hInst_, &ctx);

    RECT pr; GetWindowRect(hwnd_, &pr);
    RECT dr; GetWindowRect(dlg, &dr);
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    SetWindowPos(dlg, nullptr,
        pr.left + (pr.right - pr.left - dw) / 2,
        pr.top  + (pr.bottom - pr.top - dh) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    EnableWindow(hwnd_, FALSE);
    ShowWindow(dlg, SW_SHOW);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(hwnd_, TRUE);
    SetForegroundWindow(hwnd_);

    if (!ctx.changed) return;
    watcher_.unwatchAll();
    setupWatchers();
    startBackgroundScan();
}

// ── Audio Settings dialog ─────────────────────────────────────────────────────

#define ID_AUDIO_USB       401
#define ID_AUDIO_WASAPI    402
#define ID_AUDIO_DEVICE    403
#define ID_AUDIO_SHARED    404
#define ID_AUDIO_EXCLUSIVE 405
#define ID_AUDIO_APPLY     406
#define ID_AUDIO_USB_DEV   407

struct AudioSettingsDlgCtx {
    Db*    db;
    HWND   parent;
    HWND   rdoUsb, rdoWasapi;
    HWND   cmbDevice;
    HWND   cmbUsbDevice;
    HWND   rdoShared, rdoExclusive;
    HWND   lblDevice, lblMode;
    HWND   lblUsbDevice;
    std::vector<WasapiDeviceInfo> devices;
    std::vector<UsbAudioDeviceInfo> usbDevices;
    bool   applied = false;
};

static void audioSetBackendControls(AudioSettingsDlgCtx* ctx, bool wasapi) {
    EnableWindow(ctx->cmbDevice,    wasapi);
    EnableWindow(ctx->rdoShared,    wasapi);
    EnableWindow(ctx->rdoExclusive, wasapi);
    EnableWindow(ctx->lblDevice,    wasapi);
    EnableWindow(ctx->lblMode,      wasapi);
    EnableWindow(ctx->cmbUsbDevice, !wasapi);
    EnableWindow(ctx->lblUsbDevice, !wasapi);
}

static LRESULT CALLBACK audioSettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = (AudioSettingsDlgCtx*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        ctx = (AudioSettingsDlgCtx*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)ctx);
        HINSTANCE hi = ((CREATESTRUCTW*)lp)->hInstance;
        int x = 12, y = 10;

        CreateWindowExW(0, L"STATIC", L"Output backend:",
            WS_CHILD | WS_VISIBLE, x, y, 200, 18, hwnd, nullptr, hi, nullptr);
        y += 22;
        ctx->rdoUsb = CreateWindowExW(0, L"BUTTON", L"USB Direct (libusbK)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            x + 8, y, 250, 20, hwnd, (HMENU)ID_AUDIO_USB, hi, nullptr);
        y += 24;
        ctx->rdoWasapi = CreateWindowExW(0, L"BUTTON", L"WASAPI",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            x + 8, y, 250, 20, hwnd, (HMENU)ID_AUDIO_WASAPI, hi, nullptr);
        y += 32;

        ctx->lblUsbDevice = CreateWindowExW(0, L"STATIC", L"USB DAC:",
            WS_CHILD | WS_VISIBLE, x, y, 60, 18, hwnd, nullptr, hi, nullptr);
        ctx->cmbUsbDevice = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            x + 64, y - 2, 370, 200, hwnd, (HMENU)ID_AUDIO_USB_DEV, hi, nullptr);
        y += 32;

        ctx->lblDevice = CreateWindowExW(0, L"STATIC", L"Device:",
            WS_CHILD | WS_VISIBLE, x, y, 60, 18, hwnd, nullptr, hi, nullptr);
        ctx->cmbDevice = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            x + 64, y - 2, 370, 200, hwnd, (HMENU)ID_AUDIO_DEVICE, hi, nullptr);
        y += 32;

        ctx->lblMode = CreateWindowExW(0, L"STATIC", L"Mode:",
            WS_CHILD | WS_VISIBLE, x, y, 200, 18, hwnd, nullptr, hi, nullptr);
        y += 22;
        ctx->rdoShared = CreateWindowExW(0, L"BUTTON",
            L"Shared  \x2014  other apps can play simultaneously",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            x + 8, y, 420, 20, hwnd, (HMENU)ID_AUDIO_SHARED, hi, nullptr);
        y += 24;
        ctx->rdoExclusive = CreateWindowExW(0, L"BUTTON",
            L"Exclusive  \x2014  lower latency, blocks other apps",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            x + 8, y, 420, 20, hwnd, (HMENU)ID_AUDIO_EXCLUSIVE, hi, nullptr);
        y += 36;

        CreateWindowExW(0, L"BUTTON", L"Apply",
            WS_CHILD | WS_VISIBLE, 346, y, 80, 28, hwnd, (HMENU)ID_AUDIO_APPLY, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE, 434, y, 80, 28, hwnd, (HMENU)IDCANCEL, hi, nullptr);

        // Populate USB device list
        ctx->usbDevices = UsbAudioDriver::enumerateUsbAudioDevices();
        for (auto& ud : ctx->usbDevices) {
            std::wstring wname(utf8ToWide(ud.name));
            SendMessageW(ctx->cmbUsbDevice, CB_ADDSTRING, 0, (LPARAM)wname.c_str());
        }
        auto savedVid = ctx->db->loadSetting("usb_vid");
        auto savedPid = ctx->db->loadSetting("usb_pid");
        int usbSel = 0;
        if (!savedVid.empty() && !savedPid.empty()) {
            uint16_t sv = (uint16_t)strtoul(savedVid.c_str(), nullptr, 16);
            uint16_t sp = (uint16_t)strtoul(savedPid.c_str(), nullptr, 16);
            for (int i = 0; i < (int)ctx->usbDevices.size(); i++) {
                if (ctx->usbDevices[i].vid == sv && ctx->usbDevices[i].pid == sp) {
                    usbSel = i; break;
                }
            }
        }
        if (!ctx->usbDevices.empty())
            SendMessageW(ctx->cmbUsbDevice, CB_SETCURSEL, usbSel, 0);

        // Populate WASAPI device list
        SendMessageW(ctx->cmbDevice, CB_ADDSTRING, 0, (LPARAM)L"(Default device)");
        ctx->devices = WasapiOutput::enumerateDevices();
        for (auto& d : ctx->devices)
            SendMessageW(ctx->cmbDevice, CB_ADDSTRING, 0, (LPARAM)d.name.c_str());

        bool wasapi = (ctx->db->loadSetting("audio_backend") == "wasapi");
        SendMessageW(wasapi ? ctx->rdoWasapi : ctx->rdoUsb, BM_SETCHECK, BST_CHECKED, 0);
        audioSetBackendControls(ctx, wasapi);

        auto savedId = ctx->db->loadSetting("wasapi_device_id");
        int devSel = 0;
        for (int i = 0; i < (int)ctx->devices.size(); i++) {
            int idLen = WideCharToMultiByte(CP_UTF8, 0, ctx->devices[i].id.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string id(idLen, '\0');
            WideCharToMultiByte(CP_UTF8, 0, ctx->devices[i].id.c_str(), -1, id.data(), idLen, nullptr, nullptr);
            if (!id.empty() && id.back() == '\0') id.pop_back();
            if (id == savedId) { devSel = i + 1; break; }
        }
        SendMessageW(ctx->cmbDevice, CB_SETCURSEL, devSel, 0);

        bool exclusive = (ctx->db->loadSetting("wasapi_mode") == "exclusive");
        SendMessageW(exclusive ? ctx->rdoExclusive : ctx->rdoShared, BM_SETCHECK, BST_CHECKED, 0);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_AUDIO_USB:
            audioSetBackendControls(ctx, false);
            break;
        case ID_AUDIO_WASAPI:
            audioSetBackendControls(ctx, true);
            break;
        case ID_AUDIO_APPLY: {
            bool wasapi = (SendMessageW(ctx->rdoWasapi, BM_GETCHECK, 0, 0) == BST_CHECKED);
            ctx->db->saveSetting("audio_backend", wasapi ? "wasapi" : "usb");
            if (wasapi) {
                int sel = (int)SendMessageW(ctx->cmbDevice, CB_GETCURSEL, 0, 0);
                std::string devId;
                if (sel > 0 && sel <= (int)ctx->devices.size()) {
                    auto& ws = ctx->devices[sel - 1].id;
                    int dLen = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
                    devId.assign(dLen, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, devId.data(), dLen, nullptr, nullptr);
                    if (!devId.empty() && devId.back() == '\0') devId.pop_back();
                }
                ctx->db->saveSetting("wasapi_device_id", devId);
                bool excl = (SendMessageW(ctx->rdoExclusive, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->db->saveSetting("wasapi_mode", excl ? "exclusive" : "shared");
            } else {
                int usel = (int)SendMessageW(ctx->cmbUsbDevice, CB_GETCURSEL, 0, 0);
                if (usel >= 0 && usel < (int)ctx->usbDevices.size()) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "%04X", ctx->usbDevices[usel].vid);
                    ctx->db->saveSetting("usb_vid", buf);
                    snprintf(buf, sizeof(buf), "%04X", ctx->usbDevices[usel].pid);
                    ctx->db->saveSetting("usb_pid", buf);
                }
            }
            ctx->applied = true;
            DestroyWindow(hwnd);
            break;
        }
        case IDCANCEL:
            DestroyWindow(hwnd);
            break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        PostMessageW(ctx->parent, WM_NULL, 0, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void PlayerWindow::onAudioSettings() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = audioSettingsDlgProc;
        wc.hInstance     = hInst_;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MatrixAudioSettings";
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    AudioSettingsDlgCtx ctx{ &db_, hwnd_ };
    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"MatrixAudioSettings", L"Audio Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, 530, 297, hwnd_, nullptr, hInst_, &ctx);

    RECT pr; GetWindowRect(hwnd_, &pr);
    RECT dr; GetWindowRect(dlg, &dr);
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    SetWindowPos(dlg, nullptr,
        pr.left + (pr.right - pr.left - dw) / 2,
        pr.top  + (pr.bottom - pr.top  - dh) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    EnableWindow(hwnd_, FALSE);
    ShowWindow(dlg, SW_SHOW);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(hwnd_, TRUE);
    SetForegroundWindow(hwnd_);

    if (!ctx.applied) return;

    onStop();

    useWasapi_ = (db_.loadSetting("audio_backend") == "wasapi");
    wasapiMode_ = (db_.loadSetting("wasapi_mode") == "exclusive")
                  ? WasapiMode::Exclusive : WasapiMode::Shared;
    auto devIdUtf8 = db_.loadSetting("wasapi_device_id");
    wasapiDeviceId_ = utf8ToWide(devIdUtf8);

    if (useWasapi_) {
        output_ = std::make_unique<WasapiOutput>(wasapiDeviceId_, wasapiMode_);
    } else {
        auto vidStr = db_.loadSetting("usb_vid");
        auto pidStr = db_.loadSetting("usb_pid");
        uint16_t vid = vidStr.empty() ? (uint16_t)0x32BB : (uint16_t)strtoul(vidStr.c_str(), nullptr, 16);
        uint16_t pid = pidStr.empty() ? (uint16_t)0x0004 : (uint16_t)strtoul(pidStr.c_str(), nullptr, 16);

        usbDriver_.close();
        usbOpen_ = usbDriver_.open(vid, pid);
        if (usbOpen_) usbDriver_.parseDescriptors();
        output_ = std::make_unique<UsbAudioOutput>(usbDriver_);
    }
}

// ── Bitperfect / DSP mode toggle ─────────────────────────────────────────────

void PlayerWindow::toggleBitperfectMode() {
    bool newMode = !bitperfectMode_.load();
    bitperfectMode_.store(newMode);
    db_.saveSetting("audio_mode", newMode ? "bitperfect" : "reference_eq");

    // Both directions take effect on the next track started, not mid-play.
    // (Don't clear the EQ engine here: the running Reference EQ callback checks
    // isActive() every chunk, so clearing would kill EQ live and make the switch
    // asymmetric. The bit-perfect branch of onPlay clears EQ itself next track.)
    printf("[Audio] Switched to %s mode (applies on next track)\n",
           newMode ? "BITPERFECT" : "REFERENCE EQ");
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ── EQ Settings dialog ──────────────────────────────────────────────────────

#define ID_EQ_SEARCH  501
#define ID_EQ_LIST    502
#define ID_EQ_ASSIGN  503
#define ID_EQ_CLEAR   504
#define ID_EQ_CLOSE   505

struct EqSettingsDlgCtx {
    Db*                        db;
    HWND                       parent;
    HWND                       editSearch;
    HWND                       listProfiles;
    HWND                       lblDevice;
    HWND                       lblSelected;
    HWND                       lblDetails;
    const std::vector<EqProfile>* allProfiles;
    std::vector<int>           filteredIndices;
    std::string                deviceKey;
    EqManager*                 eqManager;
    const EqProfileStore*      profileStore;
    int                        currentSampleRate;
    int                        currentChannels;
    bool                       bitperfectActive = false;
    bool                       changed = false;
};

static void eqFilterList(EqSettingsDlgCtx* ctx) {
    wchar_t searchBuf[256] = {};
    GetWindowTextW(ctx->editSearch, searchBuf, 256);
    std::wstring search(searchBuf);
    for (auto& c : search) c = towlower(c);

    SendMessageW(ctx->listProfiles, WM_SETREDRAW, FALSE, 0);
    SendMessageW(ctx->listProfiles, LB_RESETCONTENT, 0, 0);
    ctx->filteredIndices.clear();

    for (int i = 0; i < (int)ctx->allProfiles->size(); i++) {
        auto& p = (*ctx->allProfiles)[i];
        std::wstring nameW(utf8ToWide(p.name));
        std::wstring nameLower = nameW;
        for (auto& c : nameLower) c = towlower(c);

        if (!search.empty() && nameLower.find(search) == std::wstring::npos)
            continue;

        std::wstring label = nameW;
        if (!p.form.empty()) {
            std::wstring formW(utf8ToWide(p.form));
            label += L"  (" + formW + L")";
        }
        SendMessageW(ctx->listProfiles, LB_ADDSTRING, 0, (LPARAM)label.c_str());
        ctx->filteredIndices.push_back(i);
    }
    SendMessageW(ctx->listProfiles, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(ctx->listProfiles, nullptr, TRUE);
}

static void eqUpdateSelection(EqSettingsDlgCtx* ctx) {
    int sel = (int)SendMessageW(ctx->listProfiles, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || sel >= (int)ctx->filteredIndices.size()) {
        SetWindowTextW(ctx->lblSelected, L"No profile selected");
        SetWindowTextW(ctx->lblDetails, L"");
        return;
    }
    int idx = ctx->filteredIndices[sel];
    auto& p = (*ctx->allProfiles)[idx];
    std::wstring nameW(utf8ToWide(p.name));
    std::wstring formW(utf8ToWide(p.form));
    std::wstring selText = nameW;
    if (!formW.empty()) selText += L"  (" + formW + L")";
    SetWindowTextW(ctx->lblSelected, selText.c_str());

    wchar_t detailBuf[128];
    swprintf_s(detailBuf, 128, L"Preamp: %.1f dB  |  Filters: %d",
               p.preamp, (int)p.filters.size());
    SetWindowTextW(ctx->lblDetails, detailBuf);
}

static LRESULT CALLBACK eqSettingsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = (EqSettingsDlgCtx*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        ctx = (EqSettingsDlgCtx*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)ctx);
        HINSTANCE hi = ((CREATESTRUCTW*)lp)->hInstance;
        int x = 12, y = 10;

        std::wstring devKeyW(utf8ToWide(ctx->deviceKey));
        std::wstring devLabel = L"Current device: " + devKeyW;
        ctx->lblDevice = CreateWindowExW(0, L"STATIC", devLabel.c_str(),
            WS_CHILD | WS_VISIBLE, x, y, 460, 18, hwnd, nullptr, hi, nullptr);
        y += 22;

        if (ctx->bitperfectActive) {
            CreateWindowExW(0, L"STATIC",
                L"Bitperfect mode active \x2014 EQ changes apply when DSP mode is enabled.",
                WS_CHILD | WS_VISIBLE, x, y, 460, 18, hwnd, nullptr, hi, nullptr);
            y += 22;
        }

        // Show current assignment
        EqAssignment assign;
        std::wstring currentAssign = L"No EQ assigned";
        if (ctx->db->loadEqAssignment(ctx->deviceKey, assign) ||
            ctx->db->loadEqAssignment("global", assign)) {
            std::wstring n(utf8ToWide(assign.name));
            currentAssign = L"Current EQ: " + n;
        }
        CreateWindowExW(0, L"STATIC", currentAssign.c_str(),
            WS_CHILD | WS_VISIBLE, x, y, 460, 18, hwnd, nullptr, hi, nullptr);
        y += 30;

        CreateWindowExW(0, L"STATIC", L"Search:",
            WS_CHILD | WS_VISIBLE, x, y + 2, 50, 18, hwnd, nullptr, hi, nullptr);
        ctx->editSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            x + 54, y, 406, 22, hwnd, (HMENU)ID_EQ_SEARCH, hi, nullptr);
        y += 30;

        ctx->listProfiles = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            x, y, 460, 240, hwnd, (HMENU)ID_EQ_LIST, hi, nullptr);
        y += 248;

        ctx->lblSelected = CreateWindowExW(0, L"STATIC", L"No profile selected",
            WS_CHILD | WS_VISIBLE, x, y, 460, 18, hwnd, nullptr, hi, nullptr);
        y += 20;
        ctx->lblDetails = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE, x, y, 460, 18, hwnd, nullptr, hi, nullptr);
        y += 30;

        CreateWindowExW(0, L"BUTTON", L"Assign to Device",
            WS_CHILD | WS_VISIBLE, x, y, 140, 28, hwnd, (HMENU)ID_EQ_ASSIGN, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Clear",
            WS_CHILD | WS_VISIBLE, x + 150, y, 80, 28, hwnd, (HMENU)ID_EQ_CLEAR, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE, 392, y, 80, 28, hwnd, (HMENU)ID_EQ_CLOSE, hi, nullptr);

        eqFilterList(ctx);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == ID_EQ_SEARCH) {
            eqFilterList(ctx);
        } else if (HIWORD(wp) == LBN_SELCHANGE && LOWORD(wp) == ID_EQ_LIST) {
            eqUpdateSelection(ctx);
        } else if (LOWORD(wp) == ID_EQ_ASSIGN) {
            int sel = (int)SendMessageW(ctx->listProfiles, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR || sel >= (int)ctx->filteredIndices.size()) {
                MessageBoxW(hwnd, L"Select a profile first.", L"EQ", MB_OK);
                break;
            }
            int idx = ctx->filteredIndices[sel];
            auto& p = (*ctx->allProfiles)[idx];
            ctx->db->saveEqAssignment(ctx->deviceKey, p.name, p.source, p.form);
            if (!ctx->bitperfectActive) {
                auto* profile = ctx->profileStore->findByKey(p.name, p.source, p.form);
                if (profile && ctx->eqManager)
                    ctx->eqManager->applyProfile(profile, ctx->currentSampleRate, ctx->currentChannels);
            }
            ctx->changed = true;
            std::wstring nameW(utf8ToWide(p.name));
            if (ctx->bitperfectActive) {
                std::wstring msg = L"Saved: " + nameW + L"\nWill be active when DSP mode is enabled.";
                MessageBoxW(hwnd, msg.c_str(), L"EQ Profile Saved (Bitperfect)", MB_OK | MB_ICONINFORMATION);
            } else {
                std::wstring msg = L"Assigned: " + nameW;
                MessageBoxW(hwnd, msg.c_str(), L"EQ Profile Assigned", MB_OK | MB_ICONINFORMATION);
            }
        } else if (LOWORD(wp) == ID_EQ_CLEAR) {
            ctx->db->clearEqAssignment(ctx->deviceKey);
            if (ctx->eqManager) ctx->eqManager->clear();
            ctx->changed = true;
            MessageBoxW(hwnd, L"EQ assignment cleared.", L"EQ", MB_OK | MB_ICONINFORMATION);
        } else if (LOWORD(wp) == ID_EQ_CLOSE) {
            DestroyWindow(hwnd);
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        PostMessageW(ctx->parent, WM_NULL, 0, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void PlayerWindow::onEqSettings() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = eqSettingsDlgProc;
        wc.hInstance     = hInst_;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MatrixEqSettings";
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    int sr = 44100, ch = 2;
    if (output_) {
        int r = output_->getConfiguredRate();
        int c = output_->getConfiguredChannels();
        if (r > 0) sr = r;
        if (c > 0) ch = c;
    }

    EqSettingsDlgCtx ctx{};
    ctx.db             = &db_;
    ctx.parent         = hwnd_;
    ctx.allProfiles    = &eqProfiles_.getAll();
    ctx.deviceKey      = getActiveDeviceKey();
    ctx.eqManager      = &eqManager_;
    ctx.profileStore   = &eqProfiles_;
    ctx.currentSampleRate = sr;
    ctx.currentChannels   = ch;
    ctx.bitperfectActive  = bitperfectMode_.load();

    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"MatrixEqSettings", L"EQ / AutoEQ Profiles",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, 500, 520, hwnd_, nullptr, hInst_, &ctx);

    RECT pr; GetWindowRect(hwnd_, &pr);
    RECT dr; GetWindowRect(dlg, &dr);
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    SetWindowPos(dlg, nullptr,
        pr.left + (pr.right - pr.left - dw) / 2,
        pr.top  + (pr.bottom - pr.top  - dh) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    EnableWindow(hwnd_, FALSE);
    ShowWindow(dlg, SW_SHOW);

    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(hwnd_, TRUE);
    SetForegroundWindow(hwnd_);
}

void PlayerWindow::onAddFolder() {
    wchar_t path[MAX_PATH] = {};
    BROWSEINFOW bi = {};
    bi.hwndOwner = hwnd_;
    bi.lpszTitle = L"Select music folder to add";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);

    int rLen = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    std::string root(rLen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, -1, root.data(), rLen, nullptr, nullptr);
    if (!root.empty() && root.back() == '\0') root.pop_back();
    db_.addMusicRoot(root);

    HWND h = hwnd_;
    watcher_.watchRoot(root, [h](const std::string&) {
        PostMessageW(h, WM_APP_SCAN_DONE, 1, 0);
    });

    startBackgroundScan();
}

// ── Album / Track selection (simplified for custom UI) ──────────────────────

void PlayerWindow::onAlbumSelected(int idx) {
    selectedAlbumIdx_ = idx;
    trackPanelOpen_ = true;
    trackScrollY_ = 0;
    loadTrackPanelArt(idx);
    recalcLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PlayerWindow::onTrackSelected(int idx) {
    currentTrack_ = idx;
}

// ── Playback ─────────────────────────────────────────────────────────────────

std::string PlayerWindow::getActiveDeviceKey() {
    if (useWasapi_) {
        auto devId = db_.loadSetting("wasapi_device_id");
        return devId.empty() ? "wasapi" : "wasapi:" + devId;
    }
    auto vid = db_.loadSetting("usb_vid");
    auto pid = db_.loadSetting("usb_pid");
    if (vid.empty()) vid = "32BB";
    if (pid.empty()) pid = "0004";
    return vid + ":" + pid;
}

void PlayerWindow::applyDeviceEq(int sampleRate, int channels) {
    std::string key = getActiveDeviceKey();
    EqAssignment assign;
    if (!db_.loadEqAssignment(key, assign) &&
        !db_.loadEqAssignment("global", assign)) {
        eqManager_.clear();
        return;
    }
    auto* profile = eqProfiles_.findByKey(assign.name, assign.source, assign.form);
    if (profile)
        eqManager_.applyProfile(profile, sampleRate, channels);
    else
        eqManager_.clear();
}

void PlayerWindow::onPlay() {
    if (currentAlbum_ < 0 || currentTrack_ < 0) return;
    const Track& t = albums_[currentAlbum_].tracks[currentTrack_];
    printf("[Play] album=%d track=%d path='%s'\n",
           currentAlbum_, currentTrack_, t.filePath.c_str());
    fflush(stdout);

    {
        std::lock_guard<std::mutex> lk(gaplessMu_);
        stopGapless_.store(true);
        gaplessSignal_ = false;
        gaplessCv_.notify_one();
    }
    if (gaplessThread_.joinable()) gaplessThread_.join();
    stopGapless_.store(false);

    decoder_.stop();
    nextDecoder_.close();
    active_ = &decoder_;

    if (!decoder_.open(t.filePath)) return;

    // Update UI state
    currentTitleW_ = utf8ToWide(t.title);
    currentArtistW_ = utf8ToWide(t.artist);

    int durationSec = t.durationMs > 0 ? t.durationMs / 1000
                    : active_->totalFrames() / (active_->sampleRate() ? active_->sampleRate() : 44100);
    seekTotalMs_ = durationSec * 1000;
    seekPosMs_ = 0;
    playedFrames_.store(0);
    isPlaying_ = true;

    // Load transport art
    if (currentAlbum_ >= 0 && currentAlbum_ < (int)albums_.size())
        loadTransportArt(albums_[currentAlbum_].artPath);

    // Update track panel selection
    if (selectedAlbumIdx_ != currentAlbum_) {
        selectedAlbumIdx_ = currentAlbum_;
        trackPanelOpen_ = true;
        trackScrollY_ = 0;
        loadTrackPanelArt(currentAlbum_);
        recalcLayout();
    }

    InvalidateRect(hwnd_, nullptr, FALSE);

    output_->stop();
    int fileSr = active_->sampleRate();
    int outSr  = fileSr;
    bool isBitperfect = bitperfectMode_.load();
    // Bit-perfect: request the file's native depth so the DAC negotiates a matched
    // format (16->16, 24->24). configure() auto-relaxes to the highest available
    // depth if the exact one is unsupported; dr_flac's s32 output is left-justified,
    // so feeding it into a wider slot stays bit-exact. Normal path always uses 32.
    int reqBits = isBitperfect ? active_->bitsPerSample() : 32;
    bool cfgOk = output_->configure(fileSr, active_->channels(), reqBits, isBitperfect);
    if (!cfgOk && !useWasapi_ && !isBitperfect) {
        auto rates = usbDriver_.getOutputRates();
        outSr  = pickOutputRate(fileSr, rates);
        cfgOk  = output_->configure(outSr, active_->channels(), 32, isBitperfect);
        printf("[USB] native %d Hz unsupported, resampling to %d Hz\n", fileSr, outSr);
    }
    if (!cfgOk) {
        if (isBitperfect) {
            MessageBoxW(hwnd_, L"DAC does not support native sample rate.\nStrict Bitperfect mode active: playback aborted to preserve audio purity.",
                L"Bitperfect Failure", MB_OK | MB_ICONERROR);
        } else {
            MessageBoxW(hwnd_, L"Audio output failed to configure.\nCheck Audio Settings.",
                L"Audio configure failed", MB_OK | MB_ICONERROR);
        }
        active_->stop();
        isPlaying_ = false;
        return;
    }
    outSr = output_->getConfiguredRate();

    int capturedOutSr  = outSr;
    int capturedFileSr = fileSr;
    int capturedDacCh  = output_->getConfiguredChannels();
    auto* outPtr = output_.get();

    // Both modes decode the lossless int32 stream; Bit-Perfect writes it verbatim,
    // Reference EQ applies parametric EQ in place (double math, single rounded snap)
    // before the same int32 write. Exactly one branch below populates this callback.
    PcmS32Callback callbackI32;

  if (isBitperfect) {
    eqManager_.clear();
    if (capturedFileSr != capturedOutSr)
        printf("[Bitperfect] WARNING: rate mismatch %d->%d should have aborted configure\n",
               capturedFileSr, capturedOutSr);
    printf("[Bitperfect] lossless int32 path: %d-bit source @ %d Hz -> DAC %d-bit\n",
           active_->bitsPerSample(), capturedOutSr, usbDriver_.getConfiguredBitDepth());
    callbackI32 = [this, outPtr](const int32_t* d, int n) {
        if (d == nullptr || n == 0) return;
        int srcCh  = active_->channels();
        int frames = srcCh > 0 ? n / srcCh : n;
        int got = outPtr->writeInt32Blocking(d, n);
        if (got < n) {
            static DWORD lastShortLog = 0;
            DWORD nowMs = GetTickCount();
            if ((nowMs - lastShortLog) >= 1000) {
                printf("[Bitperfect] short write: wanted=%d got=%d\n", n, got);
                fflush(stdout);
                lastShortLog = nowMs;
            }
        }
        playedFrames_.fetch_add(frames, std::memory_order_relaxed);
    };
  } else {
    // Reference EQ: bit-transparent except for parametric EQ. Lossless int32
    // decode, EQ applied in 64-bit double via EqProcessor::process32 (single
    // rounded snap to the 32-bit grid), 32-bit to the DAC at the native rate.
    // No resample, no upmix, no software gain beyond the EQ preamp.
    applyDeviceEq(capturedOutSr, active_->channels());
    printf("[ReferenceEQ] int32 EQ path: %d-bit source @ %d Hz -> DAC %d-bit, EQ %s\n",
           active_->bitsPerSample(), capturedOutSr, usbDriver_.getConfiguredBitDepth(),
           eqManager_.isActive() ? "active" : "bypass");

    callbackI32 = [this, outPtr](const int32_t* d, int n) {
        if (d == nullptr || n == 0) return;
        int srcCh  = active_->channels();
        int frames = srcCh > 0 ? n / srcCh : n;

        // EQ in place on the decoder's own int32 buffer (not reused after return).
        if (eqManager_.isActive())
            eqManager_.processInPlaceInt32(const_cast<int32_t*>(d), n);

        int got = outPtr->writeInt32Blocking(d, n);
        if (got < n) {
            static DWORD lastShortLog = 0;
            DWORD nowMs = GetTickCount();
            if ((nowMs - lastShortLog) >= 1000) {
                printf("[ReferenceEQ] short write: wanted=%d got=%d\n", n, got);
                fflush(stdout);
                lastShortLog = nowMs;
            }
        }
        playedFrames_.fetch_add(frames, std::memory_order_relaxed);
    };

  } // end Reference EQ setup

    prepareNextTrack();
    active_->setDoneCallback([this] {
        std::lock_guard<std::mutex> lk(gaplessMu_);
        gaplessSignal_ = true;
        gaplessCv_.notify_one();
    });

    // Both Bit-Perfect and Reference EQ decode via the lossless int32 path.
    active_->startAsyncInt32(callbackI32);

    int preBufferSamples = output_->getPreBufferSamples();
    if (!output_->waitForData(preBufferSamples, 2000))
        printf("[Audio] WARNING: pre-buffer incomplete (%zu / %d samples)\n",
               output_->ringAvailable(), preBufferSamples);

    bool startOk = output_->start();
    printf("[onPlay] output_->start() returned %s\n", startOk ? "true" : "false");
    fflush(stdout);
    if (!startOk) {
        MessageBoxW(hwnd_, L"Audio output failed to start.\nCheck Audio Settings.",
            L"Audio start failed", MB_OK | MB_ICONERROR);
        active_->stop();
        isPlaying_ = false;
        return;
    }
    printf("[onPlay] USB streaming started, ring=%zu\n", output_->ringAvailable());
    fflush(stdout);

    startGaplessCoordinator(callbackI32, capturedOutSr, capturedDacCh);
    SetTimer(hwnd_, TIMER_SEEK_UPDATE, 250, nullptr);
}

void PlayerWindow::onStop() {
    {
        std::lock_guard<std::mutex> lk(gaplessMu_);
        stopGapless_.store(true);
        gaplessSignal_ = false;
        gaplessCv_.notify_one();
    }
    if (gaplessThread_.joinable()) gaplessThread_.join();
    stopGapless_.store(false);

    decoder_.stop();
    nextDecoder_.close();
    active_ = &decoder_;
    nextAlbum_ = nextTrack_ = -1;
    if (output_) output_->stop();
    KillTimer(hwnd_, TIMER_SEEK_UPDATE);
    isPlaying_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PlayerWindow::prepareNextTrack() {
    nextAlbum_ = currentAlbum_;
    nextTrack_ = currentTrack_ + 1;
    if (nextAlbum_ < 0 || nextAlbum_ >= (int)albums_.size() ||
        nextTrack_ >= (int)albums_[nextAlbum_].tracks.size()) {
        nextAlbum_ = nextTrack_ = -1;
        return;
    }
    Decoder* preload = (active_ == &decoder_) ? &nextDecoder_ : &decoder_;
    preload->close();
    preload->open(albums_[nextAlbum_].tracks[nextTrack_].filePath);
}

void PlayerWindow::startGaplessCoordinator(PcmS32Callback cbI32, int outSr, int dacCh) {
    if (gaplessThread_.joinable()) gaplessThread_.join();
    // Capture the mode at play start: gapless continuation reuses the callback
    // built for this mode, so a later toggle only takes effect on the next onPlay.
    bool bitperfect = bitperfectMode_.load();
    gaplessThread_ = std::thread([this, cbI32, bitperfect, outSr, dacCh] {
        while (true) {
            std::unique_lock<std::mutex> lk(gaplessMu_);
            gaplessCv_.wait(lk, [this] { return gaplessSignal_ || stopGapless_.load(); });
            if (stopGapless_.load()) break;
            gaplessSignal_ = false;
            lk.unlock();

            printf("[%s][Gapless] EOF fired: playedFrames=%lld ring_avail=%zu\n",
                   logTs(),
                   (long long)playedFrames_.load(std::memory_order_relaxed),
                   output_->ringAvailable());
            fflush(stdout);

            if (nextAlbum_ < 0) break;

            Decoder* incoming = (active_ == &decoder_) ? &nextDecoder_ : &decoder_;
            // Compare incoming file properties against the *current* decoder, not the
            // WASAPI output rate. In shared mode outSr is the OS mix rate (often
            // 48000 Hz) which never matches a 44100 Hz file, causing every transition
            // to be incorrectly non-seamless. The real question is: does the next
            // track need a WASAPI reconfigure? Only when bit-depth changes (bitperfect)
            // or when sample rate / channel count change.
            bool rateMatch = (incoming->sampleRate() == active_->sampleRate() &&
                              incoming->channels()    == active_->channels());
            bool seamless  = rateMatch;
            if (bitperfect && incoming->bitsPerSample() != active_->bitsPerSample())
                seamless = false;
            printf("[%s][Gapless] incoming sr=%d ch=%d bits=%d | active sr=%d ch=%d bits=%d"
                   " | outSr=%d dacCh=%d | seamless=%s\n", logTs(),
                   incoming->sampleRate(), incoming->channels(), incoming->bitsPerSample(),
                   active_->sampleRate(), active_->channels(), active_->bitsPerSample(),
                   outSr, dacCh, seamless ? "true" : "false");
            fflush(stdout);

            active_ = incoming;
            currentAlbum_ = nextAlbum_;
            currentTrack_ = nextTrack_;

            PostMessageW(hwnd_, WM_APP_TRACK_CHANGE, currentAlbum_, currentTrack_);

            if (seamless) {
                incoming->setDoneCallback([this] {
                    std::lock_guard<std::mutex> lk2(gaplessMu_);
                    gaplessSignal_ = true;
                    gaplessCv_.notify_one();
                });
                incoming->startAsyncInt32(cbI32);
            } else {
                PostMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(ID_BTN_PLAY, BN_CLICKED), 0);
                break;
            }

            prepareNextTrack();
        }
    });
}

void PlayerWindow::onSeek(int posMs) {
    if (output_) output_->flush();
    active_->seekMs(posMs);
    playedFrames_.store((int)((int64_t)posMs * active_->sampleRate() / 1000));
}

void PlayerWindow::onTimer() {
    if (!active_->isRunning()) return;
    int sr = active_->sampleRate();
    if (sr == 0) return;
    seekPosMs_ = (int)((int64_t)playedFrames_.load(std::memory_order_relaxed) * 1000 / sr);
    if (seekTotalMs_ <= 0)
        seekTotalMs_ = (int)((int64_t)active_->totalFrames() * 1000 / sr);
    InvalidateRect(hwnd_, &rcTransport_, FALSE);
}

void PlayerWindow::onArtClick() {
    if (currentAlbum_ < 0) return;
    artWin_.show(albums_[currentAlbum_].artPath);
}

// ── Background scan ──────────────────────────────────────────────────────────

void PlayerWindow::setupWatchers() {
    HWND h = hwnd_;
    for (auto& root : db_.loadMusicRoots()) {
        watcher_.watchRoot(root, [h](const std::string&) {
            PostMessageW(h, WM_APP_SCAN_DONE, 1, 0);
        });
    }
}

void PlayerWindow::startBackgroundScan() {
    if (scanning_.load()) return;
    if (scanThread_.joinable()) scanThread_.join();

    scanning_.store(true);
    HWND h = hwnd_;

    scanThread_ = std::thread([this, h]() {
        auto roots = db_.loadMusicRoots();
        auto cache = db_.loadFileCache();

        std::vector<Album> allAlbums;
        int totalScanned = 0, totalSkipped = 0, totalRemoved = 0;

        for (auto& root : roots) {
            auto result = scanLibraryIncremental(root, cache);
            totalScanned += result.filesScanned;
            totalSkipped += result.filesSkipped;

            auto full = scanLibraryParallel(root);
            for (auto& a : full) {
                Album* ex = nullptr;
                for (auto& ea : allAlbums)
                    if (ea.name == a.name && ea.artist == a.artist) { ex = &ea; break; }
                if (!ex) { allAlbums.push_back(std::move(a)); continue; }
                for (auto& t : a.tracks) {
                    bool dup = false;
                    for (auto& et : ex->tracks)
                        if (et.filePath == t.filePath) { dup = true; break; }
                    if (!dup) ex->tracks.push_back(std::move(t));
                }
                ex->sortTracks();
            }
        }

        purgeStaleFiles(allAlbums, totalRemoved);

        printf("[Scan] Done: %d scanned, %d skipped, %d removed\n",
               totalScanned, totalSkipped, totalRemoved);

        {
            std::lock_guard<std::mutex> lk(scanMu_);
            scanResult_ = std::move(allAlbums);
        }
        scanning_.store(false);
        PostMessageW(h, WM_APP_SCAN_DONE, 0, 0);
    });
}

void PlayerWindow::onScanDone() {
    if (scanning_.load()) return;

    std::vector<Album> newAlbums;
    {
        std::lock_guard<std::mutex> lk(scanMu_);
        newAlbums = std::move(scanResult_);
    }

    if (newAlbums.empty() && albums_.empty()) return;

    albums_ = std::move(newAlbums);

    std::vector<Track> allTracks;
    for (auto& a : albums_)
        for (auto& t : a.tracks) allTracks.push_back(t);
    db_.saveTracks(allTracks);
    db_.saveAlbums(albums_);

    clearGridArtCache();
    trackPanelArtAlbum_ = -1;
    if (trackPanelArtBmp_) { DeleteObject(trackPanelArtBmp_); trackPanelArtBmp_ = nullptr; }

    recalcLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);

    printf("[Library] Updated: %d albums, %d tracks\n",
           (int)albums_.size(), (int)allTracks.size());
}

// ── Message loop ─────────────────────────────────────────────────────────────

LRESULT CALLBACK PlayerWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    PlayerWindow* self = (PlayerWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (self) return self->handleMsg(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PlayerWindow::handleMsg(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        recalcLayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT rc; GetClientRect(hwnd_, &rc);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        FillRect(memDC, &rc, hbrMain_);
        SetBkMode(memDC, TRANSPARENT);

        paintSidebar(memDC);
        if (activeNavItem_ == 0)
            paintGrid(memDC);
        else
            paintSettingsPage(memDC);
        if (trackPanelOpen_)
            paintTrackPanel(memDC);
        paintTransport(memDC);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_SEEK_UPDATE) onTimer();
        return 0;

    case WM_APP_SCAN_DONE:
        if (wp == 1) startBackgroundScan();
        else         onScanDone();
        return 0;

    case WM_APP_TRACK_CHANGE: {
        int album = (int)wp, track = (int)lp;
        if (album >= 0 && album < (int)albums_.size() &&
            track >= 0 && track < (int)albums_[album].tracks.size()) {
            const auto& nt = albums_[album].tracks[track];
            currentTitleW_ = utf8ToWide(nt.title);
            currentArtistW_ = utf8ToWide(nt.artist);
            playedFrames_.store(0);
            seekTotalMs_ = nt.durationMs > 0 ? nt.durationMs : 0;
            seekPosMs_ = 0;
            loadTransportArt(albums_[album].artPath);
            selectedAlbumIdx_ = album;
            loadTrackPanelArt(album);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == ID_BTN_PLAY && HIWORD(wp) == BN_CLICKED)
            onPlay();
        return 0;

    case WM_MOUSEMOVE:
        onMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_MOUSELEAVE:
        onMouseLeave();
        return 0;

    case WM_LBUTTONDOWN:
        onLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_LBUTTONUP:
        onLButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_LBUTTONDBLCLK:
        onLButtonDblClk(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_MOUSEWHEEL:
        onMouseWheel(GET_X_LPARAM(lp), GET_Y_LPARAM(lp),
                     GET_WHEEL_DELTA_WPARAM(wp));
        return 0;

    case WM_KEYDOWN:
        switch (wp) {
        case VK_SPACE:
            if (isPlaying_) onStop(); else if (currentAlbum_ >= 0) onPlay();
            return 0;
        case VK_ESCAPE:
            if (trackPanelOpen_) {
                trackPanelOpen_ = false;
                recalcLayout();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        break;

    case WM_DESTROY:
        onStop();
        watcher_.unwatchAll();
        if (scanThread_.joinable()) scanThread_.join();
        usbDriver_.close();

        // Clean up fonts
        HFONT* fonts[] = { &hFontBrand_, &hFontSidebar_, &hFontSidebarActive_, &hFontAlbumTitle_,
                           &hFontArtist_, &hFontTrackRow_, &hFontTrackNumber_, &hFontTransportTitle_,
                           &hFontTransportArtist_, &hFontTime_, &hFontNowPlaying_,
                           &hFontSettingsItem_, &hFontPlaceholder_ };
        for (auto* f : fonts) { if (*f) DeleteObject(*f); *f = nullptr; }

        // Clean up brushes
        HBRUSH* brushes[] = { &hbrMain_, &hbrSidebar_, &hbrTransport_, &hbrTrackPanel_,
                              &hbrHover_, &hbrPlaceholder_ };
        for (auto* b : brushes) { if (*b) DeleteObject(*b); *b = nullptr; }

        // Clean up icon bitmaps
        HBITMAP* icons[] = { &iconPlay_, &iconPause_, &iconStop_, &iconPrev_, &iconNext_,
                             &iconClose_, &iconPlayGreen_, &iconPauseGreen_ };
        for (auto* ic : icons) { if (*ic) DeleteObject(*ic); *ic = nullptr; }

        // Clean up art caches
        clearGridArtCache();
        if (trackPanelArtBmp_) { DeleteObject(trackPanelArtBmp_); trackPanelArtBmp_ = nullptr; }
        if (transportArtBmp_) { DeleteObject(transportArtBmp_); transportArtBmp_ = nullptr; }
        if (thumbBitmap_) { DeleteObject(thumbBitmap_); thumbBitmap_ = nullptr; }

        clearArtworkCache();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
