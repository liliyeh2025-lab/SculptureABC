// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
//
// Face group colours drawn over the renderer's picture. Upstream's renderer looks each triangle's group up by
// gl_PrimitiveID, which GLSL ES 3.00 (WebGL2) lacks: render/GLCompat.cpp turns it into 0, and every cluster then
// takes its first triangle's colour. This pass colours the triangles itself instead: the mesh's own positions and
// triangles, each triangle rotated so that its last corner (the provoking vertex, whose flat colour the triangle
// takes) carries the triangle's group colour; a triangle whose corners all belong to other groups gets an extra copy
// of a corner. The colours multiply what is already on screen, so the clay shading shows through.
#pragma once

#include "render/GL.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>
#include <vector>

namespace sf {
class Mesh;
}

class GroupOverlay {
public:
    // Call before the renderer syncs the mesh (the renderer clears the mesh's change marks, Mesh::dirtyBegin /
    // facesDirty...): notes what changed.
    void noteChanges(const sf::Mesh& mesh);
    // Multiplies the face group colours of `mesh` (its visible triangles) into the current framebuffer, depth-tested
    // against what the renderer drew with the same `viewProj`.
    void draw(const sf::Mesh& mesh, const glm::mat4& viewProj);
    void release();
    void invalidate() { stale_ = true; } // the scene changed under it (another Part, undo...)

private:
    bool build(const sf::Mesh& mesh);
    void uploadPositions(const sf::Mesh& mesh, uint32_t begin, uint32_t end);

    GLuint program_ = 0, vao_ = 0, positions_ = 0, colors_ = 0, indices_ = 0;
    GLint viewProjAt_ = -1;
    bool failed_ = false; // the shaders didn't build: draw nothing
    bool stale_ = true;   // groups, visibility or topology changed: build again
    const sf::Mesh* mesh_ = nullptr;
    size_t vertices_ = 0, triangles_ = 0;
    GLsizei indexCount_ = 0;
    std::vector<uint32_t> extraSource_; // the extra vertices (after the mesh's): the mesh vertex each one copies
    uint32_t dirtyBegin_ = std::numeric_limits<uint32_t>::max(), dirtyEnd_ = 0; // positions moved since the last draw
};
