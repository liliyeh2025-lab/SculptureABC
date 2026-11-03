// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// MikkTSpace tangents (third_party/mikktspace, zlib): the tangent frame Blender, Unreal, Unity, Godot,
// Substance and xNormal all compute for a mesh with UVs, so a tangent-space normal map baked against it
// shows the same in every one of them.
#pragma once

#include "core/Mesh.h"

#include <glm/glm.hpp>

#include <vector>

namespace sf {

// One tangent per triangle corner (3 per triangle, like Mesh::uvs): xyz is the unit tangent, w the
// bitangent sign (bitangent = w * cross(normal, tangent)), computed from the positions, the vertex normals
// (Mesh::normals) and the UVs. Empty when the mesh has no UVs.
std::vector<glm::vec4> computeTangents(const Mesh& mesh);

} // namespace sf
