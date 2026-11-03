// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
//
// WebAssembly speed check: the same two loops built with different compiler switches (wasm-speed.ps1), to find a
// switch that keeps a browser's engine from optimizing (iPad: the app ran ~28x slower than desktop Chromium).
#include <emscripten.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

static std::vector<float> a;

static double floatLoop(int n) {
    if (a.size() != size_t(n)) {
        a.assign(size_t(n), 0.0f);
        for (int i = 0; i < n; ++i) a[size_t(i)] = float(i & 1023) * 0.001f;
    }
    float sum = 0.0f;
    for (int rep = 0; rep < 10; ++rep)
        for (int i = 0; i < n; ++i) {
            a[size_t(i)] = a[size_t(i)] * 0.9999f + 0.5f;
            sum += a[size_t(i)];
        }
    return sum;
}

// Integer work with a dependency chain and branches (not vectorizable).
static double intLoop(int n) {
    uint32_t x = 2463534242u, count = 0;
    for (int i = 0; i < n * 10; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        if (x & 1u) ++count;
    }
    return double(count);
}

extern "C" {
EMSCRIPTEN_KEEPALIVE double runFloat(int n) {
#ifdef USE_EH
    try {
        const double s = floatLoop(n);
        if (s < -1.0) throw std::runtime_error("never");
        return s;
    } catch (...) {
        return -1.0;
    }
#else
    return floatLoop(n);
#endif
}

EMSCRIPTEN_KEEPALIVE double runInt(int n) {
#ifdef USE_EH
    try {
        const double s = intLoop(n);
        if (s < -1.0) throw std::runtime_error("never");
        return s;
    } catch (...) {
        return -1.0;
    }
#else
    return intLoop(n);
#endif
}
}
