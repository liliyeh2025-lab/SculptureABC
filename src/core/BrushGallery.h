// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The community brush gallery (github.com/islandmed/sculptforge-brushes): an
// index.json listing shared .sfbrush files with their preview images and
// SHA-256 hashes. This is the network-free part - parsing and validating the
// index, resolving its relative paths, searching - so it can be tested; the
// downloads live in ai/BrushGalleryClient.h. Format: docs/brushes.md.
#pragma once

#include <string>
#include <vector>

namespace sf {

inline constexpr const char* kDefaultBrushGalleryUrl =
    "https://raw.githubusercontent.com/islandmed/sculptforge-brushes/main/index.json";
inline constexpr const char* kBrushGalleryShareUrl =
    "https://github.com/islandmed/sculptforge-brushes#share-your-brush";
inline constexpr const char* kBrushGalleryIndexFormat = "sculptforge-brush-gallery";

struct GalleryBrush {
    std::string name;
    std::string description;
    std::string author;
    std::vector<std::string> tags;
    std::string file;    // .sfbrush path relative to the index
    std::string preview; // PNG path relative to the index (may be empty)
    std::string sha256;  // lowercase hex of the .sfbrush file
    std::string added;   // YYYY-MM-DD
};

struct GalleryIndex {
    std::vector<GalleryBrush> brushes; // sorted by name
};

// Parses an index.json. Entries that are malformed (missing name, unsafe path,
// bad hash, ...) are skipped and described in `warnings`; the call fails only
// when the text is not a gallery index at all or is from a newer, incompatible format.
bool parseGalleryIndex(const std::string& text, GalleryIndex& out, std::string& error,
                       std::vector<std::string>* warnings = nullptr);

// A relative path inside the gallery: no scheme, no leading '/', no '..', no
// backslashes, only [A-Za-z0-9._-] and '/' (so an index can never point outside it).
bool isSafeGalleryPath(const std::string& path);
bool isSha256Hex(const std::string& text);

// Normalises the gallery setting to the index location: an http(s) or file://
// URL or a local path, either to index.json or to the folder holding it.
std::string galleryIndexLocation(const std::string& setting);
// `relative` resolved against the folder of `indexLocation` (URL or path).
std::string resolveGalleryPath(const std::string& indexLocation, const std::string& relative);
// file:// URLs and plain paths are read from disk; returns the path ("" for http/https).
std::string galleryLocalPath(const std::string& location);

// True if `data` hashes to `expectedHex` (case-insensitive).
bool galleryHashMatches(const std::string& data, const std::string& expectedHex);

// Case-insensitive search over name, description, author and tags; empty
// `query` matches everything. `tag` (if not empty) must be one of the brush's tags.
bool galleryBrushMatches(const GalleryBrush& brush, const std::string& query, const std::string& tag);
// Every tag used in the index, most used first (ties alphabetical).
std::vector<std::string> galleryTags(const GalleryIndex& index);

} // namespace sf
