#pragma once
#ifndef CANE_GL_H
#define CANE_GL_H
#include "glshaders.h"
#include "rocel.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/rotate_vector.hpp>
#include <glm/gtx/vector_angle.hpp>
#include <glm/gtx/string_cast.hpp>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"
#include "imgui/ImGuiFileDialog.h"
#include "mouse.h"
#include <vector>

enum AspectMode
{
	FitToScreen,
	Fixed_4_3,
	Fixed_16_9,
	Fixed_16_10,
	PS2_4_3,
	PS2_16_9,
};

enum WindowMode
{
	WindowMode_Windowed,
	WindowMode_Borderless,
	WindowMode_Fullscreen
};

enum GuiStyle
{
	GuiStyle_PS2,
	GuiStyle_Modern
};

struct CMGL
{
	glm::mat4 matWorldToClip;
	glm::vec4 cameraPos;
};

struct alignas(16) ROGL
{
	glm::mat4 model;         //   0 -  63
	float     uAlpha;        //  64 -  67
	float     uFog;          //  68 -  71
	float     darken;        //  72 -  75
	int32_t   grfglob;       //  76 -  79
	// Zero for ordinary world globs. TV portrait globs store their dedicated
	// TV-light SSBO index plus one (1 = left, 2 = right).
	int32_t   blotTvLight;   //  80 -  83
	int32_t   warpType;      //  84 -  87
	int32_t   warpCmat;      //  88 -  91
	int32_t   warpCvtx;      //  92 -  95
	glm::mat4 amatDpos[4];   //  96 - 351
	glm::mat4 amatDuv[4];    // 352 - 607
	int32_t   fDynamic;      // 608 - 611
	float     sRadius;       // 612 - 615
	int32_t   fDynamicLight; // 616 - 619
	int32_t   trlk;          // 620 - 623
	glm::vec4 posCenter;     // 624 - 639
	// Vertex-SSBO fallback only: static lights that can reach this draw (built on the CPU,
	// superset of the shader's own per-light tests, so the lighting result is unchanged).
	// relightHdr[0] = number of indices + 1; 0 means "not built, scan the whole table".
	int32_t   relightHdr[4] = { 0, 0, 0, 0 };   // 640 - 655
	int32_t   relightIdx[28] = {};              // 656 - 767
};
static_assert(sizeof(ROGL) == 768, "ROGL must stay std140-compatible with the RO uniform block");

// Per-group BLIP parameters. One aligned range is streamed for every queued
// BLIP group, replacing several individual glUniform updates.
struct alignas(16) BLIPGROUPGPU
{
	glm::mat4 model;
	glm::vec4 params;   // groupAlpha, rSFlying, blipMode, unused
	glm::vec4 clqScale; // g0, g1, g2, unused
	glm::vec4 clqAlpha; // g0, g1, g2, unused
};

struct STREAM
{
	GLuint bufferObject;
	GLuint bindIndex;
	GLsizeiptr frameBase;
	GLsizeiptr cursor;
	GLsizeiptr stride; // use GLsizeiptr, not GLuint
	GLsizeiptr frameSize; // bytes in this frame slice
	uint8_t* mappedPtr;
	GLsync* fences; // length = g_frames
	std::vector<uint8_t> staging;
	// Records appended mid-frame (AppendStream) live in this small private buffer. Writing them into
	// bufferObject -- which hundreds of earlier draws of the same frame have already read -- made the
	// Adreno driver duplicate/stall on the whole ring (~9 ms for a 768-byte write).
	// One buffer object per frame slot (bufferObject always points at the current slot's buffer). A slot is
	// only written after its fence says the GPU is done with it, so the driver never sees a write to a
	// buffer that is still in use -- one shared multi-frame buffer made Adreno copy/stall 5-9 ms per upload.
	GLuint slotBuffers[8] = {};
	GLsizeiptr totalSize = 0;
	GLuint appendBuffer = 0;
	GLsizeiptr appendCursor = 0;
	GLsizeiptr appendSize = 0;
};

struct BLOTLINEVERTEX
{
	glm::vec3 position;
	glm::vec2 uv;
	glm::vec4 color;
};

class GL
{
	public:
	// Window Object
	GLFWwindow* window;
	// Frame Buffer Object
	GLuint fbo;
	// Frame Buffer Color
	GLuint fbc;
	// Render Buffer Object
	GLuint rbo;

	GLuint dyshFbo;
	GLuint dyshFbc;
	GLuint dyshRbo;

	int dyshWidth;
	int dyshHeight;

	GLuint fboMSAA;
	GLuint rboColorMSAA;
	GLuint rboDepthStencilMSAA;

	// Screen Array Object
	GLuint sao;
	// Screen Buffer Object
	GLuint sbo;
	glm::mat4 screenProjection;

	// Text Array Object
	GLuint gao;
	// Text Buffer Object
	GLuint gbo;
	GLuint geo;
	// Dynamic glyph geometry. Text is expanded into screen-space triangles so
	// an entire font/texture run can be submitted with one draw call.
	GLuint fontVao;
	GLuint fontVbo;
	GLsizeiptr fontVboCapacity;

	// Instanced particle billboard geometry.
	GLuint blipVao;
	GLuint blipVbo;
	GLuint blipEbo;
	GLuint blipInstanceVbo;
	GLsizeiptr blipInstanceCapacity;
	GLuint sqtrVao;
	GLuint sqtrVbo;
	glm::mat4 blotProjection;

	// Window width
	float width;
	// Window height
	float height;
	// Pixel dimensions of the off-screen 3D render target. These may differ
	// from width/height when internal resolution scaling is enabled.
	int renderWidth = 1;
	int renderHeight = 1;
	int outputWidth = 1;
	int outputHeight = 1;
	int presentX = 0;
	int presentY = 0;
	int presentWidth = 1;
	int presentHeight = 1;
	// Uniform transform from the original 640 x 492.8 UI canvas into the
	// current framebuffer. Updated by FrameBufferSizeCallBack.
	float uiScale = 1.0f;
	glm::vec2 uiOrigin = glm::vec2(0.0f);
	glm::vec2 screenOffset;
	glm::vec2 screenOffsetOriginal;

	float aspectRatio;
	AspectMode aspectMode;

	void InitGL();
	void CreateFramebuffers(int w, int h);
	bool ResizeFramebuffers(int w, int h);
	void UpdateGLProjections();
	void TerminateGL();
};

void InitCameraUbo();
void InitFrameStream(STREAM* pstream, int streamSize);
void BeginFrameStream(STREAM* pstream);
GLsizeiptr StageFrameStream(STREAM* pstream, const void* ptr, int copySize);
void UploadFrameStream(STREAM* pstream);
void BindFrameStreamRange(STREAM* pstream, GLsizeiptr offset, int size);
void AppendStream(STREAM* pstream, void* ptr, int size, int copySize);
void EndFrameStream(STREAM* pstream);
void DeleteFrameStream(STREAM* pstream);
bool ApplyMsaaSettings();
bool ApplyInternalResolutionSettings();
bool ApplyWindowModeSettings(WindowMode mode);
bool ApplyMonitorSettings(int monitorIndex);
int VideoMonitorCount();
const char* VideoMonitorName(int monitorIndex);
int VideoMonitorIndexByName(const char* name);
void ApplyOutputGammaSettings(float gamma);
void ApplyAutoIconifySettings(bool enabled);
bool ApplyAspectRatioSettings(AspectMode mode);
void FrameBufferSizeCallBack(GLFWwindow* window, int width, int height);

extern GL g_gl;
extern GLuint cmUBO;
void UpdateCmUbo(const void* data);
extern STREAM ropStream;
extern STREAM rcbStream;
extern STREAM blipStream;
extern GLuint geomUBO;
extern GLint glslBlipCurrentTime;
extern GLint glslBlipDtFrame;
extern GLint glslBlipCameraMat;
extern GLint glslBlipAlphaPass;
extern GLuint glslLsmShadow;
extern GLuint glslLsmDiffuse;
extern GLuint glslFogType;
extern GLuint glslFogNear;
extern GLuint glslFogFar;
extern GLuint glslFogMax;
extern GLuint glslFogColor;
extern GLuint glslfAlphaTest;
extern GLuint glslAlphaCutOff;
extern GLuint glslRko;
extern GLuint glslProjectedVolumeColor;
extern GLuint glslProjectedVolumeFinalPass;
extern GLuint glslfAnimateUv;
extern GLuint glsluvOffsets;
extern GLuint glslUnSelfIllum;
extern GLuint glslSubGlobPosCenter;
extern GLuint glslSubGlobRadius;
extern GLuint glslDyshMatWorldClip;
extern GLuint glslDyshModel;
extern GLuint glslDyshfSkin;
extern GLuint glslAmbientMap;
extern GLuint glslDiffuseMap;
extern GLuint glslSaturateMap;
extern GLuint glslGeomModelToClip;
extern GLuint glslGeomDebugColor;
extern GLuint u_fontTexLoc;
extern GLuint glslfCull;
extern uint64_t screenTextureHandle;
extern GLuint g_sceneFbo;
extern int g_msaaSamples;
extern bool g_fMsaa;
extern bool g_fVsync;
extern int g_internalResolutionHeight;
extern int g_resolutionScalePercent;
extern int g_monitorIndex;
extern int g_textureFiltering;
extern float g_outputGamma;
extern float g_guiScale;
extern GuiStyle g_guiStyle;
extern WindowMode g_windowMode;
extern bool g_fAutoIconify;
extern float g_drawDistanceMultiplier;
extern int g_frames;
extern float s_dxDisplay;
extern float s_dyDisplay;
extern float s_dxDisplayOriginal;
extern float s_dyDisplayOriginal;
extern GLuint lineVao;
extern GLuint lineVbo;
extern GLuint glslfSkin;
extern GLuint glslCaneBake;
extern GLint  glslCelSkin;
extern GLuint glslfPose;
extern GLuint glslPoseCount;
extern GLuint glslPoseWeights;
extern GLuint glslCelPose;
extern GLuint glslCelPoseCount;
extern GLuint glslCelPoseWeights;

#endif
