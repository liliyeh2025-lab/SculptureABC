// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
#include "MobileGL.h"

#include <cstring>

namespace mgl {

#define SCULPTURE_GL_DEFINE(ret, name, ...) PFN_##name name = nullptr;
SCULPTURE_GLES3_FUNCTIONS(SCULPTURE_GL_DEFINE)
#undef SCULPTURE_GL_DEFINE

namespace {
bool gIsES = false;
std::string gHeader;
} // namespace

const char* load(ProcLoader loader) {
#define SCULPTURE_GL_LOAD(ret, name, ...)                              \
    name = reinterpret_cast<PFN_##name>(loader("gl" #name));          \
    if (!name) return "gl" #name;
    SCULPTURE_GLES3_FUNCTIONS(SCULPTURE_GL_LOAD)
#undef SCULPTURE_GL_LOAD

    const char* version = reinterpret_cast<const char*>(GetString(GL_VERSION));
    gIsES = version && std::strncmp(version, "OpenGL ES", 9) == 0;
    gHeader = gIsES ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                    : "#version 330 core\n";
    return nullptr;
}

bool isES() { return gIsES; }

const std::string& shaderHeader() { return gHeader; }

namespace {
GLuint compile(GLenum type, const char* source, std::string& error) {
    GLuint shader = CreateShader(type);
    const char* parts[2] = {gHeader.c_str(), source};
    ShaderSource(shader, 2, parts, nullptr);
    CompileShader(shader);
    GLint ok = 0;
    GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        GetShaderInfoLog(shader, sizeof log, nullptr, log);
        error += (type == GL_VERTEX_SHADER ? "vertex: " : "fragment: ");
        error += log;
        DeleteShader(shader);
        return 0;
    }
    return shader;
}
} // namespace

GLuint buildProgram(const char* vertexSource, const char* fragmentSource, std::string& error) {
    GLuint vs = compile(GL_VERTEX_SHADER, vertexSource, error);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragmentSource, error);
    if (!vs || !fs) {
        if (vs) DeleteShader(vs);
        if (fs) DeleteShader(fs);
        return 0;
    }
    GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);
    DeleteShader(vs);
    DeleteShader(fs);
    GLint ok = 0;
    GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        GetProgramInfoLog(program, sizeof log, nullptr, log);
        error += "link: ";
        error += log;
        DeleteProgram(program);
        return 0;
    }
    return program;
}

} // namespace mgl
