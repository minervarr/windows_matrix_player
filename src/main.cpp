#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <thread>
#include <atomic>
#include <vector>
#include "player_window.h"
#include "usb_audio.h"

#pragma comment(lib, "winmm.lib")

// Minimal iso-streaming self-test. Streams a pure generated 440 Hz sine
// straight through the engine data path (writeFloat32 -> ring -> submitTransfer)
// with NO decoder, NO GUI, NO EQ, NO resampler, NO gapless coordinator.
// If this pops, the bug is in audio_engine / libusb iso on Windows. If clean,
// the bug is in how PlayerWindow feeds the engine.
// Trigger by setting env var MATRIX_ISO_TEST=1 before launching.
static int runIsoSelfTest() {
    printf("[isotest] starting minimal iso streaming self-test\n"); fflush(stdout);
    UsbAudioDriver drv;
    if (!drv.open(0x32BB, 0x0004)) { printf("[isotest] open failed\n"); return 1; }
    drv.parseDescriptors();
    const int rate = 48000, ch = 2;
    if (!drv.configure(rate, ch, 32)) { printf("[isotest] configure failed\n"); return 1; }

    std::atomic<bool> run{true};
    std::thread feeder([&]{
        const double w = 2.0 * 3.14159265358979323846 * 440.0 / rate;
        double phase = 0.0;
        const int FR = 256;
        std::vector<float> buf(FR * ch);
        while (run.load()) {
            for (int i = 0; i < FR; i++) {
                float s = 0.2f * (float)sin(phase);
                phase += w; if (phase > 2.0*3.14159265358979323846) phase -= 2.0*3.14159265358979323846;
                buf[i*ch] = s; buf[i*ch+1] = s;
            }
            int total = FR * ch, off = 0;
            while (off < total && run.load()) {
                int wr = drv.writeFloat32(buf.data() + off, total - off);
                if (wr > 0) off += wr; else Sleep(1);
            }
        }
    });

    Sleep(80);          // let the feeder pre-fill the ring
    drv.start();
    printf("[isotest] streaming 440 Hz for 20 s -- listen for popcorn\n"); fflush(stdout);
    Sleep(20000);
    run.store(false); feeder.join();
    drv.stop(); drv.close();
    printf("[isotest] done\n"); fflush(stdout);
    return 0;
}

static void openLogFile() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring logPath = exePath;
    logPath = logPath.substr(0, logPath.rfind(L'\\') + 1) + L"matrix_player.log";
    _wfreopen(logPath.c_str(), L"w", stdout);
    _wfreopen(logPath.c_str(), L"a", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    openLogFile();
    // Raise system timer resolution to 1 ms for the app lifetime so any
    // Sleep()/WaitForSingleObject() in audio paths (notably the pre-buffer
    // wait in PlayerWindow::onPlay) doesn't get rounded to ~15.6 ms.
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (getenv("MATRIX_ISO_TEST")) {
        int rc = runIsoSelfTest();
        CoUninitialize();
        timeEndPeriod(1);
        return rc;
    }

    PlayerWindow player;
    if (!player.create(hInst)) { timeEndPeriod(1); return 1; }
    player.run();

    CoUninitialize();
    timeEndPeriod(1);
    return 0;
}
