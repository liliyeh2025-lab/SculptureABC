// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
//
// Single-threaded web build (no -pthread: the page is not cross-origin isolated on a plain-http LAN address, so
// there is no SharedArrayBuffer). Emscripten's stand-in pthread_create then fails, and std::thread throws.
// Upstream's core starts threads in a fork-join style: a thread does its share of a loop and the caller joins it.
// So here a "thread" simply runs to completion inside pthread_create, and pthread_join hands back its result.
// Together with reporting one logical core (so ThreadPool::shared() starts no long-lived workers and the parallel
// loops take their inline path), upstream's sources build and run unchanged.
//
// Not compiled into a -pthread build (__EMSCRIPTEN_PTHREADS__), where the real implementations are used.
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

typedef struct InlineThread {
    void* result;
} InlineThread;

// Strong definitions replace the weak stand-ins in Emscripten's library_pthread_stub.c.
int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
    (void)attr;
    InlineThread* t = (InlineThread*)malloc(sizeof(InlineThread));
    if (!t) return EAGAIN;
    t->result = start(arg);
    *thread = (pthread_t)t;
    return 0;
}

int pthread_join(pthread_t thread, void** result) {
    InlineThread* t = (InlineThread*)thread;
    if (!t) return ESRCH;
    if (result) *result = t->result;
    free(t);
    return 0;
}

int pthread_detach(pthread_t thread) {
    free((InlineThread*)thread);
    return 0;
}

// std::thread::hardware_concurrency() reports one core: mobile/src/platform/single_core.js replaces the
// JavaScript function sysconf asks (emscripten_num_logical_cores), so nothing plans work for threads that would
// only run one after another anyway.

#endif
