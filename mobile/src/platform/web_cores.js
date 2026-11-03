// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
// Multithreaded web build: std::thread::hardware_concurrency() asks sysconf, which asks this. At most 8, so the
// workers started at load (PTHREAD_POOL_SIZE in mobile/CMakeLists.txt) always cover the core's threads.
addToLibrary({
  emscripten_num_logical_cores: () => Math.max(1, Math.min(navigator.hardwareConcurrency || 4, 8)),
});
