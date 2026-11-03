// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ImageTo3D.h"

#include "core/Primitives.h"
#include "core/Sculpt.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace sf {

namespace {

float heightAt(const Image& img, float u, float v, bool invert) {
    float h = img.luminance(u, v);
    if (invert) h = 1.0f - h;
    return h * img.alpha(u, v);
}

} // namespace

Mesh makeImageRelief(const Image& image, const ReliefOptions& o) {
    Mesh m;
    if (image.empty()) return m;
    const int res = std::clamp(o.resolution, 8, 2048);
    const float aspect = image.aspect();
    const int nx = aspect >= 1.0f ? res : std::max(2, int(std::lround(res * aspect)));
    const int ny = aspect >= 1.0f ? std::max(2, int(std::lround(res / aspect))) : res;
    const float width = 2.0f, height = 2.0f / aspect;
    const float depth = o.depth * width;

    m.positions.reserve(size_t(nx + 1) * size_t(ny + 1));
    for (int j = 0; j <= ny; ++j) {
        for (int i = 0; i <= nx; ++i) {
            float u = float(i) / float(nx), v = float(j) / float(ny);
            m.positions.emplace_back((u - 0.5f) * width, (0.5f - v) * height, heightAt(image, u, v, o.invert) * depth);
        }
    }
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            uint32_t a = uint32_t(j * (nx + 1) + i), b = a + 1, c = a + uint32_t(nx + 1), d = c + 1;
            m.triangles.push_back({a, c, b});
            m.triangles.push_back({b, c, d});
        }
    }
    m.rebuildTopology();
    m.recomputeAllNormals();
    return m;
}

size_t projectImage(Mesh& mesh, int layer, const Image& image, const glm::mat4& viewProj, float viewAspect,
                    const glm::vec3& cameraPos, const ProjectOptions& o) {
    if (image.empty() || mesh.vertexCount() == 0) return 0;
    // Fit the image inside the view ("contain"), scaled by coverage.
    const float ia = image.aspect();
    float sx = 1.0f, sy = 1.0f;
    if (ia > viewAspect) sy = viewAspect / ia;
    else sx = ia / viewAspect;
    sx *= o.coverage;
    sy *= o.coverage;

    auto& target = mesh.layerTarget(layer);
    const float invWeight = 1.0f / mesh.layerTargetWeight(layer);
    const bool layered = !mesh.layers.empty();
    std::vector<uint32_t> moved;
    for (uint32_t v = 0; v < mesh.vertexCount(); ++v) {
        const glm::vec3 p = mesh.positions[v];
        glm::vec4 clip = viewProj * glm::vec4(p, 1.0f);
        if (clip.w <= 0.0f) continue;
        glm::vec2 ndc = glm::vec2(clip) / clip.w;
        float u = (ndc.x / sx + 1.0f) * 0.5f;
        float t = 1.0f - (ndc.y / sy + 1.0f) * 0.5f;
        if (u < 0.0f || u > 1.0f || t < 0.0f || t > 1.0f) continue;
        float facing = glm::dot(mesh.normals[v], glm::normalize(cameraPos - p));
        float f = std::clamp((facing - 0.05f) / 0.25f, 0.0f, 1.0f); // fade at grazing angles
        float h = heightAt(image, u, t, o.invert) * f * vertexInfluence(mesh, v); // respects mask/hidden like brushes
        if (h <= 0.0f) continue;
        glm::vec3 d = mesh.normals[v] * (o.depth * h);
        mesh.positions[v] += d;
        if (layered) target[v] += d * invWeight;
        moved.push_back(v);
    }
    mesh.updateNormals(moved);
    return moved.size();
}

Mesh inflateSilhouette(const Image& image, const InflateOptions& o, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return Mesh{};
    };
    if (image.empty()) return fail("No image");

    const int res = std::clamp(o.resolution, 16, 512);
    const float aspect = image.aspect();
    const int gw = aspect >= 1.0f ? res : std::max(4, int(std::lround(res * aspect)));
    const int gh = aspect >= 1.0f ? std::max(4, int(std::lround(res / aspect))) : res;
    // Grid with a one-cell empty border so every shape has an outside.
    const int W = gw + 2, H = gh + 2;
    auto idx = [W](int x, int y) { return size_t(y) * size_t(W) + size_t(x); };

    // 1. Mask: opaque pixels, or pixels that differ from the border colour.
    std::vector<uint8_t> inside(size_t(W) * size_t(H), 0);
    const bool useAlpha = image.hasTransparency();
    float border = 0.0f;
    if (!useAlpha) {
        int n = 0;
        for (int k = 0; k < 64; ++k) {
            float s = float(k) / 63.0f;
            border += image.luminance(s, 0.0f) + image.luminance(s, 1.0f) + image.luminance(0.0f, s) +
                      image.luminance(1.0f, s);
            n += 4;
        }
        border /= float(n);
    }
    for (int y = 0; y < gh; ++y) {
        for (int x = 0; x < gw; ++x) {
            float u = (float(x) + 0.5f) / float(gw), v = (float(y) + 0.5f) / float(gh);
            bool in = useAlpha ? image.alpha(u, v) > 0.5f : std::abs(image.luminance(u, v) - border) > 0.2f;
            inside[idx(x + 1, y + 1)] = in;
        }
    }

    // 2. Drop specks: keep connected components at least 5% the size of the largest.
    std::vector<int> label(inside.size(), -1);
    std::vector<size_t> sizes;
    std::vector<size_t> stack;
    for (size_t start = 0; start < inside.size(); ++start) {
        if (!inside[start] || label[start] >= 0) continue;
        int l = int(sizes.size());
        size_t count = 0;
        stack.assign(1, start);
        label[start] = l;
        while (!stack.empty()) {
            size_t c = stack.back();
            stack.pop_back();
            ++count;
            int cx = int(c % size_t(W)), cy = int(c / size_t(W));
            const int nx[4] = {cx + 1, cx - 1, cx, cx}, ny[4] = {cy, cy, cy + 1, cy - 1};
            for (int k = 0; k < 4; ++k) {
                size_t n = idx(nx[k], ny[k]);
                if (inside[n] && label[n] < 0) {
                    label[n] = l;
                    stack.push_back(n);
                }
            }
        }
        sizes.push_back(count);
    }
    if (sizes.empty()) return fail("No shape found - use an image with a transparent background or a plain backdrop");
    const size_t largest = *std::max_element(sizes.begin(), sizes.end());
    for (size_t c = 0; c < inside.size(); ++c)
        if (inside[c] && sizes[size_t(label[c])] < std::max<size_t>(4, largest / 20)) inside[c] = 0;

    // 3. Quads between four inside cells form the sheet. Iteratively remove
    //    quads that would make the closed surface degenerate: quads whose four
    //    corners are all on the rim (front and back would coincide) and
    //    "bow-tie" corners where two quads touch only diagonally.
    std::vector<uint8_t> quad(inside.size(), 0); // quad anchored at its top-left cell
    for (int y = 1; y < H - 2; ++y)
        for (int x = 1; x < W - 2; ++x)
            quad[idx(x, y)] = inside[idx(x, y)] && inside[idx(x + 1, y)] && inside[idx(x, y + 1)] &&
                              inside[idx(x + 1, y + 1)];
    auto q = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && quad[idx(x, y)]; };
    auto incident = [&](int x, int y) { return int(q(x - 1, y - 1)) + q(x, y - 1) + q(x - 1, y) + q(x, y); };
    std::vector<uint8_t> rim(inside.size(), 0);
    for (bool changed = true; changed;) {
        changed = false;
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x) {
                // Bow-tie: only the two diagonal quads around this corner exist.
                bool d1 = q(x - 1, y - 1) && q(x, y) && !q(x, y - 1) && !q(x - 1, y);
                bool d2 = q(x, y - 1) && q(x - 1, y) && !q(x - 1, y - 1) && !q(x, y);
                if (d1) { quad[idx(x, y)] = 0; changed = true; }
                if (d2) { quad[idx(x - 1, y)] = 0; changed = true; }
            }
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x) {
                int n = incident(x, y);
                rim[idx(x, y)] = n > 0 && n < 4;
            }
        for (int y = 1; y < H - 2; ++y)
            for (int x = 1; x < W - 2; ++x)
                if (quad[idx(x, y)] && rim[idx(x, y)] && rim[idx(x + 1, y)] && rim[idx(x, y + 1)] &&
                    rim[idx(x + 1, y + 1)]) {
                    quad[idx(x, y)] = 0;
                    changed = true;
                }
    }

    // 4. Distance from each sheet vertex to the rim.
    std::vector<glm::vec2> contour;
    for (int y = 1; y < H - 1; ++y)
        for (int x = 1; x < W - 1; ++x)
            if (rim[idx(x, y)]) contour.emplace_back(float(x), float(y));
    std::vector<float> dist(inside.size(), 0.0f);
    float maxDist = 0.0f;
    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            if (rim[idx(x, y)] || incident(x, y) == 0) continue;
            float best = 1e30f;
            for (const auto& c : contour) {
                float dx = c.x - float(x), dy = c.y - float(y);
                best = std::min(best, dx * dx + dy * dy);
            }
            dist[idx(x, y)] = std::sqrt(best);
            maxDist = std::max(maxDist, dist[idx(x, y)]);
        }
    }
    if (maxDist <= 0.0f) return fail("The shape is too thin to inflate - try a bolder silhouette");

    // 4b. Heights from a circular profile, then relaxed so the creases along
    //     the shape's medial axis round off (rim stays pinned at zero).
    auto profile = [&](float d) { return std::sqrt(std::max(0.0f, 2.0f * maxDist * d - d * d)); };
    std::vector<float> height(inside.size(), 0.0f);
    for (size_t c = 0; c < inside.size(); ++c) height[c] = dist[c] > 0.0f ? profile(dist[c]) : 0.0f;
    {
        std::vector<float> next = height;
        const int iterations = std::max(4, int(maxDist * 0.6f));
        for (int it = 0; it < iterations; ++it) {
            for (int y = 1; y < H - 1; ++y)
                for (int x = 1; x < W - 1; ++x) {
                    size_t c = idx(x, y);
                    if (dist[c] <= 0.0f) continue;
                    float sum = 0.0f;
                    int n = 0;
                    const int nx[4] = {x + 1, x - 1, x, x}, ny[4] = {y, y, y + 1, y - 1};
                    for (int k = 0; k < 4; ++k) {
                        size_t nc = idx(nx[k], ny[k]);
                        if (dist[nc] > 0.0f || rim[nc]) {
                            sum += height[nc];
                            ++n;
                        }
                    }
                    // Blend toward the neighbour average but never below the
                    // profile's rounded rim, so edges keep their fullness.
                    if (n) next[c] = std::max(0.5f * height[c] + 0.5f * sum / float(n), profile(std::min(dist[c], 2.0f)));
                }
            height.swap(next);
        }
    }

    // 5. Front sheet over the quads. A circular cross-section profile gives
    //    a rounded, "balloon" body: h = sqrt(2Rd - d^2).
    struct SheetVertex {
        glm::vec2 xy; // grid units
        float h;
        bool rim;
        float d = 0.0f; // distance to the rim in cells
    };
    std::vector<SheetVertex> sheet;
    std::vector<glm::uvec3> tris;
    constexpr uint32_t kNone = ~0u;
    std::vector<uint32_t> slot(inside.size(), kNone);
    auto vertexFor = [&](int x, int y) {
        size_t c = idx(x, y);
        if (slot[c] == kNone) {
            slot[c] = uint32_t(sheet.size());
            sheet.push_back({glm::vec2(float(x), float(y)), rim[c] ? 0.0f : height[c], rim[c] != 0, dist[c]});
        }
        return slot[c];
    };
    for (int y = 1; y < H - 2; ++y) {
        for (int x = 1; x < W - 2; ++x) {
            if (!quad[idx(x, y)]) continue;
            uint32_t a = vertexFor(x, y), b = vertexFor(x + 1, y), c = vertexFor(x, y + 1), d = vertexFor(x + 1, y + 1);
            tris.push_back({a, c, b});
            tris.push_back({b, c, d});
        }
    }
    if (tris.empty()) return fail("The shape is too thin to inflate - try a bolder silhouette");

    // 6. Split "chords": interior edges whose ends are both on the rim. Front
    //    and back would otherwise share them and pinch the surface.
    {
        std::unordered_map<uint64_t, int> edgeUse;
        auto key = [](uint32_t a, uint32_t b) { return a < b ? (uint64_t(a) << 32 | b) : (uint64_t(b) << 32 | a); };
        for (const auto& t : tris)
            for (int k = 0; k < 3; ++k) ++edgeUse[key(t[k], t[(k + 1) % 3])];
        std::vector<std::pair<uint32_t, uint32_t>> chords;
        for (const auto& [k, n] : edgeUse) {
            uint32_t a = uint32_t(k >> 32), b = uint32_t(k & 0xffffffffu);
            if (n == 2 && sheet[a].rim && sheet[b].rim) chords.emplace_back(a, b);
        }
        for (const auto& [a, b] : chords) {
            const uint32_t mid = uint32_t(sheet.size());
            sheet.push_back({(sheet[a].xy + sheet[b].xy) * 0.5f, profile(0.5f), false, 0.5f});
            const size_t count = tris.size();
            for (size_t i = 0; i < count; ++i) {
                glm::uvec3 t = tris[i];
                for (int k = 0; k < 3; ++k) {
                    uint32_t p = t[k], qv = t[(k + 1) % 3], o = t[(k + 2) % 3];
                    if ((p == a && qv == b) || (p == b && qv == a)) {
                        tris[i] = {p, mid, o};
                        tris.push_back({mid, qv, o});
                        break;
                    }
                }
            }
        }
    }

    // 7. Mirror into a back sheet that shares the rim, giving a closed surface.
    const float cell = 2.0f / float(std::max(gw, gh));
    Mesh m;
    std::vector<uint32_t> backIndex(sheet.size());
    for (const auto& v : sheet)
        m.positions.emplace_back((v.xy.x - float(W) * 0.5f) * cell, (float(H) * 0.5f - v.xy.y) * cell,
                                 v.h * cell * o.thickness);
    for (uint32_t i = 0; i < sheet.size(); ++i) {
        if (sheet[i].rim) {
            backIndex[i] = i;
        } else {
            backIndex[i] = uint32_t(m.positions.size());
            glm::vec3 p = m.positions[i];
            m.positions.emplace_back(p.x, p.y, -p.z);
        }
    }
    for (const auto& t : tris) {
        m.triangles.push_back(t);
        m.triangles.push_back({backIndex[t.x], backIndex[t.z], backIndex[t.y]});
    }

    m.rebuildTopology();
    {
        // Rim and near-rim flags per mesh vertex (back copies share the front's flags).
        std::vector<uint8_t> isRim(m.vertexCount(), 0), nearRim(m.vertexCount(), 0);
        for (uint32_t i = 0; i < sheet.size(); ++i) {
            isRim[i] = sheet[i].rim;
            nearRim[i] = nearRim[backIndex[i]] = !sheet[i].rim && sheet[i].d <= 4.0f;
        }
        // Straighten the pixel-stepped outline along itself.
        std::vector<glm::vec3> next = m.positions;
        for (int it = 0; it < 12; ++it) {
            for (uint32_t v = 0; v < m.vertexCount(); ++v) {
                if (!isRim[v]) continue;
                glm::vec3 sum(0.0f);
                int n = 0;
                for (uint32_t u : m.neighbors(v))
                    if (isRim[u]) sum += m.positions[u], ++n;
                if (n >= 2) next[v] = glm::mix(m.positions[v], sum / float(n), 0.5f);
            }
            m.positions = next;
        }
        // Even out the steep band around the rim, rim included, so the seam rounds off.
        for (int it = 0; it < 20; ++it) {
            for (uint32_t v = 0; v < m.vertexCount(); ++v) {
                if (!nearRim[v] && !(isRim[v] && it % 2)) continue;
                glm::vec3 sum(0.0f);
                auto ring = m.neighbors(v);
                for (uint32_t u : ring) sum += m.positions[u];
                next[v] = glm::mix(m.positions[v], sum / float(ring.size()), 0.5f);
            }
            m.positions = next;
        }
    }
    taubinSmooth(m, 12);
    for (int i = 0; i < std::clamp(o.subdivisions, 0, 3); ++i) m = loopSubdivide(m);
    m.rebuildTopology();
    m.recomputeAllNormals();
    return m;
}

} // namespace sf
