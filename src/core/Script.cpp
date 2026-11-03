// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Script.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace sf {

namespace {

ArgValue parseValue(const std::string& raw, bool quoted) {
    if (!quoted) {
        if (raw == "true") return true;
        if (raw == "false") return false;
        char* end = nullptr;
        double d = std::strtod(raw.c_str(), &end);
        if (!raw.empty() && end == raw.c_str() + raw.size()) return d;
    }
    return raw;
}

} // namespace

bool parseScript(const std::string& text, std::vector<ScriptStatement>& out, std::string& error) {
    out.clear();
    size_t i = 0;
    int line = 1;
    std::vector<std::pair<std::string, bool>> tokens; // (text, quoted)
    int stmtLine = 1;

    auto flush = [&]() -> bool {
        if (tokens.empty()) return true;
        ScriptStatement st;
        st.line = stmtLine;
        st.command = tokens[0].first;
        for (size_t t = 1; t < tokens.size(); ++t) {
            // Tokens look like key=value; a quoted value arrives as key= followed by the quoted token.
            std::string tok = tokens[t].first;
            size_t eq = tok.find('=');
            if (eq == std::string::npos || eq == 0) {
                error = "Line " + std::to_string(stmtLine) + ": expected key=value, got '" + tok + "'";
                return false;
            }
            std::string key = tok.substr(0, eq);
            std::string value = tok.substr(eq + 1);
            bool quoted = false;
            if (value.empty() && t + 1 < tokens.size() && tokens[t + 1].second) {
                value = tokens[++t].first;
                quoted = true;
            }
            st.args[key] = parseValue(value, quoted);
        }
        out.push_back(std::move(st));
        tokens.clear();
        return true;
    };

    while (i < text.size()) {
        char c = text[i];
        if (c == '\n' || c == ';') {
            if (!flush()) return false;
            if (c == '\n') ++line;
            ++i;
            continue;
        }
        if (c == '#') {
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (tokens.empty()) stmtLine = line;
        if (c == '"') {
            std::string s;
            ++i;
            while (i < text.size() && text[i] != '"') {
                // Only \" and \\ are escapes, so Windows paths like "C:\Users\me\a.obj" stay intact.
                if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == '"' || text[i + 1] == '\\')) ++i;
                if (text[i] == '\n') ++line;
                s += text[i++];
            }
            if (i >= text.size()) {
                error = "Line " + std::to_string(line) + ": unterminated string";
                return false;
            }
            ++i;
            tokens.emplace_back(std::move(s), true);
            continue;
        }
        std::string s;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != ';' &&
               text[i] != '#') {
            if (text[i] == '"' && !s.empty() && s.back() == '=') break; // key="quoted value"
            s += text[i++];
        }
        tokens.emplace_back(std::move(s), false);
    }
    return flush();
}

std::string formatValue(const ArgValue& value) {
    if (const bool* b = std::get_if<bool>(&value)) return *b ? "true" : "false";
    if (const double* d = std::get_if<double>(&value)) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%g", *d);
        if (std::strtod(buf, nullptr) != *d) std::snprintf(buf, sizeof buf, "%.17g", *d);
        return buf;
    }
    const std::string& s = std::get<std::string>(value);
    bool quote = s.empty() || parseValue(s, false).index() != 2; // "true" or "12" must stay strings
    for (char c : s)
        if (std::isspace(static_cast<unsigned char>(c)) || c == ';' || c == '#' || c == '"') quote = true;
    if (!quote) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + '"';
}

std::string formatStatement(const std::string& command, const Args& args) {
    std::string out = command;
    for (const auto& [key, value] : args) out += " " + key + "=" + formatValue(value);
    return out;
}

} // namespace sf
