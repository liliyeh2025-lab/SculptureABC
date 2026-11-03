// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How a renderer lays a mesh out on the GPU, and which parts of it a change needs re-sent.
//
// The triangles are cut into chunks of kChunkTriangles consecutive triangles (a compact bit of surface on any
// mesh whose triangle order is not random: a divided mesh keeps each cage face's triangles together at every
// level, imports and remeshes come out in scan order). The chunks are sorted along a Morton curve of their
// centres, and every kClusterChunks consecutive chunks make a cluster: a compact patch of up to
// kClusterTriangles triangles even when the cage faces are small (a remesh divided three times). The renderer
// culls whole clusters each frame (outside the view, or all facing away) and draws the rest with one
// multi-draw. Every cluster has its own contiguous range of GPU vertices (block aligned), numbered by first
// use in its triangles, so its indices fit in 16 bits relative to the range's start. Vertices on the seams
// between clusters have a copy in each cluster that uses them (about a tenth of the vertices). The mesh's own
// numbering is untouched.
//
// Subdivided meshes number their vertices level by level, so the vertices under one brush dab are scattered
// over the whole index range; in this layout they sit in a few clusters. dirtyRuns() turns the vertices the
// mesh marked dirty (Mesh::markDirty) into a few short GPU index ranges (every copy included) to gather and
// upload.
#pragma once

#include "core/Mesh.h"

#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace sf {

class UploadOrder {
public:
    static constexpr uint32_t kBlockShift = 7, kBlock = 1u << kBlockShift; // GPU upload granularity: 128 vertices
    static constexpr uint32_t kChunkShift = 6, kChunkTriangles = 1u << kChunkShift; // 64 triangles
    static constexpr uint32_t kClusterChunks = 64, kClusterTriangles = kChunkTriangles * kClusterChunks; // 4096
    static constexpr uint32_t kNone = ~0u;

    struct Cluster {
        uint32_t firstVertex = 0; // GPU vertex range [firstVertex, firstVertex + vertexCount), block aligned
        uint32_t vertexCount = 0;
        uint32_t firstTriangle = 0; // position of its first triangle in the clusters' triangle order
        uint32_t triangleCount = 0;
    };

    // Lays out `mesh` (its triangles and vertex count, and positions for the chunk order). `onCluster`, when
    // given, gets each cluster's localTriangles as they are made (in parallel, from any thread), which saves
    // a renderer a second pass to make them.
    using ClusterCallback = std::function<void(uint32_t cluster, const uint16_t* localTriangles)>;
    void build(const Mesh& mesh, const ClusterCallback& onCluster = {});
    // Every directed edge a->b of the mesh has as many b->a (found while building, from a 64-bit hash sum, so a
    // false yes is a 2^-64 chance): the mesh is closed and consistently wound.
    bool closed() const { return closed_; }
    // The number of clusters build() makes for `triangles` triangles.
    static size_t clusterCount(size_t triangles) {
        const size_t chunks = (triangles + kChunkTriangles - 1) >> kChunkShift;
        return (chunks + kClusterChunks - 1) / kClusterChunks;
    }
    // GPU vertex slots, including the padding that block-aligns each cluster.
    size_t size() const { return fromGpu_.size(); }
    // fromGpu()[i] = the mesh vertex GPU slot i holds (padding repeats a vertex of its cluster).
    std::span<const uint32_t> fromGpu() const { return fromGpu_; }
    // The first GPU slot holding mesh vertex v (kNone for vertices no triangle uses).
    uint32_t toGpu(uint32_t v) const { return toGpu_[v]; }
    // v has a copy in more than one cluster (it is on a seam between clusters).
    bool shared(uint32_t v) const { return v < vertices_ && ((hasCopy_[v >> 6] >> (v & 63)) & 1); }
    // Every GPU slot holding v, in increasing order (one for most vertices).
    void slotsOf(uint32_t v, std::vector<uint32_t>& out) const;

    // The clusters, in order; their triangles follow each other in the "clusters' triangle order" (the order
    // a renderer keeps per-triangle data in).
    std::span<const Cluster> clusters() const { return clusters_; }
    size_t triangleCount() const { return triangles_; }
    uint32_t firstTriangle(uint32_t c) const { return clusters_[c].firstTriangle; }
    uint32_t clusterTriangles(uint32_t c) const { return clusters_[c].triangleCount; }
    // The mesh triangle at position i of cluster c (i < clusterTriangles(c)).
    uint32_t triangle(uint32_t c, uint32_t i) const {
        return (chunks_[size_t(c) * kClusterChunks + (i >> kChunkShift)] << kChunkShift) + (i & (kChunkTriangles - 1));
    }
    // The cluster holding GPU slot i.
    uint32_t clusterOfSlot(uint32_t i) const;
    // The triangles of cluster c as indices into its vertex range (3 per triangle; a triangle with an index
    // outside the mesh becomes 0, 0, 0). `mesh` must be the one built from. Thread safe.
    void localTriangles(const Mesh& mesh, uint32_t c, uint16_t* out) const;

    // Sorted, disjoint GPU index ranges [begin, end) holding every slot of every vertex marked dirty, in whole
    // blocks (runs a few blocks apart are joined). Everything, as one range, when most of the mesh is dirty.
    void dirtyRuns(const Mesh& mesh, std::vector<std::pair<uint32_t, uint32_t>>& out);

    size_t memoryBytes() const;

private:
    size_t triangles_ = 0, vertices_ = 0;
    bool closed_ = false;
    std::vector<Cluster> clusters_;
    std::vector<uint32_t> chunks_; // chunk ids in cluster order (the mesh's partial last chunk, if any, last)
    std::vector<uint32_t> toGpu_, fromGpu_;
    // The copies: (mesh vertex, slot) for every slot but a vertex's first, sorted; hasCopy_ has a bit per vertex.
    std::vector<std::pair<uint32_t, uint32_t>> copies_;
    std::vector<uint64_t> hasCopy_;
    std::vector<uint8_t> marked_;  // per GPU block, while dirtyRuns collects (all zero between calls)
    std::vector<uint32_t> blocks_; // the marked blocks
};

} // namespace sf
