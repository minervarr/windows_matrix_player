#include <windows.h>
#include <cstdio>
#include "player_window.h"

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
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    PlayerWindow player;
    if (!player.create(hInst)) return 1;
    player.run();

    CoUninitialize();
    return 0;
}
