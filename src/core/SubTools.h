// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SubTools, ZBrush style: a scene is a list of separate meshes (body, eyes,
// teeth, armour) that are sculpted one at a time, shown or hidden, merged and
// split. The data model lives in Document (see SubTool); this file holds the
// mesh operations and the subtool_* commands.
#pragma once

#include "core/Document.h"
#include "io/MeshObject.h"

#include <string>
#include <vector>

namespace sf {

class CommandRegistry;

// Concatenates meshes into one. Every attribute any input has is kept (inputs
// without it get the default), polygroup ids are offset so each input keeps
// its own groups (inputs without polygroups become one group each), and sculpt
// layers are matched by name.
Mesh mergeMeshes(const std::vector<const Mesh*>& meshes);

// The visible SubTools merged into one mesh (for exporters that write a single
// mesh). With one SubTool this is a copy of it, whether shown or not.
Mesh mergedVisibleMesh(const Document& doc);

// The visible SubTools that have triangles, in list order and named after
// them, for exporters that write one object per SubTool. With a single SubTool
// it is that one (shown or not) named doc.assetName(), so one-object exports
// stay as they were.
std::vector<MeshObjectRef> visibleSubtoolObjects(const Document& doc);

// Replaces the scene with one SubTool per object (the first one active), named
// after the objects. A single object, or an unnamed one, is named
// `fallbackName` (e.g. the file name). Clears the edit history like setMesh.
void setSceneObjects(Document& doc, std::vector<MeshObject> objects, const std::string& fallbackName);

// One mesh per polygroup, in increasing group id order (empty if there's only one group).
std::vector<Mesh> splitByPolygroups(const Mesh& mesh);
// One mesh per connected piece, largest first (empty if the mesh is one piece).
std::vector<Mesh> splitIntoParts(const Mesh& mesh);

// subtool_list, subtool_select, subtool_append, subtool_duplicate, subtool_delete,
// subtool_rename, subtool_visibility, subtool_solo, subtool_show_all, subtool_move,
// subtool_merge_down, subtool_merge_visible, subtool_split_groups, subtool_split_parts.
void registerSubToolCommands(CommandRegistry& registry);

} // namespace sf
