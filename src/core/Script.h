// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tiny command script language used by sculptforge-cli and (later) the in-app
// console:   open_obj path="my head.obj"; subdivide levels=2
//            export_lods path=out/head.glb levels=4   # comment
// Statements end at ';' or newline. Values: true/false, numbers, or strings
// (quote strings containing spaces).
#pragma once

#include "core/Commands.h"

#include <string>
#include <vector>

namespace sf {

struct ScriptStatement {
    std::string command;
    Args args;
    int line = 0;
};

bool parseScript(const std::string& text, std::vector<ScriptStatement>& out, std::string& error);

// The inverse of parseScript: `remesh resolution=200`, `layer_add name="Big Smile"`. Strings are quoted
// when they contain spaces or would otherwise read back as a number or bool; numbers use the shortest
// form that reads back exactly.
std::string formatValue(const ArgValue& value);
std::string formatStatement(const std::string& command, const Args& args);

} // namespace sf
