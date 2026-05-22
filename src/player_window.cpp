#include "player_window.h"
#include "artwork.h"
#include <commdlg.h>
#include <shlobj.h>
#include <cstdio>
#include <cmath>
#include <vector>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

// Linear interpolation resampler. Stateless per-chunk; keeps a fractional
// position across calls via the carried tail sample.
static std::vector<float> resample(
    const float* in, int inFrames, int channels,
    int inRate, int outRate,
    std::vector<float>& tail)  // tail holds one frame (channels floats) from previous chunk
{
    if (inRate == outRate) return std::vector<float>(in, in + inFrames * channels);

    // Number of output frames for this input chunk
    int outFrames = (int)std::ceil((double)inFrames * outRate / inRate);
    std::vector<float> out(outFrames * channels);

    // Build a view: [tail | in] so index -1 is tail
    auto sample = [&](int frame, int ch) -> float {
        if (frame < 0) return tail.empty() ? 0.f : tail[ch];
        int idx = frame * channels + ch;
        return (idx < inFrames * channels) ? in[idx] : in[(inFrames - 1) * channels + ch];
    };

    for (int i = 0; i < outFrames; i++) {
        double t = (double)i * inRate / outRate;   // position in input frames
        int    x0   = (int)t;
        float  frac = (float)(t - x0);
        int    x1   = x0 + 1;
        for (int c = 0; c < channels; c++) {
            out[i * channels + c] = sample(x0, c) * (1.f - frac)
                                  + sample(x1, c) * frac;
        }
    }

    // Save last input frame as tail for next chunk
    tail.resize(channels);
    for (int c = 0; c < channels; c++)
        tail[c] = in[(inFrames - 1) * channels + c];

    return out;
}

// Pick the best supported output rate for a given input rate.
// Prefers integer multiples; otherwise the lowest supported rate >= inRate.
static int pickOutputRate(int inRate, const std::vector<int>& supported) {
    // Prefer exact integer multiple
    for (int r : supported)
        if (r % inRate == 0) return r;
    // Fallback: lowest rate above inRate
    for (int r : supported)
        if (r > inRate) return r;
    // Last resort: highest available
    return supported.empty() ? 48000 : supported.back();
}

static const wchar_t* MAIN_CLASS = L"MatrixPlayerMain";

bool PlayerWindow::create(HINSTANCE hInst) {
    hInst_ = hInst;
    INITCOMMONCONTROLSEX ice = { sizeof(ice), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&ice);

    WNDCLASSEXW wc = {};
    wc.cbSize       = sizeof(wc);
    wc.lpfnWndProc  = wndProc;
    wc.hInstance    = hInst;
    wc.hbrBackground= (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName= MAIN_CLASS;
    wc.hCursor      = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(0, MAIN_CLASS, L"Matrix Player",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1024, 640,
        nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    // Create child controls
    HWND btn = CreateWindowExW(0, L"BUTTON", L"Choose Folder",
        WS_CHILD | WS_VISIBLE, 0, 0, 120, 28, hwnd_, (HMENU)ID_BTN_FOLDER, hInst, nullptr);

    listAlbums_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
        0, 0, 200, 400, hwnd_, (HMENU)ID_LIST_ALBUMS, hInst, nullptr);

    listTracks_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
        0, 0, 400, 400, hwnd_, (HMENU)ID_LIST_TRACKS, hInst, nullptr);

    artStatic_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", nullptr,
        WS_CHILD | WS_VISIBLE | SS_BITMAP | SS_NOTIFY,
        0, 0, 180, 180, hwnd_, (HMENU)ID_STATIC_ART, hInst, nullptr);

    btnPlay_ = CreateWindowExW(0, L"BUTTON", L"Play",
        WS_CHILD | WS_VISIBLE, 0, 0, 70, 28, hwnd_, (HMENU)ID_BTN_PLAY, hInst, nullptr);

    btnStop_ = CreateWindowExW(0, L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE, 0, 0, 70, 28, hwnd_, (HMENU)ID_BTN_STOP, hInst, nullptr);

    seekbar_ = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        0, 0, 400, 24, hwnd_, (HMENU)ID_SEEKBAR, hInst, nullptr);

    lblTitle_ = CreateWindowExW(0, L"STATIC", L"No track selected",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 400, 20, hwnd_, (HMENU)ID_STATIC_TITLE, hInst, nullptr);

    lblTime_ = CreateWindowExW(0, L"STATIC", L"0:00 / 0:00",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 120, 20, hwnd_, (HMENU)ID_STATIC_TIME, hInst, nullptr);

    artWin_.create(hInst);
    layoutControls();

    // Open DB next to exe
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dbPathW = exePath;
    dbPathW = dbPathW.substr(0, dbPathW.rfind(L'\\') + 1) + L"matrix_player.db";
    std::string dbPath(dbPathW.begin(), dbPathW.end());
    db_.open(dbPath);

    // Open USB DAC: Hiby FC4 VID=0x32BB PID=0x0004 (libusbK must be bound via Zadig)
    if (usbDriver_.open(0x32BB, 0x0004)) {
        usbDriver_.parseDescriptors();
        auto rates = usbDriver_.getOutputRates();
        printf("[USB] DAC opened. Supported rates:");
        for (int r : rates) printf(" %d", r);
        printf("\n");
    } else {
        printf("[USB] Failed to open DAC VID=32BB PID=0004 - check libusbK binding in Zadig\n");
        MessageBoxW(hwnd_,
            L"USB DAC not found (VID=32BB PID=0004).\n\n"
            L"Steps to fix:\n"
            L"1. Open Zadig\n"
            L"2. Select the Hiby FC4 interface MI_00\n"
            L"3. Install libusbK driver\n"
            L"4. Restart this app\n\n"
            L"Audio will be silent until the DAC is bound.",
            L"USB DAC not found", MB_OK | MB_ICONWARNING);
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

void PlayerWindow::layoutControls() {
    RECT rc; GetClientRect(hwnd_, &rc);
    int W = rc.right, H = rc.bottom;
    int pad = 8;

    // Top bar: Choose Folder button
    SetWindowPos(GetDlgItem(hwnd_, ID_BTN_FOLDER), nullptr, pad, pad, 140, 28, SWP_NOZORDER);

    // Album list: left column
    int listTop = pad + 28 + pad;
    int listH   = H - listTop - 70;
    SetWindowPos(listAlbums_, nullptr, pad, listTop, 200, listH, SWP_NOZORDER);

    // Art thumbnail: below album list... actually above track list on right of album list
    int artX = pad + 200 + pad;
    SetWindowPos(artStatic_, nullptr, artX, listTop, 180, 180, SWP_NOZORDER);

    // Track list: right of art
    int trackX = artX + 180 + pad;
    int trackW = W - trackX - pad;
    SetWindowPos(listTracks_, nullptr, trackX, listTop, trackW, listH, SWP_NOZORDER);

    // Bottom controls
    int ctrlY = H - 60;
    SetWindowPos(btnPlay_, nullptr, pad, ctrlY, 70, 28, SWP_NOZORDER);
    SetWindowPos(btnStop_, nullptr, pad + 80, ctrlY, 70, 28, SWP_NOZORDER);
    SetWindowPos(lblTitle_, nullptr, pad + 160, ctrlY, 400, 20, SWP_NOZORDER);
    SetWindowPos(lblTime_,  nullptr, pad + 160, ctrlY + 22, 140, 20, SWP_NOZORDER);
    SetWindowPos(seekbar_,  nullptr, pad + 310, ctrlY + 20, W - 330 - pad, 24, SWP_NOZORDER);
}

void PlayerWindow::onChooseFolder() {
    wchar_t path[MAX_PATH] = {};
    BROWSEINFOW bi = {};
    bi.hwndOwner = hwnd_;
    bi.lpszTitle = L"Select music folder";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);

    std::string root(path, path + wcslen(path));
    albums_ = scanLibrary(root);

    // Persist
    std::vector<Track> allTracks;
    for (auto& a : albums_)
        for (auto& t : a.tracks) allTracks.push_back(t);
    db_.saveTracks(allTracks);
    db_.saveAlbums(albums_);

    populateAlbumList();
}

void PlayerWindow::populateAlbumList() {
    SendMessageW(listAlbums_, LB_RESETCONTENT, 0, 0);
    for (auto& a : albums_) {
        std::wstring ws(a.name.begin(), a.name.end());
        SendMessageW(listAlbums_, LB_ADDSTRING, 0, (LPARAM)ws.c_str());
    }
}

void PlayerWindow::populateTrackList(int albumIdx) {
    SendMessageW(listTracks_, LB_RESETCONTENT, 0, 0);
    if (albumIdx < 0 || albumIdx >= (int)albums_.size()) return;
    for (auto& t : albums_[albumIdx].tracks) {
        std::wstring ws(t.title.begin(), t.title.end());
        SendMessageW(listTracks_, LB_ADDSTRING, 0, (LPARAM)ws.c_str());
    }
    showThumbnailArt(albums_[albumIdx].artPath);
}

void PlayerWindow::showThumbnailArt(const std::string& path) {
    if (thumbBitmap_) { DeleteObject(thumbBitmap_); thumbBitmap_ = nullptr; }
    thumbBitmap_ = loadArtwork(path, 180, 180);
    SendMessageW(artStatic_, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)thumbBitmap_);
}

void PlayerWindow::onAlbumSelected(int idx) {
    currentAlbum_ = idx;
    populateTrackList(idx);
}

void PlayerWindow::onTrackSelected(int idx) {
    currentTrack_ = idx;
}

void PlayerWindow::onPlay() {
    if (currentAlbum_ < 0 || currentTrack_ < 0) return;
    const Track& t = albums_[currentAlbum_].tracks[currentTrack_];

    decoder_.stop();
    if (!decoder_.open(t.filePath)) return;

    std::wstring ws(t.title.begin(), t.title.end());
    SetWindowTextW(lblTitle_, ws.c_str());

    int durationSec = t.durationMs > 0 ? t.durationMs / 1000
                                       : decoder_.totalFrames() / (decoder_.sampleRate() ? decoder_.sampleRate() : 44100);
    SendMessageW(seekbar_, TBM_SETRANGE, TRUE, MAKELONG(0, durationSec));
    SendMessageW(seekbar_, TBM_SETPOS, TRUE, 0);

    playedFrames_.store(0);

    usbDriver_.stop();
    int fileSr = decoder_.sampleRate();
    int outSr  = fileSr;
    bool cfgOk = usbDriver_.configure(fileSr, decoder_.channels(), 32);
    if (!cfgOk) {
        // Native rate unsupported — pick best available and resample
        auto rates = usbDriver_.getOutputRates();
        outSr  = pickOutputRate(fileSr, rates);
        cfgOk  = usbDriver_.configure(outSr, decoder_.channels(), 32);
        printf("[USB] native %d Hz unsupported, resampling to %d Hz\n", fileSr, outSr);
    }
    bool startOk = cfgOk && usbDriver_.start();
    printf("[USB] configure(%d Hz, %dch, 32bit) -> %s | start -> %s\n",
        outSr, decoder_.channels(),
        cfgOk ? "ok" : "FAILED", startOk ? "ok" : "FAILED");
    if (!startOk) {
        MessageBoxW(hwnd_, L"USB DAC failed to start. Check matrix_player.log.",
            L"USB start failed", MB_OK | MB_ICONERROR);
        decoder_.stop();
        return;
    }

    int capturedOutSr  = outSr;
    int capturedFileSr = fileSr;
    int capturedDacCh  = usbDriver_.getConfiguredChannels();
    auto tail = std::make_shared<std::vector<float>>();
    decoder_.startAsync([this, capturedFileSr, capturedOutSr, capturedDacCh, tail](const float* d, int n) {
        int srcCh  = decoder_.channels();
        int frames = srcCh > 0 ? n / srcCh : n;

        auto writeAll = [this](const float* p, int count) {
            while (count > 0 && decoder_.isRunning()) {
                int written = usbDriver_.writeFloat32(p, count);
                if (written > 0) { p += written; count -= written; }
                else Sleep(2);
            }
        };

        // Resample if needed, then upmix if DAC has more channels than the source.
        std::vector<float> buf;
        const float* send = d;
        int sendCount = n;

        if (capturedFileSr != capturedOutSr) {
            buf = resample(d, frames, srcCh, capturedFileSr, capturedOutSr, *tail);
            send = buf.data();
            sendCount = (int)buf.size();
        }

        if (capturedDacCh > srcCh && srcCh > 0) {
            // Upmix: duplicate channels to fill the DAC's expected channel count.
            // Most common case: mono source → stereo DAC (srcCh=1, capturedDacCh=2).
            int sendFrames = sendCount / srcCh;
            std::vector<float> up(sendFrames * capturedDacCh);
            for (int f = 0; f < sendFrames; f++)
                for (int c = 0; c < capturedDacCh; c++)
                    up[f * capturedDacCh + c] = send[f * srcCh + (c % srcCh)];
            writeAll(up.data(), (int)up.size());
        } else {
            writeAll(send, sendCount);
        }
        playedFrames_.fetch_add(frames, std::memory_order_relaxed);
    });

    SetTimer(hwnd_, TIMER_SEEK_UPDATE, 500, nullptr);
}

void PlayerWindow::onStop() {
    decoder_.stop();
    usbDriver_.stop();
    KillTimer(hwnd_, TIMER_SEEK_UPDATE);
}

void PlayerWindow::onSeek(int posMs) {
    decoder_.seekMs(posMs);
}

void PlayerWindow::onTimer() {
    if (!decoder_.isRunning()) return;
    int sr = decoder_.sampleRate();
    if (sr == 0) return;
    int posMs  = playedFrames_.load(std::memory_order_relaxed) * 1000 / sr;
    int totMs  = decoder_.totalFrames() * 1000 / sr;
    SendMessageW(seekbar_, TBM_SETPOS, TRUE, posMs / 1000);
    updateTimeLabel(posMs, totMs);
}

void PlayerWindow::updateTimeLabel(int posMs, int totalMs) {
    wchar_t buf[64];
    swprintf_s(buf, L"%d:%02d / %d:%02d",
        posMs/60000, (posMs%60000)/1000,
        totalMs/60000, (totalMs%60000)/1000);
    SetWindowTextW(lblTime_, buf);
}

void PlayerWindow::onArtClick() {
    if (currentAlbum_ < 0) return;
    artWin_.show(albums_[currentAlbum_].artPath);
}

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
        layoutControls();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_SEEK_UPDATE) onTimer();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
            case ID_BTN_FOLDER: onChooseFolder(); break;
            case ID_BTN_PLAY:   onPlay();         break;
            case ID_BTN_STOP:   onStop();         break;
            case ID_LIST_ALBUMS:
                if (HIWORD(wp) == LBN_SELCHANGE)
                    onAlbumSelected((int)SendMessageW(listAlbums_, LB_GETCURSEL, 0, 0));
                break;
            case ID_LIST_TRACKS:
                if (HIWORD(wp) == LBN_SELCHANGE || HIWORD(wp) == LBN_DBLCLK)
                    onTrackSelected((int)SendMessageW(listTracks_, LB_GETCURSEL, 0, 0));
                break;
            case ID_STATIC_ART:
                if (HIWORD(wp) == STN_CLICKED) onArtClick();
                break;
        }
        return 0;
    case WM_HSCROLL:
        if ((HWND)lp == seekbar_) {
            int pos = (int)SendMessageW(seekbar_, TBM_GETPOS, 0, 0);
            onSeek(pos * 1000);
        }
        return 0;
    case WM_DESTROY:
        onStop();
        usbDriver_.close();
        clearArtworkCache();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
