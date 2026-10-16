// GLFW-on-SDL2 shim for Android. See GLFW/glfw3.h.
#include <GLFW/glfw3.h>
#include <SDL.h>
#include <jni.h>
#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <android/log.h>

// Bring-up logging; compiled out of release builds (CANE_DIAG=OFF).
#ifdef CANE_DIAG
#define CANE_DIAG_LOG(...) __android_log_print(ANDROID_LOG_INFO, "cane", __VA_ARGS__)
#else
#define CANE_DIAG_LOG(...) ((void)0)
#endif
#include "imgui/imgui.h"
#include "touch_overlay.h"

namespace {

struct Win { SDL_Window* sdl = nullptr; SDL_GLContext ctx = nullptr; bool shouldClose = false;
	GLFWframebuffersizefun fbCb = nullptr; GLFWcursorposfun curCb = nullptr;
	GLFWmousebuttonfun btnCb = nullptr; GLFWscrollfun scrCb = nullptr; };
struct Mon { int dummy; };

Win* g_win = nullptr;
Mon  g_mon;
GLFWmonitor* g_monList[1] = { (GLFWmonitor*)&g_mon };
GLFWvidmode  g_mode{ 1920, 1080, 8, 8, 8, 60 };
GLFWerrorfun g_errCb = nullptr;

constexpr int kMaxPads = 16;
struct Pad { SDL_JoystickID id = -1; SDL_GameController* gc = nullptr; SDL_Joystick* js = nullptr;
	float axes[32]; unsigned char buttons[64]; unsigned char hats[8]; };
Pad g_pads[kMaxPads];

void OpenPad(int deviceIndex)
{
	// Android exposes the accelerometer as a "joystick" (3 axes, no buttons).
	// Input.cpp picks the active pad by "activity" (largest |axis| / any
	// button) with a +3.0 stickiness bonus, and treats non-gamepad devices as
	// raw sticks; a phone held at a tilt reports |axis| ~ 1 forever, so the
	// accelerometer outranks an idle real pad and then can never be displaced.
	// Never expose it to the game.
	if (const char* nm = SDL_JoystickNameForIndex(deviceIndex); nm && std::strstr(nm, "Accelerometer"))
	{
		CANE_DIAG_LOG("[Input] ignoring sensor device idx=%d '%s'", deviceIndex, nm);
		return;
	}
	for (Pad& p : g_pads) if (p.js && SDL_JoystickInstanceID(p.js) == SDL_JoystickGetDeviceInstanceID(deviceIndex)) return;
	for (Pad& p : g_pads) {
		if (p.js) continue;
		p.js = SDL_JoystickOpen(deviceIndex);
		if (!p.js) {
			__android_log_print(ANDROID_LOG_WARN, "cane", "[Input] SDL_JoystickOpen(%d) failed: %s", deviceIndex, SDL_GetError());
			return;
		}
		p.id = SDL_JoystickInstanceID(p.js);
		if (SDL_IsGameController(deviceIndex)) p.gc = SDL_GameControllerOpen(deviceIndex);
		CANE_DIAG_LOG(
			"[Input] pad opened slot=%d idx=%d id=%d isGC=%d axes=%d buttons=%d hats=%d",
			(int)(&p - g_pads), deviceIndex, (int)p.id, p.gc ? 1 : 0,
			SDL_JoystickNumAxes(p.js), SDL_JoystickNumButtons(p.js), SDL_JoystickNumHats(p.js));
		return;
	}
}
void ClosePad(SDL_JoystickID id)
{
	for (Pad& p : g_pads) if (p.js && p.id == id) {
		if (p.gc) SDL_GameControllerClose(p.gc);
		SDL_JoystickClose(p.js); p = Pad{};
	}
}

int ScancodeFor(int key)
{
	if (key >= 'A' && key <= 'Z') return SDL_SCANCODE_A + (key - 'A');
	if (key >= '1' && key <= '9') return SDL_SCANCODE_1 + (key - '1');
	if (key == '0') return SDL_SCANCODE_0;
	if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12) return SDL_SCANCODE_F1 + (key - GLFW_KEY_F1);
	if (key >= GLFW_KEY_F13 && key <= GLFW_KEY_F24) return SDL_SCANCODE_F13 + (key - GLFW_KEY_F13);
	if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) return key == GLFW_KEY_KP_0 ? SDL_SCANCODE_KP_0 : SDL_SCANCODE_KP_1 + (key - GLFW_KEY_KP_1);
	switch (key) {
	case GLFW_KEY_SPACE: return SDL_SCANCODE_SPACE;
	case GLFW_KEY_APOSTROPHE: return SDL_SCANCODE_APOSTROPHE;
	case GLFW_KEY_COMMA: return SDL_SCANCODE_COMMA;
	case GLFW_KEY_MINUS: return SDL_SCANCODE_MINUS;
	case GLFW_KEY_PERIOD: return SDL_SCANCODE_PERIOD;
	case GLFW_KEY_SLASH: return SDL_SCANCODE_SLASH;
	case GLFW_KEY_SEMICOLON: return SDL_SCANCODE_SEMICOLON;
	case GLFW_KEY_EQUAL: return SDL_SCANCODE_EQUALS;
	case GLFW_KEY_LEFT_BRACKET: return SDL_SCANCODE_LEFTBRACKET;
	case GLFW_KEY_BACKSLASH: return SDL_SCANCODE_BACKSLASH;
	case GLFW_KEY_RIGHT_BRACKET: return SDL_SCANCODE_RIGHTBRACKET;
	case GLFW_KEY_GRAVE_ACCENT: return SDL_SCANCODE_GRAVE;
	case GLFW_KEY_ESCAPE: return SDL_SCANCODE_ESCAPE;
	case GLFW_KEY_ENTER: return SDL_SCANCODE_RETURN;
	case GLFW_KEY_TAB: return SDL_SCANCODE_TAB;
	case GLFW_KEY_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
	case GLFW_KEY_INSERT: return SDL_SCANCODE_INSERT;
	case GLFW_KEY_DELETE: return SDL_SCANCODE_DELETE;
	case GLFW_KEY_RIGHT: return SDL_SCANCODE_RIGHT;
	case GLFW_KEY_LEFT: return SDL_SCANCODE_LEFT;
	case GLFW_KEY_DOWN: return SDL_SCANCODE_DOWN;
	case GLFW_KEY_UP: return SDL_SCANCODE_UP;
	case GLFW_KEY_PAGE_UP: return SDL_SCANCODE_PAGEUP;
	case GLFW_KEY_PAGE_DOWN: return SDL_SCANCODE_PAGEDOWN;
	case GLFW_KEY_HOME: return SDL_SCANCODE_HOME;
	case GLFW_KEY_END: return SDL_SCANCODE_END;
	case GLFW_KEY_CAPS_LOCK: return SDL_SCANCODE_CAPSLOCK;
	case GLFW_KEY_SCROLL_LOCK: return SDL_SCANCODE_SCROLLLOCK;
	case GLFW_KEY_NUM_LOCK: return SDL_SCANCODE_NUMLOCKCLEAR;
	case GLFW_KEY_PRINT_SCREEN: return SDL_SCANCODE_PRINTSCREEN;
	case GLFW_KEY_PAUSE: return SDL_SCANCODE_PAUSE;
	case GLFW_KEY_KP_EQUAL: return SDL_SCANCODE_KP_EQUALS;
	case GLFW_KEY_LEFT_SHIFT: return SDL_SCANCODE_LSHIFT;
	case GLFW_KEY_LEFT_CONTROL: return SDL_SCANCODE_LCTRL;
	case GLFW_KEY_LEFT_ALT: return SDL_SCANCODE_LALT;
	case GLFW_KEY_LEFT_SUPER: return SDL_SCANCODE_LGUI;
	case GLFW_KEY_RIGHT_SHIFT: return SDL_SCANCODE_RSHIFT;
	case GLFW_KEY_RIGHT_CONTROL: return SDL_SCANCODE_RCTRL;
	case GLFW_KEY_RIGHT_ALT: return SDL_SCANCODE_RALT;
	case GLFW_KEY_RIGHT_SUPER: return SDL_SCANCODE_RGUI;
	case GLFW_KEY_MENU: return SDL_SCANCODE_MENU;
	case CANE_KEY_ANDROID_BACK: return SDL_SCANCODE_AC_BACK;
	}
	return SDL_SCANCODE_UNKNOWN;
}

// A tap on Back can go down and up between two frames; keep it "pressed" for a few polls.
static int g_backLatch = 0;

void Handle(const SDL_Event& e)
{
	switch (e.type) {
	case SDL_KEYDOWN:
		if (e.key.keysym.scancode == SDL_SCANCODE_AC_BACK) g_backLatch = 4;
		break;
	case SDL_FINGERDOWN:
	case SDL_FINGERMOTION:
	case SDL_FINGERUP:
		cane::touch::OnFinger(e.type, static_cast<long long>(e.tfinger.fingerId), e.tfinger.x, e.tfinger.y);
		break;
	case SDL_QUIT: if (g_win) g_win->shouldClose = true; break;
	case SDL_CONTROLLERDEVICEADDED:
	case SDL_JOYDEVICEADDED: {
		const int idx = e.type == SDL_JOYDEVICEADDED ? e.jdevice.which : e.cdevice.which;
		const char* name = e.type == SDL_JOYDEVICEADDED ? SDL_JoystickNameForIndex(idx) : SDL_GameControllerNameForIndex(idx);
		CANE_DIAG_LOG("[Input] device added type=0x%X idx=%d name='%s'",
			static_cast<unsigned>(e.type), idx, name ? name : "?");
		OpenPad(idx); break; }
	case SDL_CONTROLLERDEVICEREMOVED:
		CANE_DIAG_LOG("[Input] controller removed id=%d", e.cdevice.which);
		ClosePad(e.cdevice.which); break;
	case SDL_JOYDEVICEREMOVED:
		CANE_DIAG_LOG("[Input] joystick removed id=%d", e.jdevice.which);
		ClosePad(e.jdevice.which); break;
	case SDL_WINDOWEVENT:
		if (g_win && g_win->fbCb && (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED)) {
			int w = 0, h = 0; SDL_GL_GetDrawableSize(g_win->sdl, &w, &h);
			g_win->fbCb((GLFWwindow*)g_win, w, h);
		}
		break;
	case SDL_MOUSEMOTION: if (g_win && g_win->curCb) g_win->curCb((GLFWwindow*)g_win, e.motion.x, e.motion.y); break;
	case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
		if (g_win && g_win->btnCb) g_win->btnCb((GLFWwindow*)g_win, e.button.button - 1, e.type == SDL_MOUSEBUTTONDOWN, 0);
		break;
	case SDL_MOUSEWHEEL: if (g_win && g_win->scrCb) g_win->scrCb((GLFWwindow*)g_win, e.wheel.x, e.wheel.y); break;
	}
}

Pad* PadFor(int jid) { return (jid >= 0 && jid < kMaxPads && g_pads[jid].js) ? &g_pads[jid] : nullptr; }

} // namespace

extern "C" {

extern "C" void cane_install_crash_handler(const char* logsDir);

int glfwInit(void)
{
	{
		// native crash backtraces go to <files>/Logs/crash-<pid>.txt (picked up by the debug-log export)
		const char* base = SDL_AndroidGetExternalStoragePath();
		static char logs[512];
		if (base != nullptr) snprintf(logs, sizeof(logs), "%s/Logs", base); else logs[0] = 0;
		cane_install_crash_handler(logs);
	}
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");   // see OpenPad()
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");          // touches drive the on-screen controller, not a fake mouse
	SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");    // Back goes to the game (pause menu), never closes the activity
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
		if (g_errCb) g_errCb(GLFW_PLATFORM_ERROR, SDL_GetError());
		return GLFW_FALSE;
	}
	cane::touch::Init();
	SDL_DisplayMode dm;
	if (SDL_GetDesktopDisplayMode(0, &dm) == 0) { g_mode.width = dm.w; g_mode.height = dm.h; g_mode.refreshRate = dm.refresh_rate ? dm.refresh_rate : 60; }
	return GLFW_TRUE;
}
void glfwTerminate(void) { for (Pad& p : g_pads) if (p.js) ClosePad(p.id); SDL_Quit(); }
int  glfwGetError(const char** d) { if (d) *d = SDL_GetError(); return 0; }
GLFWerrorfun glfwSetErrorCallback(GLFWerrorfun cb) { auto o = g_errCb; g_errCb = cb; return o; }
void glfwWindowHint(int, int) {}   // Context is fixed: OpenGL ES 3.2, see glfwCreateWindow.

GLFWwindow* glfwCreateWindow(int, int, const char* title, GLFWmonitor*, GLFWwindow*)
{
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
	auto* w = new Win();
	w->sdl = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, g_mode.width, g_mode.height,
		SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI);
	if (!w->sdl) { if (g_errCb) g_errCb(GLFW_PLATFORM_ERROR, SDL_GetError()); delete w; return nullptr; }
	w->ctx = SDL_GL_CreateContext(w->sdl);
	if (!w->ctx) { if (g_errCb) g_errCb(GLFW_PLATFORM_ERROR, SDL_GetError()); SDL_DestroyWindow(w->sdl); delete w; return nullptr; }
	g_win = w;
	for (int i = 0; i < SDL_NumJoysticks(); ++i) OpenPad(i);
	{
		const int n = SDL_NumJoysticks();
		CANE_DIAG_LOG("[Input] %d joystick device(s) at window create", n);
		for (int i = 0; i < n; ++i) {
			const char* name = SDL_JoystickNameForIndex(i);
			CANE_DIAG_LOG(
				"[Input] dev[%d] name='%s' vid=0x%04X pid=0x%04X isGC=%d",
				i, name ? name : "?", SDL_JoystickGetDeviceVendor(i),
				SDL_JoystickGetDeviceProduct(i), SDL_IsGameController(i));
		}
	}
	return (GLFWwindow*)w;
}
void glfwDestroyWindow(GLFWwindow* wp)
{
	auto* w = (Win*)wp; if (!w) return;
	SDL_GL_DeleteContext(w->ctx); SDL_DestroyWindow(w->sdl);
	if (g_win == w) g_win = nullptr; delete w;
}
void glfwMakeContextCurrent(GLFWwindow* wp) { auto* w = (Win*)wp; if (w) SDL_GL_MakeCurrent(w->sdl, w->ctx); }
// Some Android EGL implementations return NULL from eglGetProcAddress for
// individual core ES entry points (seen on Mali: glVertexAttribDivisor = NULL,
// which glad then calls through and segfaults). Fall back to dlsym in the
// GLES libraries so every core ES 3.x symbol resolves.
GLFWglproc glfwGetProcAddress(const char* n)
{
	void* p = SDL_GL_GetProcAddress(n);
	if (p == nullptr)
	{
		void* dflt = dlsym(RTLD_DEFAULT, n);
		static void* glesv2 = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_LOCAL);
		void* v2 = (glesv2 != nullptr) ? dlsym(glesv2, n) : nullptr;
		static void* glesv3 = dlopen("libGLESv3.so", RTLD_LAZY | RTLD_LOCAL);
		void* v3 = (glesv3 != nullptr) ? dlsym(glesv3, n) : nullptr;
		p = dflt ? dflt : (v2 ? v2 : v3);
#ifdef CANE_DIAG
		__android_log_print(ANDROID_LOG_WARN, "cane",
			"glfwGetProcAddress: '%s' NULL from SDL, dlsym default=%p glesv2=%p glesv3=%p glesv2handle=%p",
			n, dflt, v2, v3, glesv2);
#endif
	}
	return (GLFWglproc)p;
}
void glfwSwapInterval(int i) { SDL_GL_SetSwapInterval(i); }
#ifdef CANE_DIAG
static unsigned s_swapCount = 0;
#endif
void glfwSwapBuffers(GLFWwindow* wp)
{
#ifdef CANE_DIAG
	++s_swapCount;
#endif
	SDL_GL_SwapWindow(((Win*)wp)->sdl);
}
int  glfwWindowShouldClose(GLFWwindow* wp) { return ((Win*)wp)->shouldClose ? 1 : 0; }
void glfwPollEvents(void)
{
	if (g_backLatch > 0) --g_backLatch;
	cane::touch::BeginFrame();
	SDL_Event e; while (SDL_PollEvent(&e)) Handle(e);
	{
		bool physical = false;
		for (int i = 0; i < kMaxPads; ++i) if (g_pads[i].js) { physical = true; break; }
		cane::touch::SetPhysicalPadPresent(physical);
	}
	// 1 Hz input snapshot: keyboard state + physical pad state for diagnostics.
#ifdef CANE_DIAG
	static Uint32 nextDump = 0;
	const Uint32 now = SDL_GetTicks();
	if ((Sint32)(now - nextDump) >= 0)
	{
		nextDump = now + 1000;
		const Uint8* kb = SDL_GetKeyboardState(nullptr);
		int present = 0, firstSlot = -1;
		for (int s = 0; s < kMaxPads; ++s)
			if (g_pads[s].js) { if (firstSlot < 0) firstSlot = s; ++present; }
		unsigned btnBits = 0; float ax0 = 0.0f, ay0 = 0.0f;
		unsigned gcBits = 0; float glx = 0.0f, gly = 0.0f;
		if (firstSlot >= 0)
		{
			Pad& p = g_pads[firstSlot];
			const int nb = std::min(SDL_JoystickNumButtons(p.js), 16);
			for (int b = 0; b < nb; ++b) if (SDL_JoystickGetButton(p.js, b)) btnBits |= 1u << b;
			ax0 = SDL_JoystickGetAxis(p.js, 0) / 32767.0f;
			ay0 = SDL_JoystickGetAxis(p.js, 1) / 32767.0f;
			if (p.gc)
			{
				static const SDL_GameControllerButton gcMap[15] = {
					SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
					SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
					SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_GUIDE,
					SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
					SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
					SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT };
				for (int i = 0; i < 15; ++i) if (SDL_GameControllerGetButton(p.gc, gcMap[i])) gcBits |= 1u << i;
				glx = SDL_GameControllerGetAxis(p.gc, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
				gly = SDL_GameControllerGetAxis(p.gc, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
			}
		}
		const unsigned fps = s_swapCount;
		s_swapCount = 0;
		__android_log_print(ANDROID_LOG_INFO, "cane",
			"[Input] fps=%u kb RET=%d BSPC=%d SPACE=%d ESC=%d | pads=%d btn16=0x%04X ax0=%.2f ay0=%.2f gc=0x%03X lx=%.2f ly=%.2f",
			fps, kb[SDL_SCANCODE_RETURN], kb[SDL_SCANCODE_BACKSPACE], kb[SDL_SCANCODE_SPACE], kb[SDL_SCANCODE_ESCAPE],
			present, btnBits, ax0, ay0, gcBits, glx, gly);
	}
#endif
}
void glfwWaitEventsTimeout(double t) { SDL_Event e; if (SDL_WaitEventTimeout(&e, (int)(t * 1000.0))) { Handle(e); glfwPollEvents(); } }

void glfwGetFramebufferSize(GLFWwindow* wp, int* w, int* h) { int a = 0, b = 0; SDL_GL_GetDrawableSize(((Win*)wp)->sdl, &a, &b); if (w) *w = a; if (h) *h = b; }
void glfwGetWindowSize(GLFWwindow* wp, int* w, int* h) { int a = 0, b = 0; SDL_GetWindowSize(((Win*)wp)->sdl, &a, &b); if (w) *w = a; if (h) *h = b; }
void glfwGetWindowPos(GLFWwindow*, int* x, int* y) { if (x) *x = 0; if (y) *y = 0; }
// Added for upstream 0.4.1 (window management is the OS's job on Android: no positioning, no iconify, and the
// focus callback is only used for the desktop auto-iconify option, which the Android menu hides).
void glfwSetWindowPos(GLFWwindow*, int, int) {}
void glfwIconifyWindow(GLFWwindow*) {}
GLFWwindowfocusfun glfwSetWindowFocusCallback(GLFWwindow*, GLFWwindowfocusfun) { return nullptr; }
const char* glfwGetMonitorName(GLFWmonitor*) { return "Built-in display"; }
double glfwGetTime(void) { return (double)SDL_GetTicks64() / 1000.0; }
void glfwGetWindowContentScale(GLFWwindow*, float* x, float* y) { if (x) *x = 1.f; if (y) *y = 1.f; }
int  glfwGetWindowAttrib(GLFWwindow* wp, int a)
{
	auto f = SDL_GetWindowFlags(((Win*)wp)->sdl);
	if (a == GLFW_FOCUSED) return (f & SDL_WINDOW_INPUT_FOCUS) ? 1 : 0;
	if (a == GLFW_DECORATED) return 0;
	return 0;
}
void glfwSetWindowAttrib(GLFWwindow*, int, int) {}
GLFWmonitor* glfwGetWindowMonitor(GLFWwindow*) { return g_monList[0]; }   // always "fullscreen"
void glfwSetWindowMonitor(GLFWwindow*, GLFWmonitor*, int, int, int, int, int) {}

GLFWmonitor*  glfwGetPrimaryMonitor(void) { return g_monList[0]; }
GLFWmonitor** glfwGetMonitors(int* c) { if (c) *c = 1; return g_monList; }
void glfwGetMonitorPos(GLFWmonitor*, int* x, int* y) { if (x) *x = 0; if (y) *y = 0; }
void glfwGetMonitorWorkarea(GLFWmonitor*, int* x, int* y, int* w, int* h) { if (x) *x = 0; if (y) *y = 0; if (w) *w = g_mode.width; if (h) *h = g_mode.height; }
const GLFWvidmode* glfwGetVideoMode(GLFWmonitor*) { return &g_mode; }
const GLFWvidmode* glfwGetVideoModes(GLFWmonitor*, int* c) { if (c) *c = 1; return &g_mode; }

GLFWframebuffersizefun glfwSetFramebufferSizeCallback(GLFWwindow* w, GLFWframebuffersizefun cb) { auto* x = (Win*)w; auto o = x->fbCb; x->fbCb = cb; return o; }
GLFWcursorposfun   glfwSetCursorPosCallback(GLFWwindow* w, GLFWcursorposfun cb)     { auto* x = (Win*)w; auto o = x->curCb; x->curCb = cb; return o; }
GLFWmousebuttonfun glfwSetMouseButtonCallback(GLFWwindow* w, GLFWmousebuttonfun cb) { auto* x = (Win*)w; auto o = x->btnCb; x->btnCb = cb; return o; }
GLFWscrollfun      glfwSetScrollCallback(GLFWwindow* w, GLFWscrollfun cb)           { auto* x = (Win*)w; auto o = x->scrCb; x->scrCb = cb; return o; }

int glfwGetKey(GLFWwindow*, int key)
{
	if (key == CANE_KEY_ANDROID_BACK && g_backLatch > 0) return GLFW_PRESS;
	const int sc = ScancodeFor(key);
	if (sc == SDL_SCANCODE_UNKNOWN) return GLFW_RELEASE;
	return SDL_GetKeyboardState(nullptr)[sc] ? GLFW_PRESS : GLFW_RELEASE;
}
const char* glfwGetKeyName(int key, int)
{
	const int sc = ScancodeFor(key);
	if (sc == SDL_SCANCODE_UNKNOWN) return nullptr;
	const char* n = SDL_GetKeyName(SDL_GetKeyFromScancode((SDL_Scancode)sc));
	return (n && *n) ? n : nullptr;
}
int  glfwGetMouseButton(GLFWwindow*, int b) { return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(b + 1)) ? GLFW_PRESS : GLFW_RELEASE; }
void glfwGetCursorPos(GLFWwindow*, double* x, double* y) { int a = 0, b = 0; SDL_GetMouseState(&a, &b); if (x) *x = a; if (y) *y = b; }
void glfwSetInputMode(GLFWwindow*, int, int) {}
int  glfwGetInputMode(GLFWwindow*, int mode) { return mode == GLFW_CURSOR ? GLFW_CURSOR_NORMAL : 0; }

int glfwJoystickPresent(int jid) { return PadFor(jid) ? GLFW_TRUE : GLFW_FALSE; }
int glfwJoystickIsGamepad(int jid) { Pad* p = PadFor(jid); return (p && p->gc) ? GLFW_TRUE : GLFW_FALSE; }
const float* glfwGetJoystickAxes(int jid, int* c)
{
	Pad* p = PadFor(jid); if (!p) { if (c) *c = 0; return nullptr; }
	int n = std::min(SDL_JoystickNumAxes(p->js), 32);
	for (int i = 0; i < n; ++i) p->axes[i] = SDL_JoystickGetAxis(p->js, i) / 32767.0f;
	if (c) *c = n; return p->axes;
}
const unsigned char* glfwGetJoystickButtons(int jid, int* c)
{
	Pad* p = PadFor(jid); if (!p) { if (c) *c = 0; return nullptr; }
	int n = std::min(SDL_JoystickNumButtons(p->js), 64);
	for (int i = 0; i < n; ++i) p->buttons[i] = SDL_JoystickGetButton(p->js, i);
	if (c) *c = n; return p->buttons;
}
const unsigned char* glfwGetJoystickHats(int jid, int* c)
{
	Pad* p = PadFor(jid); if (!p) { if (c) *c = 0; return nullptr; }
	int n = std::min(SDL_JoystickNumHats(p->js), 8);
	for (int i = 0; i < n; ++i) p->hats[i] = SDL_JoystickGetHat(p->js, i);   // SDL_HAT_* == GLFW_HAT_* bit values
	if (c) *c = n; return p->hats;
}
int glfwGetGamepadState(int jid, GLFWgamepadstate* s)
{
	Pad* p = PadFor(jid); if (!p || !p->gc) return GLFW_FALSE;
	static const SDL_GameControllerButton map[15] = {
		SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
		SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
		SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_GUIDE,
		SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
		SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
		SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT };
	for (int i = 0; i < 15; ++i) s->buttons[i] = SDL_GameControllerGetButton(p->gc, map[i]);
#ifdef CANE_DIAG
	{
		static unsigned s_lastBits = ~0u;
		unsigned bits = 0;
		for (int i = 0; i < 15; ++i) if (s->buttons[i]) bits |= 1u << i;
		if (bits != s_lastBits) {
			s_lastBits = bits;
			CANE_DIAG_LOG("[Input] gamepad buttons=0x%04X (glfw idx: A0 B1 X2 Y3 LB4 RB5 BACK6 START7 GUIDE8 LS9 RS10 UP11 RIGHT12 DOWN13 LEFT14)", bits);
		}
	}
#endif
	auto ax = [&](SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(p->gc, a) / 32767.0f; };
	s->axes[GLFW_GAMEPAD_AXIS_LEFT_X]  = ax(SDL_CONTROLLER_AXIS_LEFTX);
	s->axes[GLFW_GAMEPAD_AXIS_LEFT_Y]  = ax(SDL_CONTROLLER_AXIS_LEFTY);
	s->axes[GLFW_GAMEPAD_AXIS_RIGHT_X] = ax(SDL_CONTROLLER_AXIS_RIGHTX);
	s->axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] = ax(SDL_CONTROLLER_AXIS_RIGHTY);
	// GLFW triggers rest at -1 and reach +1; SDL reports 0..1.
	s->axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER]  = ax(SDL_CONTROLLER_AXIS_TRIGGERLEFT)  * 2.0f - 1.0f;
	s->axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = ax(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) * 2.0f - 1.0f;
	return GLFW_TRUE;
}

} // extern "C"

// ---- ImGui stub backend (debug overlay is display-only on Android) ----
bool ImGui_ImplGlfw_InitForOpenGL(GLFWwindow*, bool) { return true; }
void ImGui_ImplGlfw_NewFrame()
{
	ImGuiIO& io = ImGui::GetIO();
	int w = 0, h = 0;
	if (g_win) SDL_GL_GetDrawableSize(g_win->sdl, &w, &h);
	io.DisplaySize = ImVec2((float)w, (float)h);
	static Uint64 last = SDL_GetPerformanceCounter();
	const Uint64 now = SDL_GetPerformanceCounter();
	io.DeltaTime = std::max(1e-5f, (float)((double)(now - last) / (double)SDL_GetPerformanceFrequency()));
	last = now;
}
void ImGui_ImplGlfw_Shutdown() {}

// 1 when a hardware keyboard is attached (Configuration.keyboard != KEYBOARD_NOKEYS). Used by the menus to
// hide the keyboard-mapping entry on handhelds and phones.
extern "C" int cane_android_has_hw_keyboard()
{
	JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
	jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
	if (env == nullptr || activity == nullptr)
		return 0;
	int result = 0;
	jclass activityClass = env->GetObjectClass(activity);
	jmethodID getResources = env->GetMethodID(activityClass, "getResources", "()Landroid/content/res/Resources;");
	jobject resources = getResources != nullptr ? env->CallObjectMethod(activity, getResources) : nullptr;
	if (resources != nullptr)
	{
		jclass resourcesClass = env->GetObjectClass(resources);
		jmethodID getConfiguration = env->GetMethodID(resourcesClass, "getConfiguration", "()Landroid/content/res/Configuration;");
		jobject configuration = getConfiguration != nullptr ? env->CallObjectMethod(resources, getConfiguration) : nullptr;
		if (configuration != nullptr)
		{
			jclass configurationClass = env->GetObjectClass(configuration);
			jfieldID keyboard = env->GetFieldID(configurationClass, "keyboard", "I");
			if (keyboard != nullptr)
				result = env->GetIntField(configuration, keyboard) != 1 ? 1 : 0;
			env->DeleteLocalRef(configurationClass);
			env->DeleteLocalRef(configuration);
		}
		env->DeleteLocalRef(resourcesClass);
		env->DeleteLocalRef(resources);
	}
	if (env->ExceptionCheck())
		env->ExceptionClear();
	env->DeleteLocalRef(activityClass);
	env->DeleteLocalRef(activity);
	return result;
}

// Opens the Java save backup / restore screen (CaneActivity.openSavesScreen).
extern "C" void cane_android_open_saves_screen()
{
	JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
	jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
	if (env == nullptr || activity == nullptr)
		return;
	jclass cls = env->GetObjectClass(activity);
	jmethodID method = env->GetMethodID(cls, "openSavesScreen", "()V");
	if (method != nullptr)
		env->CallVoidMethod(activity, method);
	if (env->ExceptionCheck())
		env->ExceptionClear();
	env->DeleteLocalRef(cls);
	env->DeleteLocalRef(activity);
}

// The on-screen controller as a gamepad state for JOY::Update (Input.cpp). Returns 1 while it is active.
extern "C" int cane_touch_pad_state(GLFWgamepadstate* state)
{
	if (!cane::touch::Active()) return 0;
	cane::touch::FillState(state);
	return 1;
}

// Called from the main loop every frame (between ImGui::NewFrame and ImGui::Render).
extern "C" void cane_touch_draw()
{
	const ImGuiIO& io = ImGui::GetIO();
	cane::touch::Draw(ImGui::GetForegroundDrawList(), io.DisplaySize.x, io.DisplaySize.y);
}

// Menu labels / actions for the three touch settings (0 = mode, 1 = opacity, 2 = size).
extern "C" const char* cane_touch_label(int which, char* buf, int size)
{
	return which == 0 ? cane::touch::ModeLabel(buf, size) : which == 1 ? cane::touch::OpacityLabel(buf, size) : cane::touch::SizeLabel(buf, size);
}
extern "C" void cane_touch_cycle(int which)
{
	if (which == 0) cane::touch::CycleMode(); else if (which == 1) cane::touch::CycleOpacity();
	else if (which == 2) cane::touch::CycleSize(); else cane::touch::StartEdit();
}
