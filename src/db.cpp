#include "db.h"
#include "../third_party/sqlite3.h"
#include <cstdio>

struct Db::Impl {
    sqlite3* db = nullptr;
};

static const char* SCHEMA = R"(
CREATE TABLE IF NOT EXISTS tracks (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    title       TEXT,
    artist      TEXT,
    album       TEXT,
    file_path   TEXT UNIQUE,
    duration_ms INTEGER,
    sample_rate INTEGER,
    channels    INTEGER,
    bit_depth   INTEGER,
    file_size   INTEGER
);
CREATE TABLE IF NOT EXISTS albums (
    id       INTEGER PRIMARY KEY AUTOINCREMENT,
    name     TEXT,
    artist   TEXT,
    art_path TEXT
);
)";

bool Db::open(const std::string& dbPath) {
    impl_ = new Impl;
    if (sqlite3_open(dbPath.c_str(), &impl_->db) != SQLITE_OK) {
        fprintf(stderr, "[DB] open failed: %s\n", sqlite3_errmsg(impl_->db));
        return false;
    }
    sqlite3_exec(impl_->db, SCHEMA, nullptr, nullptr, nullptr);
    sqlite3_exec(impl_->db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    return true;
}

void Db::close() {
    if (impl_) {
        if (impl_->db) sqlite3_close(impl_->db);
        delete impl_;
        impl_ = nullptr;
    }
}

void Db::saveTracks(const std::vector<Track>& tracks) {
    if (!impl_->db) return;
    sqlite3_exec(impl_->db, "BEGIN;", nullptr, nullptr, nullptr);
    const char* sql =
        "INSERT OR REPLACE INTO tracks "
        "(title, artist, album, file_path, duration_ms, sample_rate, channels, bit_depth, file_size) "
        "VALUES (?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    for (auto& t : tracks) {
        sqlite3_bind_text(stmt, 1, t.title.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, t.artist.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, t.album.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, t.filePath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (stmt, 5, t.durationMs);
        sqlite3_bind_int (stmt, 6, t.sampleRate);
        sqlite3_bind_int (stmt, 7, t.channels);
        sqlite3_bind_int (stmt, 8, t.bitDepth);
        sqlite3_bind_int64(stmt, 9, t.fileSize);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(impl_->db, "COMMIT;", nullptr, nullptr, nullptr);
}

std::vector<Track> Db::loadTracks() {
    std::vector<Track> out;
    if (!impl_->db) return out;
    const char* sql = "SELECT id,title,artist,album,file_path,duration_ms,sample_rate,channels,bit_depth,file_size FROM tracks;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Track t;
        t.id          = sqlite3_column_int(stmt, 0);
        auto col = [&](int i) -> std::string {
            auto* s = (const char*)sqlite3_column_text(stmt, i);
            return s ? s : "";
        };
        t.title      = col(1);
        t.artist     = col(2);
        t.album      = col(3);
        t.filePath   = col(4);
        t.durationMs = sqlite3_column_int(stmt, 5);
        t.sampleRate = sqlite3_column_int(stmt, 6);
        t.channels   = sqlite3_column_int(stmt, 7);
        t.bitDepth   = sqlite3_column_int(stmt, 8);
        t.fileSize   = sqlite3_column_int64(stmt, 9);
        out.push_back(std::move(t));
    }
    sqlite3_finalize(stmt);
    return out;
}

void Db::saveAlbums(const std::vector<Album>& albums) {
    if (!impl_->db) return;
    sqlite3_exec(impl_->db, "BEGIN;", nullptr, nullptr, nullptr);
    const char* sql = "INSERT OR REPLACE INTO albums (name, artist, art_path) VALUES (?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    for (auto& a : albums) {
        sqlite3_bind_text(stmt, 1, a.name.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, a.artist.c_str(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, a.artPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(impl_->db, "COMMIT;", nullptr, nullptr, nullptr);
}

std::vector<Album> Db::loadAlbums() {
    std::vector<Album> out;
    if (!impl_->db) return out;
    const char* sql = "SELECT name, artist, art_path FROM albums;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Album a;
        auto col = [&](int i) -> std::string {
            auto* s = (const char*)sqlite3_column_text(stmt, i);
            return s ? s : "";
        };
        a.name    = col(0);
        a.artist  = col(1);
        a.artPath = col(2);
        out.push_back(std::move(a));
    }
    sqlite3_finalize(stmt);
    return out;
}
