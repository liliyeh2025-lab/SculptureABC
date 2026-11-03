// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ZBrush interoperability commands: goz_import, goz_export, goz_send,
// goz_receive and export_stl. Registered by CommandRegistry's constructor.
#pragma once

#include "core/Commands.h"
#include "io/MeshObject.h"

#include <string>
#include <vector>

namespace sf {

void registerGozCommands(CommandRegistry& registry);

// Shared with the Blender bridge (io/BlenderCommands.cpp):
// The GoZ name of SubTool `i` (active or not): the object it came from (ZBrush or Blender) or was sent as.
std::string& subtoolGozName(Document& doc, int i);
// Visible SubTools with triangles as objects to send: the GoZ name each came with, else its SubTool name.
// `indices` receives the SubTool of each object.
std::vector<MeshObjectRef> gozObjects(Document& doc, std::vector<int>& indices);
// Puts received objects into the scene: each updates the SubTool with its GoZ name (an undoable `undoLabel` step
// in that SubTool's history), the first one replaces a fresh scene (one SubTool that came from nowhere, renamed
// after the object when `renameFresh` or several objects arrive), the others become new SubTools. The active
// SubTool stays selected.
void receiveObjects(Document& doc, std::vector<MeshObject>& objects, const std::string& undoLabel, bool renameFresh);

} // namespace sf
