cmake_minimum_required(VERSION 3.22.1)
project("matrix_player_windows")

set(CMAKE_CXX_STANDARD 17)

# ---------- audio_engine static lib (submodule) ----------
add_subdirectory(audio_engine/src/main/cpp/windows engine_build)

# ---------- sqlite3 (vendored amalgamation) ----------
add_library(sqlite3 STATIC third_party/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party)
target_compile_options(sqlite3 PRIVATE /W0)

# ---------- matrix_player_windows executable ----------
add_executable(${CMAKE_PROJECT_NAME} WIN32
    src/main.cpp
    src/player_window.cpp
    src/library.cpp
    src/decoder.cpp
    src/db.cpp
    src/artwork.cpp
    src/art_window.cpp)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    src
    third_party
    audio_engine/src/main/cpp
    audio_engine/src/main/cpp/libusb/libusb)

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    /O2
    /W3)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    audio_engine_windows
    usb1
    winusb
    setupapi
    sqlite3
    comctl32
    comdlg32
    shell32
    gdiplus
    ole32)
