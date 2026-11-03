// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turning pictures into geometry, entirely locally:
//  - relief:  an image becomes a height-mapped surface (emblems, coins, height maps, textures)
//  - project: an image is stamped onto the current model from a view (like a stencil)
//  - inflate: a sketch's silhouette is inflated into a closed, puffy 3D base mesh
// Full photo-to-3D reconstruction is delegated to external generators (see ai/Generator3D.h).
#pragma once

#include "core/Image.h"
#include "core/Mesh.h"

#include <glm/glm.hpp>

namespace sf {

struct ReliefOptions {
    int resolution = 384;  // grid cells along the image's long side
    float depth = 0.12f;   // relief height as a fraction of the plane width
    bool invert = false;   // dark = high instead of light = high
};
Mesh makeImageRelief(const Image& image, const ReliefOptions& options);

struct ProjectOptions {
    float depth = 0.05f;   // world-space displacement at full brightness
    bool invert = false;
    float coverage = 0.9f; // fraction of the view the image fills
};
// Displaces vertices along their normals by the image as seen through
// `viewProj` (image fitted to a view of aspect `viewAspect`). Only surfaces
// facing the camera are affected. Offsets go into sculpt layer `layer`
// (-1 = base). Returns the number of vertices moved; normals are updated.
size_t projectImage(Mesh& mesh, int layer, const Image& image, const glm::mat4& viewProj, float viewAspect,
                    const glm::vec3& cameraPos, const ProjectOptions& options);

struct InflateOptions {
    int resolution = 160;   // grid cells along the long side
    float thickness = 1.0f; // 1 = round cross-sections, lower = flatter
    int subdivisions = 1;   // Loop subdivision passes for smoothness
};
// The shape is the image's opaque area (if it has transparency) or whatever
// contrasts with the border colour. Returns an empty mesh if no shape is found.
Mesh inflateSilhouette(const Image& image, const InflateOptions& options, std::string* error = nullptr);

} // namespace sf
