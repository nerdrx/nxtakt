// Windows OpenGL entry-point loader.
//
// On Linux libGL exports the whole core profile, so src/gfx/gl.h declares the
// prototypes and the linker does the rest. Windows does not work that way:
// opengl32.dll exports GL 1.1 and nothing else -- everything from GL 1.2
// onwards (which is to say every shader, buffer and vertex-array call this
// renderer makes) has to be fetched at run time from the ICD via
// wglGetProcAddress, and only after a context is current.
//
// That is the whole reason this file exists. Without it the GUI link fails on
// 29 undefined symbols; with a naive version of it, it links and then crashes
// on the first null pointer.
//
// How it avoids macros
// --------------------
// The pointers are declared in `namespace lat`, with exactly the names of the
// functions they stand in for. Every GL call in this codebase is written
// inside `namespace lat` (renderer.cpp, font.cpp), so ordinary unqualified
// lookup finds lat::glCreateShader before it ever considers the global one.
// No #define glCreateShader anywhere, so nothing downstream can be surprised
// by a macro named after a GL function.
//
// It also fails in the right direction: a GL 2.0+ call added OUTSIDE namespace
// lat resolves to the global name instead, which opengl32's import library
// does not have, so it is a link error naming the function -- not a silent
// call into a null pointer at run time.
//
// They are `inline` variables (C++17), so every translation unit that includes
// gl.h sees the same one and there is no loader .cpp to add to the build. The
// Linux file list is therefore untouched by this file's existence.
#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
// glext.h supplies the tokens (GL_R8, GL_TEXTURE0, GL_ARRAY_BUFFER, ...) and
// the PFNGL*PROC typedefs. GL_GLEXT_PROTOTYPES is deliberately NOT defined:
// we want the typedefs without the prototypes, because on this platform there
// is no library to satisfy the prototypes with.
#include <GL/glext.h>

namespace lat {

// Every GL entry point this program uses that is newer than GL 1.1. Add a line
// when the renderer grows a call; forgetting to is a link error, not a crash,
// because the name would then resolve to the global one (see above).
//
// The GL 1.1 calls -- glBindTexture, glTexImage2D, glDrawArrays, glEnable,
// glViewport, glScissor, glGetString and friends -- are deliberately absent:
// opengl32.dll exports those directly and its import library resolves them at
// link time on every driver.
#define LAT_GL_ENTRY_POINTS(X)                                    \
    X(PFNGLACTIVETEXTUREPROC,          glActiveTexture)           \
    X(PFNGLATTACHSHADERPROC,           glAttachShader)            \
    X(PFNGLBINDBUFFERPROC,             glBindBuffer)              \
    X(PFNGLBINDFRAMEBUFFERPROC,        glBindFramebuffer)         \
    X(PFNGLBINDVERTEXARRAYPROC,        glBindVertexArray)         \
    X(PFNGLBLENDEQUATIONPROC,          glBlendEquation)           \
    X(PFNGLBLENDFUNCSEPARATEPROC,      glBlendFuncSeparate)       \
    X(PFNGLBUFFERDATAPROC,             glBufferData)              \
    X(PFNGLCOMPILESHADERPROC,          glCompileShader)           \
    X(PFNGLCREATEPROGRAMPROC,          glCreateProgram)           \
    X(PFNGLCREATESHADERPROC,           glCreateShader)            \
    X(PFNGLDELETEBUFFERSPROC,          glDeleteBuffers)           \
    X(PFNGLDELETEPROGRAMPROC,          glDeleteProgram)           \
    X(PFNGLDELETESHADERPROC,           glDeleteShader)            \
    X(PFNGLDELETEVERTEXARRAYSPROC,     glDeleteVertexArrays)      \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC,glEnableVertexAttribArray) \
    X(PFNGLGENBUFFERSPROC,             glGenBuffers)              \
    X(PFNGLGENVERTEXARRAYSPROC,        glGenVertexArrays)         \
    X(PFNGLGETPROGRAMINFOLOGPROC,      glGetProgramInfoLog)       \
    X(PFNGLGETPROGRAMIVPROC,           glGetProgramiv)            \
    X(PFNGLGETSHADERINFOLOGPROC,       glGetShaderInfoLog)        \
    X(PFNGLGETSHADERIVPROC,            glGetShaderiv)             \
    X(PFNGLGETUNIFORMLOCATIONPROC,     glGetUniformLocation)      \
    X(PFNGLLINKPROGRAMPROC,            glLinkProgram)             \
    X(PFNGLSHADERSOURCEPROC,           glShaderSource)            \
    X(PFNGLUNIFORM1IPROC,              glUniform1i)               \
    X(PFNGLUNIFORM2FPROC,              glUniform2f)               \
    X(PFNGLUSEPROGRAMPROC,             glUseProgram)              \
    X(PFNGLVERTEXATTRIBPOINTERPROC,    glVertexAttribPointer)

#define LAT_GL_DECLARE(type, name) inline type name = nullptr;
LAT_GL_ENTRY_POINTS(LAT_GL_DECLARE)
#undef LAT_GL_DECLARE

namespace detail {

// wglGetProcAddress is only valid with a context current, and its failure
// value is not just NULL: some drivers (and the Microsoft software rasteriser)
// hand back 1, 2, 3 or -1. Treating those as function pointers is a classic
// way to get a crash that looks like a driver bug.
//
// The GetProcAddress fallback catches the opposite case: a few implementations
// export some 1.2/1.3-era entry points from opengl32.dll itself and return
// NULL for them from wglGetProcAddress.
inline void* glProc(HMODULE gl32, const char* name) {
    void* p = (void*)wglGetProcAddress(name);
    const INT_PTR v = (INT_PTR)p;
    if (v == 0 || v == 1 || v == 2 || v == 3 || v == -1)
        p = gl32 ? (void*)GetProcAddress(gl32, name) : nullptr;
    return p;
}

} // namespace detail

// Resolve everything. Call ONCE, with the real context current -- entry points
// fetched under one context are not portable to another on some drivers, and
// fetched with no context at all they are all null.
//
// Returns false and names the first missing function if the driver is short of
// what the renderer needs; the caller should treat that as "no usable GL",
// because the alternative is a null call on the first frame.
inline bool loadGLWin32(void (*logMissing)(const char*) = nullptr) {
    HMODULE gl32 = GetModuleHandleW(L"opengl32.dll");
    if (!gl32) gl32 = LoadLibraryW(L"opengl32.dll");

    bool ok = true;
#define LAT_GL_RESOLVE(type, name)                                  \
    name = (type)detail::glProc(gl32, #name);                       \
    if (!name) { ok = false; if (logMissing) logMissing(#name); }
    LAT_GL_ENTRY_POINTS(LAT_GL_RESOLVE)
#undef LAT_GL_RESOLVE
    return ok;
}

} // namespace lat

#endif // _WIN32
