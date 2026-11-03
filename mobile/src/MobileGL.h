// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
//
// OpenGL ES 3.0 function table for the tablet shell. Types and constants come from upstream's
// render/GL.h; the pointers live in their own namespace (mgl) and cover only what GLES 3.0 has, so
// loading never fails on a phone because of a desktop-only function.
#pragma once

#include "render/GL.h"

#include <string>

constexpr GLenum GL_SHADING_LANGUAGE_VERSION = 0x8B8C;
constexpr GLenum GL_BACK = 0x0405;

#define SCULPTURE_GLES3_FUNCTIONS(X)                                                        \
    X(void, Clear, GLbitfield)                                                               \
    X(void, ClearColor, GLfloat, GLfloat, GLfloat, GLfloat)                                  \
    X(void, Viewport, GLint, GLint, GLsizei, GLsizei)                                        \
    X(void, Enable, GLenum)                                                                  \
    X(void, Disable, GLenum)                                                                 \
    X(void, DepthFunc, GLenum)                                                               \
    X(void, DepthMask, GLboolean)                                                            \
    X(void, CullFace, GLenum)                                                                \
    X(const GLubyte*, GetString, GLenum)                                                     \
    X(GLenum, GetError, void)                                                                \
    X(void, GetIntegerv, GLenum, GLint*)                                                     \
    X(void, GenBuffers, GLsizei, GLuint*)                                                    \
    X(void, DeleteBuffers, GLsizei, const GLuint*)                                           \
    X(void, BindBuffer, GLenum, GLuint)                                                      \
    X(void, BufferData, GLenum, GLsizeiptr, const void*, GLenum)                             \
    X(void, GenVertexArrays, GLsizei, GLuint*)                                               \
    X(void, DeleteVertexArrays, GLsizei, const GLuint*)                                      \
    X(void, BindVertexArray, GLuint)                                                         \
    X(void, EnableVertexAttribArray, GLuint)                                                 \
    X(void, VertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)     \
    X(GLuint, CreateShader, GLenum)                                                          \
    X(void, ShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*)               \
    X(void, CompileShader, GLuint)                                                           \
    X(void, GetShaderiv, GLuint, GLenum, GLint*)                                             \
    X(void, GetShaderInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)                            \
    X(GLuint, CreateProgram)                                                                 \
    X(void, AttachShader, GLuint, GLuint)                                                    \
    X(void, LinkProgram, GLuint)                                                             \
    X(void, GetProgramiv, GLuint, GLenum, GLint*)                                            \
    X(void, GetProgramInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)                           \
    X(void, DeleteShader, GLuint)                                                            \
    X(void, DeleteProgram, GLuint)                                                           \
    X(void, UseProgram, GLuint)                                                              \
    X(GLint, GetUniformLocation, GLuint, const GLchar*)                                      \
    X(void, UniformMatrix4fv, GLint, GLsizei, GLboolean, const GLfloat*)                     \
    X(void, UniformMatrix3fv, GLint, GLsizei, GLboolean, const GLfloat*)                     \
    X(void, Uniform3fv, GLint, GLsizei, const GLfloat*)                                      \
    X(void, DrawElements, GLenum, GLsizei, GLenum, const void*)

namespace mgl {

#define SCULPTURE_GL_DECLARE(ret, name, ...)          \
    using PFN_##name = ret(SF_GLAPI*)(__VA_ARGS__);   \
    extern PFN_##name name;
SCULPTURE_GLES3_FUNCTIONS(SCULPTURE_GL_DECLARE)
#undef SCULPTURE_GL_DECLARE

using GenericProc = void (*)();
using ProcLoader = GenericProc (*)(const char* name);

// Returns the name of the first missing function, or nullptr on success.
const char* load(ProcLoader loader);

// True when the current context is OpenGL ES (phones, tablets, or a GLES context on Windows).
bool isES();

// Nomad-style shader header: shader sources carry no #version line; this is put in front.
//   GLES:       "#version 300 es" + default precisions
//   desktop GL: "#version 330 core"
const std::string& shaderHeader();

// Compiles and links a program from header-less vertex / fragment sources. Returns 0 on failure
// and writes the log to `error`.
GLuint buildProgram(const char* vertexSource, const char* fragmentSource, std::string& error);

} // namespace mgl
