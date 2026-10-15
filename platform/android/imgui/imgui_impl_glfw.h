#pragma once
// Android: the Dear ImGui debug overlay is rendered by the OpenGL ES backend,
// but there is no pointer/keyboard backend (controller-only target). These
// feed ImGui just the display size and frame time it needs to run.
struct GLFWwindow;
bool ImGui_ImplGlfw_InitForOpenGL(GLFWwindow* window, bool install_callbacks);
void ImGui_ImplGlfw_NewFrame();
void ImGui_ImplGlfw_Shutdown();
