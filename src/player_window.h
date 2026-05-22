#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <atomic>
#include "library.h"
#include "decoder.h"
#include "db.h"
#include "art_window.h"
#include "usb_audio.h"

// IDs for Win32 child controls
#define ID_BTN_FOLDER   101
#define ID_LIST_ALBUMS  102
#define ID_LIST_TRACKS  103
#define ID_BTN_PLAY     104
#define ID_BTN_STOP     105
#define ID_SEEKBAR      106
#define ID_STATIC_TITLE 107
#define ID_STATIC_TIME  108
#define ID_STATIC_ART   109   // clickable album art thumbnail area

// Timer IDs
#define TIMER_SEEK_UPDATE 1   // 500ms tick to refresh seekbar + time label

class PlayerWindow {
public:
    bool create(HINSTANCE hInst);
    void run();   // message loop

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleMsg(UINT, WPARAM, LPARAM);

    void onChooseFolder();
    void onAlbumSelected(int idx);
    void onTrackSelected(int idx);
    void onPlay();
    void onStop();
    void onSeek(int posMs);
    void onTimer();
    void onArtClick();

    void layoutControls();
    void populateAlbumList();
    void populateTrackList(int albumIdx);
    void updateTimeLabel(int posMs, int totalMs);
    void showThumbnailArt(const std::string& path);

    HWND hwnd_       = nullptr;
    HWND listAlbums_ = nullptr;
    HWND listTracks_ = nullptr;
    HWND btnPlay_    = nullptr;
    HWND btnStop_    = nullptr;
    HWND seekbar_    = nullptr;
    HWND lblTitle_   = nullptr;
    HWND lblTime_    = nullptr;
    HWND artStatic_  = nullptr;
    HINSTANCE hInst_ = nullptr;

    std::vector<Album> albums_;
    int  currentAlbum_ = -1;
    int  currentTrack_ = -1;

    Decoder          decoder_;
    Db               db_;
    ArtWindow        artWin_;
    UsbAudioDriver   usbDriver_;

    HBITMAP thumbBitmap_ = nullptr;
    std::atomic<int> playedFrames_{0};
};
