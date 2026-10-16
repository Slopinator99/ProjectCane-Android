#pragma once
// On-screen controller for Android. It is presented to the game as a virtual gamepad (see glfw_sdl.cpp), so the
// game's own input code needs no changes: left stick, right stick, D-pad, the four face buttons, L1/L2/R1/R2,
// Start and Select. It is drawn with ImGui's foreground draw list on top of the final image.

struct GLFWgamepadstate;
struct ImDrawList;

namespace cane::touch
{
enum Mode { Auto = 0, On = 1, Off = 2 };

void SetSafeInsets(float left, float top, float right, float bottom);   // display cutout (pixels): controls stay clear
void Init();                                            // loads touch_settings.cfg
void OnFinger(unsigned eventType, long long fingerId, float nx, float ny);   // SDL_FINGERDOWN/MOTION/UP
void BeginFrame();                                      // once per poll: drops fingers that lifted and were already seen
void SetPhysicalPadPresent(bool present);               // Auto mode hides the overlay while a real pad is connected
bool Active();                                          // acts as a pad and is drawn
void FillState(GLFWgamepadstate* state);
void Draw(ImDrawList* list, float displayW, float displayH);

const char* ModeLabel(char* buf, int size);
const char* OpacityLabel(char* buf, int size);
const char* SizeLabel(char* buf, int size);
void CycleMode();
void CycleOpacity();
void CycleSize();
void StartEdit();                                       // layout editor: drag controls, resize them, Done to leave
}
