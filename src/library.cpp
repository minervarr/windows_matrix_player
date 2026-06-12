#include "library.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <map>
#include <cctype>
#include <cstdio>
#include <future>
#include "dr_flac.h"
#include "dr_wav.h"

namespace fs = std::filesystem;

static const char* COVER_NAMES[] = { "cover", "folder", "front", "albumart", "album" };
static const char* COVER_EXTS[]  = { ".jpg", ".jpeg", ".png" };

std::string resolveArtPath(const std::string& folderPath) {
    fs::path dir = fs::u8path(folderPath);
    // Pass 1: preferred names in priority order
    for (const char* name : COVER_NAMES) {
        for (const char* ext : COVER_EXTS) {
            fs::path candidate = dir / (std::string(name) + ext);
            if (fs::exists(candidate)) return candidate.u8string();
        }
    }
    // Pass 2: pick the first image found (don't require exactly one)
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png")
            return entry.path().u8string();
    }
    return "";
}

static std::string deriveAlbumArtist(const std::vector<Track>& tracks) {
    std::string result;
    // Prefer ALBUMARTIST tags — check all tracks for consistency
    for (auto& t : tracks) {
        if (t.albumArtist.empty()) continue;
        if (result.empty()) result = t.albumArtist;
        else if (result != t.albumArtist) return "Various Artists";
    }
    if (!result.empty()) return result;
    // Fall back to ARTIST — if tracks disagree, it's a various-artists album
    for (auto& t : tracks) {
        if (t.artist.empty()) continue;
        if (result.empty()) result = t.artist;
        else if (result != t.artist) return "Various Artists";
    }
    return result;
}

void Album::sortTracks() {
    std::sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) {
        if (a.trackNumber != b.trackNumber) return a.trackNumber < b.trackNumber;
        return a.title < b.title;
    });
}

// ── FLAC metadata parsing ────────────────────────────────────────────────────

struct VorbisCtx {
    std::string title, artist, albumArtist, album;
    int trackNumber = 0;
};

static void onFlacMeta(void* userdata, drflac_metadata* meta) {
    if (meta->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
    auto* ctx = (VorbisCtx*)userdata;
    drflac_vorbis_comment_iterator iter;
    drflac_init_vorbis_comment_iterator(&iter,
        meta->data.vorbis_comment.commentCount,
        meta->data.vorbis_comment.pComments);
    drflac_uint32 len;
    const char* comment;
    while ((comment = drflac_next_vorbis_comment(&iter, &len)) != nullptr) {
        std::string s(comment, len);
        auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        for (auto& c : key) c = (char)toupper((unsigned char)c);
        if (key == "TITLE")        ctx->title       = val;
        else if (key == "ARTIST")  ctx->artist      = val;
        else if (key == "ALBUMARTIST" || key == "ALBUM ARTIST")
                                   ctx->albumArtist = val;
        else if (key == "ALBUM")   ctx->album       = val;
        else if (key == "TRACKNUMBER") {
            // Handle "3/12" format
            auto slash = val.find('/');
            ctx->trackNumber = atoi(slash != std::string::npos ? val.substr(0, slash).c_str() : val.c_str());
        }
    }
}

static int64_t getFileMtime(const std::string& path) {
    auto ftime = fs::last_write_time(fs::path(path));
    return ftime.time_since_epoch().count();
}

static Track quickParseWAV(const std::string& path) {
    Track t;
    t.filePath = path;
    t.title = fs::path(fs::u8path(path)).stem().u8string();

    int wl = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wl, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wl);
    if (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &fad)) {
        LARGE_INTEGER sz;
        sz.HighPart = fad.nFileSizeHigh;
        sz.LowPart  = fad.nFileSizeLow;
        t.fileSize  = sz.QuadPart;
        LARGE_INTEGER mt;
        mt.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        mt.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
        t.fileMtime = mt.QuadPart;
    }

    drwav wav;
    if (drwav_init_file_w(&wav, wpath.c_str(), nullptr)) {
        t.sampleRate = (int)wav.sampleRate;
        t.channels   = (int)wav.channels;
        t.bitDepth   = (int)wav.bitsPerSample;
        if (wav.sampleRate > 0 && wav.totalPCMFrameCount > 0)
            t.durationMs = (int)(wav.totalPCMFrameCount * 1000 / wav.sampleRate);
        drwav_uninit(&wav);
    }
    return t;
}

static Track quickParseFLAC(const std::string& path) {
    Track t;
    t.filePath = path;
    t.title = fs::path(fs::u8path(path)).stem().u8string();

    int wl = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wl, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wl);
    if (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &fad)) {
        LARGE_INTEGER sz;
        sz.HighPart = fad.nFileSizeHigh;
        sz.LowPart  = fad.nFileSizeLow;
        t.fileSize  = sz.QuadPart;
        LARGE_INTEGER mt;
        mt.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        mt.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
        t.fileMtime = mt.QuadPart;
    }

    VorbisCtx ctx;
    drflac* flac = drflac_open_file_with_metadata_w(wpath.c_str(), onFlacMeta, &ctx, nullptr);
    if (flac) {
        t.sampleRate = (int)flac->sampleRate;
        t.channels   = (int)flac->channels;
        t.bitDepth   = (int)flac->bitsPerSample;
        if (flac->sampleRate > 0 && flac->totalPCMFrameCount > 0)
            t.durationMs = (int)(flac->totalPCMFrameCount * 1000 / flac->sampleRate);
        if (!ctx.title.empty())       t.title       = ctx.title;
        if (!ctx.artist.empty())      t.artist      = ctx.artist;
        if (!ctx.albumArtist.empty()) t.albumArtist = ctx.albumArtist;
        if (!ctx.album.empty())       t.album       = ctx.album;
        t.trackNumber = ctx.trackNumber;
        drflac_close(flac);
    }
    return t;
}

// ── Full scan (original interface, kept for compatibility) ───────────────────

std::vector<Album> scanLibrary(const std::string& rootPath) {
    std::vector<Album> albums;
    fs::path root = fs::u8path(rootPath);
    if (!fs::exists(root)) return albums;

    std::map<std::string, std::vector<Track>> byFolder;
    for (auto& entry : fs::recursive_directory_iterator(root,
            fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        std::string folder = entry.path().parent_path().u8string();
        std::string filePath = entry.path().u8string();
        if (ext == ".flac")
            byFolder[folder].push_back(quickParseFLAC(filePath));
        else if (ext == ".wav")
            byFolder[folder].push_back(quickParseWAV(filePath));
    }

    for (auto& [folder, tracks] : byFolder) {
        Album album;
        album.name   = fs::u8path(folder).filename().u8string();
        album.tracks = std::move(tracks);
        album.artPath = resolveArtPath(folder);
        album.artist = deriveAlbumArtist(album.tracks);
        album.sortTracks();
        albums.push_back(std::move(album));
    }

    std::sort(albums.begin(), albums.end(),
        [](const Album& a, const Album& b){ return a.name < b.name; });

    return albums;
}

// ── Incremental scan ─────────────────────────────────────────────────────────

IncrementalScanResult scanLibraryIncremental(
    const std::string& rootPath,
    const std::map<std::string, FileCache>& existing)
{
    IncrementalScanResult result;
    fs::path root = fs::u8path(rootPath);
    if (!fs::exists(root)) return result;

    std::map<std::string, std::vector<Track>> byFolder;

    for (auto& entry : fs::recursive_directory_iterator(root,
            fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".flac" && ext != ".wav") continue;

        std::string filePath = entry.path().u8string();
        std::string folder   = entry.path().parent_path().u8string();

        // Check if file is unchanged
        auto it = existing.find(filePath);
        if (it != existing.end()) {
            int wl2 = MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), -1, nullptr, 0);
            std::wstring wfp(wl2, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), -1, wfp.data(), wl2);
            if (!wfp.empty() && wfp.back() == L'\0') wfp.pop_back();
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExW(wfp.c_str(), GetFileExInfoStandard, &fad)) {
                LARGE_INTEGER sz, mt;
                sz.HighPart = fad.nFileSizeHigh; sz.LowPart = fad.nFileSizeLow;
                mt.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                mt.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
                if (sz.QuadPart == it->second.fileSize && mt.QuadPart == it->second.fileMtime) {
                    result.filesSkipped++;
                    continue;
                }
            }
        }

        result.filesScanned++;
        if (ext == ".flac")
            byFolder[folder].push_back(quickParseFLAC(filePath));
        else if (ext == ".wav")
            byFolder[folder].push_back(quickParseWAV(filePath));
    }

    for (auto& [folder, tracks] : byFolder) {
        Album album;
        album.name   = fs::u8path(folder).filename().u8string();
        album.tracks = std::move(tracks);
        album.artPath = resolveArtPath(folder);
        album.artist = deriveAlbumArtist(album.tracks);
        album.sortTracks();
        result.albums.push_back(std::move(album));
    }

    std::sort(result.albums.begin(), result.albums.end(),
        [](const Album& a, const Album& b){ return a.name < b.name; });

    return result;
}

// ── Parallel scan ────────────────────────────────────────────────────────────

std::vector<Album> scanLibraryParallel(const std::string& rootPath) {
    fs::path root = fs::u8path(rootPath);
    if (!fs::exists(root)) return {};

    // Collect all audio file paths first
    std::vector<std::pair<std::string, std::string>> files; // (path, ext)
    for (auto& entry : fs::recursive_directory_iterator(root,
            fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".flac" || ext == ".wav")
            files.emplace_back(entry.path().u8string(), ext);
    }

    // Parse in parallel using hardware concurrency
    unsigned numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 4;

    std::vector<std::future<std::vector<Track>>> futures;
    size_t chunkSize = (files.size() + numThreads - 1) / numThreads;

    for (unsigned i = 0; i < numThreads; i++) {
        size_t start = i * chunkSize;
        size_t end   = std::min(start + chunkSize, files.size());
        if (start >= files.size()) break;

        futures.push_back(std::async(std::launch::async, [&files, start, end]() {
            std::vector<Track> tracks;
            for (size_t j = start; j < end; j++) {
                if (files[j].second == ".flac")
                    tracks.push_back(quickParseFLAC(files[j].first));
                else
                    tracks.push_back(quickParseWAV(files[j].first));
            }
            return tracks;
        }));
    }

    // Gather results and group by folder
    std::map<std::string, std::vector<Track>> byFolder;
    for (auto& f : futures) {
        auto tracks = f.get();
        for (auto& t : tracks) {
            std::string folder = fs::u8path(t.filePath).parent_path().u8string();
            byFolder[folder].push_back(std::move(t));
        }
    }

    std::vector<Album> albums;
    for (auto& [folder, tracks] : byFolder) {
        Album album;
        album.name   = fs::u8path(folder).filename().u8string();
        album.tracks = std::move(tracks);
        album.artPath = resolveArtPath(folder);
        album.artist = deriveAlbumArtist(album.tracks);
        album.sortTracks();
        albums.push_back(std::move(album));
    }

    std::sort(albums.begin(), albums.end(),
        [](const Album& a, const Album& b){ return a.name < b.name; });

    return albums;
}

// ── Stale file cleanup ───────────────────────────────────────────────────────

void purgeStaleFiles(std::vector<Album>& albums, int& removedCount) {
    removedCount = 0;
    for (auto& album : albums) {
        auto it = std::remove_if(album.tracks.begin(), album.tracks.end(),
            [&](const Track& t) {
                if (!fs::exists(fs::u8path(t.filePath))) { removedCount++; return true; }
                return false;
            });
        album.tracks.erase(it, album.tracks.end());
    }
    // Remove empty albums
    albums.erase(
        std::remove_if(albums.begin(), albums.end(),
            [](const Album& a) { return a.tracks.empty(); }),
        albums.end());
}

// ── Folder watcher (ReadDirectoryChangesW) ───────────────────────────────────

void FolderWatcher::watchRoot(const std::string& path, Callback cb) {
    std::lock_guard<std::mutex> lk(mu_);

    // Don't double-watch
    for (auto& e : entries_)
        if (e->root == path) return;

    auto entry = std::make_unique<WatchEntry>();
    entry->root     = path;
    entry->callback = cb;
    entry->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    int wl3 = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wl3, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wl3);
    if (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();
    entry->dirHandle = CreateFileW(
        wpath.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (entry->dirHandle == INVALID_HANDLE_VALUE) {
        printf("[Watcher] Failed to open directory: %s\n", path.c_str());
        CloseHandle(entry->stopEvent);
        return;
    }

    auto* raw = entry.get();
    entry->thread = std::thread([raw]() {
        alignas(DWORD) char buf[4096];
        OVERLAPPED ovl = {};
        ovl.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        while (true) {
            ResetEvent(ovl.hEvent);
            DWORD bytesReturned = 0;
            BOOL ok = ReadDirectoryChangesW(
                raw->dirHandle, buf, sizeof(buf), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
                &bytesReturned, &ovl, nullptr);

            if (!ok) break;

            HANDLE handles[] = { ovl.hEvent, raw->stopEvent };
            DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);

            if (wait == WAIT_OBJECT_0 + 1) break; // stop requested
            if (wait != WAIT_OBJECT_0) break;

            GetOverlappedResult(raw->dirHandle, &ovl, &bytesReturned, FALSE);

            // Coalesce: wait 500ms for more changes before notifying
            Sleep(500);

            // Drain any additional changes that accumulated
            while (true) {
                ResetEvent(ovl.hEvent);
                ReadDirectoryChangesW(raw->dirHandle, buf, sizeof(buf), TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
                    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
                    &bytesReturned, &ovl, nullptr);
                DWORD drain = WaitForSingleObject(ovl.hEvent, 100);
                if (drain == WAIT_TIMEOUT) {
                    CancelIo(raw->dirHandle);
                    break;
                }
                GetOverlappedResult(raw->dirHandle, &ovl, &bytesReturned, FALSE);
            }

            raw->callback(raw->root);
        }

        CloseHandle(ovl.hEvent);
    });

    entries_.push_back(std::move(entry));
}

void FolderWatcher::unwatchRoot(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if ((*it)->root == path) {
            SetEvent((*it)->stopEvent);
            if ((*it)->thread.joinable()) (*it)->thread.join();
            CloseHandle((*it)->dirHandle);
            CloseHandle((*it)->stopEvent);
            entries_.erase(it);
            return;
        }
    }
}

void FolderWatcher::unwatchAll() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& e : entries_) {
        SetEvent(e->stopEvent);
        if (e->thread.joinable()) e->thread.join();
        CloseHandle(e->dirHandle);
        CloseHandle(e->stopEvent);
    }
    entries_.clear();
}
