cmake_minimum_required(VERSION 3.22.1)
project("matrix_player_windows")

set(CMAKE_CXX_STANDARD 17)

# ---------- audio_engine static lib (submodule) ----------
add_subdirectory(audio_engine/src/main/cpp/windows engine_build)

# ---------- sqlite3 (vendored amalgamation) ----------
add_library(sqlite3 STATIC third_party/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party)
target_compile_options(sqlite3 PRIVATE /W0)

# ---------- soxr resampler ----------
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/soxr/cmake/Modules")
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_OPENMP OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WITH_LSR_BINDINGS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/soxr)

# ---------- matrix_player_windows executable ----------
add_executable(${CMAKE_PROJECT_NAME} WIN32
    src/main.cpp
    src/player_window.cpp
    src/wasapi_output.cpp
    src/library.cpp
    src/decoder.cpp
    src/db.cpp
    src/artwork.cpp
    src/art_window.cpp
    src/icons.cpp
    src/eq_profiles.cpp
    src/eq_manager.cpp
    src/app.rc)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    src
    third_party
    third_party/soxr/src
    audio_engine/src/main/cpp
    audio_engine/src/main/cpp/libusb/libusb)

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    /O2
    /W3)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE)

# Copy EQ profiles JSON next to the executable
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/eq_profiles.json
        $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/eq_profiles.json)

# Copy CMU Serif fonts next to the executable
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/fonts
        $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/fonts)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    audio_engine_windows
    usb1
    winusb
    setupapi
    sqlite3
    soxr
    comctl32
    comdlg32
    shell32
    gdiplus
    msimg32
    ole32
    mmdevapi
    avrt)

# ---------- matrix_ab_test (standalone A/B test tool) ----------
add_executable(matrix_ab_test tools/ab_test.cpp)
set_target_properties(matrix_ab_test PROPERTIES WIN32_EXECUTABLE FALSE)
target_compile_options(matrix_ab_test PRIVATE /O2 /W3)
target_compile_definitions(matrix_ab_test PRIVATE NOMINMAX)
target_include_directories(matrix_ab_test PRIVATE
    third_party
    audio_engine/src/main/cpp
    audio_engine/src/main/cpp/libusb/libusb)
target_link_libraries(matrix_ab_test PRIVATE
    audio_engine_windows usb1 winusb setupapi avrt)
