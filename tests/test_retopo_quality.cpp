// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// QRemesh quality on hard cases: separate pieces (small ones must not vanish), thin hard-edged slabs, models
// away from the origin with symmetry, asymmetric models with symmetry. The metrics (distance to the source,
// quads, pieces kept, irregular vertices) are printed; SF_QREMESH_QUALITY=1 also runs the lion head asset and
// a 1M+ triangle timing.
#include "TestHarness.h"

#include "core/Commands.h"
#include "core/Primitives.h"
#include "core/QRemesh.h"
#include "core/SurfaceQuery.h"

#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <numeric>
#include <string>
#include <unordered_map>

using namespace sf;

namespace {

float diagonal(const Mesh& m) { return glm::length(m.bounds().extent()); }

// Connected pieces (triangles sharing vertices) and the bounding box of each.
std::vector<AABB> pieces(const Mesh& m) {
    std::vector<uint32_t> parent(m.vertexCount());
    std::iota(parent.begin(), parent.end(), 0u);
    auto find = [&](uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    for (const glm::uvec3& t : m.triangles) {
        parent[find(t.y)] = find(t.x);
        parent[find(t.z)] = find(t.x);
    }
    std::unordered_map<uint32_t, AABB> boxes;
    for (const glm::uvec3& t : m.triangles)
        for (int k = 0; k < 3; ++k) boxes[find(t.x)].expand(m.positions[t[k]]);
    std::vector<AABB> out;
    for (auto& [r, b] : boxes) out.push_back(b);
    return out;
}

// Source pieces with a result piece around their centre (within a quarter of their size).
size_t piecesKept(const Mesh& source, const Mesh& result) {
    const std::vector<AABB> src = pieces(source), res = pieces(result);
    size_t kept = 0;
    for (const AABB& s : src) {
        const glm::vec3 c = s.center();
        const float tol = 0.25f * glm::length(s.extent());
        for (const AABB& r : res)
            if (glm::length(r.center() - c) < tol && glm::length(r.extent() - s.extent()) < 2.0f * tol) {
                ++kept;
                break;
            }
    }
    return kept;
}

// Interior vertices whose valence (quads around them) isn't 4.
size_t irregularVertices(const Mesh& m) {
    std::vector<int> valence(m.vertexCount(), 0);
    std::unordered_map<uint64_t, int> edges;
    for (uint32_t t = 0; t < m.triangleCount(); ++t) {
        glm::uvec4 f;
        const int n = m.exportFace(t, f);
        for (int k = 0; k < n; ++k) {
            ++valence[f[k]];
            const uint32_t a = f[k], b = f[(k + 1) % n];
            ++edges[a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a];
        }
    }
    std::vector<uint8_t> open(m.vertexCount(), 0);
    for (auto [key, uses] : edges)
        if (uses == 1) open[key >> 32] = open[key & 0xffffffffu] = 1;
    size_t irregular = 0;
    for (uint32_t v = 0; v < m.vertexCount(); ++v)
        if (valence[v] > 0 && !open[v] && valence[v] != 4) ++irregular;
    return irregular;
}

struct Quality {
    float mean = 0, hausdorff = 0; // percent of the diagonal
    float quads = 0;               // percent of the faces
    size_t faces = 0, kept = 0, sourcePieces = 0, irregular = 0, nonManifold = 0, boundary = 0;
    float irregularPercent = 0;
    double seconds = 0;
};

Quality measure(const char* name, const Mesh& source, const QRemeshOptions& o, Mesh* result = nullptr, bool quiet = false) {
    Mesh out;
    std::string err;
    QRemeshStats st;
    Quality q;
    if (!qremesh(source, o, out, err, {}, &st)) {
        std::printf("  %-26s FAILED: %s\n", name, err.c_str());
        return q;
    }
    const SurfaceDeviation d = measureDeviation(out, source);
    const TopologyReport t = topologyReport(out);
    const float diag = diagonal(source);
    q.mean = 100.0f * d.mean / diag;
    q.hausdorff = 100.0f * d.max / diag;
    q.quads = 100.0f * t.quadFraction();
    q.faces = t.faces;
    q.sourcePieces = pieces(source).size();
    q.kept = piecesKept(source, out);
    q.irregular = irregularVertices(out);
    q.nonManifold = t.nonManifoldEdges + t.nonManifoldVertices;
    q.boundary = t.boundaryEdges;
    q.seconds = st.seconds;
    q.irregularPercent = 100.0f * float(q.irregular) / float(std::max<size_t>(out.vertexCount(), 1));
    if (!quiet)
        std::printf("  %-26s %6zu faces %5.1f%% quads  mean %.3f%%  Hausdorff %.2f%%  pieces %zu/%zu  irregular %zu "
                "(%.1f%%)  non-manifold %zu  open edges %zu  %.2f s\n",
                name, q.faces, q.quads, q.mean, q.hausdorff, q.kept, q.sourcePieces, q.irregular,
                100.0 * double(q.irregular) / double(std::max<size_t>(out.vertexCount(), 1)), q.nonManifold, q.boundary,
                q.seconds);
    if (std::getenv("SF_QREMESH_WORST")) { // where the result is furthest from the source
        TriangleBVH bvh;
        bvh.build(out);
        std::vector<std::pair<float, glm::vec3>> worst;
        for (const glm::vec3& p : source.positions) {
            SurfaceHit hit;
            worst.push_back({bvh.closestPoint(p, 10.0f, hit) ? hit.distance : 10.0f, p});
        }
        std::sort(worst.begin(), worst.end(), [](auto& a, auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < std::min<size_t>(6, worst.size()); ++i)
            std::printf("    source %.3f %.3f %.3f is %.4f away\n", worst[i].second.x, worst[i].second.y, worst[i].second.z,
                        worst[i].first);
    }
    if (const char* dump = std::getenv("SF_QREMESH_DUMP")) { // OBJ of each result for a look
        std::string path = std::string(dump) + "/" + name + ".obj";
        for (char& c : path)
            if (c == ' ' || c == '(' || c == ')') c = '_';
        if (FILE* f = std::fopen(path.c_str(), "w")) {
            for (const glm::vec3& p : out.positions) std::fprintf(f, "v %f %f %f\n", p.x, p.y, p.z);
            for (uint32_t t = 0; t < out.triangleCount(); ++t) {
                glm::uvec4 fc;
                const int k = out.exportFace(t, fc);
                if (k == 4) std::fprintf(f, "f %u %u %u %u\n", fc.x + 1, fc.y + 1, fc.z + 1, fc.w + 1);
                else if (k == 3) std::fprintf(f, "f %u %u %u\n", fc.x + 1, fc.y + 1, fc.z + 1);
            }
            std::fclose(f);
        }
    }
    if (result) *result = std::move(out);
    return q;
}

// The averages over a few seeds (single runs vary by a few percent).
void average(const std::string& name, const Mesh& source, QRemeshOptions o, int seeds) {
    Quality a;
    a.kept = ~size_t(0);
    for (int seed = 1; seed <= seeds; ++seed) {
        o.seed = uint32_t(seed);
        const Quality q = measure(name.c_str(), source, o, nullptr, seeds > 1);
        a.faces += q.faces / size_t(seeds);
        a.quads += q.quads / float(seeds);
        a.mean += q.mean / float(seeds);
        a.hausdorff += q.hausdorff / float(seeds);
        a.irregularPercent += q.irregularPercent / float(seeds);
        a.seconds += q.seconds / double(seeds);
        a.kept = std::min(a.kept, q.kept);
        a.sourcePieces = q.sourcePieces;
        a.nonManifold += q.nonManifold;
        a.boundary += q.boundary;
    }
    if (seeds > 1)
        std::printf("  %-26s %6zu faces %5.1f%% quads  mean %.3f%%  Hausdorff %.2f%%  pieces %zu/%zu  irregular %.1f%%  "
                    "non-manifold %zu  open edges %zu  %.2f s  (%d seeds)\n",
                    name.c_str(), a.faces, a.quads, a.mean, a.hausdorff, a.kept, a.sourcePieces, a.irregularPercent,
                    a.nonManifold, a.boundary, a.seconds, seeds);
}

Mesh box(glm::vec3 size, glm::vec3 center, int resolution) {
    Mesh m = makeCube(resolution, 2.0f);
    for (glm::vec3& p : m.positions) p = center + p * 0.5f * size;
    m.recomputeAllNormals();
    return m;
}

Mesh sphere(float radius, glm::vec3 center, int subdivisions) {
    Mesh m = makeIcosphere(subdivisions, radius);
    for (glm::vec3& p : m.positions) p += center;
    m.recomputeAllNormals();
    return m;
}

Mesh combine(const std::vector<Mesh>& parts) {
    Mesh m;
    for (const Mesh& p : parts) {
        const uint32_t base = uint32_t(m.positions.size());
        m.positions.insert(m.positions.end(), p.positions.begin(), p.positions.end());
        for (const glm::uvec3& t : p.triangles) m.triangles.push_back(t + glm::uvec3(base));
    }
    m.rebuildTopology();
    m.recomputeAllNormals();
    return m;
}

// A body with small separate pieces: eyes, rivets, a button (0.5% to 3% of the body's area each).
Mesh multiPart() {
    std::vector<Mesh> parts{sphere(1.0f, {0, 0, 0}, 5)};
    parts.push_back(sphere(0.12f, {0.35f, 0.3f, 0.98f}, 3));
    parts.push_back(sphere(0.12f, {-0.35f, 0.3f, 0.98f}, 3));
    for (int i = 0; i < 6; ++i) {
        const float a = float(i) * 1.047f;
        parts.push_back(box(glm::vec3(0.07f), {1.08f * std::cos(a), -0.3f, 1.08f * std::sin(a)}, 4));
    }
    parts.push_back(sphere(0.2f, {0.0f, 1.12f, 0.0f}, 3));
    return combine(parts);
}

// A thick block with a thin hard-edged fin standing on it (separate shells, as kitbashed).
Mesh blockWithFin() { return combine({box({2.0f, 1.0f, 1.2f}, {0, 0, 0}, 24), box({1.4f, 0.8f, 0.05f}, {0, 0.85f, 0}, 40)}); }

// One thin slab on its own: a 2 x 0.06 x 1.2 board.
Mesh thinSlab() { return box({2.0f, 0.06f, 1.2f}, {0, 0, 0}, 48); }

// Mirror-symmetry error of a result: the mean distance of each vertex's mirror image to the nearest vertex.
float mirrorError(const Mesh& m, int axis, float center) {
    TriangleBVH bvh;
    std::vector<glm::uvec3> points(m.vertexCount());
    for (uint32_t v = 0; v < m.vertexCount(); ++v) points[v] = {v, v, v};
    bvh.build(m.positions, points);
    double sum = 0.0;
    for (const glm::vec3& p : m.positions) {
        glm::vec3 q = p;
        q[axis] = 2.0f * center - q[axis];
        SurfaceHit hit;
        sum += bvh.closestPoint(q, 10.0f, hit) ? hit.distance : 10.0f;
    }
    return float(sum / double(std::max<size_t>(m.vertexCount(), 1)));
}

} // namespace

TEST(qremesh_keeps_small_pieces) {
    const Mesh src = multiPart();
    QRemeshOptions o;
    o.targetQuads = 2000;
    Mesh out;
    const Quality q = measure("multi-part (10 pieces)", src, o, &out);
    CHECK(q.kept == q.sourcePieces);
    CHECK(q.nonManifold == 0);
    CHECK(q.boundary == 0);
    CHECK(q.quads >= 95.0f);
    CHECK(q.hausdorff < 2.0f);
    // The total stays near the target.
    CHECK(q.faces > 1600 && q.faces < 2600);
    // With X symmetry (the pieces are placed symmetrically): mirrored, every piece still there.
    o.symmetryAxis = 0;
    Mesh sym;
    const Quality s = measure("multi-part, X symmetry", src, o, &sym);
    CHECK(s.kept == s.sourcePieces);
    CHECK(s.nonManifold == 0);
    CHECK(s.boundary == 0);
    CHECK(s.hausdorff < 2.0f);
}

TEST(qremesh_thin_hard_edged_parts) {
    // Parts thinner than a quad: the two sides must not be merged into one sheet (before: mean 1.9% / 2.2% of
    // the size off, Hausdorff 11.6% / 8.8%), and the hard edges keep their loops.
    QRemeshOptions o;
    o.targetQuads = 1500;
    const Quality fin = measure("block with thin fin", blockWithFin(), o);
    CHECK(fin.kept == 2);
    CHECK(fin.nonManifold == 0);
    CHECK(fin.boundary == 0);
    CHECK(fin.mean < 0.2f);
    CHECK(fin.hausdorff < 2.5f);
    o.targetQuads = 800;
    const Quality slab = measure("thin slab", thinSlab(), o);
    CHECK(slab.kept == 1);
    CHECK(slab.nonManifold == 0);
    CHECK(slab.boundary == 0);
    CHECK(slab.mean < 0.2f);
    CHECK(slab.hausdorff < 3.5f); // the corners of the slab are still cut a little
}

TEST(qremesh_symmetry_off_origin) {
    // A symmetric sculpt moved off the origin: the mirror plane is the model's own, not the world's.
    CommandRegistry reg;
    Document doc;
    reg.execute(doc, "new_sphere", {{"subdivisions", 5.0}});
    CHECK(reg.execute(doc, "stroke", {{"points", std::string("-0.5,0.3,2 0,0.5,2 0.5,0.3,2")}, {"brush", std::string("Standard")},
                                      {"radius", 0.18}, {"strength", 1.0}}).ok);
    Mesh src = doc.mesh;
    const glm::vec3 shift(3.0f, 0.5f, -1.0f);
    for (glm::vec3& p : src.positions) p += shift;
    src.recomputeAllNormals();
    QRemeshOptions o;
    o.targetQuads = 1600;
    o.symmetryAxis = 0;
    Mesh out;
    const Quality q = measure("sphere at x=3, X symmetry", src, o, &out);
    CHECK(q.nonManifold == 0);
    CHECK(q.boundary == 0);
    CHECK(q.hausdorff < 1.0f);
    const float err = mirrorError(out, 0, shift.x);
    std::printf("  mirror error about the model's plane x=%.1f: %.6f\n", shift.x, err);
    CHECK(err < 1e-4f);
    // An explicit plane through the origin is still possible (and here nothing is on its positive... side
    // apart from the whole model, which isn't symmetric about it: QRemesh falls back to no symmetry).
    QRemeshStats st;
    std::string e;
    o.symmetryCenter = 0.0f;
    o.symmetryAuto = false;
    Mesh world;
    CHECK(qremesh(src, o, world, e, {}, &st));
    CHECK(!st.symmetryUsed);
    CHECK(measureDeviation(world, src).max < 0.01f * diagonal(src));
}

TEST(qremesh_symmetry_on_asymmetric_models) {
    // Symmetry asked for on models that aren't symmetric must not wreck them.
    CommandRegistry reg;
    Document doc;
    reg.execute(doc, "new_sphere", {{"subdivisions", 5.0}});
    // A bump on one side only (slightly asymmetric: symmetric topology, each side on its own surface).
    CHECK(reg.execute(doc, "stroke", {{"points", std::string("0.4,0.3,2 0.6,0.3,2")}, {"brush", std::string("Standard")},
                                      {"radius", 0.2}, {"strength", 1.0}}).ok);
    QRemeshOptions o;
    o.targetQuads = 1600;
    o.symmetryAxis = 0;
    QRemeshStats st;
    Mesh out;
    std::string err;
    CHECK(qremesh(doc.mesh, o, out, err, {}, &st));
    const float diag = diagonal(doc.mesh);
    const SurfaceDeviation d = measureDeviation(out, doc.mesh);
    std::printf("  one-sided bump, X symmetry: used %d, Hausdorff %.3f%%\n", int(st.symmetryUsed), 100.0f * d.max / diag);
    CHECK(st.symmetryUsed);
    CHECK(d.max < 0.01f * diag);
    CHECK(topologyReport(out).manifold());
    // Very asymmetric: a multi-part model off to one side, an L-shaped block.
    Mesh lshape = combine({box({2.0f, 0.5f, 0.5f}, {0, 0, 0}, 16), box({0.5f, 1.5f, 0.5f}, {0.75f, 1.0f, 0}, 16)});
    for (const Mesh* m : {&lshape}) {
        Mesh r;
        CHECK(qremesh(*m, o, r, err, {}, &st));
        const SurfaceDeviation dd = measureDeviation(r, *m);
        std::printf("  L block, X symmetry: used %d, %zu quads, %zu triangles, Hausdorff %.3f%%\n", int(st.symmetryUsed), st.quads,
                    st.triangles, 100.0f * dd.max / diagonal(*m));
        CHECK(st.triangles == 0);
        CHECK(!st.symmetryUsed);
        CHECK(dd.max < 0.02f * diagonal(*m));
        CHECK(topologyReport(r).manifold());
    }
}

TEST(qremesh_quality_report) {
    if (!std::getenv("SF_QREMESH_QUALITY")) return;
    const int seeds = 4;
    QRemeshOptions o;
    o.targetQuads = 2000;
    average("multi-part (10 pieces)", multiPart(), o, seeds);
    o.targetQuads = 1500;
    average("block with thin fin", blockWithFin(), o, seeds);
    o.targetQuads = 800;
    average("thin slab", thinSlab(), o, seeds);
    o.targetQuads = 5000;
    average("cube 24", makeCube(24), o, seeds);
    // Symmetry on by default in the app: symmetric shapes get mirrored, the others must come out as without.
    o.symmetryAxis = 0;
    o.targetQuads = 2000;
    average("multi-part sym X", multiPart(), o, 1);
    o.targetQuads = 1500;
    average("block with thin fin sym X", blockWithFin(), o, 1);
    o.targetQuads = 800;
    average("thin slab sym X", thinSlab(), o, 1);
    o.targetQuads = 5000;
    average("cube 24 sym X", makeCube(24), o, 1);
    o.symmetryAxis = -1;
    std::string lion = __FILE__;
    lion = lion.substr(0, lion.rfind("tests")) + "assets/demo/lion-head.glb";
    CommandRegistry reg;
    Document doc;
    if (reg.execute(doc, "open", {{"path", lion}}).ok) {
        const Mesh m = doc.mesh;
        std::printf("  lion head: %zu triangles\n", m.triangleCount());
        for (int target : {2000, 8000}) {
            o.targetQuads = target;
            average("lion head " + std::to_string(target), m, o, seeds);
        }
        o.symmetryAxis = 0;
        o.targetQuads = 8000;
        average("lion head 8000 sym X", m, o, 1);
        o.symmetryAxis = -1;
    } else {
        std::printf("  (no lion head at %s)\n", lion.c_str());
    }
    // Timing at 1M+ triangles.
    Document big;
    reg.execute(big, "new_sphere", {{"subdivisions", 6.0}});
    reg.execute(big, "stroke", {{"points", std::string("-0.6,0.3,2 0,0.5,2 0.6,0.3,2")}, {"brush", std::string("Standard")},
                                {"radius", 0.18}, {"strength", 1.0}});
    reg.execute(big, "subdivide", {{"levels", 2.0}});
    std::printf("  big sculpt: %zu triangles\n", big.mesh.triangleCount());
    for (int target : {5000, 20000, 100000}) {
        o.targetQuads = target;
        average("1.3M sculpt " + std::to_string(target), big.mesh, o, 1);
    }
    o.targetQuads = 20000;
    o.symmetryAxis = 0;
    average("1.3M sculpt 20000 sym X", big.mesh, o, 1);
    o.symmetryAxis = -1;
    // Many parts at 1M+: the multi-part model divided.
    Document parts;
    parts.setMesh(multiPart());
    reg.execute(parts, "subdivide", {{"levels", 3.0}});
    std::printf("  multi-part divided: %zu triangles\n", parts.mesh.triangleCount());
    o.targetQuads = 20000;
    average("multi-part 1M 20000", parts.mesh, o, 1);
}

TEST(qremesh_command_reports_symmetry) {
    CommandRegistry reg;
    Document doc;
    reg.execute(doc, "new_sphere", {{"subdivisions", 4.0}});
    for (glm::vec3& p : doc.mesh.positions) p.x += 3.0f;
    doc.mesh.recomputeAllNormals();
    const Mesh sphere = doc.mesh;
    CommandResult r = reg.execute(doc, "qremesh", {{"target", 800.0}, {"symmetry", std::string("x")}});
    CHECK(r.ok);
    std::printf("  %s\n", r.message.c_str());
    CHECK(r.message.find("mirrored across X = 3") != std::string::npos);
    // The plane through the world's centre given explicitly: the sphere isn't symmetric about it.
    doc.setMesh(sphere);
    r = reg.execute(doc, "qremesh", {{"target", 800.0}, {"symmetry", std::string("x")}, {"symmetry_center", 0.0}});
    CHECK(r.ok);
    std::printf("  %s\n", r.message.c_str());
    CHECK(r.message.find("Symmetry off") != std::string::npos);
    CHECK(measureDeviation(doc.mesh, sphere).max < 0.02f * diagonal(sphere));
}
