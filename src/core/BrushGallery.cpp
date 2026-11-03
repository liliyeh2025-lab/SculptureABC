// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/BrushGallery.h"

#include "core/Json.h"
#include "core/Sha256.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>

namespace sf {

namespace {

constexpr size_t kMaxEntries = 5000;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// Case-insensitive (URL schemes).
bool startsWith(const std::string& s, const std::string& prefix) { return lower(s.substr(0, prefix.size())) == prefix; }

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && lower(s.substr(s.size() - suffix.size())) == suffix;
}

bool isUrl(const std::string& s) { return startsWith(s, "http://") || startsWith(s, "https://"); }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string clipped(const std::string& s, size_t limit) { return s.size() > limit ? s.substr(0, limit) : s; }

std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace

bool isSafeGalleryPath(const std::string& path) {
    if (path.empty() || path.size() > 200 || path.front() == '/' || path.back() == '/') return false;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        const std::string part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (unsigned char c : part)
            if (!std::isalnum(c) && c != '.' && c != '_' && c != '-') return false;
        start = end + 1;
    }
    return true;
}

bool isSha256Hex(const std::string& text) {
    return text.size() == 64 &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

bool parseGalleryIndex(const std::string& text, GalleryIndex& out, std::string& error, std::vector<std::string>* warnings) {
    out.brushes.clear();
    Json j;
    if (!Json::parse(text, j, error)) {
        error = "The gallery index is not valid JSON (" + error + ")";
        return false;
    }
    if (!j.isObject() || j["format"].asString() != kBrushGalleryIndexFormat || !j["brushes"].isArray()) {
        error = "Not a SculptForge brush gallery index";
        return false;
    }
    if (j["version"].asNumber(0) != 1.0) {
        error = j["version"].asNumber(0) > 1.0 ? "The gallery needs a newer SculptForge - please update"
                                               : "Unsupported gallery index version";
        return false;
    }
    auto warn = [&](const std::string& w) {
        if (warnings) warnings->push_back(w);
    };
    std::set<std::string> names;
    size_t index = 0;
    for (const Json& e : j["brushes"].items()) {
        ++index;
        if (out.brushes.size() >= kMaxEntries) {
            warn("Only the first " + std::to_string(kMaxEntries) + " brushes are listed");
            break;
        }
        const std::string where = "Entry " + std::to_string(index);
        if (!e.isObject()) {
            warn(where + ": not an object");
            continue;
        }
        GalleryBrush b;
        b.name = clipped(trim(e["name"].asString()), 64);
        b.file = e["file"].asString();
        b.preview = e["preview"].asString();
        b.sha256 = lower(e["sha256"].asString());
        if (b.name.empty()) {
            warn(where + ": no name");
            continue;
        }
        if (!isSafeGalleryPath(b.file) || !endsWith(b.file, ".sfbrush")) {
            warn(b.name + ": invalid file path '" + b.file + "'");
            continue;
        }
        if (!isSha256Hex(b.sha256)) {
            warn(b.name + ": missing or invalid sha256");
            continue;
        }
        if (!b.preview.empty() && (!isSafeGalleryPath(b.preview) || !endsWith(b.preview, ".png"))) {
            warn(b.name + ": invalid preview path ignored");
            b.preview.clear();
        }
        if (!names.insert(lower(b.name)).second) {
            warn(b.name + ": listed twice (the first one is kept)");
            continue;
        }
        b.description = clipped(e["description"].asString(), 500);
        b.author = clipped(e["author"].asString(), 64);
        b.added = clipped(e["added"].asString(), 32);
        for (const Json& t : e["tags"].items()) {
            const std::string tag = clipped(lower(trim(t.asString())), 32);
            if (!tag.empty() && b.tags.size() < 12 && std::find(b.tags.begin(), b.tags.end(), tag) == b.tags.end())
                b.tags.push_back(tag);
        }
        out.brushes.push_back(std::move(b));
    }
    std::sort(out.brushes.begin(), out.brushes.end(),
              [](const GalleryBrush& a, const GalleryBrush& b) { return lower(a.name) < lower(b.name); });
    return true;
}

std::string galleryIndexLocation(const std::string& setting) {
    std::string s = trim(setting);
    if (s.empty()) return kDefaultBrushGalleryUrl;
    if (endsWith(s, ".json")) return s;
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    const bool backslashes = !isUrl(s) && !startsWith(s, "file://") && s.find('\\') != std::string::npos &&
                             s.find('/') == std::string::npos;
    return s + (backslashes ? "\\" : "/") + "index.json";
}

std::string resolveGalleryPath(const std::string& indexLocation, const std::string& relative) {
    const size_t slash = indexLocation.find_last_of(isUrl(indexLocation) || startsWith(indexLocation, "file://") ? "/" : "/\\");
    if (slash == std::string::npos) return relative;
    return indexLocation.substr(0, slash + 1) + relative;
}

std::string galleryLocalPath(const std::string& location) {
    if (isUrl(location)) return {};
    if (!startsWith(location, "file://")) return location;
    std::string path = percentDecode(location.substr(7));
    if (startsWith(path, "localhost/")) path = path.substr(9);
#if defined(_WIN32)
    // file:///C:/brushes -> C:/brushes
    if (path.size() > 2 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':')
        path = path.substr(1);
#endif
    return path;
}

bool galleryHashMatches(const std::string& data, const std::string& expectedHex) {
    return isSha256Hex(expectedHex) && sha256Hex(data) == lower(expectedHex);
}

bool galleryBrushMatches(const GalleryBrush& brush, const std::string& query, const std::string& tag) {
    if (!tag.empty() && std::find(brush.tags.begin(), brush.tags.end(), lower(tag)) == brush.tags.end()) return false;
    std::string haystack = lower(brush.name + "\n" + brush.description + "\n" + brush.author);
    for (const auto& t : brush.tags) haystack += "\n" + t;
    // Every word of the query must appear somewhere.
    const std::string q = lower(query);
    size_t i = 0;
    while (i < q.size()) {
        while (i < q.size() && std::isspace(static_cast<unsigned char>(q[i]))) ++i;
        size_t j = i;
        while (j < q.size() && !std::isspace(static_cast<unsigned char>(q[j]))) ++j;
        if (j > i && haystack.find(q.substr(i, j - i)) == std::string::npos) return false;
        i = j;
    }
    return true;
}

std::vector<std::string> galleryTags(const GalleryIndex& index) {
    std::map<std::string, int> counts;
    for (const auto& b : index.brushes)
        for (const auto& t : b.tags) ++counts[t];
    std::vector<std::pair<std::string, int>> sorted(counts.begin(), counts.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<std::string> out;
    for (const auto& [tag, n] : sorted) out.push_back(tag);
    return out;
}

} // namespace sf
