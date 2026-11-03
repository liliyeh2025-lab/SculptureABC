// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
// Single-threaded web build (see InlineThreads.c): std::thread::hardware_concurrency() asks sysconf, which asks
// this. One core, so the core's parallel loops run inline and its ThreadPool starts no workers.
addToLibrary({
  emscripten_num_logical_cores: () => 1,
});
