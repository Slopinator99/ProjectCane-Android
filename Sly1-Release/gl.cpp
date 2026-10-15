#include "gl.h"
#include "render.h"
#include "sqtr.h"
#include "shadow.h"
#include "tv.h"
#include "game.h"
#include "debug.h"
#include "runtime_error.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#ifdef _WIN32
extern "C"
{
	__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace
{
int s_windowedX = 100;
int s_windowedY = 100;
int s_windowedWidth = 800;
int s_windowedHeight = 800;
bool s_lastFramebufferResizeSucceeded = true;
double s_autoIconifySuppressedUntil = 0.0;

GLFWmonitor* MonitorByIndex(int index)
{
	int count = 0;
	GLFWmonitor** monitors = glfwGetMonitors(&count);
	if (monitors == nullptr || count == 0)
		return glfwGetPrimaryMonitor();
	return monitors[(std::clamp)(index, 0, count - 1)];
}

void GlfwErrorCallback(int error, const char* description)
{
	ReportRuntimeError("Graphics", "GLFW error %d: %s", error,
		description != nullptr ? description : "No description was provided");
}

void WindowFocusCallback(GLFWwindow* window, int focused)
{
	if (focused == GLFW_FALSE && g_fAutoIconify &&
		glfwGetTime() >= s_autoIconifySuppressedUntil &&
		glfwGetWindowAttrib(window, GLFW_ICONIFIED) == GLFW_FALSE)
	{
		// GLFW_AUTO_ICONIFY only covers exclusive fullscreen windows. Apply the
		// user-facing setting explicitly so windowed and borderless modes behave
		// the same way when the game loses focus.
		glfwIconifyWindow(window);
	}
}

GLFWmonitor* MonitorForWindow(GLFWwindow* window)
{
	if (GLFWmonitor* monitor = glfwGetWindowMonitor(window))
		return monitor;

	int windowX = 0;
	int windowY = 0;
	int windowWidth = 0;
	int windowHeight = 0;
	glfwGetWindowPos(window, &windowX, &windowY);
	glfwGetWindowSize(window, &windowWidth, &windowHeight);

	GLFWmonitor* bestMonitor = glfwGetPrimaryMonitor();
	int bestOverlap = -1;
	int monitorCount = 0;
	GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
	for (int i = 0; i < monitorCount; ++i)
	{
		int monitorX = 0;
		int monitorY = 0;
		int monitorWidth = 0;
		int monitorHeight = 0;
		glfwGetMonitorWorkarea(monitors[i], &monitorX, &monitorY, &monitorWidth, &monitorHeight);
		const int overlapWidth = (std::max)(0,
			(std::min)(windowX + windowWidth, monitorX + monitorWidth) - (std::max)(windowX, monitorX));
		const int overlapHeight = (std::max)(0,
			(std::min)(windowY + windowHeight, monitorY + monitorHeight) - (std::max)(windowY, monitorY));
		const int overlap = overlapWidth * overlapHeight;
		if (overlap > bestOverlap)
		{
			bestOverlap = overlap;
			bestMonitor = monitors[i];
		}
	}

	return bestMonitor;
}
}

// §8.1 vertex-SSBO fallback flag (gl.h: see glob.h for the full description).
// Set by the gate in InitGL; only ever becomes true under CANE_GLES.
bool g_fVertexUboFallback = false;
// Run the lighting bakes in a pre-pass before the scene draws. Needed on Adreno (interleaved transform-feedback
// draws make it render one frame wrongly now and then); the Mali driver crashes in that pre-pass and does not
// have the problem, so it keeps baking inside the passes.
bool g_fBakePrepass = false;
extern GLint g_glslCmWorldToClip;
extern GLint g_glslCmCameraPos;
extern const char* const* g_tfVaryings;
extern int g_tfVaryingCount;

namespace
{
// Texel layout for the fallback data textures: one RGBA texel per vec4/uvec4
// element; mat4 matrices use a 4-wide texture (texel x = column, y = matrix)
// so textureSize().y is the exact matrix count.
void FallbackTexDims(FALLBACKTEXFMT fmt, size_t elements, int& width, int& height)
{
	if (fmt == FALLBACKTEX_MAT4)
	{
		width = 4;
		height = static_cast<int>(elements);
		return;
	}

	const int texels = static_cast<int>(elements);
	width = (texels < 1024) ? texels : 1024;
	height = (texels + width - 1) / width;
}
}

// Bindings are saved/restored around creation and upload so model-load calls
// cannot disturb render.cpp's per-unit texture cache.
void AllocFallbackTex(GLuint tex, FALLBACKTEXFMT fmt, size_t elements)
{
	int width = 0;
	int height = 0;
	FallbackTexDims(fmt, elements, width, height);
	if (tex == 0 || width <= 0 || height <= 0)
		return;

	const GLenum ifmt = (fmt == FALLBACKTEX_UVEC4) ? GL_RGBA32UI : GL_RGBA32F;
	GLint prevTexture = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexStorage2D(GL_TEXTURE_2D, 1, ifmt, width, height);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTexture));
}

void UpdateFallbackTex(GLuint tex, FALLBACKTEXFMT fmt, size_t elements, const void* data)
{
	int width = 0;
	int height = 0;
	FallbackTexDims(fmt, elements, width, height);
	if (tex == 0 || width <= 0 || height <= 0 || data == nullptr)
		return;

	// `format` is the pixel layout (GL_RGBA / GL_RGBA_INTEGER), `type` the component
	// type. GL_FLOAT is not a valid format: it raised GL_INVALID_ENUM and left every
	// float data texture (bones, edges, pose deltas, warp state) all zeros.
	const GLenum format = (fmt == FALLBACKTEX_UVEC4) ? GL_RGBA_INTEGER : GL_RGBA;
	const GLenum type = (fmt == FALLBACKTEX_UVEC4) ? GL_UNSIGNED_INT : GL_FLOAT;

	// Storage can exceed the source (VEC4/UVEC4 rows are capped at 1024
	// texels, leaving up to 1023 padding texels); zero-fill the tail so the
	// upload never reads past the source array and the shader's bounds
	// checks see zeros, not garbage. A MAT4 stores 4 texels per matrix, so
	// its storage always matches its source exactly.
	const size_t texels = static_cast<size_t>(width) * static_cast<size_t>(height);
	const size_t texelBytes = 4 * sizeof(float);
	const size_t srcElemBytes = (fmt == FALLBACKTEX_MAT4) ? (4 * texelBytes) : texelBytes;
	const size_t srcBytes = elements * srcElemBytes;
	const size_t storageBytes = texels * texelBytes;
	const void* upload = data;
	std::vector<unsigned char> padded;
	if (srcBytes < storageBytes)
	{
		padded.resize(storageBytes, 0);
		std::memcpy(padded.data(), data, srcBytes);
		upload = padded.data();
	}

	GLint prevTexture = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, upload);
#ifdef CANE_DIAG
	// A bad upload leaves the texture zeroed and nothing else complains.
	if (const GLenum err = glGetError(); err != GL_NO_ERROR)
		std::printf("[Graphics] UpdateFallbackTex fmt=%d elements=%zu failed: glGetError=0x%04X\n",
			static_cast<int>(fmt), elements, static_cast<unsigned>(err));
#endif
	glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTexture));
}

GLuint CreateFallbackTex(FALLBACKTEXFMT fmt, size_t elements, const void* data)
{
	if (elements == 0)
		return 0;

	GLuint tex = 0;
	glGenTextures(1, &tex);
	AllocFallbackTex(tex, fmt, elements);
	UpdateFallbackTex(tex, fmt, elements, data);
	return tex;
}

void DeleteFallbackTex(GLuint& tex)
{
	if (tex != 0)
		glDeleteTextures(1, &tex);

	tex = 0;
}

void GL::InitGL()
{
	// Create GLFW context and window
	glfwSetErrorCallback(GlfwErrorCallback);
	if (glfwInit() != GLFW_TRUE)
		throw std::runtime_error("GLFW could not initialize. See the diagnostic console for details.");
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_AUTO_ICONIFY, g_fAutoIconify ? GLFW_TRUE : GLFW_FALSE);

	width  = 1280;
	height = 720;
	renderWidth = static_cast<int>(width);
	renderHeight = static_cast<int>(height);
	outputWidth = static_cast<int>(width);
	outputHeight = static_cast<int>(height);
	presentX = 0;
	presentY = 0;
	presentWidth = outputWidth;
	presentHeight = outputHeight;

	aspectRatio = float(width) / float(height);
	aspectMode = FitToScreen;

	window = glfwCreateWindow(width, height, "Sly Cooper and the Thievius Raccoonus", nullptr, nullptr);

	if (!window)
	{
		const char* description = nullptr;
		const int error = glfwGetError(&description);
		ReportRuntimeError("Graphics", "Could not create the OpenGL 4.3 window (GLFW error %d: %s)",
			error, description != nullptr ? description : "No description was provided");
		glfwTerminate();
		throw std::runtime_error(
			"The game window could not be created. Update the graphics driver and verify OpenGL 4.3 support.");
	}

	glfwMakeContextCurrent(window);
	glfwSetFramebufferSizeCallback(window, FrameBufferSizeCallBack);
	glfwSetWindowFocusCallback(window, WindowFocusCallback);
	glfwSetCursorPosCallback(window, MOUSE::CursorPosCallback);
	glfwSetMouseButtonCallback(window, MOUSE::MouseButtonCallback);
	glfwSetScrollCallback(window, MOUSE::MouseWheelCallback);

	if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
	{
		ReportRuntimeError("Graphics",
			"GLAD could not load the required OpenGL functions. Update the graphics driver and verify OpenGL 4.3 support.");
		throw std::runtime_error("OpenGL function loading failed.");
	}

	// GLFW window dimensions are logical screen coordinates and can differ
	// from the drawable pixel dimensions on a DPI-scaled display. Initialize
	// every framebuffer-related dimension from the actual drawable size so the
	// first frame does not depend on receiving a later resize callback.
	int framebufferWidth = 0;
	int framebufferHeight = 0;
	glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
	framebufferWidth = std::max(1, framebufferWidth);
	framebufferHeight = std::max(1, framebufferHeight);
	width = static_cast<float>(framebufferWidth);
	height = static_cast<float>(framebufferHeight);
	renderWidth = framebufferWidth;
	renderHeight = framebufferHeight;
	outputWidth = framebufferWidth;
	outputHeight = framebufferHeight;
	presentX = 0;
	presentY = 0;
	presentWidth = framebufferWidth;
	presentHeight = framebufferHeight;
	aspectRatio = width / height;

	const char* glVendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
	const char* glRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
	const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
	std::cout << "OpenGL vendor: " << (glVendor != nullptr ? glVendor : "unknown") << '\n'
		<< "OpenGL renderer: " << (glRenderer != nullptr ? glRenderer : "unknown") << '\n'
		<< "OpenGL version: " << (glVersion != nullptr ? glVersion : "unknown") << std::endl;

	// The main renderer intentionally uses vertex-stage SSBOs for lights,
	// shadows, skinning, poses, warps, and cached lighting. OpenGL 4.3 exposes
	// implementation-dependent per-stage limits, so reject an incapable driver
	// explicitly instead of letting glob.vert fail later with a vendor-specific
	// linker message or silently missing effects.
	GLint maxVertexSsboBlocks = 0;
	GLint maxCombinedSsboBlocks = 0;
	GLint maxSsboBindings = 0;
	GLint maxVertexOutputComponents = 0;
	glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &maxVertexSsboBlocks);
	glGetIntegerv(GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS, &maxCombinedSsboBlocks);
	glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &maxSsboBindings);
	glGetIntegerv(GL_MAX_VERTEX_OUTPUT_COMPONENTS, &maxVertexOutputComponents);

	constexpr GLint kRequiredVertexSsboBlocks = 10;
	constexpr GLint kRequiredCombinedSsboBlocks = 11;
	constexpr GLint kRequiredSsboBindings = 14;
	constexpr GLint kRequiredVertexOutputComponents = 48;

#ifdef CANE_GLES
	// §8.1 fallback: GLES drivers such as Mali report zero vertex-stage SSBO
	// blocks and their compilers reject any vertex shader that declares one.
	// Re-route the vertex stages through std140 uniform blocks + data textures
	// when the replacement resources exist: 7 vertex UBO blocks (cm, RO,
	// lights, active lights, shadows, active shadows, TV lights), binding
	// points up to 13, SHADOWBLK is the largest block (16 + 255*224 = 57136
	// bytes) and data textures live on vertex units 4..8.
	if (maxVertexSsboBlocks < kRequiredVertexSsboBlocks)
	{
		GLint maxVertexUboBlocks = 0;
		GLint maxUboBindings = 0;
		GLint maxUboBlockSize = 0;
		GLint maxVertexTextureUnits = 0;
		GLint maxTextureSize = 0;
		glGetIntegerv(GL_MAX_VERTEX_UNIFORM_BLOCKS, &maxVertexUboBlocks);
		glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &maxUboBindings);
		glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &maxUboBlockSize);
		glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &maxVertexTextureUnits);
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);

		constexpr GLint kRequiredVertexUboBlocks = 8;
		constexpr GLint kRequiredUboBindings = 14;
		constexpr GLint kRequiredUboBlockSize = 57136;
		constexpr GLint kRequiredVertexTextureUnits = 9;
		constexpr GLint kRequiredTextureSize = 1024;

		if (maxVertexOutputComponents >= kRequiredVertexOutputComponents &&
			maxVertexUboBlocks >= kRequiredVertexUboBlocks &&
			maxUboBindings >= kRequiredUboBindings &&
			maxUboBlockSize >= kRequiredUboBlockSize &&
			maxVertexTextureUnits >= kRequiredVertexTextureUnits &&
			maxTextureSize >= kRequiredTextureSize)
		{
			g_fVertexUboFallback = true;
			g_fBakePrepass = glRenderer != nullptr && std::strstr(glRenderer, "Adreno") != nullptr;
			std::cout << "[Graphics] GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = "
				<< maxVertexSsboBlocks << " (< " << kRequiredVertexSsboBlocks
				<< "); engaging the vertex-SSBO fallback (lights/shadows -> "
				<< "std140 UBOs, per-mesh arrays -> data textures, cached "
				<< "lighting -> per-draw recompute)." << std::endl;
		}
	}
#endif

	if (!g_fVertexUboFallback &&
		(maxVertexSsboBlocks < kRequiredVertexSsboBlocks ||
		maxCombinedSsboBlocks < kRequiredCombinedSsboBlocks ||
		maxSsboBindings < kRequiredSsboBindings ||
		maxVertexOutputComponents < kRequiredVertexOutputComponents))
	{
		ReportRuntimeError("Graphics",
			"GPU shader limits are below ProjectCane's renderer requirements. "
			"Vertex SSBOs %d/%d, combined SSBOs %d/%d, SSBO bindings %d/%d, "
			"vertex outputs %d/%d. Update the graphics driver or select a more capable GPU.",
			maxVertexSsboBlocks, kRequiredVertexSsboBlocks,
			maxCombinedSsboBlocks, kRequiredCombinedSsboBlocks,
			maxSsboBindings, kRequiredSsboBindings,
			maxVertexOutputComponents, kRequiredVertexOutputComponents);
		throw std::runtime_error("GPU shader resource limits are insufficient.");
	}

	// ========== ImGui Setup ==========
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	ImGui_ImplGlfw_InitForOpenGL(window, true);
	#ifdef CANE_GLES
	ImGui_ImplOpenGL3_Init("#version 300 es");
#else
	ImGui_ImplOpenGL3_Init("#version 430");
#endif
	ImGui::StyleColorsDark();

	const float imguiOffset = g_fDebugGuiOpen ? ImGui::GetFrameHeight() : 0.0f;
	const float initialSceneHeight = std::max(1.0f, height - imguiOffset);
	uiScale = std::min(width / 640.0f, initialSceneHeight / 492.80002f);
	uiOrigin.x = (width - 640.0f * uiScale) * 0.5f;
	uiOrigin.y = (initialSceneHeight - 492.80002f * uiScale) * 0.5f;

	// ========== Framebuffer setup ==========
	CreateFramebuffers(width, height);

	// ========== Screen Quad ==========
	float screen[] = {
		// pos      // tex
		 1.0f, -1.0f,  1.0f, 0.0f,
		-1.0f, -1.0f,  0.0f, 0.0f,
		-1.0f,  1.0f,  0.0f, 1.0f,

		 1.0f,  1.0f,  1.0f, 1.0f,
		 1.0f, -1.0f,  1.0f, 0.0f,
		-1.0f,  1.0f,  0.0f, 1.0f
	};

	glGenVertexArrays(1, &sao);
	glGenBuffers(1, &sbo);

	glBindVertexArray(sao);
	glBindBuffer(GL_ARRAY_BUFFER, sbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(screen), screen, GL_STATIC_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

	glBindVertexArray(0);

	// ========== Batched font geometry ==========
	fontVboCapacity = sizeof(BLOTLINEVERTEX) * 6 * 256;
	glGenVertexArrays(1, &fontVao);
	glGenBuffers(1, &fontVbo);
	glBindVertexArray(fontVao);
	glBindBuffer(GL_ARRAY_BUFFER, fontVbo);
	glBufferData(GL_ARRAY_BUFFER, fontVboCapacity, nullptr, GL_STREAM_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BLOTLINEVERTEX),
		reinterpret_cast<void*>(offsetof(BLOTLINEVERTEX, position)));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(BLOTLINEVERTEX),
		reinterpret_cast<void*>(offsetof(BLOTLINEVERTEX, uv)));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(BLOTLINEVERTEX),
		reinterpret_cast<void*>(offsetof(BLOTLINEVERTEX, color)));
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// ========== Instanced BLIP quad ==========
	const float blipCorners[] = {
		-1.0f, -1.0f,
		 1.0f, -1.0f,
		 1.0f,  1.0f,
		-1.0f,  1.0f
	};
	const uint16_t blipIndices[] = { 0, 1, 2, 0, 2, 3 };

	glGenVertexArrays(1, &blipVao);
	glGenBuffers(1, &blipVbo);
	glGenBuffers(1, &blipEbo);
	glGenBuffers(1, &blipInstanceVbo);
	glBindVertexArray(blipVao);

	glBindBuffer(GL_ARRAY_BUFFER, blipVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(blipCorners), blipCorners, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, blipEbo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(blipIndices), blipIndices, GL_STATIC_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, blipInstanceVbo);
	blipInstanceCapacity = 64 * 384;
	glBufferData(GL_ARRAY_BUFFER, blipInstanceCapacity, nullptr, GL_STREAM_DRAW);
	for (GLuint attribute = 1; attribute <= 4; ++attribute)
	{
		glEnableVertexAttribArray(attribute);
		glVertexAttribPointer(attribute, 4, GL_FLOAT, GL_FALSE, 64,
			reinterpret_cast<void*>(static_cast<uintptr_t>((attribute - 1) * 16)));
		glVertexAttribDivisor(attribute, 1);
	}
	glBindVertexArray(0);

	// ========== SQTR ribbon trail ==========
	glGenVertexArrays(1, &sqtrVao);
	glGenBuffers(1, &sqtrVbo);
	glBindVertexArray(sqtrVao);
	glBindBuffer(GL_ARRAY_BUFFER, sqtrVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(SQTRGPU) * 64 * 2, nullptr, GL_STREAM_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SQTRGPU),
		reinterpret_cast<void*>(offsetof(SQTRGPU, pos)));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(SQTRGPU),
		reinterpret_cast<void*>(offsetof(SQTRGPU, rgba)));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(SQTRGPU),
		reinterpret_cast<void*>(offsetof(SQTRGPU, uv)));
	glBindVertexArray(0);

	glViewport(0, 0, width, height - imguiOffset);

	UpdateGLProjections();

	float quadVertices[] = {
		// x, y,        u, v
		 0.0f, 0.0f,	0.0f, 0.0f, // top-left
		 1.0f, 0.0f,	1.0f, 0.0f, // top-right
		 1.0f, 1.0f,	1.0f, 1.0f, // bottom-right
		 0.0f, 1.0f,	0.0f, 1.0f  // bottom-left
	};

	uint16_t indices[] = {
		0,1,2, 0,2,3
	};

	glGenVertexArrays(1, &gao);
	glGenBuffers(1, &gbo);

	glBindVertexArray(gao);
	glBindBuffer(GL_ARRAY_BUFFER, gbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

	glGenBuffers(1, &geo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

	glBindVertexArray(0);

	blotProjection = glm::ortho(0.0f, float(width), float(height - imguiOffset), 0.0f, -1.0f, 1.0f);

	glScreenShader.Init("screen.vert", NULL, "screen.frag");
	glScreenShader.Use();
	glUniform1i(glGetUniformLocation(glScreenShader.ID, "screenTexture"), 0);
	glUniform1f(glGetUniformLocation(glScreenShader.ID, "outputGamma"), g_outputGamma);

	glDyshadow.Init("dysh.vert", NULL, "dysh.frag");
	glDyshadow.Use();
	
	glslDyshMatWorldClip = glGetUniformLocation(glDyshadow.ID, "matWorldToClip");
	glslDyshModel        = glGetUniformLocation(glDyshadow.ID, "model");
	glslDyshfSkin        = glGetUniformLocation(glDyshadow.ID, "fSkin");

	// Vertex-SSBO fallback: data-texture sampler units (fixed per program so
	// BindShaderStorageBuffer can route its index->unit mapping blindly).
	if (g_fVertexUboFallback)
		glUniform1i(glGetUniformLocation(glDyshadow.ID, "bonesTex"), 5);

	{
		// Fallback only: the static-lighting bake captures these vertex outputs (36 bytes per vertex).
		static const char* const kBakeVaryings[] = { "vMatAmbient", "vMatMidtone", "vMatLight" };
		if (g_fVertexUboFallback)
		{
			g_tfVaryings = kBakeVaryings;
			g_tfVaryingCount = 3;
		}
		glGlobShader.Init("glob.vert", NULL, "glob.frag");
		g_tfVaryings = nullptr;
		g_tfVaryingCount = 0;
	}
	ropStream.bindIndex = 1;
	GLint align = 256;
	glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);

	ropStream.stride = (sizeof(ROGL) + align - 1) & ~(align - 1);
	glGlobShader.Use();

	glslLsmShadow  = glGetUniformLocation(glGlobShader.ID, "swp.uShadow");
	glslLsmDiffuse = glGetUniformLocation(glGlobShader.ID, "swp.uMidtone");
	glslFogType    = glGetUniformLocation(glGlobShader.ID, "swp.fogType");
	glslFogNear    = glGetUniformLocation(glGlobShader.ID, "swp.fogNear");
	glslFogFar     = glGetUniformLocation(glGlobShader.ID, "swp.fogFar");
	glslFogMax     = glGetUniformLocation(glGlobShader.ID, "swp.fogMax");
	glslFogColor   = glGetUniformLocation(glGlobShader.ID, "swp.fogColor");

	glslfAlphaTest  = glGetUniformLocation(glGlobShader.ID, "fAlphaTest");
	glslAlphaCutOff = glGetUniformLocation(glGlobShader.ID, "alphaCutOff");

	glslRko              = glGetUniformLocation(glGlobShader.ID, "rko");
	glslProjectedVolumeColor = glGetUniformLocation(glGlobShader.ID, "projectedVolumeColor");
	glslProjectedVolumeFinalPass = glGetUniformLocation(glGlobShader.ID, "projectedVolumeFinalPass");
	g_glslCmWorldToClip = glGetUniformLocation(glGlobShader.ID, "caneCmWorldToClip");
	g_glslCmCameraPos = glGetUniformLocation(glGlobShader.ID, "caneCmCameraPos");
	glslfAnimateUv       = glGetUniformLocation(glGlobShader.ID, "fAnimateUv");
	glsluvOffsets		 = glGetUniformLocation(glGlobShader.ID, "uvOffsets");
	glslUnSelfIllum		 = glGetUniformLocation(glGlobShader.ID, "unSelfIllum");
	glslSubGlobPosCenter = glGetUniformLocation(glGlobShader.ID, "subGlobPosCenter");
	glslSubGlobRadius    = glGetUniformLocation(glGlobShader.ID, "subGlobRadius");

	glslAmbientMap  = glGetUniformLocation(glGlobShader.ID, "ambientMap");
	glslDiffuseMap  = glGetUniformLocation(glGlobShader.ID, "diffuseMap");
	glslSaturateMap = glGetUniformLocation(glGlobShader.ID, "saturateMap");
	glUniform1i(glslAmbientMap, 0);
	glUniform1i(glslDiffuseMap, 1);
	glUniform1i(glslSaturateMap, 2);

	glUniform1i(glGetUniformLocation(glGlobShader.ID, "shadowTextureArray"), SHADOW_TEXTURE_UNIT);

	// Vertex-SSBO fallback sampler units: warpState 4, bones 5, poseDpos 6,
	// poseDnormal 7 (units 0..3 stay the material/shadow maps above).
	if (g_fVertexUboFallback)
	{
		glUniform1i(glGetUniformLocation(glGlobShader.ID, "warpStateTex"), 4);
		glUniform1i(glGetUniformLocation(glGlobShader.ID, "bonesTex"), 5);
		glUniform1i(glGetUniformLocation(glGlobShader.ID, "poseDposTex"), 6);
		glUniform1i(glGetUniformLocation(glGlobShader.ID, "poseDnormalTex"), 7);
		glUniform1i(glGetUniformLocation(glGlobShader.ID, "lightTex"), 10);   // kLightTexUnit
		glslCaneBake = glGetUniformLocation(glGlobShader.ID, "caneBake");
		glUniform1i(glslCaneBake, 0);
	}

	glCelBorderShader.Init("celborder.vert", NULL, "celborder.frag");
	rcbStream.bindIndex = 1;
	align = 256;
	glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);

	rcbStream.stride = (sizeof(ROCEL) + align - 1) & ~(align - 1);

	// Vertex-SSBO fallback sampler units: edges 4, bones 5, edge bone
	// indices 6, edge bone weights 7, edge pose deltas 8.
	if (g_fVertexUboFallback)
	{
		glCelBorderShader.Use();
		glUniform1i(glGetUniformLocation(glCelBorderShader.ID, "edgesTex"), 4);
		glUniform1i(glGetUniformLocation(glCelBorderShader.ID, "bonesTex"), 5);
		glUniform1i(glGetUniformLocation(glCelBorderShader.ID, "edgeBoneIdxTex"), 6);
		glUniform1i(glGetUniformLocation(glCelBorderShader.ID, "edgeBoneWTex"), 7);
		glUniform1i(glGetUniformLocation(glCelBorderShader.ID, "edgePoseTex"), 8);
	}

	glGeomShader.Init("geom.vert", NULL, "geom.frag");
	glslGeomModelToClip = glGetUniformLocation(glGeomShader.ID, "modelToClip");
	glslGeomDebugColor = glGetUniformLocation(glGeomShader.ID, "debugColor");

	glBlotShader.Init("blot.vert", NULL, "blot.frag");
	glBlotShader.Use();

	u_projectionLoc = glGetUniformLocation(glBlotShader.ID, "u_projection");
	u_modelLoc      = glGetUniformLocation(glBlotShader.ID, "u_model");
	uvRectLoc       = glGetUniformLocation(glBlotShader.ID, "u_uvRect");
	u_useVertexColorLoc = glGetUniformLocation(glBlotShader.ID, "u_useVertexColor");
	glUniform1i(u_useVertexColorLoc, 0);
	blotColorLoc    = glGetUniformLocation(glBlotShader.ID, "blotColor");
	u_fontTexLoc    = glGetUniformLocation(glBlotShader.ID, "u_fontTex");
	glUniform1i(u_fontTexLoc, 0);

	glUniformMatrix4fv(u_projectionLoc, 1, GL_FALSE, glm::value_ptr(g_gl.blotProjection));

	glBlipShader.Init("blip.vert", NULL, "blip.frag");
	glBlipShader.Use();
	glUniform1i(glGetUniformLocation(glBlipShader.ID, "diffuseMap"), 0);
	glslBlipCurrentTime = glGetUniformLocation(glBlipShader.ID, "currentTime");
	glslBlipDtFrame = glGetUniformLocation(glBlipShader.ID, "dtFrame");
	glslBlipCameraMat = glGetUniformLocation(glBlipShader.ID, "cameraMat");
	glslBlipAlphaPass = glGetUniformLocation(glBlipShader.ID, "alphaPass");

	blipStream.bindIndex = 1;
	align = 256;
	glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
	blipStream.stride = (sizeof(BLIPGROUPGPU) + align - 1) & ~(align - 1);
	const GLuint blipGroupBlock = glGetUniformBlockIndex(glBlipShader.ID, "BlipGroup");
	if (blipGroupBlock != GL_INVALID_INDEX)
		glUniformBlockBinding(glBlipShader.ID, blipGroupBlock, blipStream.bindIndex);

	glDepthMask(GL_TRUE);
	glCullFace(GL_BACK);
	glFrontFace(GL_CCW);
	////
	glGenVertexArrays(1, &lineVao);
	glGenBuffers(1, &lineVbo);

	glBindVertexArray(lineVao);
	glBindBuffer(GL_ARRAY_BUFFER, lineVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(BLOTLINEVERTEX) * 2, nullptr, GL_DYNAMIC_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BLOTLINEVERTEX), reinterpret_cast<void*>(offsetof(BLOTLINEVERTEX, position)));

	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(BLOTLINEVERTEX), reinterpret_cast<void*>(offsetof(BLOTLINEVERTEX, uv)));

	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(BLOTLINEVERTEX), reinterpret_cast<void*>(offsetof(BLOTLINEVERTEX, color)));

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	////
	glGenTextures(1, &whiteTex);
	glBindTexture(GL_TEXTURE_2D, whiteTex);

	uint32_t white = 0xFFFFFFFFu;

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	glBindTexture(GL_TEXTURE_2D, 0);

	glslDyshfSkin = glGetUniformLocation(glDyshadow.ID, "fSkin");
	glslfSkin     = glGetUniformLocation(glGlobShader.ID, "fSkin");
	glslCelSkin   = glGetUniformLocation(glCelBorderShader.ID, "fSkin");

	glslfPose       = glGetUniformLocation(glGlobShader.ID, "fPose");
	glslPoseCount   = glGetUniformLocation(glGlobShader.ID, "poseCount");
	glslPoseWeights = glGetUniformLocation(glGlobShader.ID, "poseWeights[0]");   // fallback only; -1 on desktop (SSBO)

	glslCelPose        = glGetUniformLocation(glCelBorderShader.ID, "fPose");
	glslCelPoseCount   = glGetUniformLocation(glCelBorderShader.ID, "poseCount");
	glslCelPoseWeights = glGetUniformLocation(glCelBorderShader.ID, "poseWeights[0]");
}

void GL::CreateFramebuffers(int w, int h)
{
	// delete old
	if (fbo)  glDeleteFramebuffers(1, &fbo);
	if (fbc)  glDeleteTextures(1, &fbc);
	if (rbo)  glDeleteRenderbuffers(1, &rbo);

	if (fboMSAA) glDeleteFramebuffers(1, &fboMSAA);
	if (rboColorMSAA) glDeleteRenderbuffers(1, &rboColorMSAA);
	if (rboDepthStencilMSAA) glDeleteRenderbuffers(1, &rboDepthStencilMSAA);

	if (dyshFbo) glDeleteFramebuffers(1, &dyshFbo);
	if (dyshFbc) glDeleteTextures(1, &dyshFbc);
	if (dyshRbo) glDeleteRenderbuffers(1, &dyshRbo);

	fbo = fbc = rbo = 0;
	fboMSAA = rboColorMSAA = rboDepthStencilMSAA = 0;
	dyshFbo = dyshFbc = dyshRbo = 0;

	// resolved FBO (texture)
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);

	glGenTextures(1, &fbc);
	glBindTexture(GL_TEXTURE_2D, fbc);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbc, 0);

	glGenRenderbuffers(1, &rbo);
	glBindRenderbuffer(GL_RENDERBUFFER, rbo);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
	{
		ReportRuntimeError("Graphics", "The initial scene framebuffer could not be created.");
		throw std::runtime_error("Initial scene framebuffer creation failed.");
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// MSAA FBO (renderbuffers)
	if (g_fMsaa)
	{
		GLint maxSamples = 0;
		glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
		g_msaaSamples = std::max(1, std::min(g_msaaSamples, maxSamples));

		glGenFramebuffers(1, &fboMSAA);
		glBindFramebuffer(GL_FRAMEBUFFER, fboMSAA);

		glGenRenderbuffers(1, &rboColorMSAA);
		glBindRenderbuffer(GL_RENDERBUFFER, rboColorMSAA);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_msaaSamples, GL_RGBA8, w, h);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rboColorMSAA);

		glGenRenderbuffers(1, &rboDepthStencilMSAA);
		glBindRenderbuffer(GL_RENDERBUFFER, rboDepthStencilMSAA);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_msaaSamples, GL_DEPTH24_STENCIL8, w, h);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rboDepthStencilMSAA);

		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			ReportRuntimeError("Graphics",
				"The initial %dx MSAA framebuffer could not be created; MSAA has been disabled.",
				g_msaaSamples);
			glDeleteFramebuffers(1, &fboMSAA);
			glDeleteRenderbuffers(1, &rboColorMSAA);
			glDeleteRenderbuffers(1, &rboDepthStencilMSAA);
			fboMSAA = rboColorMSAA = rboDepthStencilMSAA = 0;
			g_fMsaa = false;
		}

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	dyshWidth  = 1024;
	dyshHeight = 1024;

	glGenFramebuffers(1, &dyshFbo);
	glBindFramebuffer(GL_FRAMEBUFFER, dyshFbo);

	glGenRenderbuffers(1, &dyshRbo);
	glBindRenderbuffer(GL_RENDERBUFFER, dyshRbo);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, dyshWidth, dyshHeight);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, dyshRbo);

	// No color layer is attached until DrawDysh selects one from the shared
	// texture array. Keep the idle framebuffer complete as a depth-only target.
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
	{
		ReportRuntimeError("Graphics", "The dynamic-shadow depth framebuffer is incomplete.");
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

bool GL::ResizeFramebuffers(int w, int h)
{
	const int fbW = std::max(1, w);
	const int fbH = std::max(1, h);
	GLint maxTextureSize = 0;
	GLint maxRenderbufferSize = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
	glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRenderbufferSize);
	const GLint maxFramebufferSize = std::min(maxTextureSize, maxRenderbufferSize);
	if (fbW > maxFramebufferSize || fbH > maxFramebufferSize)
	{
		ReportRuntimeError("Graphics",
			"Internal resolution %dx%d exceeds this GPU's maximum framebuffer size of %d.",
			fbW, fbH, maxFramebufferSize);
		return false;
	}

	GLuint newFbo = 0;
	GLuint newColor = 0;
	GLuint newDepth = 0;
	GLuint newMsaaFbo = 0;
	GLuint newMsaaColor = 0;
	GLuint newMsaaDepth = 0;
	for (GLenum staleError = glGetError(); staleError != GL_NO_ERROR;
		staleError = glGetError())
	{
		ReportRuntimeError("Graphics",
			"OpenGL error 0x%X was pending before framebuffer resize began.",
			staleError);
	}

	glGenFramebuffers(1, &newFbo);
	glBindFramebuffer(GL_FRAMEBUFFER, newFbo);
	glGenTextures(1, &newColor);
	glBindTexture(GL_TEXTURE_2D, newColor);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fbW, fbH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, newColor, 0);
	glGenRenderbuffers(1, &newDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, newDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, fbW, fbH);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, newDepth);

	GLenum error = glGetError();
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	bool complete = error == GL_NO_ERROR && status == GL_FRAMEBUFFER_COMPLETE;
	if (complete && g_fMsaa)
	{
		GLint maxSamples = 0;
		glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
		g_msaaSamples = std::max(1, std::min(g_msaaSamples, maxSamples));
		glGenFramebuffers(1, &newMsaaFbo);
		glBindFramebuffer(GL_FRAMEBUFFER, newMsaaFbo);
		glGenRenderbuffers(1, &newMsaaColor);
		glBindRenderbuffer(GL_RENDERBUFFER, newMsaaColor);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_msaaSamples, GL_RGBA8, fbW, fbH);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, newMsaaColor);
		glGenRenderbuffers(1, &newMsaaDepth);
		glBindRenderbuffer(GL_RENDERBUFFER, newMsaaDepth);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_msaaSamples, GL_DEPTH24_STENCIL8, fbW, fbH);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, newMsaaDepth);
		error = glGetError();
		status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		complete = error == GL_NO_ERROR && status == GL_FRAMEBUFFER_COMPLETE;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (!complete)
	{
		if (newFbo) glDeleteFramebuffers(1, &newFbo);
		if (newColor) glDeleteTextures(1, &newColor);
		if (newDepth) glDeleteRenderbuffers(1, &newDepth);
		if (newMsaaFbo) glDeleteFramebuffers(1, &newMsaaFbo);
		if (newMsaaColor) glDeleteRenderbuffers(1, &newMsaaColor);
		if (newMsaaDepth) glDeleteRenderbuffers(1, &newMsaaDepth);
		ReportRuntimeError("Graphics",
			"Could not allocate the %dx%d%s framebuffer (OpenGL error 0x%X, status 0x%X). The previous setting was retained.",
			fbW, fbH, g_fMsaa ? " MSAA" : "", error, status);
		return false;
	}

	if (fbo) glDeleteFramebuffers(1, &fbo);
	if (fbc) glDeleteTextures(1, &fbc);
	if (rbo) glDeleteRenderbuffers(1, &rbo);
	if (fboMSAA) glDeleteFramebuffers(1, &fboMSAA);
	if (rboColorMSAA) glDeleteRenderbuffers(1, &rboColorMSAA);
	if (rboDepthStencilMSAA) glDeleteRenderbuffers(1, &rboDepthStencilMSAA);
	fbo = newFbo;
	fbc = newColor;
	rbo = newDepth;
	fboMSAA = newMsaaFbo;
	rboColorMSAA = newMsaaColor;
	rboDepthStencilMSAA = newMsaaDepth;
	renderWidth = fbW;
	renderHeight = fbH;
	return true;
}
void GL::UpdateGLProjections()
{
	const float windowAspect = static_cast<float>(width) / static_cast<float>(height);

	float scaleX = 1.0f;
	float scaleY = 1.0f;

	if (aspectMode == FitToScreen)
	{
		screenProjection = glm::mat4(1.0f);
	}

	else if (windowAspect > aspectRatio) {
		scaleX = aspectRatio / windowAspect;
		screenProjection = glm::scale(glm::mat4(1.0f), glm::vec3(scaleX, 1.0f, 1.0f));
	}
	else {
		scaleY = windowAspect / aspectRatio;
		screenProjection = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, scaleY, 1.0f));
	}
}

void GL::TerminateGL()
{
	if (whiteTex)
	{
		glDeleteTextures(1, &whiteTex);
		whiteTex = 0;
	}
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &fbc);
	glDeleteRenderbuffers(1, &rbo);
	
	glDeleteFramebuffers(1, &fboMSAA);
	glDeleteRenderbuffers(1, &rboColorMSAA);
	glDeleteRenderbuffers(1, &rboDepthStencilMSAA);

	glDeleteFramebuffers(1, &dyshFbo);
	glDeleteTextures(1, &dyshFbc);
	glDeleteRenderbuffers(1, &dyshRbo);

	glDeleteVertexArrays(1, &sao);
	glDeleteBuffers(1, &sbo);
	glDeleteVertexArrays(1, &gao);
	glDeleteBuffers(1, &gbo);
	glDeleteBuffers(1, &geo);
	glDeleteVertexArrays(1, &fontVao);
	glDeleteBuffers(1, &fontVbo);

	glDeleteVertexArrays(1, &blipVao);
	glDeleteBuffers(1, &blipVbo);
	glDeleteBuffers(1, &blipEbo);
	glDeleteBuffers(1, &blipInstanceVbo);
	glDeleteVertexArrays(1, &sqtrVao);
	glDeleteBuffers(1, &sqtrVbo);

	glDeleteVertexArrays(1, &lineVao);
	glDeleteBuffers(1, &lineVbo);

	DeleteFrameStream(&ropStream);
	DeleteFrameStream(&rcbStream);
	DeleteFrameStream(&blipStream);
	glDeleteBuffers(1, &geomUBO);
	glDeleteBuffers(1, &cmUBO);

	glScreenShader.Delete();
	glGlobShader.Delete();
	glCelBorderShader.Delete();
	glGeomShader.Delete();
	glBlotShader.Delete();
	glBlipShader.Delete();

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(window);
	glfwTerminate();
}

void InitCameraUbo()
{
	// Camera UBO
	glGenBuffers(1, &cmUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, cmUBO);
	glBufferData(GL_UNIFORM_BUFFER, sizeof(CMGL), nullptr, GL_DYNAMIC_DRAW);

	GLuint CMGLblockIndex = glGetUniformBlockIndex(glGlobShader.ID, "CMGL");
	glUniformBlockBinding(glGlobShader.ID, CMGLblockIndex, 0);
	glBindBufferBase(GL_UNIFORM_BUFFER, 0, cmUBO);
}

// The camera block is rewritten several times per frame (world, TV portrait passes, debug overlays) while
// earlier draws that used it are still queued. Writing the one shared buffer in place makes some drivers
// (Adreno) show a later camera on earlier draws, so every update goes into the next buffer of a ring.
void UpdateCmUbo(const void* data)
{
	// Respecify the whole block: the driver hands later draws fresh storage and leaves earlier ones alone.
	glBindBuffer(GL_UNIFORM_BUFFER, cmUBO);
	glBufferData(GL_UNIFORM_BUFFER, sizeof(CMGL), data, GL_DYNAMIC_DRAW);
	glBindBufferBase(GL_UNIFORM_BUFFER, 0, cmUBO);
	if (g_fVertexUboFallback && g_glslCmWorldToClip >= 0)
	{
		GLint prev = 0;
		glGetIntegerv(GL_CURRENT_PROGRAM, &prev);
		glUseProgram(glGlobShader.ID);
		glUniformMatrix4fv(g_glslCmWorldToClip, 1, GL_FALSE, static_cast<const float*>(data));
		if (g_glslCmCameraPos >= 0)
			glUniform4fv(g_glslCmCameraPos, 1, static_cast<const float*>(data) + 16);
		glUseProgram(static_cast<GLuint>(prev));
	}
}

void InitFrameStream(STREAM* pstream, int streamSize)
{
	if (!pstream->fences)
		pstream->fences = (GLsync*)calloc(g_frames, sizeof(GLsync));

	GLsizeiptr perFrameBytes = (GLsizeiptr)streamSize * pstream->stride;
	const int slots = (std::min)(g_frames, 8);

	for (int i = 0; i < slots; ++i)
	{
		if (pstream->slotBuffers[i] == 0)
			glGenBuffers(1, &pstream->slotBuffers[i]);
		glBindBuffer(GL_UNIFORM_BUFFER, pstream->slotBuffers[i]);
		glBufferData(GL_UNIFORM_BUFFER, perFrameBytes, nullptr, GL_STREAM_DRAW);
	}
	pstream->bufferObject = pstream->slotBuffers[0];

	pstream->totalSize = perFrameBytes;
	pstream->frameBase = 0;
	pstream->cursor    = 0;
	pstream->frameSize = perFrameBytes;
}

void BeginFrameStream(STREAM* pstream)
{
	int slot = g_cframe % g_frames;

	GLsync s = pstream->fences[slot];
	if (s)
	{
		// poll
		GLenum r = glClientWaitSync(s, 0, 0);
		if (r == GL_TIMEOUT_EXPIRED)
		{
			// only stall if we have to
			glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
		}
		glDeleteSync(s);
		pstream->fences[slot] = nullptr;
	}

	pstream->bufferObject = pstream->slotBuffers[slot % 8];
	pstream->frameBase = 0;
	pstream->cursor = 0;
	pstream->mappedPtr = nullptr;
	pstream->staging.resize(static_cast<size_t>(pstream->frameSize));
}

GLsizeiptr StageFrameStream(STREAM* pstream, const void* ptr, int copySize)
{
	if (copySize < 0 || copySize > pstream->stride ||
		pstream->cursor + pstream->stride > pstream->frameSize)
	{
		__debugbreak();
		return -1;
	}

	const GLsizeiptr offset = pstream->frameBase + pstream->cursor;
	uint8_t* destination = pstream->staging.data() + pstream->cursor;
	std::memcpy(destination, ptr, static_cast<size_t>(copySize));
	if (copySize < pstream->stride)
		std::memset(destination + copySize, 0,
			static_cast<size_t>(pstream->stride - copySize));

	pstream->cursor += pstream->stride;
	return offset;
}

void UploadFrameStream(STREAM* pstream)
{
	if (pstream->cursor == 0)
		return;

	// BeginFrameStream retired this slot's previous fence, so the whole range
	// is CPU-owned until EndFrameStream fences the draws that consume it.
	glBindBuffer(GL_UNIFORM_BUFFER, pstream->bufferObject);
	glBufferSubData(GL_UNIFORM_BUFFER, pstream->frameBase, pstream->cursor, pstream->staging.data());
}

void BindFrameStreamRange(STREAM* pstream, GLsizeiptr offset, int size)
{
	if (offset >= 0)
		glBindBufferRange(GL_UNIFORM_BUFFER, pstream->bindIndex,
			pstream->bufferObject, offset, size);
}

void AppendStream(STREAM* pstream, void* ptr, int size, int copySize)
{

	// Mid-frame records go into a small private ring (see STREAM::appendBuffer). Writes to a buffer
	// are ordered after the draws already queued, so wrapping the ring is safe.
	if (pstream->appendBuffer == 0)
	{
		pstream->appendSize = pstream->stride * 128;
		glGenBuffers(1, &pstream->appendBuffer);
		glBindBuffer(GL_UNIFORM_BUFFER, pstream->appendBuffer);
		glBufferData(GL_UNIFORM_BUFFER, pstream->appendSize, nullptr, GL_STREAM_DRAW);
		pstream->appendCursor = 0;
	}
	if (pstream->appendCursor + pstream->stride > pstream->appendSize)
		pstream->appendCursor = 0;

	const GLsizeiptr off = pstream->appendCursor;
	pstream->appendCursor += pstream->stride;

	glBindBuffer(GL_UNIFORM_BUFFER, pstream->appendBuffer);
	glBufferSubData(GL_UNIFORM_BUFFER, off, copySize, ptr);
	glBindBufferRange(GL_UNIFORM_BUFFER, pstream->bindIndex, pstream->appendBuffer, off, size);
}

void EndFrameStream(STREAM* pstream)
{
	if (pstream->cursor == 0)
		return;

	int slot = g_cframe % g_frames;

	pstream->fences[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void DeleteFrameStream(STREAM* pstream)
{
	if (pstream->appendBuffer != 0)
	{
		glDeleteBuffers(1, &pstream->appendBuffer);
		pstream->appendBuffer = 0;
	}
	if (pstream->fences)
	{
		for (int i = 0; i < g_frames; i++)
		{
			if (pstream->fences[i])
				glDeleteSync(pstream->fences[i]);
		}

		free(pstream->fences);
		pstream->fences = nullptr;
	}

	if (pstream->bufferObject)
	{
		glBindBuffer(GL_UNIFORM_BUFFER, pstream->bufferObject);

		if (pstream->mappedPtr)
		{
			glUnmapBuffer(GL_UNIFORM_BUFFER);
			pstream->mappedPtr = nullptr;
		}

		for (int i = 0; i < 8; ++i)
		{
			if (pstream->slotBuffers[i])
			{
				glDeleteBuffers(1, &pstream->slotBuffers[i]);
				pstream->slotBuffers[i] = 0;
			}
		}
		pstream->bufferObject = 0;
	}

	pstream->frameBase = 0;
	pstream->cursor    = 0;
	pstream->frameSize = 0;
	pstream->staging.clear();
	pstream->staging.shrink_to_fit();
}

bool ApplyMsaaSettings()
{
	if (!g_fMsaa)
	{
		if (g_gl.fboMSAA) glDeleteFramebuffers(1, &g_gl.fboMSAA);
		if (g_gl.rboColorMSAA) glDeleteRenderbuffers(1, &g_gl.rboColorMSAA);
		if (g_gl.rboDepthStencilMSAA) glDeleteRenderbuffers(1, &g_gl.rboDepthStencilMSAA);
		g_gl.fboMSAA = g_gl.rboColorMSAA = g_gl.rboDepthStencilMSAA = 0;
		// Video options are changed from UpdateUi after the scene target has
		// already been bound for this frame.  Continue that frame on the resolved
		// scene target instead of accidentally drawing it to the window.
		g_sceneFbo = g_gl.fbo;
		glBindFramebuffer(GL_FRAMEBUFFER, g_sceneFbo);
		glViewport(0, 0, g_gl.renderWidth, g_gl.renderHeight);
		return true;
	}

	// MSAA storage must match the off-screen scene target, not the UI canvas.
	int w = g_gl.renderWidth;
	int h = g_gl.renderHeight;
	w = std::max(w, 1);
	h = std::max(h, 1);

	GLint maxSamples = 0;
	glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
	g_msaaSamples = std::max(1, std::min(g_msaaSamples, maxSamples));

	GLuint newFbo = 0;
	GLuint newColor = 0;
	GLuint newDepth = 0;
	for (GLenum staleError = glGetError(); staleError != GL_NO_ERROR;
		staleError = glGetError())
	{
		ReportRuntimeError("Graphics",
			"OpenGL error 0x%X was pending before MSAA framebuffer creation began.",
			staleError);
	}
	glGenFramebuffers(1, &newFbo);
	glBindFramebuffer(GL_FRAMEBUFFER, newFbo);

	glGenRenderbuffers(1, &newColor);
	glBindRenderbuffer(GL_RENDERBUFFER, newColor);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_msaaSamples, GL_RGBA8, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, newColor);

	glGenRenderbuffers(1, &newDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, newDepth);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_msaaSamples, GL_DEPTH24_STENCIL8, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, newDepth);

	const GLenum error = glGetError();
	const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (error != GL_NO_ERROR || status != GL_FRAMEBUFFER_COMPLETE)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		if (newFbo) glDeleteFramebuffers(1, &newFbo);
		if (newColor) glDeleteRenderbuffers(1, &newColor);
		if (newDepth) glDeleteRenderbuffers(1, &newDepth);
		if (g_gl.fboMSAA) glDeleteFramebuffers(1, &g_gl.fboMSAA);
		if (g_gl.rboColorMSAA) glDeleteRenderbuffers(1, &g_gl.rboColorMSAA);
		if (g_gl.rboDepthStencilMSAA) glDeleteRenderbuffers(1, &g_gl.rboDepthStencilMSAA);
		g_gl.fboMSAA = 0;
		g_gl.rboColorMSAA = 0;
		g_gl.rboDepthStencilMSAA = 0;
		g_fMsaa = false;
		g_sceneFbo = g_gl.fbo;
		glBindFramebuffer(GL_FRAMEBUFFER, g_sceneFbo);
		glViewport(0, 0, g_gl.renderWidth, g_gl.renderHeight);
		ReportRuntimeError("Graphics",
			"Could not allocate %dx MSAA at %dx%d (OpenGL error 0x%X, status 0x%X). MSAA has been disabled.",
			g_msaaSamples, w, h, error, status);
		return false;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (g_gl.fboMSAA) glDeleteFramebuffers(1, &g_gl.fboMSAA);
	if (g_gl.rboColorMSAA) glDeleteRenderbuffers(1, &g_gl.rboColorMSAA);
	if (g_gl.rboDepthStencilMSAA) glDeleteRenderbuffers(1, &g_gl.rboDepthStencilMSAA);
	g_gl.fboMSAA = newFbo;
	g_gl.rboColorMSAA = newColor;
	g_gl.rboDepthStencilMSAA = newDepth;
	// ApplyMsaaSettings can run in the middle of a frame.  Bind the replacement
	// immediately so world/UI drawing and the resolve at frame end agree on the
	// same framebuffer.
	g_sceneFbo = g_gl.fboMSAA;
	glBindFramebuffer(GL_FRAMEBUFFER, g_sceneFbo);
	glViewport(0, 0, g_gl.renderWidth, g_gl.renderHeight);
	return true;
}

bool ApplyInternalResolutionSettings()
{
	int width = 1;
	int height = 1;
	glfwGetFramebufferSize(g_gl.window, &width, &height);
	s_lastFramebufferResizeSucceeded = true;
	FrameBufferSizeCallBack(g_gl.window, width, height);
	return s_lastFramebufferResizeSucceeded;
}

bool ApplyWindowModeSettings(WindowMode mode)
{
	if (g_gl.window == nullptr)
		return false;

	GLFWmonitor* monitor = MonitorByIndex(g_monitorIndex);
	if (monitor == nullptr)
		return false;

	if (g_windowMode == WindowMode_Windowed)
	{
		glfwGetWindowPos(g_gl.window, &s_windowedX, &s_windowedY);
		glfwGetWindowSize(g_gl.window, &s_windowedWidth, &s_windowedHeight);
	}

	// Some window systems deliver the focus event from a monitor/mode change
	// later, during event polling. Cover that delayed event as well as callbacks
	// delivered synchronously by GLFW.
	s_autoIconifySuppressedUntil = glfwGetTime() + 0.5;
	glfwGetError(nullptr);
	if (mode == WindowMode_Windowed)
	{
		glfwSetWindowAttrib(g_gl.window, GLFW_DECORATED, GLFW_TRUE);
		glfwSetWindowMonitor(g_gl.window, nullptr, s_windowedX, s_windowedY,
			(std::max)(s_windowedWidth, 320), (std::max)(s_windowedHeight, 240), GLFW_DONT_CARE);
	}
	else if (mode == WindowMode_Borderless)
	{
		int x = 0;
		int y = 0;
		glfwGetMonitorPos(monitor, &x, &y);
		const GLFWvidmode* videoMode = glfwGetVideoMode(monitor);
		if (videoMode == nullptr)
			return false;

		// Borderless is a fullscreen-sized window, not a work-area-sized window.
		// The work area excludes the taskbar and left a visible strip along the
		// bottom of the selected monitor.
		glfwSetWindowAttrib(g_gl.window, GLFW_DECORATED, GLFW_FALSE);
		glfwSetWindowMonitor(g_gl.window, nullptr, x, y,
			videoMode->width, videoMode->height, GLFW_DONT_CARE);
	}
	else
	{
		const GLFWvidmode* videoMode = glfwGetVideoMode(monitor);
		if (videoMode == nullptr)
			return false;
		glfwSetWindowAttrib(g_gl.window, GLFW_DECORATED, GLFW_TRUE);
		glfwSetWindowMonitor(g_gl.window, monitor, 0, 0,
			videoMode->width, videoMode->height, videoMode->refreshRate);
	}
	if (glfwGetError(nullptr) != GLFW_NO_ERROR)
		return false;

	g_windowMode = mode;
	glfwSwapInterval(g_fVsync ? 1 : 0);
	return true;
}

int VideoMonitorCount()
{
	int count = 0;
	glfwGetMonitors(&count);
	return (std::max)(count, 1);
}

const char* VideoMonitorName(int monitorIndex)
{
	GLFWmonitor* monitor = MonitorByIndex(monitorIndex);
	const char* name = monitor != nullptr ? glfwGetMonitorName(monitor) : nullptr;
	return name != nullptr ? name : "Primary Monitor";
}

int VideoMonitorIndexByName(const char* name)
{
	if (name == nullptr || *name == '\0')
		return -1;
	int count = 0;
	GLFWmonitor** monitors = glfwGetMonitors(&count);
	for (int i = 0; monitors != nullptr && i < count; ++i)
	{
		const char* candidate = glfwGetMonitorName(monitors[i]);
		if (candidate != nullptr && std::strcmp(candidate, name) == 0)
			return i;
	}
	return -1;
}

bool ApplyMonitorSettings(int monitorIndex)
{
	const int previousMonitor = g_monitorIndex;
	g_monitorIndex = (std::clamp)(monitorIndex, 0, VideoMonitorCount() - 1);
	if (!ApplyWindowModeSettings(g_windowMode))
	{
		g_monitorIndex = previousMonitor;
		return false;
	}
	if (g_windowMode == WindowMode_Windowed && g_gl.window != nullptr)
	{
		GLFWmonitor* monitor = MonitorByIndex(g_monitorIndex);
		if (monitor == nullptr)
			return false;
		int x = 0, y = 0, width = 0, height = 0;
		int windowWidth = 0, windowHeight = 0;
		glfwGetMonitorWorkarea(monitor, &x, &y, &width, &height);
		glfwGetWindowSize(g_gl.window, &windowWidth, &windowHeight);
		s_windowedX = x + (width - windowWidth) / 2;
		s_windowedY = y + (height - windowHeight) / 2;
		s_autoIconifySuppressedUntil = glfwGetTime() + 0.5;
		glfwGetError(nullptr);
		glfwSetWindowPos(g_gl.window, s_windowedX, s_windowedY);
		if (glfwGetError(nullptr) != GLFW_NO_ERROR)
		{
			g_monitorIndex = previousMonitor;
			return false;
		}
	}
	return true;
}

void ApplyOutputGammaSettings(float gamma)
{
	g_outputGamma = std::clamp(gamma, 0.50f, 1.50f);
	if (glScreenShader.ID != 0)
	{
		glScreenShader.Use();
		glUniform1f(glGetUniformLocation(glScreenShader.ID, "outputGamma"), g_outputGamma);
	}
}

void ApplyAutoIconifySettings(bool enabled)
{
	g_fAutoIconify = enabled;
	if (g_gl.window != nullptr)
		glfwSetWindowAttrib(g_gl.window, GLFW_AUTO_ICONIFY, enabled ? GLFW_TRUE : GLFW_FALSE);
}

bool ApplyAspectRatioSettings(AspectMode mode)
{
	const AspectMode previousMode = g_gl.aspectMode;
	const float previousAspect = g_gl.aspectRatio;
	g_gl.aspectMode = mode;
	switch (mode)
	{
		case Fixed_4_3: g_gl.aspectRatio = 4.0f / 3.0f; break;
		case Fixed_16_10: g_gl.aspectRatio = 16.0f / 10.0f; break;
		case Fixed_16_9: g_gl.aspectRatio = 16.0f / 9.0f; break;
		case PS2_4_3: g_gl.aspectRatio = 4.0f / 3.0f; break;
		case PS2_16_9: g_gl.aspectRatio = 16.0f / 9.0f; break;
		case FitToScreen:
		default: break;
	}
	if (ApplyInternalResolutionSettings())
		return true;
	g_gl.aspectMode = previousMode;
	g_gl.aspectRatio = previousAspect;
	ApplyInternalResolutionSettings();
	return false;
}

void FrameBufferSizeCallBack(GLFWwindow* window, int width, int height)
{
	constexpr float kVirtualUiWidth = 640.0f;
	constexpr float kVirtualUiHeight = 492.80002f;

	float imguiOffset = 0.0f;
	if (g_fDebugGuiOpen && ImGui::GetCurrentContext() != nullptr)
	{
		float contentScaleX = 1.0f;
		float contentScaleY = 1.0f;
		glfwGetWindowContentScale(window, &contentScaleX, &contentScaleY);
		imguiOffset = ImGui::GetFrameHeight() * contentScaleY;
	}

	const int outputW = std::max(1, width);
	const int outputH = std::max(1, int(std::ceil(float(height) - imguiOffset)));
	float targetAspect = static_cast<float>(outputW) / static_cast<float>(outputH);
	if (g_gl.aspectMode == Fixed_4_3 || g_gl.aspectMode == PS2_4_3)
		targetAspect = 4.0f / 3.0f;
	else if (g_gl.aspectMode == Fixed_16_9 || g_gl.aspectMode == PS2_16_9)
		targetAspect = 16.0f / 9.0f;
	else if (g_gl.aspectMode == Fixed_16_10)
		targetAspect = 16.0f / 10.0f;
	g_gl.aspectRatio = targetAspect;
	int presentW = outputW;
	int presentH = std::max(1, static_cast<int>(std::lround(outputW / targetAspect)));
	if (presentH > outputH)
	{
		presentH = outputH;
		presentW = std::max(1, static_cast<int>(std::lround(outputH * targetAspect)));
	}

	int sceneW = presentW;
	int sceneH = presentH;

	if (g_internalResolutionHeight > 0)
	{
		sceneH = g_internalResolutionHeight;
		// The original PS2 raster is explicitly 640x448. Other internal
		// resolutions preserve the selected presentation aspect ratio.
		if (sceneH == 448)
			sceneW = 640;
		else
			sceneW = std::max(1, static_cast<int>(std::lround(
				static_cast<double>(sceneH) * static_cast<double>(presentW) /
				static_cast<double>(presentH))));
	}
	else if (g_resolutionScalePercent != 100)
	{
		const float scale = static_cast<float>(g_resolutionScalePercent) * 0.01f;
		sceneW = (std::max)(1, static_cast<int>(std::lround(presentW * scale)));
		sceneH = (std::max)(1, static_cast<int>(std::lround(presentH * scale)));
	}

	// UI and HUD layout always use the actual output size. Only the scene
	// framebuffer changes with the internal-resolution option.
	g_gl.width  = static_cast<float>(presentW);
	g_gl.height = static_cast<float>(presentH);
	g_gl.outputWidth = outputW;
	g_gl.outputHeight = outputH;
	g_gl.presentX = (outputW - presentW) / 2;
	g_gl.presentY = (outputH - presentH) / 2;
	g_gl.presentWidth = presentW;
	g_gl.presentHeight = presentH;

	// Preserve the PS2 UI aspect ratio and center the virtual canvas.  UI
	// renderers can now share one resize result instead of independently
	// stretching X and Y or mixing virtual and framebuffer coordinates.
	g_gl.uiScale = std::min(
		static_cast<float>(presentW) / kVirtualUiWidth,
		static_cast<float>(presentH) / kVirtualUiHeight);
	g_gl.uiOrigin.x = (static_cast<float>(presentW) - kVirtualUiWidth * g_gl.uiScale) * 0.5f;
	g_gl.uiOrigin.y = (static_cast<float>(presentH) - kVirtualUiHeight * g_gl.uiScale) * 0.5f;

	const int previousSceneW = g_gl.renderWidth;
	const int previousSceneH = g_gl.renderHeight;
	s_lastFramebufferResizeSucceeded = g_gl.ResizeFramebuffers(sceneW, sceneH);
	if (!s_lastFramebufferResizeSucceeded)
	{
		sceneW = previousSceneW;
		sceneH = previousSceneH;
	}

	// Aspect/internal-resolution settings can be changed from UpdateUi after
	// this frame's scene target has already been bound. ResizeFramebuffers
	// replaces and deletes that target, so update the cached binding now rather
	// than allowing the rest of the frame to reuse a deleted framebuffer name.
	g_sceneFbo = g_fMsaa ? g_gl.fboMSAA : g_gl.fbo;
	glBindFramebuffer(GL_FRAMEBUFFER, g_sceneFbo);
	glViewport(0, 0, sceneW, sceneH);

	g_gl.UpdateGLProjections();

	g_gl.blotProjection = glm::ortho(0.0f, float(presentW), float(presentH), 0.0f, -1.0f, 1.0f);

	glBlotShader.Use();
	glUniformMatrix4fv(u_projectionLoc, 1, GL_FALSE, glm::value_ptr(g_gl.blotProjection));

	if (g_pcm != nullptr)
		RecalcCm(g_pcm);

	ResizeTvUiForFramebuffer();
	RepositionAllBlots();
}

GL g_gl;
GLuint cmUBO = 0;
STREAM ropStream;
STREAM rcbStream;
STREAM blipStream;
GLuint geomUBO = 0;
GLint glslBlipCurrentTime = -1;
GLint glslBlipDtFrame = -1;
GLint glslBlipCameraMat = -1;
GLint glslBlipAlphaPass = -1;
GLuint glslLsmShadow = 0;
GLuint glslLsmDiffuse = 0;
GLuint glslFogType = 0;
GLuint glslFogNear = 0;
GLuint glslFogFar = 0;
GLuint glslFogMax = 0;
GLuint glslFogColor = 0;
GLuint glslfAlphaTest = 0;
GLuint glslAlphaCutOff = 0;
GLuint glslRko = 0;
GLuint glslProjectedVolumeColor = 0;
GLuint glslProjectedVolumeFinalPass = 0;
GLint g_glslCmWorldToClip = -1;
GLint g_glslCmCameraPos = -1;
GLuint glslfAnimateUv = 0;
GLuint glsluvOffsets = 0;
GLuint glslUnSelfIllum = 0;
GLuint glslSubGlobPosCenter = 0;
GLuint glslSubGlobRadius = 0;
GLuint glslDyshMatWorldClip = 0;
GLuint glslDyshModel = 0;
GLuint glslDyshfSkin = 0;
GLuint glslAmbientMap = 0;
GLuint glslDiffuseMap = 0;
GLuint glslSaturateMap = 0;
GLuint glslGeomModelToClip = 0;
GLuint glslGeomDebugColor = 0;
GLuint u_fontTexLoc = 0;
GLuint glslfCull = 0;
uint64_t screenTextureHandle = 0;
GLuint g_sceneFbo = 0;
int g_msaaSamples = 4;
bool g_fMsaa = false;
int g_internalResolutionHeight = 0;
int g_resolutionScalePercent = 100;
int g_monitorIndex = 0;
int g_textureFiltering = 1;
float g_outputGamma = 1.0f;
float g_guiScale = 1.0f;
GuiStyle g_guiStyle = GuiStyle_PS2;
WindowMode g_windowMode = WindowMode_Windowed;
bool g_fAutoIconify = true;
float g_drawDistanceMultiplier = 1.0f;
int g_frames = 3;
float s_dxDisplay;
float s_dyDisplay;
float s_dxDisplayOriginal;
float s_dyDisplayOriginal;
GLuint lineVao = 0;
GLuint lineVbo = 0;
GLuint glslfSkin = 0;
GLuint glslCaneBake = 0;
GLint glslCelSkin = 0;
GLuint glslfPose = 0;
GLuint glslPoseCount = 0;
GLuint glslPoseWeights = 0;
GLuint glslCelPose = 0;
GLuint glslCelPoseCount = 0;
GLuint glslCelPoseWeights = 0;
