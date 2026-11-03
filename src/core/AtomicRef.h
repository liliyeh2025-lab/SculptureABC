// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// AtomicRef<T>: atomic operations on a plain integer in a vector, for the parallel brush passes that mark
// vertices and BVH leaves. std::atomic_ref where the standard library has it; Apple's libc++ doesn't yet,
// so there the GCC/Clang __atomic builtins do the same (relaxed order only, all these passes need).
#pragma once

#include <atomic>

namespace sf {

#if defined(__cpp_lib_atomic_ref)
template <typename T> using AtomicRef = std::atomic_ref<T>;
#else
template <typename T> class AtomicRef {
public:
    explicit AtomicRef(T& v) : p_(&v) {}
    T load(std::memory_order) const { return __atomic_load_n(p_, __ATOMIC_RELAXED); }
    void store(T v, std::memory_order) const { __atomic_store_n(p_, v, __ATOMIC_RELAXED); }
    T exchange(T v, std::memory_order) const { return __atomic_exchange_n(p_, v, __ATOMIC_RELAXED); }
    T fetch_or(T v, std::memory_order) const { return __atomic_fetch_or(p_, v, __ATOMIC_RELAXED); }
    bool compare_exchange_weak(T& expected, T desired, std::memory_order) const {
        return __atomic_compare_exchange_n(p_, &expected, desired, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    }

private:
    T* p_;
};
#endif

} // namespace sf
