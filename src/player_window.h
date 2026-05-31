#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include "library.h"
#include "decoder.h"
#include "db.h"
#include "art_window.h"
#include "audio_output.h"
#include "wasapi_output.h"
#include "eq_profiles.h"
#include "eq_manager.h"

// Timer IDs
#define TIMER_SEEK_UPDATE    1
#define WM_APP_TRACK_CHANGE  (WM_APP+1)
#define WM_APP_SCAN_DONE     (WM_APP+2)

// Keep these IDs for the gapless coordinator's PostMessage fallback
#define ID_BTN_PLAY     104

// Theme colors
static constexpr COLORREF CLR_BG_MAIN        = RGB(10, 10, 10);
static constexpr COLORREF CLR_BG_SIDEBAR     = RGB(18, 18, 18);
static constexpr COLORREF CLR_BG_TRANSPORT   = RGB(22, 22, 22);
static constexpr COLORREF CLR_BG_TRACKPANEL  = RGB(14, 14, 14);
static constexpr COLORREF CLR_TEXT_PRIMARY    = RGB(242, 242, 242);
static constexpr COLORREF CLR_TEXT_SECONDARY  = RGB(128, 128, 128);
static constexpr COLORREF CLR_TEXT_DIM        = RGB(80, 80, 80);
static constexpr COLORREF CLR_ACCENT          = RGB(0, 200, 83);
static constexpr COLORREF CLR_HOVER           = RGB(38, 38, 38);
static constexpr COLORREF CLR_SEPARATOR       = RGB(36, 36, 36);
static constexpr COLORREF CLR_SEEKBAR_TRACK   = RGB(55, 55, 55);
static constexpr COLORREF CLR_SEEKBAR_FILL    = RGB(0, 200, 83);
static constexpr COLORREF CLR_TILE_PLACEHOLDER = RGB(28, 28, 28);
static constexpr COLORREF CLR_TEXT_ALBUM_TITLE = RGB(255, 255, 255);

class PlayerWindow {
public:
    bool create(HINSTANCE hInst);
    void run();

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleMsg(UINT, WPARAM, LPARAM);

    // Actions
    void onAddFolder();
    void onManageFolders();
    void onAudioSettings();
    void prepareNextTrack();
    void startGaplessCoordinator(PcmS32Callback cbI32, int outSr, int dacCh);
    void onAlbumSelected(int idx);
    void onTrackSelected(int idx);
    void onPlay();
    void onStop();
    void onNext();
    void onPrev();
    void onSeek(int posMs);
    void onTimer();
    void onArtClick();
    void onScanDone();
    void onEqSettings();
    void toggleBitperfectMode();
    void startBackgroundScan();
    void setupWatchers();
    std::string getActiveDeviceKey();
    void applyDeviceEq(int sampleRate, int channels);

    // Layout
    void recalcLayout();

    // Painting
    void paintSidebar(HDC hdc);
    void paintGrid(HDC hdc);
    void paintTrackPanel(HDC hdc);
    void paintTransport(HDC hdc);
    void paintSettingsPage(HDC hdc);

    // Mouse
    void onMouseMove(int x, int y);
    void onMouseLeave();
    void onLButtonDown(int x, int y);
    void onLButtonUp(int x, int y);
    void onLButtonDblClk(int x, int y);
    void onMouseWheel(int x, int y, int delta);

    // Art cache
    HBITMAP getGridArt(int albumIdx);
    void    clearGridArtCache();
    void    loadTrackPanelArt(int albumIdx);
    void    loadTransportArt(const std::string& artPath);

    // Helpers
    int  gridHitTest(int x, int y) const;
    int  trackPanelHitTest(int x, int y) const;
    int  sidebarHitTest(int x, int y) const;
    int  transportBtnHitTest(int x, int y) const;
    int  settingsHitTest(int x, int y) const;
    bool isInSeekbar(int x, int y) const;
    int  seekbarPosToMs(int x) const;

    HWND      hwnd_  = nullptr;
    HINSTANCE hInst_ = nullptr;

    // Fonts
    HFONT hFontBrand_          = nullptr;
    HFONT hFontSidebar_        = nullptr;
    HFONT hFontSidebarActive_  = nullptr;
    HFONT hFontAlbumTitle_     = nullptr;
    HFONT hFontArtist_         = nullptr;
    HFONT hFontTrackRow_       = nullptr;
    HFONT hFontTrackNumber_    = nullptr;
    HFONT hFontTransportTitle_ = nullptr;
    HFONT hFontTransportArtist_= nullptr;
    HFONT hFontTime_           = nullptr;
    HFONT hFontNowPlaying_     = nullptr;
    HFONT hFontSettingsItem_   = nullptr;
    HFONT hFontPlaceholder_    = nullptr;

    // Cached SVG icon bitmaps (rasterized once at startup)
    HBITMAP iconPlay_       = nullptr;
    HBITMAP iconPause_      = nullptr;
    HBITMAP iconStop_       = nullptr;
    HBITMAP iconPrev_       = nullptr;
    HBITMAP iconNext_       = nullptr;
    HBITMAP iconClose_      = nullptr;
    HBITMAP iconPlayGreen_  = nullptr;
    HBITMAP iconPauseGreen_ = nullptr;

    // Brushes
    HBRUSH hbrMain_       = nullptr;
    HBRUSH hbrSidebar_    = nullptr;
    HBRUSH hbrTransport_  = nullptr;
    HBRUSH hbrTrackPanel_ = nullptr;
    HBRUSH hbrHover_      = nullptr;
    HBRUSH hbrPlaceholder_= nullptr;

    // Layout zones
    RECT rcSidebar_    = {};
    RECT rcGrid_       = {};
    RECT rcTrackPanel_ = {};
    RECT rcTransport_  = {};

    // Transport sub-regions
    RECT rcTransportArt_  = {};
    RECT rcTransportInfo_ = {};
    RECT rcBtnPrev_       = {};
    RECT rcBtnPlay_       = {};
    RECT rcBtnStop_       = {};
    RECT rcBtnNext_       = {};
    RECT rcTimeDisplay_   = {};
    RECT rcSeekbar_       = {};

    // Sidebar items
    RECT rcBrand_       = {};
    RECT rcNavAlbums_   = {};
    RECT rcNavSettings_ = {};

    // Settings page items
    RECT rcSettingsAddFolder_    = {};
    RECT rcSettingsManage_       = {};
    RECT rcSettingsAudio_        = {};
    RECT rcSettingsEq_           = {};
    RECT rcSettingsBitperfect_   = {};

    // Grid state
    int gridScrollY_     = 0;
    int gridTileSize_    = 180;
    int gridArtSize_     = 150;
    int gridCols_        = 1;
    int gridTotalHeight_ = 0;
    int gridPadX_        = 24;
    int gridPadY_        = 16;

    // Track panel scroll
    int trackScrollY_   = 0;
    int trackRowHeight_  = 36;
    int trackPanelArtSize_ = 260;
    RECT rcTrackClose_   = {};
    int  trackHeaderHeight_ = 0;

    // Hover state
    int hoverAlbumIdx_      = -1;
    int hoverTrackIdx_      = -1;
    int hoverSidebarItem_   = -1;
    int hoverTransportBtn_  = -1;
    int hoverSettingsItem_  = -1;
    bool hoverTrackClose_   = false;
    bool hoverSeekbar_      = false;
    bool seekDragging_      = false;

    // Selection / navigation
    int  selectedAlbumIdx_  = -1;
    bool trackPanelOpen_    = false;
    int  activeNavItem_     = 0;  // 0=Albums, 1=Settings

    // Now-playing display state
    std::wstring currentTitleW_;
    std::wstring currentArtistW_;
    int          seekPosMs_   = 0;
    int          seekTotalMs_ = 0;

    // Art caches
    std::unordered_map<int, HBITMAP> gridArtCache_;
    HBITMAP trackPanelArtBmp_   = nullptr;
    int     trackPanelArtAlbum_ = -1;
    HBITMAP transportArtBmp_    = nullptr;
    std::string transportArtPath_;

    // Library + playback (unchanged from original)
    std::vector<Album> albums_;
    int  currentAlbum_ = -1;
    int  currentTrack_ = -1;

    Decoder          decoder_;
    Decoder          nextDecoder_;
    Decoder*         active_    = &decoder_;

    int              nextAlbum_ = -1;
    int              nextTrack_ = -1;

    std::thread              gaplessThread_;
    std::mutex               gaplessMu_;
    std::condition_variable  gaplessCv_;
    bool                     gaplessSignal_  = false;
    std::atomic<bool>        stopGapless_{false};
    Db               db_;
    ArtWindow        artWin_;
    UsbAudioDriver   usbDriver_;
    bool             usbOpen_  = false;

    std::unique_ptr<AudioOutput> output_;
    bool             useWasapi_     = false;
    std::atomic<bool> bitperfectMode_{false};
    std::wstring     wasapiDeviceId_;
    WasapiMode       wasapiMode_    = WasapiMode::Shared;

    HBITMAP thumbBitmap_ = nullptr;
    std::atomic<int> playedFrames_{0};

    std::vector<float> resampleBuf_;
    std::vector<float> upmixBuf_;

    EqProfileStore   eqProfiles_;
    EqManager        eqManager_;

    FolderWatcher        watcher_;
    std::thread          scanThread_;
    std::mutex           scanMu_;
    std::vector<Album>   scanResult_;
    std::atomic<bool>    scanning_{false};

    bool isPlaying_ = false;
    bool mouseTracking_ = false;
};
