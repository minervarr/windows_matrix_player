#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

struct Track {
    int         id = 0;
    std::string title;
    std::string artist;
    std::string albumArtist;
    std::string album;
    std::string filePath;
    int         trackNumber = 0;
    int         durationMs  = 0;
    int         sampleRate  = 0;
    int         channels    = 0;
    int         bitDepth    = 0;
    int64_t     fileSize    = 0;
    int64_t     fileMtime   = 0;
};

struct Album {
    std::string name;
    std::string artist;
    std::string artPath;
    std::vector<Track> tracks;

    void sortTracks();
};

std::vector<Album> scanLibrary(const std::string& rootPath);

struct IncrementalScanResult {
    std::vector<Album> albums;
    int filesScanned  = 0;
    int filesSkipped  = 0;
    int filesRemoved  = 0;
};

struct FileCache {
    int64_t fileSize  = 0;
    int64_t fileMtime = 0;
};

IncrementalScanResult scanLibraryIncremental(
    const std::string& rootPath,
    const std::map<std::string, FileCache>& existing);

std::vector<Album> scanLibraryParallel(const std::string& rootPath);

std::string resolveArtPath(const std::string& folderPath);

void purgeStaleFiles(std::vector<Album>& albums, int& removedCount);

class FolderWatcher {
public:
    using Callback = std::function<void(const std::string& root)>;

    void watchRoot(const std::string& path, Callback cb);
    void unwatchRoot(const std::string& path);
    void unwatchAll();
    ~FolderWatcher() { unwatchAll(); }

private:
    struct WatchEntry {
        std::string  root;
        HANDLE       dirHandle = INVALID_HANDLE_VALUE;
        HANDLE       stopEvent = nullptr;
        std::thread  thread;
        Callback     callback;
    };
    std::mutex mu_;
    std::vector<std::unique_ptr<WatchEntry>> entries_;
};
