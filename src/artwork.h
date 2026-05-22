#pragma once
#include <string>
#include <windows.h>

// Artwork loading for Win32 GDI rendering.
// TODO(libjpeg-turbo): Replace LoadImage/GDI+ path with libjpeg-turbo for
//   the same native JPEG speed the Android player gets. Especially matters
//   on 4K displays where a single cover can be a 20MB TIFF/PNG.
// TODO(art-cache): Add LRU bitmap cache (L1 in-memory, L2 disk) mirroring
//   ArtworkCache.java: thumbnail key vs fullscreen key, separate bitmaps.

// Load an image file and scale it to fit targetW x targetH, returning an HBITMAP.
// Returns NULL if the file cannot be loaded.
// Caller is responsible for DeleteObject(hbm).
HBITMAP loadArtwork(const std::string& path, int targetW, int targetH);

// Free any cached bitmaps. Call on shutdown.
void clearArtworkCache();
