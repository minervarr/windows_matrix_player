#pragma once
#include <string>
#include <vector>

// TODO(parallel-scan): Use std::thread pool (one per CPU core) like the Android
// player's ExecutorService scan for large libraries on NVMe.
// TODO(formats): Add DSF/DFF, WAV, MP3 (via FFmpeg) once decoder layer is swapped.

struct Track {
    int         id = 0;
    std::string title;
    std::string artist;
    std::string album;
    std::string filePath;
    int         durationMs = 0;
    int         sampleRate = 0;
    int         channels   = 0;
    int         bitDepth   = 0;
    int64_t     fileSize   = 0;
};

struct Album {
    std::string name;
    std::string artist;
    std::string artPath;   // resolved cover image path
    std::vector<Track> tracks;
};

// Scan a folder recursively for FLAC files and populate albums.
// Single-threaded for the skeleton.
std::vector<Album> scanLibrary(const std::string& rootPath);

// Resolve the best cover art path for a folder.
// Priority: highest-resolution image named cover/folder/front > any single image.
// TODO(art-quality): Add pixel-dimension check to pick highest-res when multiple candidates exist.
std::string resolveArtPath(const std::string& folderPath);
