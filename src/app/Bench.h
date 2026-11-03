// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/Benchmark.h"

namespace sf {

// sculptforge --bench: runs core/Benchmark.h in a window with the GPU timings added; returns the exit code.
int runAppBenchmark(BenchCommandLine cl);

} // namespace sf
