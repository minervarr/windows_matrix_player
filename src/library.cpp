#include "library.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <map>
#include <cctype>
#include "dr_flac.h"
#include "dr_wav.h"

namespace fs = std::filesystem;

static const char* COVER_NAMES[] = { "cover", "folder", "front", "albumart", "album" };
static const char* COVER_EXTS[]  = { ".jpg", ".jpeg", ".png" };

std::string resolveArtPath(const std::string& folderPath) {
    // Pass 1: preferred names in priority order
    for (const char* name : COVER_NAMES) {
        for (const char* ext : COVER_EXTS) {
            std::string candidate = folderPath + "\\" + name + ext;
            if (fs::exists(candidate)) return candidate;
        }
    }
    // Pass 2: any single image file in the folder
    // TODO(art-quality): When multiple images exist, load dimensions and pick largest.
    std::string found;
    int count = 0;
    for (auto& entry : fs::directory_iterator(folderPath)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png") {
            found = entry.path().string();
            count++;
        }
    }
    return (count == 1) ? found : "";
}

struct VorbisCtx { std::string title, artist, album; };

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
        if (key == "TITLE")  ctx->title  = val;
        else if (key == "ARTIST") ctx->artist = val;
        else if (key == "ALBUM")  ctx->album  = val;
    }
}

static Track quickParseWAV(const std::string& path) {
    Track t;
    t.filePath = path;
    t.title = fs::path(path).stem().string();

    drwav wav;
    if (drwav_init_file(&wav, path.c_str(), nullptr)) {
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
    t.title = fs::path(path).stem().string();  // fallback if no tags

    VorbisCtx ctx;
    drflac* flac = drflac_open_file_with_metadata(path.c_str(), onFlacMeta, &ctx, nullptr);
    if (flac) {
        t.sampleRate = (int)flac->sampleRate;
        t.channels   = (int)flac->channels;
        t.bitDepth   = (int)flac->bitsPerSample;
        if (flac->sampleRate > 0 && flac->totalPCMFrameCount > 0)
            t.durationMs = (int)(flac->totalPCMFrameCount * 1000 / flac->sampleRate);
        if (!ctx.title.empty())  t.title  = ctx.title;
        if (!ctx.artist.empty()) t.artist = ctx.artist;
        if (!ctx.album.empty())  t.album  = ctx.album;
        drflac_close(flac);
    }
    return t;
}

std::vector<Album> scanLibrary(const std::string& rootPath) {
    std::vector<Album> albums;
    if (!fs::exists(rootPath)) return albums;

    // Group audio files by parent folder — each folder = one album candidate.
    std::map<std::string, std::vector<Track>> byFolder;
    for (auto& entry : fs::recursive_directory_iterator(rootPath,
            fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        std::string folder = entry.path().parent_path().string();
        std::string filePath = entry.path().string();
        if (ext == ".flac")
            byFolder[folder].push_back(quickParseFLAC(filePath));
        else if (ext == ".wav")
            byFolder[folder].push_back(quickParseWAV(filePath));
    }

    for (auto& [folder, tracks] : byFolder) {
        Album album;
        album.name   = fs::path(folder).filename().string();
        album.tracks = std::move(tracks);
        album.artPath = resolveArtPath(folder);
        albums.push_back(std::move(album));
    }

    std::sort(albums.begin(), albums.end(),
        [](const Album& a, const Album& b){ return a.name < b.name; });

    return albums;
}
