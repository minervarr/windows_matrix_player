#pragma once
#include "library.h"
#include <string>

// SQLite persistence layer.
// TODO(schema): Add play_history, track_stats (play count, skip count) tables
//               following the Android player's MatrixPlayerDatabase schema.
// TODO(parallel-scan): Wire cache invalidation (file size + mtime check) to skip
//                      re-parsing unchanged files, same as Android's hybrid scan.

class Db {
public:
    bool open(const std::string& dbPath);
    void close();

    void saveTracks(const std::vector<Track>& tracks);
    std::vector<Track> loadTracks();

    void saveAlbums(const std::vector<Album>& albums);
    std::vector<Album> loadAlbums();

    ~Db() { close(); }

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
