// Minimal OpenGL 3.3 core loader.
//
// A generated loader (glad/glew) would pull in a code generator or another submodule
// to expose ~2000 entry points, of which this renderer calls about sixty. The X-macro
// list below is the whole dependency: add a line, get a function pointer.
//
// Dear ImGui's GL3 backend ships its own internal loader, so the two do not interact.
#pragma once

#include <cstddef>
#include <cstdint>

using GLenum     = unsigned int;
using GLboolean  = unsigned char;
using GLbitfield = unsigned int;
using GLbyte     = signed char;
using GLubyte    = unsigned char;
using GLshort    = short;
using GLushort   = unsigned short;
using GLint      = int;
using GLuint     = unsigned int;
using GLsizei    = int;
using GLfloat    = float;
using GLdouble   = double;
using GLchar     = char;
using GLintptr   = std::intptr_t;
using GLsizeiptr = std::ptrdiff_t;
using GLvoid     = void;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_LINES 0x0001
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 1
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_MULTISAMPLE 0x809D
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_INT 0x1405
#define GL_UNSIGNED_BYTE 0x1401
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STREAM_DRAW 0x88E0
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#define GL_SCISSOR_TEST 0x0C11

// name, return type, parameter list, argument list
#define RGV_GL_FUNCTIONS(X)                                                              \
    X(glGetString, const GLubyte*, (GLenum n), (n))                                       \
    X(glGetError, GLenum, (void), ())                                                     \
    X(glGetIntegerv, void, (GLenum n, GLint* d), (n, d))                                  \
    X(glViewport, void, (GLint x, GLint y, GLsizei w, GLsizei h), (x, y, w, h))           \
    X(glScissor, void, (GLint x, GLint y, GLsizei w, GLsizei h), (x, y, w, h))            \
    X(glClearColor, void, (GLfloat r, GLfloat g, GLfloat b, GLfloat a), (r, g, b, a))     \
    X(glClear, void, (GLbitfield m), (m))                                                 \
    X(glEnable, void, (GLenum c), (c))                                                    \
    X(glDisable, void, (GLenum c), (c))                                                   \
    X(glBlendFunc, void, (GLenum s, GLenum d), (s, d))                                    \
    X(glBlendFuncSeparate, void,                                                          \
      (GLenum sr, GLenum dr, GLenum sa, GLenum da), (sr, dr, sa, da))                     \
    X(glDrawArrays, void, (GLenum m, GLint f, GLsizei c), (m, f, c))                      \
    X(glDrawArraysInstanced, void,                                                        \
      (GLenum m, GLint f, GLsizei c, GLsizei n), (m, f, c, n))                            \
    X(glGenVertexArrays, void, (GLsizei n, GLuint* a), (n, a))                            \
    X(glDeleteVertexArrays, void, (GLsizei n, const GLuint* a), (n, a))                   \
    X(glBindVertexArray, void, (GLuint a), (a))                                           \
    X(glGenBuffers, void, (GLsizei n, GLuint* b), (n, b))                                 \
    X(glDeleteBuffers, void, (GLsizei n, const GLuint* b), (n, b))                        \
    X(glBindBuffer, void, (GLenum t, GLuint b), (t, b))                                   \
    X(glBufferData, void,                                                                 \
      (GLenum t, GLsizeiptr s, const void* d, GLenum u), (t, s, d, u))                    \
    X(glBufferSubData, void,                                                              \
      (GLenum t, GLintptr o, GLsizeiptr s, const void* d), (t, o, s, d))                  \
    X(glEnableVertexAttribArray, void, (GLuint i), (i))                                   \
    X(glDisableVertexAttribArray, void, (GLuint i), (i))                                  \
    X(glVertexAttribPointer, void,                                                        \
      (GLuint i, GLint sz, GLenum t, GLboolean n, GLsizei st, const void* p),             \
      (i, sz, t, n, st, p))                                                               \
    X(glVertexAttribDivisor, void, (GLuint i, GLuint d), (i, d))                          \
    X(glCreateShader, GLuint, (GLenum t), (t))                                            \
    X(glDeleteShader, void, (GLuint s), (s))                                              \
    X(glShaderSource, void,                                                               \
      (GLuint s, GLsizei c, const GLchar* const* str, const GLint* l), (s, c, str, l))    \
    X(glCompileShader, void, (GLuint s), (s))                                             \
    X(glGetShaderiv, void, (GLuint s, GLenum p, GLint* v), (s, p, v))                     \
    X(glGetShaderInfoLog, void,                                                           \
      (GLuint s, GLsizei b, GLsizei* l, GLchar* i), (s, b, l, i))                         \
    X(glCreateProgram, GLuint, (void), ())                                                \
    X(glDeleteProgram, void, (GLuint p), (p))                                             \
    X(glAttachShader, void, (GLuint p, GLuint s), (p, s))                                 \
    X(glDetachShader, void, (GLuint p, GLuint s), (p, s))                                 \
    X(glLinkProgram, void, (GLuint p), (p))                                               \
    X(glGetProgramiv, void, (GLuint p, GLenum n, GLint* v), (p, n, v))                    \
    X(glGetProgramInfoLog, void,                                                          \
      (GLuint p, GLsizei b, GLsizei* l, GLchar* i), (p, b, l, i))                         \
    X(glUseProgram, void, (GLuint p), (p))                                                \
    X(glGetUniformLocation, GLint, (GLuint p, const GLchar* n), (p, n))                   \
    X(glUniform1i, void, (GLint l, GLint v), (l, v))                                      \
    X(glUniform1f, void, (GLint l, GLfloat v), (l, v))                                    \
    X(glUniform2f, void, (GLint l, GLfloat a, GLfloat b), (l, a, b))                      \
    X(glUniform4f, void,                                                                  \
      (GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d), (l, a, b, c, d))             \
    X(glUniformMatrix4fv, void,                                                           \
      (GLint l, GLsizei c, GLboolean t, const GLfloat* v), (l, c, t, v))

namespace rgv::gl {

using ProcLoader = void* (*)(const char*);

// Returns false if any required entry point is missing, which on a 3.3 core context
// means the driver is not what it claimed to be.
bool load(ProcLoader loader);

// Names of entry points that failed to resolve, for a useful startup error.
const char* missing() ;

#define RGV_GL_DECL(name, ret, params, args) ret name params;
RGV_GL_FUNCTIONS(RGV_GL_DECL)
#undef RGV_GL_DECL

} // namespace rgv::gl
