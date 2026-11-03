// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Voxel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

namespace sf {

namespace {

// Running max (dilate) or min (erode) over a window of +-radius along one axis.
void filterAxis(std::vector<uint8_t>& data, const glm::ivec3& dims, int axis, int radius, bool dilate) {
    const glm::ivec3 step(1, dims.x, dims.x * dims.y);
    const int n = dims[axis];
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    std::vector<uint8_t> line(static_cast<size_t>(n)), out(static_cast<size_t>(n));
    for (int b = 0; b < dims[v]; ++b)
        for (int a = 0; a < dims[u]; ++a) {
            const size_t start = size_t(a) * size_t(step[u]) + size_t(b) * size_t(step[v]);
            for (int i = 0; i < n; ++i) line[size_t(i)] = data[start + size_t(i) * size_t(step[axis])];
            for (int i = 0; i < n; ++i) {
                uint8_t r = line[size_t(i)];
                for (int k = std::max(0, i - radius); k <= std::min(n - 1, i + radius); ++k)
                    r = dilate ? std::max(r, line[size_t(k)]) : std::min(r, line[size_t(k)]);
                out[size_t(i)] = r;
            }
            for (int i = 0; i < n; ++i) data[start + size_t(i) * size_t(step[axis])] = out[size_t(i)];
        }
}

void morph(std::vector<uint8_t>& data, const glm::ivec3& dims, int radius, bool dilate) {
    if (radius <= 0) return;
    for (int axis = 0; axis < 3; ++axis) filterAxis(data, dims, axis, radius, dilate);
}

// Labels 6-connected regions where data[i] == value, starting from `seeds`.
void floodFill(const std::vector<uint8_t>& data, uint8_t value, const glm::ivec3& dims, std::vector<uint32_t>& stack,
               std::vector<uint8_t>& reached) {
    const int64_t sx = 1, sy = dims.x, sz = int64_t(dims.x) * dims.y;
    while (!stack.empty()) {
        const uint32_t i = stack.back();
        stack.pop_back();
        const int x = int(i % uint32_t(dims.x));
        const int y = int((i / uint32_t(dims.x)) % uint32_t(dims.y));
        const int z = int(i / uint32_t(sz));
        auto visit = [&](bool inRange, int64_t offset) {
            if (!inRange) return;
            const uint32_t j = uint32_t(int64_t(i) + offset);
            if (!reached[j] && data[j] == value) {
                reached[j] = 1;
                stack.push_back(j);
            }
        };
        visit(x > 0, -sx);
        visit(x + 1 < dims.x, sx);
        visit(y > 0, -sy);
        visit(y + 1 < dims.y, sy);
        visit(z > 0, -sz);
        visit(z + 1 < dims.z, sz);
    }
}

void blurAxis(std::vector<float>& f, const glm::ivec3& dims, int axis) {
    static const std::array<float, 5> w = [] {
        std::array<float, 5> k{};
        float sum = 0.0f;
        for (int i = 0; i < 5; ++i) sum += k[size_t(i)] = std::exp(-0.5f * float((i - 2) * (i - 2)));
        for (float& x : k) x /= sum;
        return k;
    }();
    const glm::ivec3 step(1, dims.x, dims.x * dims.y);
    const int n = dims[axis];
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    std::vector<float> line(static_cast<size_t>(n));
    for (int b = 0; b < dims[v]; ++b)
        for (int a = 0; a < dims[u]; ++a) {
            const size_t start = size_t(a) * size_t(step[u]) + size_t(b) * size_t(step[v]);
            for (int i = 0; i < n; ++i) line[size_t(i)] = f[start + size_t(i) * size_t(step[axis])];
            for (int i = 0; i < n; ++i) {
                float s = 0.0f;
                for (int k = -2; k <= 2; ++k) s += w[size_t(k + 2)] * line[size_t(std::clamp(i + k, 0, n - 1))];
                f[start + size_t(i) * size_t(step[axis])] = s;
            }
        }
}

// Closest point to `p` on triangle abc (Ericson, Real-Time Collision Detection 5.1.5).
glm::vec3 closestOnTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));
    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

} // namespace

bool makeVoxelGrid(const AABB& box, int resolution, int padding, VoxelGrid& grid, size_t maxCells) {
    if (!box.valid()) return false;
    resolution = std::max(resolution, 4);
    const glm::vec3 ext = glm::max(box.extent(), glm::vec3(1e-6f));
    const float cell = std::max({ext.x, ext.y, ext.z}) / float(resolution);
    glm::ivec3 dims = glm::ivec3(glm::ceil(ext / cell)) + 1 + 2 * padding;
    if (double(dims.x) * dims.y * dims.z > double(maxCells)) return false;
    grid.dims = dims;
    grid.cell = cell;
    grid.origin = box.center() - glm::vec3(dims - 1) * 0.5f * cell;
    grid.values.assign(grid.size(), 0.0f);
    return true;
}

std::vector<uint8_t> solidifyShell(const VoxelGrid& shell, float threshold, int closeRadius, bool keepAllParts) {
    const glm::ivec3 dims = shell.dims;
    const size_t n = shell.size();
    std::vector<uint8_t> wall(n);
    for (size_t i = 0; i < n; ++i) wall[i] = shell.values[i] > threshold ? 1 : 0;
    morph(wall, dims, closeRadius, true);

    // Flood the empty space from the grid border; whatever it can't reach is inside.
    std::vector<uint8_t> outside(n, 0);
    std::vector<uint32_t> stack;
    for (int z = 0; z < dims.z; ++z)
        for (int y = 0; y < dims.y; ++y)
            for (int x = 0; x < dims.x; ++x) {
                const bool border = x == 0 || y == 0 || z == 0 || x == dims.x - 1 || y == dims.y - 1 || z == dims.z - 1;
                const size_t i = shell.index(x, y, z);
                if (border && !wall[i] && !outside[i]) {
                    outside[i] = 1;
                    stack.push_back(uint32_t(i));
                }
            }
    floodFill(wall, 0, dims, stack, outside);

    std::vector<uint8_t> solid(n);
    for (size_t i = 0; i < n; ++i) solid[i] = outside[i] ? 0 : 1;
    morph(solid, dims, closeRadius, false); // undo the gap-closing dilation

    if (!keepAllParts) {
        // Keep the largest connected piece (captures are full of floaters).
        std::vector<uint32_t> label(n, 0);
        const int64_t sy = dims.x, sz = int64_t(dims.x) * dims.y;
        uint32_t current = 0, bestLabel = 0;
        size_t bestCount = 0;
        for (size_t seed = 0; seed < n; ++seed) {
            if (!solid[seed] || label[seed]) continue;
            label[seed] = ++current;
            stack.assign(1, uint32_t(seed));
            size_t count = 0;
            while (!stack.empty()) {
                const uint32_t i = stack.back();
                stack.pop_back();
                ++count;
                const int x = int(i % uint32_t(dims.x));
                const int y = int((i / uint32_t(dims.x)) % uint32_t(dims.y));
                const int z = int(i / uint32_t(sz));
                const int64_t offsets[6] = {-1, 1, -sy, sy, -sz, sz};
                const bool valid[6] = {x > 0, x + 1 < dims.x, y > 0, y + 1 < dims.y, z > 0, z + 1 < dims.z};
                for (int k = 0; k < 6; ++k) {
                    if (!valid[k]) continue;
                    const uint32_t j = uint32_t(int64_t(i) + offsets[k]);
                    if (solid[j] && !label[j]) {
                        label[j] = current;
                        stack.push_back(j);
                    }
                }
            }
            if (count > bestCount) {
                bestCount = count;
                bestLabel = current;
            }
        }
        for (size_t i = 0; i < n; ++i) solid[i] = label[i] == bestLabel && bestLabel ? 1 : 0;
    }
    return solid;
}

Mesh meshFromSolid(const VoxelGrid& grid, const std::vector<uint8_t>& solid, int smoothIterations) {
    std::vector<float> f(solid.begin(), solid.end());
    for (int axis = 0; axis < 3; ++axis) blurAxis(f, grid.dims, axis);
    return meshFromField(grid, f, 0.5f, smoothIterations);
}

Mesh meshFromField(const VoxelGrid& grid, const std::vector<float>& f, float iso, int smoothIterations) {
    const glm::ivec3 dims = grid.dims;

    // One vertex per cell that the surface passes through (surface nets).
    const glm::ivec3 cells = dims - 1;
    std::vector<int32_t> cellVertex(size_t(cells.x) * size_t(cells.y) * size_t(cells.z), -1);
    auto cellIndex = [&](int x, int y, int z) { return (size_t(z) * size_t(cells.y) + size_t(y)) * size_t(cells.x) + size_t(x); };
    Mesh m;
    for (int z = 0; z < cells.z; ++z)
        for (int y = 0; y < cells.y; ++y)
            for (int x = 0; x < cells.x; ++x) {
                std::array<float, 8> c;
                int inside = 0;
                for (int k = 0; k < 8; ++k) {
                    c[size_t(k)] = f[grid.index(x + (k & 1), y + ((k >> 1) & 1), z + ((k >> 2) & 1))];
                    inside += c[size_t(k)] > iso;
                }
                if (inside == 0 || inside == 8) continue;
                glm::vec3 sum(0.0f);
                int crossings = 0;
                for (int a = 0; a < 8; ++a)
                    for (int bit = 1; bit < 8; bit <<= 1) {
                        if (a & bit) continue;
                        const int b = a | bit;
                        const float fa = c[size_t(a)], fb = c[size_t(b)];
                        if ((fa > iso) == (fb > iso)) continue;
                        const float t = (iso - fa) / (fb - fa);
                        const glm::vec3 pa(a & 1, (a >> 1) & 1, (a >> 2) & 1);
                        const glm::vec3 pb(b & 1, (b >> 1) & 1, (b >> 2) & 1);
                        sum += glm::mix(pa, pb, t);
                        ++crossings;
                    }
                cellVertex[cellIndex(x, y, z)] = int32_t(m.positions.size());
                m.positions.push_back(grid.position(x, y, z) + sum / float(crossings) * grid.cell);
            }

    // One quad per grid edge with a sign change, joining the four cells around it.
    for (int z = 0; z < dims.z; ++z)
        for (int y = 0; y < dims.y; ++y)
            for (int x = 0; x < dims.x; ++x) {
                const glm::ivec3 p(x, y, z);
                const bool in0 = f[grid.index(x, y, z)] > iso;
                for (int axis = 0; axis < 3; ++axis) {
                    glm::ivec3 q = p;
                    q[axis] += 1;
                    if (q[axis] >= dims[axis]) continue;
                    const bool in1 = f[grid.index(q.x, q.y, q.z)] > iso;
                    if (in0 == in1) continue;
                    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
                    if (p[u] < 1 || p[v] < 1 || p[u] > cells[u] - 1 || p[v] > cells[v] - 1 || p[axis] > cells[axis] - 1)
                        continue;
                    std::array<int32_t, 4> quad;
                    const int du[4] = {-1, 0, 0, -1}, dv[4] = {-1, -1, 0, 0};
                    bool ok = true;
                    for (int k = 0; k < 4; ++k) {
                        glm::ivec3 c = p;
                        c[u] += du[k];
                        c[v] += dv[k];
                        quad[size_t(k)] = cellVertex[cellIndex(c.x, c.y, c.z)];
                        ok &= quad[size_t(k)] >= 0;
                    }
                    if (!ok) continue;
                    if (!in0) std::swap(quad[1], quad[3]); // outward normal must point to the outside sample
                    const auto& P = m.positions;
                    const bool diag02 = glm::distance(P[size_t(quad[0])], P[size_t(quad[2])]) <=
                                        glm::distance(P[size_t(quad[1])], P[size_t(quad[3])]);
                    const glm::uvec4 qq(quad[0], quad[1], quad[2], quad[3]);
                    if (diag02) {
                        m.triangles.push_back({qq.x, qq.y, qq.z});
                        m.triangles.push_back({qq.x, qq.z, qq.w});
                    } else {
                        m.triangles.push_back({qq.x, qq.y, qq.w});
                        m.triangles.push_back({qq.y, qq.z, qq.w});
                    }
                    // Surface nets makes quads: keep each one (Mesh::quadPartners), like ZBrush's DynaMesh.
                    m.pairQuad(uint32_t(m.triangles.size() - 2), uint32_t(m.triangles.size() - 1));
                }
            }
    m.rebuildTopology();
    taubinSmooth(m, smoothIterations);
    m.recomputeAllNormals();
    return m;
}

void rasterizeMeshShell(const Mesh& mesh, VoxelGrid& grid) {
    const auto& P = mesh.positions;
    for (const auto& t : mesh.triangles) {
        const glm::vec3 a = (P[t.x] - grid.origin) / grid.cell;
        const glm::vec3 b = (P[t.y] - grid.origin) / grid.cell;
        const glm::vec3 c = (P[t.z] - grid.origin) / grid.cell;
        const float longest = std::max({glm::distance(a, b), glm::distance(b, c), glm::distance(c, a)});
        const int steps = std::max(1, int(std::ceil(longest * 2.0f)));
        for (int i = 0; i <= steps; ++i)
            for (int j = 0; i + j <= steps; ++j) {
                const float u = float(i) / float(steps), v = float(j) / float(steps);
                const glm::ivec3 s(glm::round(a + (b - a) * u + (c - a) * v));
                if (glm::any(glm::lessThan(s, glm::ivec3(0))) || glm::any(glm::greaterThanEqual(s, grid.dims))) continue;
                grid.values[grid.index(s.x, s.y, s.z)] = 1.0f;
            }
    }
}

std::vector<float> insideDistanceField(const Mesh& mesh, const VoxelGrid& grid, const std::vector<uint8_t>& solid, float band) {
    // Exact distance (in cells) to the nearest triangle for every sample within `band` cells of the surface, found by
    // visiting the samples around each triangle. z-slabs run on separate threads, so no two threads write a sample.
    const glm::ivec3 dims = grid.dims;
    const size_t n = grid.size();
    std::vector<float> dist2(n, band * band);
    std::vector<int32_t> nearest(n, -1);
    const auto& P = mesh.positions;
    const float inv = 1.0f / grid.cell;
    auto work = [&](int z0, int z1) {
        for (uint32_t t = 0; t < mesh.triangleCount(); ++t) {
            const glm::uvec3 tri = mesh.triangles[t];
            const glm::vec3 a = (P[tri.x] - grid.origin) * inv, b = (P[tri.y] - grid.origin) * inv,
                            c = (P[tri.z] - grid.origin) * inv;
            const glm::ivec3 lo = glm::max(glm::ivec3(glm::floor(glm::min(a, glm::min(b, c)) - band)), glm::ivec3(0));
            const glm::ivec3 hi = glm::min(glm::ivec3(glm::ceil(glm::max(a, glm::max(b, c)) + band)), dims - 1);
            const int za = std::max(lo.z, z0), zb = std::min(hi.z, z1 - 1);
            for (int z = za; z <= zb; ++z)
                for (int y = lo.y; y <= hi.y; ++y)
                    for (int x = lo.x; x <= hi.x; ++x) {
                        const glm::vec3 p(x, y, z);
                        const glm::vec3 d = p - closestOnTriangle(p, a, b, c);
                        const float d2 = glm::dot(d, d);
                        const size_t i = grid.index(x, y, z);
                        if (d2 < dist2[i]) {
                            dist2[i] = d2;
                            nearest[i] = int32_t(t);
                        }
                    }
        }
    };
    const int threads = int(std::clamp(std::thread::hardware_concurrency(), 1u, 16u));
    const int slab = (dims.z + threads - 1) / threads;
    std::vector<std::thread> pool;
    for (int k = 1; k < threads; ++k)
        if (k * slab < dims.z) pool.emplace_back(work, k * slab, std::min(dims.z, (k + 1) * slab));
    work(0, std::min(dims.z, slab));
    for (auto& th : pool) th.join();

    // Sign: the flood fill is certain about the outside and about the deep inside; next to the surface the
    // nearest triangle's side decides, which keeps sub-voxel detail instead of voxel stair-steps.
    std::vector<uint8_t> nearOutside(n, 0);
    for (int z = 0; z < dims.z; ++z)
        for (int y = 0; y < dims.y; ++y)
            for (int x = 0; x < dims.x; ++x) {
                if (solid[grid.index(x, y, z)]) continue;
                for (int dz = -2; dz <= 2; ++dz)
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx) {
                            const glm::ivec3 q(x + dx, y + dy, z + dz);
                            if (glm::all(glm::greaterThanEqual(q, glm::ivec3(0))) && glm::all(glm::lessThan(q, dims)))
                                nearOutside[grid.index(q.x, q.y, q.z)] = 1;
                        }
            }
    std::vector<uint8_t> inside(n);
    for (int z = 0; z < dims.z; ++z)
        for (int y = 0; y < dims.y; ++y)
            for (int x = 0; x < dims.x; ++x) {
                const size_t i = grid.index(x, y, z);
                if (!solid[i]) inside[i] = 0;
                else if (!nearOutside[i] || nearest[i] < 0) inside[i] = 1;
                else {
                    const glm::uvec3 tri = mesh.triangles[size_t(nearest[i])];
                    const glm::vec3 nrm = glm::cross(P[tri.y] - P[tri.x], P[tri.z] - P[tri.x]);
                    const glm::vec3 p = grid.position(x, y, z);
                    const glm::vec3 onSurface = closestOnTriangle(p, P[tri.x], P[tri.y], P[tri.z]);
                    // A surface buried inside another part (solid just outside it too) doesn't bound the solid.
                    const float len = glm::length(nrm);
                    const glm::ivec3 probe(glm::round((onSurface + nrm * (2.5f * grid.cell / std::max(len, 1e-20f)) -
                                                       grid.origin) / grid.cell));
                    const bool buried = len > 0.0f && glm::all(glm::greaterThanEqual(probe, glm::ivec3(0))) &&
                                        glm::all(glm::lessThan(probe, dims)) && solid[grid.index(probe.x, probe.y, probe.z)];
                    inside[i] = buried || glm::dot(p - onSurface, nrm) < 0.0f;
                }
            }
    // Surfaces buried inside the solid (overlapping parts) make small "outside" pockets: anything outside that
    // can't reach the real outside is inside, so merged parts stay one closed surface.
    std::vector<uint8_t> reached(n, 0);
    std::vector<uint32_t> stack;
    for (size_t i = 0; i < n; ++i)
        if (!solid[i]) {
            reached[i] = 1;
            stack.push_back(uint32_t(i));
        }
    floodFill(inside, 0, dims, stack, reached);
    std::vector<float> f(n);
    for (size_t i = 0; i < n; ++i) {
        const float d = std::sqrt(dist2[i]);
        f[i] = inside[i] || !reached[i] ? d : -d;
    }
    return f;
}

Mesh voxelRemesh(const Mesh& mesh, int resolution, std::string* error) {
    VoxelGrid grid;
    if (mesh.triangleCount() == 0 || !makeVoxelGrid(mesh.bounds(), resolution, 4, grid)) {
        if (error) *error = mesh.triangleCount() == 0 ? "The mesh is empty" : "Resolution too high for this mesh";
        return {};
    }
    rasterizeMeshShell(mesh, grid);
    std::vector<uint8_t> solid = solidifyShell(grid, 0.5f, 1, true);
    // Mesh the signed distance to the original surface (positive inside), not the blocky occupancy: the result
    // lands on the original surface with no voxel terracing.
    Mesh out = meshFromField(grid, insideDistanceField(mesh, grid, solid, 3.0f), 0.0f, 2);
    if (out.triangleCount() == 0 && error) *error = "Remesh produced no surface";
    // Mask and polypaint follow the nearest original vertex; polygroups, hidden
    // flags and UVs don't survive a rebuild (everything becomes visible group 0).
    // The original's quads don't either: the new surface is all quads of its own.
    transferVertexAttributes(mesh, out);
    return out;
}

} // namespace sf
