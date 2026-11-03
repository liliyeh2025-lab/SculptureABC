// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs upstream's renderer (src/render, written for desktop OpenGL 3.3 core) unchanged on OpenGL ES 3.0 / WebGL2.
// Upstream reaches GL only through the function table in render/GL.h, which a loader fills. On a GLES context we
// fill it with the real functions plus stand-ins for what GLES 3.0 / WebGL2 lacks:
//
// - Shaders: "#version 330 core" becomes "#version 300 es" with highp precisions; samplerBuffer becomes a 2D texture
//   (4096 texels a row) read through sfBufferFetch; gl_PrimitiveID (fragment shaders) becomes 0; geometry shaders
//   fail to compile with a clear message (only the textured / normal-mapped paths use them).
// - Texture buffers (glTexBuffer): the buffer's bytes are copied into that 2D texture on the GPU (pixel unpack
//   buffer), and again whenever the buffer is written.
// - glDrawElementsBaseVertex / glMultiDrawElementsBaseVertex: the vertex attributes are re-pointed `base` vertices
//   further on, then a plain glDrawElements. gl_VertexID then lacks the base, which upstream detects at start-up
//   (Renderer::checkVertexIdBase) and works around.
// - glMapBufferRange: a CPU copy sent with glBufferSubData on unmap (vertex arrays: not mapped, upstream then streams
//   them through its own staging).
// - glPolygonMode(GL_LINE): not in GLES; triangle draws in line mode are skipped (upstream's wireframe uses real
//   line lists; only fallback paths outline triangles).
// - GL_MULTISAMPLE enable / disable: ignored (the canvas / window's own multisampling applies).
#pragma once

#include "render/GL.h"

namespace sculpture {

// Fills upstream's gl:: table from `loader` (e.g. SDL_GL_GetProcAddress). `es`: the context is OpenGL ES / WebGL2
// (stand-ins as above); otherwise the real desktop functions are used as they are. Returns the name of the first
// missing function, or nullptr.
const char* loadUpstreamGL(gl::ProcLoader loader, bool es);

// Texel rows of the 2D textures standing in for texture buffers.
constexpr int kBufferTextureWidth = 4096;

} // namespace sculpture
