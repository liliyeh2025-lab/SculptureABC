// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// IndexBits: a set of vertex (or triangle) indices as one bit each, for the per-dab brush passes (BVH vertex
// queries, normal updates). Threads add indices with an atomic OR; take() then lists the members in increasing
// index order and empties the set. The order doesn't depend on which thread added what, so results stay the
// same with any thread count, and passes over the list walk the mesh's arrays front to back instead of
// jumping around (the indices a dab touches lie in a few dozen runs).
//
// Threads add with insert() (atomic), or each into a set of its own with insertParallel(), which is faster when
// many threads add a lot.
//
// A summary bit per kChunk indices marks the chunks holding members, so take() only visits those: its cost
// follows the size of the set, not of the mesh.
#pragma once

#include "core/AtomicRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace sf {

class IndexBits {
public:
    static constexpr uint32_t kChunkWords = 64, kChunk = kChunkWords * 64; // 4096 indices per summary bit

    // Holds indices [0, n); empties the set.
    void resize(size_t n);
    size_t capacity() const { return words_.size() * 64; }
    bool empty() const;

    bool contains(uint32_t i) const {
        return (AtomicRef<uint64_t>(const_cast<uint64_t&>(words_[i >> 6])).load(std::memory_order_relaxed) >> (i & 63)) & 1;
    }
    // Adds `i` (safe to call from several threads at once); returns true if it wasn't a member yet.
    bool insert(uint32_t i) {
        const uint64_t bit = uint64_t(1) << (i & 63);
        AtomicRef<uint64_t> w(words_[i >> 6]);
        if (w.load(std::memory_order_relaxed) & bit) return false;
        const uint64_t old = w.fetch_or(bit, std::memory_order_relaxed);
        if (old & bit) return false;
        if (old == 0) {
            const uint32_t c = i / kChunk;
            const uint64_t sbit = uint64_t(1) << (c & 63);
            AtomicRef<uint64_t> s(summary_[c >> 6]);
            if (!(s.load(std::memory_order_relaxed) & sbit)) s.fetch_or(sbit, std::memory_order_relaxed);
        }
        return true;
    }

    // insert() for a set only one thread writes (no atomics).
    bool insertLocal(uint32_t i) {
        uint64_t& w = words_[i >> 6];
        const uint64_t bit = uint64_t(1) << (i & 63);
        if (w & bit) return false;
        if (w == 0) summary_[(i / kChunk) >> 6] |= uint64_t(1) << ((i / kChunk) & 63);
        w |= bit;
        return true;
    }
    // Calls add(begin, end, set) for the blocks [b * block, (b + 1) * block) of [0, n) on the shared pool; `add`
    // marks members with set.insertLocal(). Each job adds to a scratch set of its own (no atomics, and no cache
    // lines bouncing between cores), merged into this one at the end. Small or serial runs add here directly,
    // as do runs while another thread is using the scratch sets (shared by every IndexBits, a bit per index of
    // the largest one, per thread).
    void insertParallel(size_t n, size_t block, const std::function<void(size_t, size_t, IndexBits&)>& add);
    // Moves every member of `parts` (each as big as this set) into this one, in parallel by chunk.
    void absorb(std::span<IndexBits> parts);

    // Appends the members to `out` in increasing order and empties the set (in parallel on the shared pool
    // for big sets). `perWord(wordIndex, bits)`, if given, sees every non-zero word once (from any thread, but
    // each 64-word chunk on one thread), e.g. to OR the set into another bitmap of the same layout.
    void take(std::vector<uint32_t>& out, const std::function<void(size_t, uint64_t)>& perWord = {});

    size_t memoryBytes() const { return (words_.capacity() + summary_.capacity()) * sizeof(uint64_t); }

private:
    void chunks(std::vector<uint32_t>& out) const;

    std::vector<uint64_t> words_, summary_;
    std::vector<uint32_t> chunks_, counts_;
};

} // namespace sf
