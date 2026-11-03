// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Tangents.h"

extern "C" {
#include "mikktspace.h"
}

namespace sf {

namespace {

struct MikkMesh {
    const Mesh* mesh;
    std::vector<glm::vec4>* out;
};

const Mesh& meshOf(const SMikkTSpaceContext* c) { return *static_cast<const MikkMesh*>(c->m_pUserData)->mesh; }

int numFaces(const SMikkTSpaceContext* c) { return int(meshOf(c).triangleCount()); }
int numVerts(const SMikkTSpaceContext*, const int) { return 3; }
void position(const SMikkTSpaceContext* c, float out[], const int f, const int v) {
    const Mesh& m = meshOf(c);
    const glm::vec3& p = m.positions[m.triangles[size_t(f)][v]];
    out[0] = p.x, out[1] = p.y, out[2] = p.z;
}
void normal(const SMikkTSpaceContext* c, float out[], const int f, const int v) {
    const Mesh& m = meshOf(c);
    const glm::vec3& n = m.normals[m.triangles[size_t(f)][v]];
    out[0] = n.x, out[1] = n.y, out[2] = n.z;
}
void texCoord(const SMikkTSpaceContext* c, float out[], const int f, const int v) {
    const glm::vec2& t = meshOf(c).uvs[size_t(f) * 3 + size_t(v)];
    out[0] = t.x, out[1] = t.y;
}
void setTangent(const SMikkTSpaceContext* c, const float t[], const float sign, const int f, const int v) {
    (*static_cast<const MikkMesh*>(c->m_pUserData)->out)[size_t(f) * 3 + size_t(v)] = glm::vec4(t[0], t[1], t[2], sign);
}

} // namespace

std::vector<glm::vec4> computeTangents(const Mesh& mesh) {
    std::vector<glm::vec4> out;
    const size_t nt = mesh.triangleCount();
    if (nt == 0 || mesh.uvs.size() != nt * 3 || mesh.normals.size() != mesh.vertexCount()) return out;
    out.assign(nt * 3, glm::vec4(1, 0, 0, 1));
    SMikkTSpaceInterface iface{};
    iface.m_getNumFaces = numFaces;
    iface.m_getNumVerticesOfFace = numVerts;
    iface.m_getPosition = position;
    iface.m_getNormal = normal;
    iface.m_getTexCoord = texCoord;
    iface.m_setTSpaceBasic = setTangent;
    MikkMesh user{&mesh, &out};
    SMikkTSpaceContext ctx{&iface, &user};
    genTangSpaceDefault(&ctx);
    return out;
}

} // namespace sf
