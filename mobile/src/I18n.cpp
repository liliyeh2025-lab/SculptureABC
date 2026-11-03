// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
#include "I18n.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#include <cstring>
#include <unordered_map>

namespace i18n {
namespace {

std::unordered_map<std::string, std::string> gTable;
std::unordered_map<const char*, const char*> gCache; // literal address -> shown text (Chinese mode)
std::string gAll;
bool gChinese = true;

void appendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

// A JSON string starting at text[i] == '"'; leaves i after the closing quote.
bool readString(const std::string& text, size_t& i, std::string& out) {
    out.clear();
    if (i >= text.size() || text[i] != '"') return false;
    for (++i; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') {
            ++i;
            return true;
        }
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i >= text.size()) return false;
        switch (text[i]) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'u': {
            if (i + 4 >= text.size()) return false;
            const uint32_t c16 = uint32_t(std::strtoul(text.substr(i + 1, 4).c_str(), nullptr, 16));
            appendUtf8(out, c16);
            i += 4;
            break;
        }
        default: out += text[i]; break; // \" \\ \/
        }
    }
    return false;
}

void skipSpace(const std::string& text, size_t& i) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t')) ++i;
}

} // namespace

bool load(const std::string& path, std::string& error) {
    size_t size = 0;
    char* data = static_cast<char*>(SDL_LoadFile(path.c_str(), &size));
    if (!data) {
        error = "can't read " + path;
        return false;
    }
    const std::string text(data, size);
    SDL_free(data);

    gTable.clear();
    gCache.clear();
    gAll.clear();
    size_t i = 0;
    skipSpace(text, i);
    if (i < text.size() && text[i] == '{') ++i;
    std::string key, value;
    for (;;) {
        skipSpace(text, i);
        if (i >= text.size()) break;
        if (text[i] == '}') break;
        if (text[i] == ',') {
            ++i;
            continue;
        }
        if (!readString(text, i, key)) {
            error = path + ": bad key near byte " + std::to_string(i);
            return false;
        }
        skipSpace(text, i);
        if (i >= text.size() || text[i] != ':') {
            error = path + ": missing ':' near byte " + std::to_string(i);
            return false;
        }
        ++i;
        skipSpace(text, i);
        if (!readString(text, i, value)) {
            error = path + ": bad value near byte " + std::to_string(i);
            return false;
        }
        gAll += value;
        if (key.empty() || key[0] == '_') continue; // "_comment", "_glyphs": text the font needs, not a translation
        gTable[key] = value;
    }
    return true;
}

void setChinese(bool on) { gChinese = on; }
bool chinese() { return gChinese; }

const char* T(const char* english) {
    if (!gChinese || !english) return english;
    auto cached = gCache.find(english);
    if (cached != gCache.end()) return cached->second;
    // "Label##id": translate the label, keep the id.
    const char* hashes = std::strstr(english, "##");
    const std::string label = hashes ? std::string(english, hashes) : std::string(english);
    const char* shown = english;
    auto it = gTable.find(label);
    if (it != gTable.end()) {
        if (hashes) {
            // (Kept for the program's life, like the table.)
            auto* joined = new std::string(it->second + hashes);
            shown = joined->c_str();
        } else {
            shown = it->second.c_str();
        }
    }
    gCache[english] = shown;
    return shown;
}

const std::string& allText() { return gAll; }

} // namespace i18n
