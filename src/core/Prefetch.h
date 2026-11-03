// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// prefetch(p): asks the CPU to start loading the cache line holding `p`, for the brush passes that walk
// triangles and positions scattered over memory (normal updates, BVH refits). A hint only: no effect on results.
#pragma once

namespace sf {

inline void prefetch(const void* p) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p);
#else
    (void)p;
#endif
}

} // namespace sf
