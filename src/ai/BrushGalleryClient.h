// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Downloads from the community brush gallery (see core/BrushGallery.h): the
// index, preview images and brushes, with an on-disk cache so the gallery
// still opens offline. Blocking - call from a worker thread. The gallery can
// be an https URL, a file:// URL or a local folder (tests, studio mirrors).
#pragma once

#include "core/BrushGallery.h"
#include "core/Commands.h"

#include <string>

namespace sf::ai {

struct GalleryFetch {
    bool ok = false;
    std::string data;
    std::string error;
    bool fromCache = false; // the network failed and this is the last good copy
};

// The gallery location: $SCULPTFORGE_BRUSH_GALLERY, else the saved setting, else the official gallery.
std::string brushGalleryLocation();
bool brushGalleryFromEnvironment();
// Saves the gallery setting (empty = the official gallery) in <app data>/brush-gallery.json.
bool saveBrushGallerySetting(const std::string& location, std::string& error);
std::string brushGallerySetting(); // the saved setting ("" if none)

// Cache folder for one gallery: <app data>/brush-gallery-cache/<hash of its location>.
std::string brushGalleryCacheDir(const std::string& indexLocation);

// The index text; falls back to the cached copy when the gallery is unreachable.
GalleryFetch fetchGalleryIndex(const std::string& indexLocation);
// A brush's preview PNG (cached by the brush's hash, so an edited brush gets a fresh preview).
GalleryFetch fetchGalleryPreview(const std::string& indexLocation, const GalleryBrush& brush);
// Downloads a brush, checks it against the index's SHA-256 and stores it in
// the cache; `localPath` is the verified file, ready for brush_install.
bool downloadGalleryBrush(const std::string& indexLocation, const GalleryBrush& brush, std::string& localPath,
                          std::string& error);

// Installs a downloaded, verified brush file: brush_install when the registry
// has it (adds it to the user's library), otherwise brush_load (this session only).
CommandResult installGalleryBrushFile(const CommandRegistry& registry, Document& doc, const std::string& path);

// brush_gallery_search and brush_gallery_install (scripts, AI agents).
void registerBrushGalleryCommands(CommandRegistry& registry);

} // namespace sf::ai
