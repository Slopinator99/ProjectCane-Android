#pragma once
// Force-included for every translation unit when CANE_GLES is defined
// (Android / OpenGL ES 3.2 builds). The game was written against desktop
// OpenGL 4.3 through glad; this header maps the handful of calls that do not
// exist in OpenGL ES onto their ES equivalents, so the renderer sources stay
// untouched. Everything else (VAOs, UBOs, SSBOs, sync objects, BaseVertex
// draws, multisample renderbuffers) is core in ES 3.2.
#ifdef CANE_GLES

#include <glad/glad.h>
#include <cstdint>

// --- Entry points whose ES names differ -----------------------------------
#undef glDepthRange
#undef glClearDepth
#undef glGetDoublev
#undef glDrawBuffer
#undef glPolygonMode
#undef glPointSize
#undef glDrawElementsInstancedBaseInstance

inline void cane_glDepthRange(double n, double f) { glDepthRangef((GLfloat)n, (GLfloat)f); }
inline void cane_glClearDepth(double d)           { glClearDepthf((GLfloat)d); }

// GL_DEPTH_RANGE is the only double query the game makes (two values).
inline void cane_glGetDoublev(GLenum pname, GLdouble* out)
{
	GLfloat tmp[4] = {};
	glGetFloatv(pname, tmp);
	const int n = (pname == GL_DEPTH_RANGE) ? 2 : 1;
	for (int i = 0; i < n; ++i) out[i] = tmp[i];
}

inline void cane_glDrawBuffer(GLenum mode) { glDrawBuffers(1, &mode); }

// Wireframe debug view only; ES has no polygon mode. Intentionally a no-op.
inline void cane_glPolygonMode(GLenum, GLenum) {}

// ES has no glPointSize: the point size is written by the vertex shader.
// blot.vert reads u_pointSize; tv.cpp pushes this value into it.
inline float& cane_pointSize() { static float s = 1.0f; return s; }
inline void cane_glPointSize(float v) { cane_pointSize() = v; }

// ES 3.2 core has no base-instance draw. The only user is the particle
// (blip) renderer whose per-instance attributes are locations 1..4, 64-byte
// stride, divisor 1, sourced from the currently bound GL_ARRAY_BUFFER. Shift
// the attribute pointers by baseInstance*stride instead.
inline void cane_glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type,
	const void* indices, GLsizei instances, GLuint baseInstance)
{
	constexpr GLsizei kStride = 64;
	const uintptr_t shift = (uintptr_t)baseInstance * kStride;
	for (GLuint a = 1; a <= 4; ++a)
		glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, kStride, (const void*)((a - 1) * 16 + shift));
	glDrawElementsInstanced(mode, count, type, indices, instances);
	if (baseInstance != 0)
		for (GLuint a = 1; a <= 4; ++a)
			glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, kStride, (const void*)((a - 1) * 16));
}

#define glDepthRange                         cane_glDepthRange
#define glClearDepth                         cane_glClearDepth
#define glGetDoublev                         cane_glGetDoublev
#define glDrawBuffer                         cane_glDrawBuffer
#define glPolygonMode                        cane_glPolygonMode
#define glPointSize                          cane_glPointSize
#define glDrawElementsInstancedBaseInstance  cane_glDrawElementsInstancedBaseInstance

#ifndef GL_POINT_SIZE
#define GL_POINT_SIZE 0x0B11
#endif
// glGetFloatv(GL_POINT_SIZE) is not valid in ES; answer from our shadow value.
#undef glGetFloatv
inline void cane_glGetFloatv(GLenum pname, GLfloat* out)
{
	if (pname == GL_POINT_SIZE) { *out = cane_pointSize(); return; }
	glad_glGetFloatv(pname, out);
}
#define glGetFloatv cane_glGetFloatv

// ---- Debug-only GL call profiler (CANE_DIAG): CPU time and call count per GL function ----
#ifdef CANE_DIAG
#include <chrono>
struct CaneGlCounters
{
	unsigned fboBinds = 0, clears = 0, blits = 0, draws = 0, programs = 0, texBinds = 0;
	unsigned bufUploads = 0, texUploads = 0, copies = 0, readbacks = 0, flushes = 0;
	unsigned long long bufBytes = 0;
};
inline CaneGlCounters& cane_gl() { static CaneGlCounters c; return c; }
enum CaneGlId {
	CGL_glBindFramebuffer,
	CGL_glClear,
	CGL_glBlitFramebuffer,
	CGL_glDrawElements,
	CGL_glDrawArrays,
	CGL_glDrawElementsBaseVertex,
	CGL_glDrawElementsInstanced,
	CGL_glUseProgram,
	CGL_glBindTexture,
	CGL_glActiveTexture,
	CGL_glTexSubImage2D,
	CGL_glTexImage2D,
	CGL_glCopyTexSubImage2D,
	CGL_glReadPixels,
	CGL_glFlush,
	CGL_glFinish,
	CGL_glBufferSubData,
	CGL_glBufferData,
	CGL_glBindBuffer,
	CGL_glBindBufferBase,
	CGL_glBindBufferRange,
	CGL_glBindVertexArray,
	CGL_glUniform1i,
	CGL_glUniform1f,
	CGL_glUniform2f,
	CGL_glUniform3f,
	CGL_glUniform4f,
	CGL_glUniform1fv,
	CGL_glUniformMatrix4fv,
	CGL_glEnable,
	CGL_glDisable,
	CGL_glDepthMask,
	CGL_glDepthFunc,
	CGL_glBlendFunc,
	CGL_glCullFace,
	CGL_glColorMask,
	CGL_glStencilFunc,
	CGL_glStencilOp,
	CGL_glStencilMask,
	CGL_glLineWidth,
	CGL_glViewport,
	CGL_glScissor,
	CGL_COUNT
};
inline const char* const* cane_gl_names()
{
	static const char* const n[] = {
	"glBindFramebuffer",
	"glClear",
	"glBlitFramebuffer",
	"glDrawElements",
	"glDrawArrays",
	"glDrawElementsBaseVertex",
	"glDrawElementsInstanced",
	"glUseProgram",
	"glBindTexture",
	"glActiveTexture",
	"glTexSubImage2D",
	"glTexImage2D",
	"glCopyTexSubImage2D",
	"glReadPixels",
	"glFlush",
	"glFinish",
	"glBufferSubData",
	"glBufferData",
	"glBindBuffer",
	"glBindBufferBase",
	"glBindBufferRange",
	"glBindVertexArray",
	"glUniform1i",
	"glUniform1f",
	"glUniform2f",
	"glUniform3f",
	"glUniform4f",
	"glUniform1fv",
	"glUniformMatrix4fv",
	"glEnable",
	"glDisable",
	"glDepthMask",
	"glDepthFunc",
	"glBlendFunc",
	"glCullFace",
	"glColorMask",
	"glStencilFunc",
	"glStencilOp",
	"glStencilMask",
	"glLineWidth",
	"glViewport",
	"glScissor" };
	return n;
}
struct CaneGlProf { unsigned long long ns[CGL_COUNT] = {}; unsigned cnt[CGL_COUNT] = {}; };
inline CaneGlProf& cane_prof() { static CaneGlProf p; return p; }
template <class F> inline void cane_timed(int id, F&& f)
{
	const auto t0 = std::chrono::steady_clock::now();
	f();
	cane_prof().ns[id] += (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
	++cane_prof().cnt[id];
}
#undef glBindFramebuffer
#undef glClear
#undef glBlitFramebuffer
#undef glDrawElements
#undef glDrawArrays
#undef glDrawElementsBaseVertex
#undef glDrawElementsInstanced
#undef glUseProgram
#undef glBindTexture
#undef glActiveTexture
#undef glTexSubImage2D
#undef glTexImage2D
#undef glCopyTexSubImage2D
#undef glReadPixels
#undef glFlush
#undef glFinish
#undef glBufferSubData
#undef glBufferData
#undef glBindBuffer
#undef glBindBufferBase
#undef glBindBufferRange
#undef glBindVertexArray
#undef glUniform1i
#undef glUniform1f
#undef glUniform2f
#undef glUniform3f
#undef glUniform4f
#undef glUniform1fv
#undef glUniformMatrix4fv
#undef glEnable
#undef glDisable
#undef glDepthMask
#undef glDepthFunc
#undef glBlendFunc
#undef glCullFace
#undef glColorMask
#undef glStencilFunc
#undef glStencilOp
#undef glStencilMask
#undef glLineWidth
#undef glViewport
#undef glScissor
#define glBindFramebuffer(...) cane_timed(CGL_glBindFramebuffer, [&]{ glad_glBindFramebuffer(__VA_ARGS__); })
#define glClear(...) cane_timed(CGL_glClear, [&]{ glad_glClear(__VA_ARGS__); })
#define glBlitFramebuffer(...) cane_timed(CGL_glBlitFramebuffer, [&]{ glad_glBlitFramebuffer(__VA_ARGS__); })
#define glDrawElements(...) cane_timed(CGL_glDrawElements, [&]{ glad_glDrawElements(__VA_ARGS__); })
#define glDrawArrays(...) cane_timed(CGL_glDrawArrays, [&]{ glad_glDrawArrays(__VA_ARGS__); })
#define glDrawElementsBaseVertex(...) cane_timed(CGL_glDrawElementsBaseVertex, [&]{ glad_glDrawElementsBaseVertex(__VA_ARGS__); })
#define glDrawElementsInstanced(...) cane_timed(CGL_glDrawElementsInstanced, [&]{ glad_glDrawElementsInstanced(__VA_ARGS__); })
#define glUseProgram(...) cane_timed(CGL_glUseProgram, [&]{ glad_glUseProgram(__VA_ARGS__); })
#define glBindTexture(...) cane_timed(CGL_glBindTexture, [&]{ glad_glBindTexture(__VA_ARGS__); })
#define glActiveTexture(...) cane_timed(CGL_glActiveTexture, [&]{ glad_glActiveTexture(__VA_ARGS__); })
#define glTexSubImage2D(...) cane_timed(CGL_glTexSubImage2D, [&]{ glad_glTexSubImage2D(__VA_ARGS__); })
#define glTexImage2D(...) cane_timed(CGL_glTexImage2D, [&]{ glad_glTexImage2D(__VA_ARGS__); })
#define glCopyTexSubImage2D(...) cane_timed(CGL_glCopyTexSubImage2D, [&]{ glad_glCopyTexSubImage2D(__VA_ARGS__); })
#define glReadPixels(...) cane_timed(CGL_glReadPixels, [&]{ glad_glReadPixels(__VA_ARGS__); })
#define glFlush(...) cane_timed(CGL_glFlush, [&]{ glad_glFlush(__VA_ARGS__); })
#define glFinish(...) cane_timed(CGL_glFinish, [&]{ glad_glFinish(__VA_ARGS__); })
// glBufferSubData is profiled per call (target, size, time) so the slow uploads can be named.
struct CaneBsdRec { unsigned long long ns; long long size; unsigned target; unsigned buffer; };
inline CaneBsdRec* cane_bsd_top() { static CaneBsdRec r[6] = {}; return r; }
inline void cane_glBufferSubData(GLenum t, GLintptr o, GLsizeiptr n, const void* d)
{
	GLint bound = 0;
	glad_glGetIntegerv(t == GL_UNIFORM_BUFFER ? GL_UNIFORM_BUFFER_BINDING : t == GL_ARRAY_BUFFER ? GL_ARRAY_BUFFER_BINDING : t == GL_ELEMENT_ARRAY_BUFFER ? GL_ELEMENT_ARRAY_BUFFER_BINDING : GL_SHADER_STORAGE_BUFFER_BINDING, &bound);
	const auto t0 = std::chrono::steady_clock::now();
	glad_glBufferSubData(t, o, n, d);
	const unsigned long long ns = (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
	cane_prof().ns[CGL_glBufferSubData] += ns;
	++cane_prof().cnt[CGL_glBufferSubData];
	CaneBsdRec* top = cane_bsd_top();
	int minI = 0;
	for (int i = 1; i < 6; ++i) if (top[i].ns < top[minI].ns) minI = i;
	if (ns > top[minI].ns) top[minI] = CaneBsdRec{ ns, (long long)n, (unsigned)t, (unsigned)bound };
}
#define glBufferSubData(...) cane_glBufferSubData(__VA_ARGS__)
#define glBufferData(...) cane_timed(CGL_glBufferData, [&]{ glad_glBufferData(__VA_ARGS__); })
#define glBindBuffer(...) cane_timed(CGL_glBindBuffer, [&]{ glad_glBindBuffer(__VA_ARGS__); })
#define glBindBufferBase(...) cane_timed(CGL_glBindBufferBase, [&]{ glad_glBindBufferBase(__VA_ARGS__); })
#define glBindBufferRange(...) cane_timed(CGL_glBindBufferRange, [&]{ glad_glBindBufferRange(__VA_ARGS__); })
#define glBindVertexArray(...) cane_timed(CGL_glBindVertexArray, [&]{ glad_glBindVertexArray(__VA_ARGS__); })
#define glUniform1i(...) cane_timed(CGL_glUniform1i, [&]{ glad_glUniform1i(__VA_ARGS__); })
#define glUniform1f(...) cane_timed(CGL_glUniform1f, [&]{ glad_glUniform1f(__VA_ARGS__); })
#define glUniform2f(...) cane_timed(CGL_glUniform2f, [&]{ glad_glUniform2f(__VA_ARGS__); })
#define glUniform3f(...) cane_timed(CGL_glUniform3f, [&]{ glad_glUniform3f(__VA_ARGS__); })
#define glUniform4f(...) cane_timed(CGL_glUniform4f, [&]{ glad_glUniform4f(__VA_ARGS__); })
#define glUniform1fv(...) cane_timed(CGL_glUniform1fv, [&]{ glad_glUniform1fv(__VA_ARGS__); })
#define glUniformMatrix4fv(...) cane_timed(CGL_glUniformMatrix4fv, [&]{ glad_glUniformMatrix4fv(__VA_ARGS__); })
#define glEnable(...) cane_timed(CGL_glEnable, [&]{ glad_glEnable(__VA_ARGS__); })
#define glDisable(...) cane_timed(CGL_glDisable, [&]{ glad_glDisable(__VA_ARGS__); })
#define glDepthMask(...) cane_timed(CGL_glDepthMask, [&]{ glad_glDepthMask(__VA_ARGS__); })
#define glDepthFunc(...) cane_timed(CGL_glDepthFunc, [&]{ glad_glDepthFunc(__VA_ARGS__); })
#define glBlendFunc(...) cane_timed(CGL_glBlendFunc, [&]{ glad_glBlendFunc(__VA_ARGS__); })
#define glCullFace(...) cane_timed(CGL_glCullFace, [&]{ glad_glCullFace(__VA_ARGS__); })
#define glColorMask(...) cane_timed(CGL_glColorMask, [&]{ glad_glColorMask(__VA_ARGS__); })
#define glStencilFunc(...) cane_timed(CGL_glStencilFunc, [&]{ glad_glStencilFunc(__VA_ARGS__); })
#define glStencilOp(...) cane_timed(CGL_glStencilOp, [&]{ glad_glStencilOp(__VA_ARGS__); })
#define glStencilMask(...) cane_timed(CGL_glStencilMask, [&]{ glad_glStencilMask(__VA_ARGS__); })
#define glLineWidth(...) cane_timed(CGL_glLineWidth, [&]{ glad_glLineWidth(__VA_ARGS__); })
#define glViewport(...) cane_timed(CGL_glViewport, [&]{ glad_glViewport(__VA_ARGS__); })
#define glScissor(...) cane_timed(CGL_glScissor, [&]{ glad_glScissor(__VA_ARGS__); })
inline void cane_glBufferSubData_unused() {}
#endif // CANE_DIAG


#endif // CANE_GLES
