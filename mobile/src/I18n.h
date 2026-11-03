// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
//
// Interface text: the code writes English, T() swaps in the Traditional Chinese of mobile/i18n/zh-TW.json (a flat
// {"English": "中文"} object). Text without a translation shows in English. The ImGui "##id" suffix is kept.
#pragma once

#include <cstdio>
#include <string>

namespace i18n {

// Reads the table. False (and `error`) when the file can't be read or parsed; T() then shows English.
bool load(const std::string& path, std::string& error);

void setChinese(bool on);
bool chinese();

// The text to show for `english` (a string literal: results are cached by its address).
const char* T(const char* english);

// printf into a std::string, for text with numbers or names in it: format(T("Opened %s"), name).
template <typename... A>
std::string format(const char* fmt, A... args) {
    char buf[512];
    std::snprintf(buf, sizeof buf, fmt, args...);
    return buf;
}

// Every translation run together (the characters the Chinese font has to carry).
const std::string& allText();

} // namespace i18n

using i18n::T;
