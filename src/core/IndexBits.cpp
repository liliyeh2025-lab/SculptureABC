// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/IndexBits.h"

#include "core/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <mutex>

namespace sf {

void IndexBits::resize(size_t n) {
    const size_t chunks = (n + kChunk - 1) / kChunk;
    words_.assign(chunks * kChunkWords, 0);
    summary_.assign((chunks + 63) / 64, 0);
}

bool IndexBits::empty() const {
    for (uint64_t s : summary_)
        if (s) return false;
    return true;
}

void IndexBits::chunks(std::vector<uint32_t>& out) const {
    out.clear();
    for (size_t i = 0; i < summary_.size(); ++i)
        for (uint64_t bits = summary_[i]; bits; bits &= bits - 1) out.push_back(uint32_t(i * 64 + size_t(std::countr_zero(bits))));
}

namespace {
std::mutex scratchMutex;
std::vector<IndexBits> scratchParts;
} // namespace

void IndexBits::insertParallel(size_t n, size_t block, const std::function<void(size_t, size_t, IndexBits&)>& add) {
    block = std::max<size_t>(block, 1);
    const size_t blocks = (n + block - 1) / block;
    const size_t jobs = std::min<size_t>(size_t(ThreadPool::shared().threads()), blocks);
    std::unique_lock lock(scratchMutex, std::defer_lock);
    if (jobs < 2 || blocks < 4 || ThreadPool::shared().serial() || !lock.try_lock()) {
        for (size_t b = 0; b < n; b += block) add(b, std::min(n, b + block), *this);
        return;
    }
    std::vector<IndexBits>& parts = scratchParts;
    if (parts.size() < jobs) parts.resize(jobs);
    for (size_t j = 0; j < jobs; ++j)
        if (parts[j].capacity() < capacity()) parts[j].resize(capacity());
    std::atomic<size_t> next{0};
    poolFor(jobs, [&](size_t jb, size_t je) {
        for (size_t j = jb; j < je; ++j)
            for (size_t b; (b = next.fetch_add(1, std::memory_order_relaxed)) < blocks;)
                add(b * block, std::min(n, (b + 1) * block), parts[j]);
    }, 1, 2);
    absorb(std::span(parts.data(), jobs));
}

void IndexBits::absorb(std::span<IndexBits> parts) {
    // The chunks any part holds (as summary words), then each chunk's words ORed together on one thread.
    std::vector<uint64_t> any(summary_.size(), 0);
    for (const IndexBits& p : parts)
        for (size_t i = 0; i < p.summary_.size() && i < any.size(); ++i) any[i] |= p.summary_[i];
    std::vector<uint32_t>& list = chunks_;
    list.clear();
    for (size_t i = 0; i < any.size(); ++i)
        for (uint64_t bits = any[i]; bits; bits &= bits - 1) list.push_back(uint32_t(i * 64 + size_t(std::countr_zero(bits))));
    for (size_t i = 0; i < any.size(); ++i) summary_[i] |= any[i];
    poolFor(list.size(), [&](size_t b, size_t e) {
        for (size_t k = b; k < e; ++k) {
            const size_t first = size_t(list[k]) * kChunkWords;
            const uint64_t sbit = uint64_t(1) << (list[k] & 63);
            for (IndexBits& p : parts) {
                if (!(p.summary_[list[k] >> 6] & sbit)) continue;
                for (uint32_t j = 0; j < kChunkWords; ++j) {
                    words_[first + j] |= p.words_[first + j];
                    p.words_[first + j] = 0;
                }
            }
        }
    }, 32);
    for (IndexBits& p : parts) std::fill(p.summary_.begin(), p.summary_.end(), 0);
}

void IndexBits::take(std::vector<uint32_t>& out, const std::function<void(size_t, uint64_t)>& perWord) {
    chunks(chunks_);
    const size_t nc = chunks_.size();
    if (nc == 0) return;
    // Count each chunk's members, then write them at their offsets (blocks of chunks, so small sets run inline).
    constexpr size_t kBlock = 32; // chunks per job: small sets run inline
    counts_.resize(nc + 1);
    poolFor(nc, [&](size_t b, size_t e) {
        for (size_t k = b; k < e; ++k) {
            const uint64_t* w = words_.data() + size_t(chunks_[k]) * kChunkWords;
            uint32_t n = 0;
            for (uint32_t j = 0; j < kChunkWords; ++j) n += uint32_t(std::popcount(w[j]));
            counts_[k + 1] = n;
        }
    }, kBlock);
    counts_[0] = 0;
    for (size_t k = 0; k < nc; ++k) counts_[k + 1] += counts_[k];
    const size_t base = out.size();
    out.resize(base + counts_[nc]);
    poolFor(nc, [&](size_t b, size_t e) {
        for (size_t k = b; k < e; ++k) {
            const size_t first = size_t(chunks_[k]) * kChunkWords;
            uint32_t* dst = out.data() + base + counts_[k];
            for (uint32_t j = 0; j < kChunkWords; ++j) {
                uint64_t bits = words_[first + j];
                if (!bits) continue;
                if (perWord) perWord(first + j, bits);
                words_[first + j] = 0;
                const uint32_t i0 = uint32_t((first + j) * 64);
                for (; bits; bits &= bits - 1) *dst++ = i0 + uint32_t(std::countr_zero(bits));
            }
        }
    }, kBlock);
    for (uint32_t c : chunks_) summary_[c >> 6] &= ~(uint64_t(1) << (c & 63));
}

} // namespace sf
