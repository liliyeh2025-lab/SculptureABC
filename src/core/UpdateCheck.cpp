// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/UpdateCheck.h"

#include "core/Json.h"

#include <algorithm>
#include <cctype>

namespace sf {

namespace {

bool httpsUrl(const std::string& s) { return s.rfind("https://", 0) == 0 && s.size() > 8; }

std::vector<uint64_t> versionParts(const std::string& v) {
    std::vector<uint64_t> parts;
    size_t i = v.size() > 1 && (v[0] == 'v' || v[0] == 'V') && std::isdigit(static_cast<unsigned char>(v[1])) ? 1 : 0;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
        uint64_t n = 0;
        for (; i < v.size() && std::isdigit(static_cast<unsigned char>(v[i])); ++i)
            n = n < (UINT64_MAX - 9) / 10 ? n * 10 + uint64_t(v[i] - '0') : n;
        parts.push_back(n);
        if (i + 1 < v.size() && v[i] == '.' && std::isdigit(static_cast<unsigned char>(v[i + 1]))) ++i;
        else break;
    }
    return parts;
}

} // namespace

bool parseUpdateFeed(const std::string& json, UpdateInfo& out, std::string& error) {
    Json j;
    if (!Json::parse(json, j, error)) return false;
    if (!j.isObject()) {
        error = "the update feed is not a JSON object";
        return false;
    }
    UpdateInfo info;
    if (j["version"].isString()) info.version = j["version"].asString();
    if (j["download"].isString()) info.downloadUrl = j["download"].asString();
    if (j["notes"].isString()) info.notesUrl = j["notes"].asString();
    if (versionParts(info.version).empty()) {
        error = "the update feed has no version";
        return false;
    }
    if (!httpsUrl(info.downloadUrl)) {
        error = "the update feed has no https download link";
        return false;
    }
    if (!httpsUrl(info.notesUrl)) info.notesUrl = info.downloadUrl;
    for (const auto& h : j["highlights"].items())
        if (h.isString() && !h.asString().empty() && info.highlights.size() < 4)
            info.highlights.push_back(h.asString().substr(0, 200));
    out = std::move(info);
    return true;
}

int compareVersions(const std::string& a, const std::string& b) {
    const auto pa = versionParts(a), pb = versionParts(b);
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        const uint64_t x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

} // namespace sf
