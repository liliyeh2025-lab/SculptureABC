// SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
#include "render/GLCompat.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace sculpture {

namespace {

constexpr GLenum kCopyWriteBuffer = 0x8F37, kPixelUnpackBuffer = 0x88EC, kMaxTextureSize = 0x0D33;
constexpr GLenum kRed = 0x1903, kRg = 0x8227, kRgbaInteger = 0x8D99;
constexpr GLenum kByte = 0x1400, kShort = 0x1402, kInt = 0x1404, kHalfFloat = 0x140B;
constexpr GLenum kR32F = 0x822E, kR8 = 0x8229, kRgba8ui = 0x8D7C, kRgba16ui = 0x8D76, kRg32ui = 0x823C;
constexpr int kRowShift = 12; // log2(kBufferTextureWidth)
static_assert((1 << kRowShift) == kBufferTextureWidth);

// The real functions the stand-ins call.
struct Real {
    gl::PFN_BindBuffer BindBuffer;
    gl::PFN_BufferData BufferData;
    gl::PFN_BufferSubData BufferSubData;
    gl::PFN_BindTexture BindTexture;
    gl::PFN_ActiveTexture ActiveTexture;
    gl::PFN_TexImage2D TexImage2D;
    gl::PFN_TexSubImage2D TexSubImage2D;
    gl::PFN_TexParameteri TexParameteri;
    gl::PFN_PixelStorei PixelStorei;
    gl::PFN_GetIntegerv GetIntegerv;
    gl::PFN_Enable Enable;
    gl::PFN_Disable Disable;
    gl::PFN_CreateShader CreateShader;
    gl::PFN_DeleteShader DeleteShader;
    gl::PFN_ShaderSource ShaderSource;
    gl::PFN_BindVertexArray BindVertexArray;
    gl::PFN_DeleteVertexArrays DeleteVertexArrays;
    gl::PFN_VertexAttribPointer VertexAttribPointer;
    gl::PFN_VertexAttribIPointer VertexAttribIPointer;
    gl::PFN_DrawElements DrawElements;
    gl::PFN_DrawArrays DrawArrays;
    gl::PFN_DrawElementsInstanced DrawElementsInstanced;
    gl::PFN_DrawArraysInstanced DrawArraysInstanced;
    gl::PFN_DeleteBuffers DeleteBuffers;
    gl::PFN_DeleteTextures DeleteTextures;
} real;

gl::ProcLoader gLoader = nullptr;

// --- Shader translation --------------------------------------------------------------------------------------

std::unordered_map<GLuint, GLenum> gShaderType; // shader -> GL_VERTEX_SHADER / GL_FRAGMENT_SHADER / GL_GEOMETRY_SHADER

const char* kPreamble =
    "#version 300 es\n"
    "precision highp float;\nprecision highp int;\n"
    "precision highp sampler2D;\nprecision highp usampler2D;\nprecision highp isampler2D;\n"
    "vec4 sfBufferFetch(highp sampler2D s, int i) { return texelFetch(s, ivec2(i & 4095, i >> 12), 0); }\n"
    "uvec4 sfBufferFetch(highp usampler2D s, int i) { return texelFetch(s, ivec2(i & 4095, i >> 12), 0); }\n"
    "ivec4 sfBufferFetch(highp isampler2D s, int i) { return texelFetch(s, ivec2(i & 4095, i >> 12), 0); }\n";

bool identChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// Replaces whole-word `from` with `to`.
void replaceWord(std::string& s, const std::string& from, const std::string& to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at)) {
        const bool before = at > 0 && identChar(s[at - 1]);
        const bool after = at + from.size() < s.size() && identChar(s[at + from.size()]);
        if (before || after) {
            at += from.size();
            continue;
        }
        s.replace(at, from.size(), to);
        at += to.size();
    }
}

// texelFetch(sampler, index) (two arguments: a buffer) -> sfBufferFetch(sampler, index).
void replaceBufferFetches(std::string& s) {
    const std::string name = "texelFetch";
    for (size_t at = s.find(name); at != std::string::npos; at = s.find(name, at + 1)) {
        if ((at > 0 && identChar(s[at - 1]))) continue;
        size_t open = at + name.size();
        while (open < s.size() && (s[open] == ' ' || s[open] == '\t')) ++open;
        if (open >= s.size() || s[open] != '(') continue;
        int depth = 0, commas = 0;
        for (size_t i = open; i < s.size(); ++i) {
            const char c = s[i];
            if (c == '(' || c == '[') ++depth;
            else if (c == ')' || c == ']') {
                if (--depth == 0) break;
            } else if (c == ',' && depth == 1) ++commas;
        }
        if (commas == 1) s.replace(at, name.size(), "sfBufferFetch");
    }
}

std::string translate(std::string src, GLenum type) {
    // The #version line (upstream's sources start with one, or get one from a prefix).
    size_t v = src.find("#version");
    if (v != std::string::npos) {
        const size_t end = src.find('\n', v);
        src.erase(v, end == std::string::npos ? std::string::npos : end - v + 1);
    }
    replaceWord(src, "usamplerBuffer", "usampler2D");
    replaceWord(src, "isamplerBuffer", "isampler2D");
    replaceWord(src, "samplerBuffer", "sampler2D");
    replaceBufferFetches(src);
    // No gl_PrimitiveID in GLSL ES 3.00 fragment shaders: per-triangle data then reads its cluster's first triangle.
    if (type == GL_FRAGMENT_SHADER) replaceWord(src, "gl_PrimitiveID", "0");
    return kPreamble + src;
}

GLuint SF_GLAPI createShader(GLenum type) {
    // Geometry shaders: a fragment shader that fails to compile, with the reason in its log.
    const GLuint s = real.CreateShader(type == GL_GEOMETRY_SHADER ? GL_FRAGMENT_SHADER : type);
    if (s) gShaderType[s] = type;
    return s;
}

void SF_GLAPI deleteShader(GLuint s) {
    gShaderType.erase(s);
    real.DeleteShader(s);
}

void SF_GLAPI shaderSource(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths) {
    std::string src;
    for (GLsizei i = 0; i < count; ++i) {
        if (!strings[i]) continue;
        if (lengths && lengths[i] >= 0) src.append(strings[i], size_t(lengths[i]));
        else src += strings[i];
    }
    const auto it = gShaderType.find(shader);
    const GLenum type = it == gShaderType.end() ? GL_VERTEX_SHADER : it->second;
    std::string out = type == GL_GEOMETRY_SHADER
                          ? std::string("#version 300 es\n#error geometry shaders are not available in OpenGL ES 3.0 / WebGL2\n")
                          : translate(std::move(src), type);
    const char* p = out.c_str();
    real.ShaderSource(shader, 1, &p, nullptr);
}

// --- Texture buffers ------------------------------------------------------------------------------------------

struct Format {
    GLenum internal, format, type;
    int bytes; // per texel
};

bool formatOf(GLenum internal, Format& f) {
    switch (internal) {
    case GL_R32UI: f = {internal, GL_RED_INTEGER, GL_UNSIGNED_INT, 4}; return true;
    case GL_R16UI: f = {internal, GL_RED_INTEGER, GL_UNSIGNED_SHORT, 2}; return true;
    case GL_R16I: f = {internal, GL_RED_INTEGER, kShort, 2}; return true;
    case GL_RG32F: f = {internal, kRg, GL_FLOAT, 8}; return true;
    case GL_RGBA32F: f = {internal, GL_RGBA, GL_FLOAT, 16}; return true;
    case GL_RGBA32UI: f = {internal, kRgbaInteger, GL_UNSIGNED_INT, 16}; return true;
    case kRg32ui: f = {internal, 0x8228 /* GL_RG_INTEGER */, GL_UNSIGNED_INT, 8}; return true;
    case kRgba16ui: f = {internal, kRgbaInteger, GL_UNSIGNED_SHORT, 8}; return true;
    case kRgba8ui: f = {internal, kRgbaInteger, GL_UNSIGNED_BYTE, 4}; return true;
    case kR32F: f = {internal, kRed, GL_FLOAT, 4}; return true;
    case kR8: f = {internal, kRed, GL_UNSIGNED_BYTE, 1}; return true;
    default: return false;
    }
}

struct BufferTexture {
    GLuint buffer = 0;
    Format format{};
    size_t texels = 0; // what the texture was sized for
    int rows = 0;
};

std::unordered_map<GLuint, size_t> gBufferBytes;             // texture-buffer storage sizes, by buffer
std::unordered_map<GLuint, std::vector<GLuint>> gBufferUsers; // buffer -> textures that mirror it
std::unordered_map<GLuint, BufferTexture> gTextures;          // texture -> what it mirrors

GLuint gTexBufferBinding = 0; // what upstream bound to GL_TEXTURE_BUFFER (bound as GL_COPY_WRITE_BUFFER)
GLuint gArrayBinding = 0;
GLenum gActiveUnit = GL_TEXTURE0;
GLuint gBound2D[32] = {};
GLint gUnpackAlignment = 4, gUnpackRowLength = 0;

GLuint& boundOnActiveUnit() { return gBound2D[std::min<GLuint>(gActiveUnit - GL_TEXTURE0, 31)]; }

// Copies bytes [begin, end) of texture t's buffer into it (the whole buffer, re-sized, when `resize`).
void refresh(GLuint t, BufferTexture& bt, size_t begin, size_t end, bool resize) {
    const auto bytesIt = gBufferBytes.find(bt.buffer);
    const size_t bytes = bytesIt == gBufferBytes.end() ? 0 : bytesIt->second;
    const size_t texels = bytes / size_t(bt.format.bytes);
    const size_t W = size_t(kBufferTextureWidth);
    real.BindTexture(GL_TEXTURE_2D, t);
    real.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    real.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    real.BindBuffer(kPixelUnpackBuffer, 0);
    if (resize || texels != bt.texels) {
        bt.texels = texels;
        bt.rows = int(std::max<size_t>(1, (texels + W - 1) / W));
        real.TexImage2D(GL_TEXTURE_2D, 0, GLint(bt.format.internal), GLsizei(W), bt.rows, 0, bt.format.format, bt.format.type, nullptr);
        real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GLint(GL_NEAREST));
        real.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GLint(GL_NEAREST));
        begin = 0;
        end = bytes;
    }
    end = std::min(end, texels * size_t(bt.format.bytes));
    if (texels > 0 && begin < end) {
        real.BindBuffer(kPixelUnpackBuffer, bt.buffer);
        const size_t t0 = begin / size_t(bt.format.bytes), t1 = (end + size_t(bt.format.bytes) - 1) / size_t(bt.format.bytes);
        const size_t fullRows = texels / W;
        const size_t r0 = t0 / W, r1 = std::min(fullRows, (t1 + W - 1) / W); // whole rows
        auto offset = [&](size_t texel) { return reinterpret_cast<const void*>(texel * size_t(bt.format.bytes)); };
        if (r1 > r0)
            real.TexSubImage2D(GL_TEXTURE_2D, 0, 0, GLint(r0), GLsizei(W), GLsizei(r1 - r0), bt.format.format, bt.format.type,
                               offset(r0 * W));
        if (texels % W != 0 && t1 > fullRows * W) // the last, partial row
            real.TexSubImage2D(GL_TEXTURE_2D, 0, 0, GLint(fullRows), GLsizei(texels % W), 1, bt.format.format, bt.format.type,
                               offset(fullRows * W));
        real.BindBuffer(kPixelUnpackBuffer, 0);
    }
    real.PixelStorei(GL_UNPACK_ALIGNMENT, gUnpackAlignment);
    real.PixelStorei(GL_UNPACK_ROW_LENGTH, gUnpackRowLength);
    real.BindTexture(GL_TEXTURE_2D, boundOnActiveUnit());
}

void refreshUsers(GLuint buffer, size_t begin, size_t end, bool resize) {
    const auto it = gBufferUsers.find(buffer);
    if (it == gBufferUsers.end()) return;
    for (GLuint t : it->second) {
        auto bt = gTextures.find(t);
        if (bt != gTextures.end() && bt->second.buffer == buffer) refresh(t, bt->second, begin, end, resize);
    }
}

GLenum mapTarget(GLenum target) { return target == GL_TEXTURE_BUFFER ? kCopyWriteBuffer : target; }

void SF_GLAPI bindBuffer(GLenum target, GLuint buffer) {
    if (target == GL_TEXTURE_BUFFER) gTexBufferBinding = buffer;
    else if (target == GL_ARRAY_BUFFER) gArrayBinding = buffer;
    real.BindBuffer(mapTarget(target), buffer);
}

void SF_GLAPI bufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    real.BufferData(mapTarget(target), size, data, usage);
    if (target == GL_TEXTURE_BUFFER && gTexBufferBinding) {
        gBufferBytes[gTexBufferBinding] = size_t(size);
        refreshUsers(gTexBufferBinding, 0, size_t(size), true);
    }
}

void SF_GLAPI bufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    real.BufferSubData(mapTarget(target), offset, size, data);
    if (target == GL_TEXTURE_BUFFER && gTexBufferBinding) refreshUsers(gTexBufferBinding, size_t(offset), size_t(offset + size), false);
}

void SF_GLAPI deleteBuffers(GLsizei n, const GLuint* buffers) {
    for (GLsizei i = 0; i < n; ++i) {
        gBufferBytes.erase(buffers[i]);
        gBufferUsers.erase(buffers[i]);
        if (gTexBufferBinding == buffers[i]) gTexBufferBinding = 0;
        if (gArrayBinding == buffers[i]) gArrayBinding = 0;
    }
    real.DeleteBuffers(n, buffers);
}

void SF_GLAPI deleteTextures(GLsizei n, const GLuint* textures) {
    for (GLsizei i = 0; i < n; ++i) {
        gTextures.erase(textures[i]);
        for (GLuint& b : gBound2D)
            if (b == textures[i]) b = 0;
    }
    real.DeleteTextures(n, textures);
}

void SF_GLAPI activeTexture(GLenum unit) {
    gActiveUnit = unit;
    real.ActiveTexture(unit);
}

void SF_GLAPI bindTexture(GLenum target, GLuint texture) {
    if (target == GL_TEXTURE_BUFFER || target == GL_TEXTURE_2D) {
        boundOnActiveUnit() = texture;
        real.BindTexture(GL_TEXTURE_2D, texture);
        return;
    }
    real.BindTexture(target, texture);
}

void SF_GLAPI texBuffer(GLenum, GLenum internalFormat, GLuint buffer) {
    const GLuint t = boundOnActiveUnit();
    if (!t) return;
    Format f;
    if (!formatOf(internalFormat, f)) {
        SDL_Log("GLCompat: texture buffer format 0x%04X not handled", internalFormat);
        return;
    }
    BufferTexture& bt = gTextures[t];
    if (bt.buffer && bt.buffer != buffer) {
        auto& users = gBufferUsers[bt.buffer];
        users.erase(std::remove(users.begin(), users.end(), t), users.end());
    }
    bt.buffer = buffer;
    bt.format = f;
    if (!buffer) return;
    auto& users = gBufferUsers[buffer];
    if (std::find(users.begin(), users.end(), t) == users.end()) users.push_back(t);
    refresh(t, bt, 0, SIZE_MAX, true);
}

void SF_GLAPI pixelStorei(GLenum name, GLint value) {
    if (name == GL_UNPACK_ALIGNMENT) gUnpackAlignment = value;
    if (name == GL_UNPACK_ROW_LENGTH) gUnpackRowLength = value;
    real.PixelStorei(name, value);
}

void SF_GLAPI getIntegerv(GLenum name, GLint* value) {
    if (name == GL_MAX_TEXTURE_BUFFER_SIZE) {
        GLint size = 0;
        real.GetIntegerv(kMaxTextureSize, &size);
        *value = GLint(std::min<int64_t>(int64_t(size) * kBufferTextureWidth, INT_MAX));
        return;
    }
    real.GetIntegerv(name, value);
}

void SF_GLAPI enable(GLenum cap) {
    if (cap != GL_MULTISAMPLE) real.Enable(cap);
}

void SF_GLAPI disable(GLenum cap) {
    if (cap != GL_MULTISAMPLE) real.Disable(cap);
}

// --- Mapped buffers -------------------------------------------------------------------------------------------

struct Mapping {
    GLenum target = 0;
    GLintptr offset = 0;
    std::vector<uint8_t> bytes;
};
std::vector<Mapping> gMappings;

void* SF_GLAPI mapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield) {
    if (target == GL_ARRAY_BUFFER || length <= 0) return nullptr; // (upstream streams vertex arrays itself then)
    for (Mapping& m : gMappings)
        if (m.target == target) return nullptr; // already mapped
    gMappings.push_back({target, offset, std::vector<uint8_t>(size_t(length))});
    return gMappings.back().bytes.data();
}

GLboolean SF_GLAPI unmapBuffer(GLenum target) {
    for (size_t i = 0; i < gMappings.size(); ++i) {
        if (gMappings[i].target != target) continue;
        bufferSubData(target, gMappings[i].offset, GLsizeiptr(gMappings[i].bytes.size()), gMappings[i].bytes.data());
        gMappings.erase(gMappings.begin() + std::ptrdiff_t(i));
        return GL_TRUE;
    }
    return GL_FALSE;
}

// --- Base vertex ----------------------------------------------------------------------------------------------

struct Attrib {
    GLuint buffer = 0;
    GLint size = 0;
    GLenum type = 0;
    GLboolean normalized = GL_FALSE;
    GLsizei stride = 0;
    uintptr_t offset = 0;
    bool integer = false;
    bool set = false;
    GLint base = 0; // the base vertex the GL pointer currently includes
};
struct VertexArray {
    Attrib attribs[16];
    bool shifted = false; // some attribute points at a base other than 0
};
std::unordered_map<GLuint, VertexArray> gVaos;
GLuint gVao = 0;
bool gPolygonLines = false;

int typeBytes(GLenum t) {
    switch (t) {
    case kByte:
    case GL_UNSIGNED_BYTE: return 1;
    case kShort:
    case GL_UNSIGNED_SHORT:
    case kHalfFloat: return 2;
    default: return 4;
    }
}

void SF_GLAPI bindVertexArray(GLuint vao) {
    gVao = vao;
    real.BindVertexArray(vao);
}

void SF_GLAPI deleteVertexArrays(GLsizei n, const GLuint* vaos) {
    for (GLsizei i = 0; i < n; ++i) {
        gVaos.erase(vaos[i]);
        if (gVao == vaos[i]) gVao = 0;
    }
    real.DeleteVertexArrays(n, vaos);
}

void record(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer, bool integer) {
    if (index >= 16) return;
    Attrib& a = gVaos[gVao].attribs[index];
    a = Attrib{gArrayBinding, size, type, normalized, stride, reinterpret_cast<uintptr_t>(pointer), integer, gArrayBinding != 0, 0};
}

void SF_GLAPI vertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer) {
    record(index, size, type, normalized, stride, pointer, false);
    real.VertexAttribPointer(index, size, type, normalized, stride, pointer);
}

void SF_GLAPI vertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer) {
    record(index, size, type, GL_FALSE, stride, pointer, true);
    real.VertexAttribIPointer(index, size, type, stride, pointer);
}

// Points the bound vertex array's attributes `base` vertices on.
void applyBase(GLint base) {
    auto it = gVaos.find(gVao);
    if (it == gVaos.end() || (base == 0 && !it->second.shifted)) return;
    VertexArray& va = it->second;
    bool rebound = false;
    for (GLuint i = 0; i < 16; ++i) {
        Attrib& a = va.attribs[i];
        if (!a.set || a.base == base) continue;
        const uintptr_t stride = a.stride ? uintptr_t(a.stride) : uintptr_t(a.size * typeBytes(a.type));
        const void* p = reinterpret_cast<const void*>(a.offset + uintptr_t(int64_t(base)) * stride);
        real.BindBuffer(GL_ARRAY_BUFFER, a.buffer);
        rebound = true;
        if (a.integer) real.VertexAttribIPointer(i, a.size, a.type, a.stride, p);
        else real.VertexAttribPointer(i, a.size, a.type, a.normalized, a.stride, p);
        a.base = base;
    }
    va.shifted = base != 0;
    if (rebound) real.BindBuffer(GL_ARRAY_BUFFER, gArrayBinding);
}

bool skipped(GLenum mode) { return gPolygonLines && mode == GL_TRIANGLES; }

void SF_GLAPI polygonMode(GLenum, GLenum mode) { gPolygonLines = mode == GL_LINE; }

void SF_GLAPI drawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint base) {
    if (skipped(mode) || count <= 0) return;
    applyBase(base);
    real.DrawElements(mode, count, type, indices);
}

void SF_GLAPI multiDrawElementsBaseVertex(GLenum mode, const GLsizei* counts, GLenum type, const void* const* indices, GLsizei n,
                                          const GLint* bases) {
    if (skipped(mode)) return;
    for (GLsizei i = 0; i < n; ++i) {
        if (counts[i] <= 0) continue;
        applyBase(bases[i]);
        real.DrawElements(mode, counts[i], type, indices[i]);
    }
}

void SF_GLAPI drawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (skipped(mode)) return;
    applyBase(0);
    real.DrawElements(mode, count, type, indices);
}

void SF_GLAPI drawArrays(GLenum mode, GLint first, GLsizei count) {
    if (skipped(mode)) return;
    applyBase(0);
    real.DrawArrays(mode, first, count);
}

void SF_GLAPI drawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances) {
    applyBase(0);
    real.DrawElementsInstanced(mode, count, type, indices, instances);
}

void SF_GLAPI drawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances) {
    applyBase(0);
    real.DrawArraysInstanced(mode, first, count, instances);
}

// --- Loader ---------------------------------------------------------------------------------------------------

template <class F>
F realFn(const char* name) {
    return reinterpret_cast<F>(gLoader(name));
}

gl::GenericProc standIn(const char* name) {
    struct Entry {
        const char* name;
        gl::GenericProc fn;
    };
#define SC_ENTRY(n, f) {n, reinterpret_cast<gl::GenericProc>(&f)}
    static const Entry kEntries[] = {
        SC_ENTRY("glCreateShader", createShader),
        SC_ENTRY("glDeleteShader", deleteShader),
        SC_ENTRY("glShaderSource", shaderSource),
        SC_ENTRY("glBindBuffer", bindBuffer),
        SC_ENTRY("glBufferData", bufferData),
        SC_ENTRY("glBufferSubData", bufferSubData),
        SC_ENTRY("glDeleteBuffers", deleteBuffers),
        SC_ENTRY("glDeleteTextures", deleteTextures),
        SC_ENTRY("glActiveTexture", activeTexture),
        SC_ENTRY("glBindTexture", bindTexture),
        SC_ENTRY("glTexBuffer", texBuffer),
        SC_ENTRY("glPixelStorei", pixelStorei),
        SC_ENTRY("glGetIntegerv", getIntegerv),
        SC_ENTRY("glEnable", enable),
        SC_ENTRY("glDisable", disable),
        SC_ENTRY("glMapBufferRange", mapBufferRange),
        SC_ENTRY("glUnmapBuffer", unmapBuffer),
        SC_ENTRY("glBindVertexArray", bindVertexArray),
        SC_ENTRY("glDeleteVertexArrays", deleteVertexArrays),
        SC_ENTRY("glVertexAttribPointer", vertexAttribPointer),
        SC_ENTRY("glVertexAttribIPointer", vertexAttribIPointer),
        SC_ENTRY("glPolygonMode", polygonMode),
        SC_ENTRY("glDrawElementsBaseVertex", drawElementsBaseVertex),
        SC_ENTRY("glMultiDrawElementsBaseVertex", multiDrawElementsBaseVertex),
        SC_ENTRY("glDrawElements", drawElements),
        SC_ENTRY("glDrawArrays", drawArrays),
        SC_ENTRY("glDrawElementsInstanced", drawElementsInstanced),
        SC_ENTRY("glDrawArraysInstanced", drawArraysInstanced),
    };
#undef SC_ENTRY
    for (const Entry& e : kEntries)
        if (std::strcmp(e.name, name) == 0) return e.fn;
    return gLoader(name);
}

} // namespace

const char* loadUpstreamGL(gl::ProcLoader loader, bool es) {
    if (!es) return gl::load(loader);
    gLoader = loader;
#define SC_REAL(n) real.n = realFn<gl::PFN_##n>("gl" #n); if (!real.n) return "gl" #n;
    SC_REAL(BindBuffer) SC_REAL(BufferData) SC_REAL(BufferSubData) SC_REAL(BindTexture) SC_REAL(ActiveTexture)
    SC_REAL(TexImage2D) SC_REAL(TexSubImage2D) SC_REAL(TexParameteri) SC_REAL(PixelStorei) SC_REAL(GetIntegerv)
    SC_REAL(Enable) SC_REAL(Disable) SC_REAL(CreateShader) SC_REAL(DeleteShader) SC_REAL(ShaderSource)
    SC_REAL(BindVertexArray) SC_REAL(DeleteVertexArrays) SC_REAL(VertexAttribPointer) SC_REAL(VertexAttribIPointer)
    SC_REAL(DrawElements) SC_REAL(DrawArrays) SC_REAL(DrawElementsInstanced) SC_REAL(DrawArraysInstanced)
    SC_REAL(DeleteBuffers) SC_REAL(DeleteTextures)
#undef SC_REAL
    return gl::load(&standIn);
}

} // namespace sculpture
