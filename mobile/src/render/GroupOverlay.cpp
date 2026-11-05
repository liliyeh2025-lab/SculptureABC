// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
#include "render/GroupOverlay.h"

#include "MobileGL.h"

#include "core/Mesh.h"
#include "render/Renderer.h" // sf::polygroupColor

#include <SDL3/SDL_log.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace {

constexpr GLenum kDstColor = 0x0306, kZero = 0, kDepthFunc = 0x0B74, kLess = 0x0201;
constexpr uint32_t kUnowned = 0xFFFFFFFFu;

const char* const kVertex = R"(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
uniform mat4 uViewProj;
flat out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = uViewProj * vec4(aPos, 1.0);
    gl_Position.z -= 2e-4 * gl_Position.w; // (a hair in front of the renderer's own surface)
}
)";

const char* const kFragment = R"(
flat in vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)";

} // namespace

void GroupOverlay::noteChanges(const sf::Mesh& mesh) {
    if (&mesh != mesh_ || mesh.topologyDirty || mesh.facesDirty) stale_ = true;
    if (mesh.dirtyEnd > mesh.dirtyBegin) {
        dirtyBegin_ = std::min(dirtyBegin_, mesh.dirtyBegin);
        dirtyEnd_ = std::max(dirtyEnd_, mesh.dirtyEnd);
    }
}

void GroupOverlay::release() {
    if (program_) mgl::DeleteProgram(program_);
    if (vao_) mgl::DeleteVertexArrays(1, &vao_);
    for (GLuint* b : {&positions_, &colors_, &indices_})
        if (*b) mgl::DeleteBuffers(1, b);
    program_ = vao_ = positions_ = colors_ = indices_ = 0;
    stale_ = true;
}

bool GroupOverlay::build(const sf::Mesh& mesh) {
    // Every visible triangle picks a corner to carry its colour: one already carrying its group, else one not yet
    // taken, else a new copy of a corner. Inside a group any corner will do; only thin strips on borders need copies.
    const size_t nv = mesh.vertexCount(), nt = mesh.triangleCount();
    std::vector<uint32_t> owner(nv, kUnowned), extraGroup;
    std::vector<uint32_t> index;
    index.reserve(nt * 3);
    extraSource_.clear();
    for (uint32_t t = 0; t < nt; ++t) {
        if (mesh.triangleHidden(t)) continue;
        const uint32_t g = mesh.polygroupAt(t);
        const glm::uvec3& tri = mesh.triangles[t];
        const uint32_t c[3] = {tri.x, tri.y, tri.z};
        int k = -1;
        for (int i = 0; i < 3 && k < 0; ++i)
            if (owner[c[i]] == g) k = i;
        for (int i = 0; i < 3 && k < 0; ++i)
            if (owner[c[i]] == kUnowned) owner[c[i]] = g, k = i;
        if (k >= 0) { // (turned so that corner comes last; the winding stays)
            index.insert(index.end(), {c[(k + 1) % 3], c[(k + 2) % 3], c[k]});
            continue;
        }
        extraSource_.push_back(c[0]);
        extraGroup.push_back(g);
        index.insert(index.end(), {c[1], c[2], uint32_t(nv + extraSource_.size() - 1)});
    }

    // Colours: the group's display colour (linear) in the framebuffer's gamma, to multiply the shading by.
    std::vector<uint32_t> colors(nv + extraSource_.size(), 0xFFFFFFFFu);
    uint32_t lastGroup = kUnowned, lastColor = 0;
    const auto colorOf = [&](uint32_t g) {
        if (g != lastGroup) {
            const glm::vec3 c = glm::pow(glm::clamp(sf::polygroupColor(g), 0.0f, 1.0f), glm::vec3(1.0f / 2.2f));
            lastGroup = g;
            lastColor = uint32_t(std::lround(c.x * 255.0f)) | uint32_t(std::lround(c.y * 255.0f)) << 8 |
                        uint32_t(std::lround(c.z * 255.0f)) << 16 | 0xFF000000u;
        }
        return lastColor;
    };
    for (size_t v = 0; v < nv; ++v)
        if (owner[v] != kUnowned) colors[v] = colorOf(owner[v]);
    for (size_t e = 0; e < extraGroup.size(); ++e) colors[nv + e] = colorOf(extraGroup[e]);

    if (!vao_) {
        mgl::GenVertexArrays(1, &vao_);
        mgl::GenBuffers(1, &positions_);
        mgl::GenBuffers(1, &colors_);
        mgl::GenBuffers(1, &indices_);
    }
    mgl::BindVertexArray(vao_);
    mgl::BindBuffer(GL_ARRAY_BUFFER, colors_);
    mgl::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(colors.size() * sizeof(uint32_t)), colors.data(), GL_STATIC_DRAW);
    mgl::EnableVertexAttribArray(1);
    mgl::VertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, nullptr);
    mgl::BindBuffer(GL_ARRAY_BUFFER, positions_);
    mgl::BufferData(GL_ARRAY_BUFFER, GLsizeiptr((nv + extraSource_.size()) * sizeof(glm::vec3)), nullptr, GL_DYNAMIC_DRAW);
    mgl::EnableVertexAttribArray(0);
    mgl::VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    mgl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_);
    mgl::BufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(index.size() * sizeof(uint32_t)), index.data(), GL_STATIC_DRAW);
    mgl::BindVertexArray(0);
    mgl::BindBuffer(GL_ARRAY_BUFFER, 0);
    mesh_ = &mesh;
    vertices_ = nv;
    triangles_ = nt;
    indexCount_ = GLsizei(index.size());
    uploadPositions(mesh, 0, uint32_t(nv));
    stale_ = false;
    return true;
}

void GroupOverlay::uploadPositions(const sf::Mesh& mesh, uint32_t begin, uint32_t end) {
    end = std::min<uint32_t>(end, uint32_t(vertices_));
    mgl::BindBuffer(GL_ARRAY_BUFFER, positions_);
    if (end > begin)
        mgl::BufferSubData(GL_ARRAY_BUFFER, GLintptr(size_t(begin) * sizeof(glm::vec3)), GLsizeiptr(size_t(end - begin) * sizeof(glm::vec3)),
                           mesh.positions.data() + begin);
    if (!extraSource_.empty()) { // (few: copied whole)
        std::vector<glm::vec3> extra(extraSource_.size());
        for (size_t e = 0; e < extra.size(); ++e) extra[e] = mesh.positions[extraSource_[e]];
        mgl::BufferSubData(GL_ARRAY_BUFFER, GLintptr(vertices_ * sizeof(glm::vec3)), GLsizeiptr(extra.size() * sizeof(glm::vec3)), extra.data());
    }
    mgl::BindBuffer(GL_ARRAY_BUFFER, 0);
    dirtyBegin_ = std::numeric_limits<uint32_t>::max();
    dirtyEnd_ = 0;
}

void GroupOverlay::draw(const sf::Mesh& mesh, const glm::mat4& viewProj) {
    if (failed_ || mesh.triangleCount() == 0) return;
    if (!program_) {
        std::string error;
        program_ = mgl::buildProgram(kVertex, kFragment, error);
        if (!program_) {
            SDL_Log("group overlay: %s", error.c_str());
            failed_ = true;
            return;
        }
        viewProjAt_ = mgl::GetUniformLocation(program_, "uViewProj");
    }
    if (stale_ || &mesh != mesh_ || mesh.vertexCount() != vertices_ || mesh.triangleCount() != triangles_) build(mesh);
    else if (dirtyEnd_ > dirtyBegin_) uploadPositions(mesh, dirtyBegin_, dirtyEnd_);
    if (indexCount_ == 0) return;

    GLint depthFunc = GLint(kLess), depthTest = 0;
    mgl::GetIntegerv(kDepthFunc, &depthFunc);
    mgl::GetIntegerv(GL_DEPTH_TEST, &depthTest);
    mgl::Enable(GL_DEPTH_TEST);
    mgl::DepthFunc(GL_LEQUAL);
    mgl::DepthMask(GL_FALSE);
    mgl::Enable(GL_BLEND);
    mgl::BlendFunc(kDstColor, kZero);
    mgl::UseProgram(program_);
    mgl::UniformMatrix4fv(viewProjAt_, 1, GL_FALSE, glm::value_ptr(viewProj));
    mgl::BindVertexArray(vao_);
    mgl::DrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    mgl::BindVertexArray(0);
    mgl::UseProgram(0);
    mgl::Disable(GL_BLEND);
    mgl::DepthMask(GL_TRUE);
    mgl::DepthFunc(GLenum(depthFunc));
    if (!depthTest) mgl::Disable(GL_DEPTH_TEST);
}
