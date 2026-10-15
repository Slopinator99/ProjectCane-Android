#include "touch_overlay.h"

#include <GLFW/glfw3.h>
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <utility>

#include "imgui/imgui.h"

namespace cane::touch
{
namespace
{
// ---- groups: everything that can be moved / resized on its own ---------------------------------------------

enum Group
{
	G_LS, G_RS, G_DPAD, G_FACE, G_L1, G_L2, G_R1, G_R2, G_SELECT, G_START, G_L3, G_R3,
	G_COUNT
};
const char* const kGroupName[G_COUNT] = { "Left stick", "Right stick", "D-pad", "Face buttons", "L1", "L2", "R1", "R2",
                                          "Select", "Start", "L3", "R3" };
const char* const kGroupKey[G_COUNT] = { "ls", "rs", "dpad", "face", "l1", "l2", "r1", "r2", "select", "start", "l3", "r3" };

struct Place { float dx = 0, dy = 0, s = 1.0f; };   // offset in layout units (1080 px screen height), size multiplier
Place g_place[G_COUNT];
constexpr float kPlaceMin = 0.5f, kPlaceMax = 2.0f, kPlaceStep = 0.1f;

// ---- settings ---------------------------------------------------------------------------------------------

int g_mode = Auto;
int g_opacityPct = 50;     // 20..100 in steps of 10
int g_sizePct = 100;       // 60..160 in steps of 10 (all controls together)
constexpr int kOpacityMin = 20, kOpacityMax = 100, kSizeMin = 60, kSizeMax = 160, kStep = 10;
float Opacity() { return g_opacityPct / 100.0f; }
float Scale() { return g_sizePct / 100.0f; }
bool g_physicalPad = false;
bool g_initialised = false;
bool g_editing = false;
int g_selected = G_FACE;

std::string SettingsPath()
{
	const char* dir = SDL_AndroidGetExternalStoragePath();
	return std::string(dir != nullptr ? dir : ".") + "/touch_settings.cfg";
}

void Save()
{
	if (FILE* f = std::fopen(SettingsPath().c_str(), "w"))
	{
		std::fprintf(f, "mode=%d\nalpha=%d\nscale=%d\n", g_mode, g_opacityPct, g_sizePct);
		for (int i = 0; i < G_COUNT; ++i)
			std::fprintf(f, "place_%s=%.1f,%.1f,%.2f\n", kGroupKey[i], g_place[i].dx, g_place[i].dy, g_place[i].s);
		std::fclose(f);
	}
}

// ---- layout ------------------------------------------------------------------------------------------------

enum Control
{
	C_None = -1,
	C_StickL, C_StickR, C_Face, C_DPad,
	C_L1, C_L2, C_R1, C_R2, C_Start, C_Select, C_L3, C_R3,
	C_Count
};

struct Rect { float x, y, w, h; };

struct Layout
{
	float u = 1.0f;                 // pixels per layout unit (1080 px tall screen at 100% size = 1.0)
	float lsX = 0, lsY = 0, lsR = 0, rsX = 0, rsY = 0, rsR = 0;
	float faceX = 0, faceY = 0, faceOffset = 0, faceR = 0;
	float dpadX = 0, dpadY = 0, dpadR = 0, dpadDead = 0;
	float l3X = 0, l3Y = 0, l3R = 0, r3X = 0, r3Y = 0, r3R = 0;
	Rect l1{}, l2{}, r1{}, r2{}, start{}, select{};
};

float g_w = 1920.0f, g_h = 1080.0f;
Layout g_layout;

Rect Placed(float x, float y, float w, float h, const Place& p, float u)
{
	// Scale about the centre, then move.
	const float cx = x + w * 0.5f + p.dx * u, cy = y + h * 0.5f + p.dy * u;
	const float sw = w * p.s, sh = h * p.s;
	return { cx - sw * 0.5f, cy - sh * 0.5f, sw, sh };
}

void BuildLayout()
{
	// Default arrangement follows the ARMSX2 on-screen controller: shoulders in the top corners (L2 over L1, R2 over
	// R1), a big D-pad and the face-button diamond left and right of the middle, two large sticks low and near the
	// centre with L3/R3 beside them, Select and Start at the bottom centre. Sizes are in units of a 1080 px tall
	// screen; every group can then be moved and scaled on its own (see the layout editor).
	Layout& l = g_layout;
	const float u = std::max(0.3f, g_h / 1080.0f) * Scale();
	l.u = u;
	const float cx = g_w * 0.5f;
	auto at = [&](Group g, float x, float y) { return std::pair<float, float>(x + g_place[g].dx * u, y + g_place[g].dy * u); };

	l.lsR = 150.0f * u * g_place[G_LS].s;
	l.rsR = 150.0f * u * g_place[G_RS].s;
	std::tie(l.lsX, l.lsY) = at(G_LS, cx - 420.0f * u, g_h - 215.0f * u);
	std::tie(l.rsX, l.rsY) = at(G_RS, cx + 420.0f * u, g_h - 215.0f * u);

	std::tie(l.faceX, l.faceY) = at(G_FACE, g_w - 272.0f * u, g_h * 0.55f);
	l.faceOffset = 108.0f * u * g_place[G_FACE].s;
	l.faceR = 62.0f * u * g_place[G_FACE].s;

	std::tie(l.dpadX, l.dpadY) = at(G_DPAD, 190.0f * u, g_h * 0.546f);
	l.dpadR = 165.0f * u * g_place[G_DPAD].s;
	l.dpadDead = 34.0f * u * g_place[G_DPAD].s;

	std::tie(l.l3X, l.l3Y) = at(G_L3, cx - 615.0f * u, g_h - 77.0f * u);
	std::tie(l.r3X, l.r3Y) = at(G_R3, cx + 615.0f * u, g_h - 77.0f * u);
	l.l3R = 42.0f * u * g_place[G_L3].s;
	l.r3R = 42.0f * u * g_place[G_R3].s;

	l.l2 = Placed(100.0f * u, 63.0f * u, 107.0f * u, 90.0f * u, g_place[G_L2], u);
	l.l1 = Placed(98.0f * u, 222.0f * u, 112.0f * u, 56.0f * u, g_place[G_L1], u);
	l.r2 = Placed(g_w - 207.0f * u, 63.0f * u, 107.0f * u, 90.0f * u, g_place[G_R2], u);
	l.r1 = Placed(g_w - 210.0f * u, 222.0f * u, 112.0f * u, 56.0f * u, g_place[G_R1], u);

	const float bw = 100.0f * u, bh = 54.0f * u;
	l.select = Placed(cx - 95.0f * u - bw * 0.5f, g_h - 88.0f * u - bh * 0.5f, bw, bh, g_place[G_SELECT], u);
	l.start = Placed(cx + 95.0f * u - bw * 0.5f, g_h - 88.0f * u - bh * 0.5f, bw, bh, g_place[G_START], u);
}

bool InRect(const Rect& r, float x, float y, float pad)
{
	return x >= r.x - pad && x <= r.x + r.w + pad && y >= r.y - pad && y <= r.y + r.h + pad;
}

float Dist(float x, float y, float cx, float cy) { return std::hypot(x - cx, y - cy); }

// Which group is under a point; small controls are tested first so they can sit on top of bigger ones.
int GroupAt(float x, float y, float pad)
{
	const Layout& l = g_layout;
	if (Dist(x, y, l.l3X, l.l3Y) <= l.l3R + pad) return G_L3;
	if (Dist(x, y, l.r3X, l.r3Y) <= l.r3R + pad) return G_R3;
	if (InRect(l.l1, x, y, pad)) return G_L1;
	if (InRect(l.l2, x, y, pad)) return G_L2;
	if (InRect(l.r1, x, y, pad)) return G_R1;
	if (InRect(l.r2, x, y, pad)) return G_R2;
	if (InRect(l.start, x, y, pad)) return G_START;
	if (InRect(l.select, x, y, pad)) return G_SELECT;
	if (Dist(x, y, l.faceX, l.faceY) <= l.faceOffset + l.faceR + pad) return G_FACE;
	if (Dist(x, y, l.dpadX, l.dpadY) <= l.dpadR + pad) return G_DPAD;
	if (Dist(x, y, l.lsX, l.lsY) <= l.lsR + pad) return G_LS;
	if (Dist(x, y, l.rsX, l.rsY) <= l.rsR + pad) return G_RS;
	return -1;
}

// ---- fingers -----------------------------------------------------------------------------------------------

// A finger that lifts stays in the map until the game has read the pad once, so a tap that goes down and up
// between two frames is still seen as a press.
struct Finger { Control control = C_None; float x = 0, y = 0; bool up = false, observed = false; };
std::map<long long, Finger> g_fingers;

Control HitTest(float x, float y)
{
	const Layout& l = g_layout;
	// A stick grabs a finger from a little outside its dish so a thumb does not have to be exact.
	if (Dist(x, y, l.lsX, l.lsY) <= l.lsR * 1.33f) return C_StickL;
	if (Dist(x, y, l.rsX, l.rsY) <= l.rsR * 1.33f) return C_StickR;
	if (Dist(x, y, l.faceX, l.faceY) <= l.faceOffset + l.faceR * 1.2f) return C_Face;
	if (Dist(x, y, l.dpadX, l.dpadY) <= l.dpadR) return C_DPad;
	if (Dist(x, y, l.l3X, l.l3Y) <= l.l3R * 1.4f) return C_L3;
	if (Dist(x, y, l.r3X, l.r3Y) <= l.r3R * 1.4f) return C_R3;
	const float pad = 14.0f * l.u;
	if (InRect(l.l1, x, y, pad)) return C_L1;
	if (InRect(l.l2, x, y, pad)) return C_L2;
	if (InRect(l.r1, x, y, pad)) return C_R1;
	if (InRect(l.r2, x, y, pad)) return C_R2;
	if (InRect(l.start, x, y, pad)) return C_Start;
	if (InRect(l.select, x, y, pad)) return C_Select;
	return C_None;
}

struct Snapshot
{
	bool button[C_Count] = {};
	bool face[4] = {};              // 0 triangle(top) 1 circle(right) 2 cross(bottom) 3 square(left)
	bool dpad[4] = {};              // 0 up 1 right 2 down 3 left
	float lx = 0, ly = 0, rx = 0, ry = 0;
};

void StickVector(const Finger& f, float cx, float cy, float radius, float& ox, float& oy)
{
	float dx = f.x - cx, dy = f.y - cy;
	const float len = std::hypot(dx, dy);
	const float scale = len > radius ? radius / len : 1.0f;
	dx *= scale; dy *= scale;
	const float nx = dx / radius, ny = dy / radius;
	const float mag = std::hypot(nx, ny);
	constexpr float kDead = 0.12f;
	if (mag < kDead) { ox = 0; oy = 0; return; }
	const float rescaled = (mag - kDead) / (1.0f - kDead);
	ox = nx / mag * rescaled;
	oy = ny / mag * rescaled;
}

Snapshot Compute()
{
	Snapshot s;
	if (g_editing) return s;          // the layout editor swallows touches
	const Layout& l = g_layout;
	const float slack = 30.0f * l.u;
	for (const auto& kv : g_fingers)
	{
		const Finger& f = kv.second;
		switch (f.control)
		{
		case C_StickL: if (!f.up) StickVector(f, l.lsX, l.lsY, l.lsR, s.lx, s.ly); break;
		case C_StickR: if (!f.up) StickVector(f, l.rsX, l.rsY, l.rsR, s.rx, s.ry); break;
		case C_Face:
		{
			// The thumb may roll between buttons: take the nearest one within reach.
			const float bx[4] = { l.faceX, l.faceX + l.faceOffset, l.faceX, l.faceX - l.faceOffset };
			const float by[4] = { l.faceY - l.faceOffset, l.faceY, l.faceY + l.faceOffset, l.faceY };
			int best = -1; float bestD = l.faceR * 1.5f;
			for (int i = 0; i < 4; ++i)
			{
				const float d = Dist(f.x, f.y, bx[i], by[i]);
				if (d < bestD) { bestD = d; best = i; }
			}
			if (best >= 0) s.face[best] = true;
			break;
		}
		case C_DPad:
		{
			const float dx = f.x - l.dpadX, dy = f.y - l.dpadY;
			if (std::hypot(dx, dy) >= l.dpadDead)
			{
				const float a = std::atan2(dy, dx) * 180.0f / 3.14159265f;   // 0 = right, 90 = down
				if (a > -67.5f && a < 67.5f) s.dpad[1] = true;
				if (a > 22.5f && a < 157.5f) s.dpad[2] = true;
				if (a > 112.5f || a < -112.5f) s.dpad[3] = true;
				if (a < -22.5f && a > -157.5f) s.dpad[0] = true;
			}
			break;
		}
		case C_L1: if (InRect(l.l1, f.x, f.y, slack)) s.button[C_L1] = true; break;
		case C_L2: if (InRect(l.l2, f.x, f.y, slack)) s.button[C_L2] = true; break;
		case C_R1: if (InRect(l.r1, f.x, f.y, slack)) s.button[C_R1] = true; break;
		case C_R2: if (InRect(l.r2, f.x, f.y, slack)) s.button[C_R2] = true; break;
		case C_Start: if (InRect(l.start, f.x, f.y, slack)) s.button[C_Start] = true; break;
		case C_Select: if (InRect(l.select, f.x, f.y, slack)) s.button[C_Select] = true; break;
		case C_L3: if (Dist(f.x, f.y, l.l3X, l.l3Y) <= l.l3R * 1.4f + slack) s.button[C_L3] = true; break;
		case C_R3: if (Dist(f.x, f.y, l.r3X, l.r3Y) <= l.r3R * 1.4f + slack) s.button[C_R3] = true; break;
		default: break;
		}
	}
	return s;
}

// ---- layout editor -----------------------------------------------------------------------------------------

enum Tool { T_Done, T_Reset, T_Smaller, T_Bigger, T_Count };
const char* const kToolName[T_Count] = { "DONE", "RESET", "SMALLER", "BIGGER" };

Rect ToolRect(int i)
{
	const float u = g_layout.u, bw = 190.0f * u, bh = 62.0f * u, gap = 16.0f * u;
	const float total = T_Count * bw + (T_Count - 1) * gap;
	return { g_w * 0.5f - total * 0.5f + i * (bw + gap), 18.0f * u, bw, bh };
}

struct Drag { int group = -1; float lastX = 0, lastY = 0; };
std::map<long long, Drag> g_drags;

void ClampPlace(int g)
{
	// Keep the group's own centre on screen.
	const float u = g_layout.u;
	Place& p = g_place[g];
	p.dx = std::max(-1.2f * g_w / std::max(u, 0.01f), std::min(1.2f * g_w / std::max(u, 0.01f), p.dx));
	p.dy = std::max(-1.2f * g_h / std::max(u, 0.01f), std::min(1.2f * g_h / std::max(u, 0.01f), p.dy));
	p.s = std::max(kPlaceMin, std::min(kPlaceMax, p.s));
}

void KeepOnScreen(int g)
{
	// Rebuild with the current place and, if the group's centre left the screen, pull it back.
	BuildLayout();
	const Layout& l = g_layout;
	float cx = 0, cy = 0;
	switch (g)
	{
	case G_LS: cx = l.lsX; cy = l.lsY; break;
	case G_RS: cx = l.rsX; cy = l.rsY; break;
	case G_DPAD: cx = l.dpadX; cy = l.dpadY; break;
	case G_FACE: cx = l.faceX; cy = l.faceY; break;
	case G_L3: cx = l.l3X; cy = l.l3Y; break;
	case G_R3: cx = l.r3X; cy = l.r3Y; break;
	case G_L1: cx = l.l1.x + l.l1.w * 0.5f; cy = l.l1.y + l.l1.h * 0.5f; break;
	case G_L2: cx = l.l2.x + l.l2.w * 0.5f; cy = l.l2.y + l.l2.h * 0.5f; break;
	case G_R1: cx = l.r1.x + l.r1.w * 0.5f; cy = l.r1.y + l.r1.h * 0.5f; break;
	case G_R2: cx = l.r2.x + l.r2.w * 0.5f; cy = l.r2.y + l.r2.h * 0.5f; break;
	case G_SELECT: cx = l.select.x + l.select.w * 0.5f; cy = l.select.y + l.select.h * 0.5f; break;
	case G_START: cx = l.start.x + l.start.w * 0.5f; cy = l.start.y + l.start.h * 0.5f; break;
	}
	const float m = 30.0f * l.u;
	const float nx = std::max(m, std::min(g_w - m, cx)), ny = std::max(m, std::min(g_h - m, cy));
	g_place[g].dx += (nx - cx) / l.u;
	g_place[g].dy += (ny - cy) / l.u;
}

void EditFingerDown(long long id, float x, float y)
{
	// toolbar first
	for (int i = 0; i < T_Count; ++i)
	{
		if (!InRect(ToolRect(i), x, y, 0)) continue;
		switch (i)
		{
		case T_Done: g_editing = false; g_drags.clear(); Save(); break;
		case T_Reset: for (Place& p : g_place) p = Place(); Save(); break;
		case T_Smaller: g_place[g_selected].s = std::max(kPlaceMin, g_place[g_selected].s - kPlaceStep); KeepOnScreen(g_selected); Save(); break;
		case T_Bigger: g_place[g_selected].s = std::min(kPlaceMax, g_place[g_selected].s + kPlaceStep); KeepOnScreen(g_selected); Save(); break;
		}
		return;
	}
	const int group = GroupAt(x, y, 14.0f * g_layout.u);
	if (group >= 0)
	{
		g_selected = group;
		Drag d; d.group = group; d.lastX = x; d.lastY = y;
		g_drags[id] = d;
	}
}

void EditFingerMove(long long id, float x, float y)
{
	auto it = g_drags.find(id);
	if (it == g_drags.end()) return;
	Drag& d = it->second;
	const float u = g_layout.u;
	g_place[d.group].dx += (x - d.lastX) / u;
	g_place[d.group].dy += (y - d.lastY) / u;
	d.lastX = x; d.lastY = y;
	KeepOnScreen(d.group);
}

} // namespace

// ---- public ------------------------------------------------------------------------------------------------

void Init()
{
	if (g_initialised) return;
	g_initialised = true;
	if (FILE* f = std::fopen(SettingsPath().c_str(), "r"))
	{
		char line[96];
		while (std::fgets(line, sizeof(line), f))
		{
			int v = 0;
			if (std::sscanf(line, "mode=%d", &v) == 1 && v >= 0 && v <= 2) g_mode = v;
			else if (std::sscanf(line, "alpha=%d", &v) == 1 && v >= kOpacityMin && v <= kOpacityMax) g_opacityPct = v;
			else if (std::sscanf(line, "scale=%d", &v) == 1 && v >= kSizeMin && v <= kSizeMax) g_sizePct = v;
			else if (std::strncmp(line, "place_", 6) == 0)
			{
				for (int i = 0; i < G_COUNT; ++i)
				{
					const size_t n = std::strlen(kGroupKey[i]);
					if (std::strncmp(line + 6, kGroupKey[i], n) != 0 || line[6 + n] != '=') continue;
					float dx = 0, dy = 0, s = 1;
					if (std::sscanf(line + 7 + n, "%f,%f,%f", &dx, &dy, &s) == 3)
					{
						g_place[i].dx = dx; g_place[i].dy = dy;
						g_place[i].s = std::max(kPlaceMin, std::min(kPlaceMax, s));
					}
					break;
				}
			}
		}
		std::fclose(f);
	}
}

void SetPhysicalPadPresent(bool present) { g_physicalPad = present; }

void BeginFrame()
{
	for (auto it = g_fingers.begin(); it != g_fingers.end();)
		it = (it->second.up && it->second.observed) ? g_fingers.erase(it) : std::next(it);
}

bool Active()
{
	if (g_editing) return true;
	if (g_mode == Off) return false;
	if (g_mode == On) return true;
	return !g_physicalPad;
}

void OnFinger(unsigned eventType, long long fingerId, float nx, float ny)
{
	const float x = nx * g_w, y = ny * g_h;
	if (g_editing)
	{
		if (eventType == SDL_FINGERDOWN) EditFingerDown(fingerId, x, y);
		else if (eventType == SDL_FINGERMOTION) EditFingerMove(fingerId, x, y);
		else if (eventType == SDL_FINGERUP) { if (g_drags.erase(fingerId)) Save(); }
		return;
	}
	if (eventType == SDL_FINGERDOWN)
	{
		if (!Active()) return;
		Finger f;
		f.x = x; f.y = y;
		f.control = HitTest(x, y);
		g_fingers[fingerId] = f;
	}
	else if (eventType == SDL_FINGERMOTION)
	{
		auto it = g_fingers.find(fingerId);
		if (it != g_fingers.end()) { it->second.x = x; it->second.y = y; }
	}
	else if (eventType == SDL_FINGERUP)
	{
		auto it = g_fingers.find(fingerId);
		if (it != g_fingers.end()) { it->second.x = x; it->second.y = y; it->second.up = true; }
	}
}

void FillState(GLFWgamepadstate* s)
{
	*s = {};
	s->axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = -1.0f;
	s->axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = -1.0f;
	if (!Active()) return;
	const Snapshot c = Compute();
	for (auto& kv : g_fingers) kv.second.observed = true;
	s->buttons[GLFW_GAMEPAD_BUTTON_Y] = c.face[0];        // triangle
	s->buttons[GLFW_GAMEPAD_BUTTON_B] = c.face[1];        // circle
	s->buttons[GLFW_GAMEPAD_BUTTON_A] = c.face[2];        // cross
	s->buttons[GLFW_GAMEPAD_BUTTON_X] = c.face[3];        // square
	s->buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] = c.dpad[0];
	s->buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] = c.dpad[1];
	s->buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] = c.dpad[2];
	s->buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] = c.dpad[3];
	s->buttons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER] = c.button[C_L1];
	s->buttons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER] = c.button[C_R1];
	s->buttons[GLFW_GAMEPAD_BUTTON_START] = c.button[C_Start];
	s->buttons[GLFW_GAMEPAD_BUTTON_BACK] = c.button[C_Select];
	s->buttons[GLFW_GAMEPAD_BUTTON_LEFT_THUMB] = c.button[C_L3];
	s->buttons[GLFW_GAMEPAD_BUTTON_RIGHT_THUMB] = c.button[C_R3];
	if (c.button[C_L2]) s->axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = 1.0f;
	if (c.button[C_R2]) s->axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = 1.0f;
	s->axes[GLFW_GAMEPAD_AXIS_LEFT_X] = c.lx;
	s->axes[GLFW_GAMEPAD_AXIS_LEFT_Y] = c.ly;
	s->axes[GLFW_GAMEPAD_AXIS_RIGHT_X] = c.rx;
	s->axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] = c.ry;
}

void Draw(ImDrawList* dl, float w, float h)
{
	if (w > 0 && h > 0 && (w != g_w || h != g_h)) { g_w = w; g_h = h; }
	BuildLayout();
	if (!Active() || dl == nullptr) return;

	const Layout& l = g_layout;
	const Snapshot c = Compute();
	// In the editor the controls are drawn at full strength so they are easy to see and grab.
	const float a = g_editing ? std::max(0.7f, Opacity()) : Opacity();
	auto col = [&](float r, float g, float b, float alpha) { return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, std::min(1.0f, alpha * a))); };
	// Dark, soft shapes: pressed controls light up, idle ones stay quiet.
	const ImU32 fill = col(0.22f, 0.22f, 0.24f, 0.70f), lit = col(0.80f, 0.80f, 0.84f, 0.75f), edge = col(1.0f, 1.0f, 1.0f, 0.28f);
	const ImU32 text = col(1.0f, 1.0f, 1.0f, 0.75f);
	const float lineW = std::max(1.5f, 2.0f * l.u);
	const float fontSize = 26.0f * l.u;
	auto label = [&](float cx, float cy, const char* t, ImU32 colour, float size)
	{
		const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(size, 1e9f, 0.0f, t);
		dl->AddText(ImGui::GetFont(), size, ImVec2(cx - sz.x * 0.5f, cy - sz.y * 0.5f), colour, t);
	};

	// analog sticks: a big dish with a big thumb
	auto stick = [&](float cx, float cy, float r, float sx, float sy)
	{
		dl->AddCircleFilled(ImVec2(cx, cy), r, col(0.30f, 0.30f, 0.32f, 0.45f), 56);
		dl->AddCircle(ImVec2(cx, cy), r, edge, 56, lineW);
		const bool moved = sx != 0 || sy != 0;
		const ImVec2 thumb(cx + sx * r * 0.45f, cy + sy * r * 0.45f);
		dl->AddCircleFilled(thumb, r * 0.58f, moved ? lit : col(0.55f, 0.55f, 0.58f, 0.60f), 40);
		dl->AddCircle(thumb, r * 0.58f, edge, 40, lineW);
	};
	stick(l.lsX, l.lsY, l.lsR, c.lx, c.ly);
	stick(l.rsX, l.rsY, l.rsR, c.rx, c.ry);

	// D-pad: a plus made of four rounded arms around a centre block
	{
		const float arm = l.dpadR, thick = l.dpadR * 0.70f, gap = l.dpadR * 0.02f;
		auto bar = [&](float x0, float y0, float x1, float y1, bool on)
		{
			dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), on ? lit : fill, thick * 0.22f);
			dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), edge, thick * 0.22f, 0, lineW);
		};
		const float hx = thick * 0.5f;
		dl->AddRectFilled(ImVec2(l.dpadX - hx, l.dpadY - hx), ImVec2(l.dpadX + hx, l.dpadY + hx), fill, thick * 0.10f);
		bar(l.dpadX - hx, l.dpadY - arm, l.dpadX + hx, l.dpadY - hx - gap, c.dpad[0]);
		bar(l.dpadX - hx, l.dpadY + hx + gap, l.dpadX + hx, l.dpadY + arm, c.dpad[2]);
		bar(l.dpadX - arm, l.dpadY - hx, l.dpadX - hx - gap, l.dpadY + hx, c.dpad[3]);
		bar(l.dpadX + hx + gap, l.dpadY - hx, l.dpadX + arm, l.dpadY + hx, c.dpad[1]);
	}

	// face buttons, PlayStation colours and symbols
	{
		struct Face { float x, y; bool on; float r, g, b; int symbol; };
		const Face faces[4] = {
			{ l.faceX, l.faceY - l.faceOffset, c.face[0], 0.35f, 0.85f, 0.55f, 0 },   // triangle
			{ l.faceX + l.faceOffset, l.faceY, c.face[1], 0.95f, 0.35f, 0.35f, 1 },   // circle
			{ l.faceX, l.faceY + l.faceOffset, c.face[2], 0.45f, 0.6f, 1.0f, 2 },     // cross
			{ l.faceX - l.faceOffset, l.faceY, c.face[3], 0.95f, 0.5f, 0.85f, 3 },    // square
		};
		for (const Face& f : faces)
		{
			dl->AddCircleFilled(ImVec2(f.x, f.y), l.faceR, f.on ? col(f.r, f.g, f.b, 0.60f) : fill, 36);
			dl->AddCircle(ImVec2(f.x, f.y), l.faceR, edge, 36, lineW);
			const ImU32 sym = col(f.r, f.g, f.b, 1.0f);
			const float s = l.faceR * 0.42f;
			const float sw = lineW * 1.5f;
			switch (f.symbol)
			{
			case 0: dl->AddTriangle(ImVec2(f.x, f.y - s), ImVec2(f.x + s, f.y + s * 0.8f), ImVec2(f.x - s, f.y + s * 0.8f), sym, sw); break;
			case 1: dl->AddCircle(ImVec2(f.x, f.y), s, sym, 24, sw); break;
			case 2: dl->AddLine(ImVec2(f.x - s, f.y - s), ImVec2(f.x + s, f.y + s), sym, sw);
			        dl->AddLine(ImVec2(f.x + s, f.y - s), ImVec2(f.x - s, f.y + s), sym, sw); break;
			case 3: dl->AddRect(ImVec2(f.x - s, f.y - s), ImVec2(f.x + s, f.y + s), sym, 0.0f, 0, sw); break;
			}
		}
	}

	// shoulder / start / select pills
	auto pill = [&](const Rect& r, bool on, const char* t, float rounding)
	{
		dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), on ? lit : fill, rounding);
		dl->AddRect(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), edge, rounding, 0, lineW);
		label(r.x + r.w * 0.5f, r.y + r.h * 0.5f, t, text, std::min(fontSize, r.h * 0.55f));
	};
	pill(l.l2, c.button[C_L2], "L2", l.l2.h * 0.32f);
	pill(l.l1, c.button[C_L1], "L1", l.l1.h * 0.40f);
	pill(l.r2, c.button[C_R2], "R2", l.r2.h * 0.32f);
	pill(l.r1, c.button[C_R1], "R1", l.r1.h * 0.40f);
	pill(l.select, c.button[C_Select], "SELECT", l.select.h * 0.40f);
	pill(l.start, c.button[C_Start], "START", l.start.h * 0.40f);

	// L3 / R3: stick clicks
	auto thumbButton = [&](float cx, float cy, float r, bool on, const char* t)
	{
		dl->AddCircleFilled(ImVec2(cx, cy), r, on ? lit : fill, 28);
		dl->AddCircle(ImVec2(cx, cy), r, edge, 28, lineW);
		label(cx, cy, t, text, std::min(fontSize, r * 0.9f));
	};
	thumbButton(l.l3X, l.l3Y, l.l3R, c.button[C_L3], "L3");
	thumbButton(l.r3X, l.r3Y, l.r3R, c.button[C_R3], "R3");

	if (!g_editing) return;

	// ---- layout editor overlay: outlines, selection and the toolbar ----
	const ImU32 outline = ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.55f));
	const ImU32 chosen = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.85f, 0.2f, 0.95f));
	const float pad = 8.0f * l.u;
	for (int g = 0; g < G_COUNT; ++g)
	{
		const ImU32 oc = g == g_selected ? chosen : outline;
		const float lw = g == g_selected ? lineW * 2.0f : lineW;
		switch (g)
		{
		case G_LS: dl->AddCircle(ImVec2(l.lsX, l.lsY), l.lsR + pad, oc, 56, lw); break;
		case G_RS: dl->AddCircle(ImVec2(l.rsX, l.rsY), l.rsR + pad, oc, 56, lw); break;
		case G_DPAD: dl->AddCircle(ImVec2(l.dpadX, l.dpadY), l.dpadR + pad, oc, 56, lw); break;
		case G_FACE: dl->AddCircle(ImVec2(l.faceX, l.faceY), l.faceOffset + l.faceR + pad, oc, 56, lw); break;
		case G_L3: dl->AddCircle(ImVec2(l.l3X, l.l3Y), l.l3R + pad, oc, 32, lw); break;
		case G_R3: dl->AddCircle(ImVec2(l.r3X, l.r3Y), l.r3R + pad, oc, 32, lw); break;
		default:
		{
			const Rect& r = g == G_L1 ? l.l1 : g == G_L2 ? l.l2 : g == G_R1 ? l.r1 : g == G_R2 ? l.r2 : g == G_SELECT ? l.select : l.start;
			dl->AddRect(ImVec2(r.x - pad, r.y - pad), ImVec2(r.x + r.w + pad, r.y + r.h + pad), oc, 8.0f * l.u, 0, lw);
			break;
		}
		}
	}
	for (int i = 0; i < T_Count; ++i)
	{
		const Rect r = ToolRect(i);
		dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), ImGui::ColorConvertFloat4ToU32(ImVec4(0.12f, 0.12f, 0.16f, 0.92f)), 10.0f * l.u);
		dl->AddRect(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), i == T_Done ? chosen : outline, 10.0f * l.u, 0, lineW * 1.5f);
		label(r.x + r.w * 0.5f, r.y + r.h * 0.5f, kToolName[i], ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 1)), fontSize);
	}
	char info[96];
	std::snprintf(info, sizeof(info), "Drag a control to move it.   Selected: %s (%d%%)", kGroupName[g_selected], (int)std::lround(g_place[g_selected].s * 100.0f));
	label(g_w * 0.5f, 18.0f * l.u + 62.0f * l.u + 26.0f * l.u, info, ImGui::ColorConvertFloat4ToU32(ImVec4(1, 0.95f, 0.6f, 1)), fontSize);
}

const char* ModeLabel(char* buf, int size)
{
	static const char* names[3] = { "Auto (hide with a controller)", "Always On", "Off" };
	std::snprintf(buf, size, "Touch Controls: %s", names[g_mode]);
	return buf;
}
const char* OpacityLabel(char* buf, int size)
{
	std::snprintf(buf, size, "Touch Opacity: %d%%", g_opacityPct);
	return buf;
}
const char* SizeLabel(char* buf, int size)
{
	std::snprintf(buf, size, "Touch Size: %d%%", g_sizePct);
	return buf;
}
void CycleMode() { g_mode = (g_mode + 1) % 3; g_fingers.clear(); Save(); }
void CycleOpacity() { g_opacityPct = g_opacityPct >= kOpacityMax ? kOpacityMin : g_opacityPct + kStep; Save(); }
void CycleSize() { g_sizePct = g_sizePct >= kSizeMax ? kSizeMin : g_sizePct + kStep; g_fingers.clear(); Save(); }
void StartEdit() { g_editing = true; g_drags.clear(); g_fingers.clear(); }
}
