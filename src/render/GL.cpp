// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/GL.h"

namespace gl {

#define SF_GL_DEFINE(ret, name, ...) PFN_##name name = nullptr;
SF_GL_FUNCTIONS(SF_GL_DEFINE)
#undef SF_GL_DEFINE

const char* load(ProcLoader loader) {
#define SF_GL_LOAD(ret, name, ...)                                    \
    name = reinterpret_cast<PFN_##name>(loader("gl" #name));          \
    if (!name) return "gl" #name;
    SF_GL_FUNCTIONS(SF_GL_LOAD)
#undef SF_GL_LOAD
    return nullptr;
}

} // namespace gl
