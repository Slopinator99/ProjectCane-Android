#include "main.h"
#include "hide.h"
#include "update.h"
#include "jump.h"
#include "pipe.h"
#include "target.h"
#include "sensor.h"
#include "save.h"
#include "fmv.h"
#include "sound.h"
#include "tv.h"
#include "resource1.h"
#include "runtime_error.h"
#include "emitter.h"
#include "blip.h"
#include "content.h"
#include "../platform/platform.h"
#ifdef __ANDROID__
extern "C" void cane_touch_draw();
extern "C" void cane_second_screen_tick();
extern "C" void cane_android_request_refresh_rate(int hz);
#endif
#if defined(__ANDROID__) && defined(CANE_DIAG)
extern int g_caneSkip;
extern int g_caneFrag;
extern int g_caneVert;
extern int g_caneNoBake;
extern int g_caneNoPrebake;
extern int g_caneFrameBakes;
static int g_caneRaster = 0;
static int g_caneCapture = 0;   // debug.cane.capture N: save N consecutive frames (480x270) to the app dir
static int g_caneBinning = 0;   // 0 default, 1 CPU-optimized, 2 GPU-optimized, 3 render direct (QCOM_binning_control)
static bool g_caneHasBinning = false;
extern int g_cFrameGlobs, g_cFrameCelGlobs, g_dynamicTextureCount, g_opaqueCount, g_cutOutCount, g_celBorderCount, g_projVolumeCount, g_translucentCount, g_translucentAddCount, g_backGroundCount, g_foreGroundCount, g_blipCount, g_murkFillCount, g_blotContextCount;
#endif
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#if defined(__ANDROID__)
// Renames main() to SDL_main(); SDLActivity's native thread calls it.
#include <SDL_main.h>
#include <android/log.h>
#include <unistd.h>
#endif

bool g_fDeveloperDebugMode = false;

namespace
{
void LoadDebugModeSetting()
{
	g_fDeveloperDebugMode = cane::DebugMode();
}

class FramePacer
{
public:
    using Clock = std::chrono::steady_clock;

    FramePacer()
        : m_deadline(Clock::now())
    {
#ifdef _WIN32
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is 0x2. Use the value directly
        // so this still builds with Windows SDKs that predate the named flag.
        m_timer = CreateWaitableTimerExW(
            nullptr, nullptr, 0x2, TIMER_MODIFY_STATE | SYNCHRONIZE);

        if (m_timer == nullptr)
            m_timer = CreateWaitableTimerExW(
                nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
#endif
    }

    ~FramePacer()
    {
#ifdef _WIN32
        if (m_timer != nullptr)
            CloseHandle(m_timer);
#endif
    }

    void Wait()
    {
        constexpr auto kSpinMargin = std::chrono::microseconds(500);
        if (m_frameRate != g_targetFrameRate)
        {
            m_frameRate = g_targetFrameRate;
            m_deadline = Clock::now();
#ifdef __ANDROID__
            cane_android_request_refresh_rate(m_frameRate);   // let the screen run at a matching refresh rate
#endif
        }

        const auto frameDuration = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / static_cast<double>(m_frameRate)));

        m_deadline += frameDuration;
        auto now = Clock::now();

        if (now >= m_deadline)
        {
            // Drop missed presentation deadlines. The next call schedules one
            // complete interval from this presentation rather than catching up.
            m_deadline = now;
            return;
        }

        const auto coarseDeadline = m_deadline - kSpinMargin;
        if (now < coarseDeadline)
        {
#ifdef _WIN32
            if (m_timer != nullptr)
            {
                const auto wait100ns = std::chrono::duration_cast<
                    std::chrono::duration<LONGLONG, std::ratio<1, 10000000>>>(
                        coarseDeadline - now).count();

                LARGE_INTEGER dueTime{};
                dueTime.QuadPart = -(std::max<LONGLONG>)(1, wait100ns);

                if (SetWaitableTimer(m_timer, &dueTime, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(m_timer, INFINITE);
                else
                    std::this_thread::sleep_until(coarseDeadline);
            }
            else
            {
                std::this_thread::sleep_until(coarseDeadline);
            }
#else
			std::this_thread::sleep_until(coarseDeadline);
#endif
        }

        // YieldProcessor is a CPU pause hint, not a scheduler yield. It keeps
        // the final sub-millisecond boundary precise without giving up a full
        // Windows scheduling quantum.
        while (Clock::now() < m_deadline)
#ifdef _WIN32
            YieldProcessor();
#else
			std::this_thread::yield();
#endif
    }

    void Resync()
    {
        m_frameRate = g_targetFrameRate;
        m_deadline = Clock::now();
    }

private:
#ifdef _WIN32
    HANDLE m_timer = nullptr;
#endif
    Clock::time_point m_deadline;
    int m_frameRate = 60;
};

FramePacer* s_framePacer = nullptr;
bool s_runningFrame = false;

void FlushTransitionFrame()
{
    // The previous game frame has already been presented. Drain outstanding
    // GPU work and leave that front-buffer image visible while the synchronous
    // world transition runs. This is the original lightweight desktop path:
    // no extra framebuffer allocation, copy, redraw, or swap.
    glFinish();
}

#if defined(__ANDROID__) && defined(CANE_DIAG)
#include <sys/system_properties.h>
namespace {
using DiagClock = std::chrono::steady_clock;
struct FrameDiag
{
	double updateMs = 0, renderMs = 0, finishMs = 0, presentMs = 0, waitMs = 0, frameMs = 0;
	int frames = 0;
	DiagClock::time_point windowStart = DiagClock::now();
	DiagClock::time_point mark = DiagClock::now();
	DiagClock::time_point frameStart = DiagClock::now();
	int finishMode = -1;
	DiagClock::time_point lastPoll = DiagClock::now();
	double Lap() { auto n = DiagClock::now(); double ms = std::chrono::duration<double, std::milli>(n - mark).count(); mark = n; return ms; }
};
FrameDiag s_diag;
}
#endif

void RunGameFrame()
{
    if (s_runningFrame)
        return;

    s_runningFrame = true;
#if defined(__ANDROID__) && defined(CANE_DIAG)
    s_diag.frameStart = s_diag.mark = DiagClock::now();
    if (s_diag.finishMode < 0 || std::chrono::duration<double>(s_diag.frameStart - s_diag.lastPoll).count() >= 1.0)
    {
        // Live knobs (adb shell setprop ...), polled once a second in debug builds:
        //   debug.cane.finish 1  -> glFinish before present, to measure GPU time
        //   debug.cane.resh N    -> internal render height (0 = native)
        s_diag.lastPoll = s_diag.frameStart;
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.cane.finish", v);
        s_diag.finishMode = v[0] == '1' ? 1 : 0;
        char sk[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.cane.skip", sk);
        g_caneSkip = sk[0] ? std::atoi(sk) : 0;
        char fr[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.cane.frag", fr);   // 1 = no per-pixel shadows, 2 = flat colour
        g_caneFrag = fr[0] ? std::atoi(fr) : 0;
        char vt[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.cane.vert", vt);   // 1 = no vertex shadow loop, 2 = no lighting, 4 = no skinning
        g_caneVert = vt[0] ? std::atoi(vt) : 0;
        char cp[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.cane.capture", cp);
        g_caneCapture = cp[0] ? std::atoi(cp) : 0;
        char nb[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.cane.nobake", nb);
        g_caneNoBake = nb[0] ? std::atoi(nb) : 0;
        char np[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.cane.noprebake", np);   // 1 = bake inside the scene passes again (A/B for the Adreno glitch)
        g_caneNoPrebake = np[0] ? std::atoi(np) : 0;
        char bn[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.cane.binning", bn);
        g_caneBinning = bn[0] ? std::atoi(bn) : 0;
        char rs[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.cane.raster", rs);   // 1 = GL_RASTERIZER_DISCARD, 2 = 1px scissor
        g_caneRaster = rs[0] ? std::atoi(rs) : 0;
        char r[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.cane.resh", r);
        const int wantedRes = r[0] ? std::atoi(r) : 0;
        if (wantedRes != g_internalResolutionHeight)
        {
            g_internalResolutionHeight = wantedRes;
            int fbw = 0, fbh = 0;
            glfwGetFramebufferSize(g_gl.window, &fbw, &fbh);
            FrameBufferSizeCallBack(g_gl.window, fbw, fbh);
            __android_log_print(ANDROID_LOG_INFO, "cane", "[Frame] internal resolution height -> %d", wantedRes);
        }
    }
#endif

    // Retail leaves the movie queued while the outgoing wipe is visible.
    // Once the screen reaches black, play the movie before executing the
    // transition that replaces the current world.
    if (FCutscenePending() && g_wipe.wipes != WIPES_WipingOut)
    {
        ExecutePendingCutscenes();
    }

    if (g_transition.m_fPending != 0)
    {
		FlushTransitionFrame();
		g_transition.Execute(file);
		if (s_framePacer != nullptr)
			s_framePacer->Resync();
    }

    if (FCutscenePending() && g_wipe.wipes != WIPES_WipingOut)
    {
        ExecutePendingCutscenes();
    }

    static bool s_fF3WasDown = false;
	const bool fF3Down = g_fDeveloperDebugMode && g_gl.window != nullptr &&
        glfwGetKey(g_gl.window, GLFW_KEY_F3) == GLFW_PRESS;
    if (fF3Down && !s_fF3WasDown)
    {
        g_fDebugGuiOpen = !g_fDebugGuiOpen;

        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(g_gl.window, &framebufferWidth, &framebufferHeight);
        FrameBufferSizeCallBack(g_gl.window, framebufferWidth, framebufferHeight);
    }
    s_fF3WasDown = fF3Down;

    g_sceneFbo = g_fMsaa ? g_gl.fboMSAA : g_gl.fbo;
    glBindFramebuffer(GL_FRAMEBUFFER, g_sceneFbo);

#if defined(__ANDROID__) && defined(CANE_DIAG)
    {
        static int s_appliedBinning = -1;
        if (s_appliedBinning < 0)
        {
            const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
            g_caneHasBinning = ext != nullptr && std::strstr(ext, "GL_QCOM_binning_control") != nullptr;
            __android_log_print(ANDROID_LOG_INFO, "cane", "[Binning] GL_QCOM_binning_control %s", g_caneHasBinning ? "available" : "NOT available");
        }
        if (g_caneHasBinning && s_appliedBinning != g_caneBinning)
        {
            s_appliedBinning = g_caneBinning;
            // GL_BINNING_CONTROL_HINT_QCOM 0x8FB0; DONT_CARE / CPU_OPTIMIZED 0x8FB1 / GPU_OPTIMIZED 0x8FB2 / RENDER_DIRECT 0x8FB3
            const GLenum mode = g_caneBinning == 1 ? 0x8FB1 : g_caneBinning == 2 ? 0x8FB2 : g_caneBinning == 3 ? 0x8FB3 : GL_DONT_CARE;
            glHint(0x8FB0, mode);
            __android_log_print(ANDROID_LOG_INFO, "cane", "[Binning] hint -> %d (0x%X)", g_caneBinning, (unsigned)mode);
        }
    }
#endif
    glViewport(0, 0, g_gl.renderWidth, g_gl.renderHeight);
    glClearColor(rgbaSky.r, rgbaSky.g, rgbaSky.b, rgbaSky.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    g_joy.Update(g_gl.window);
    RenderMenuGui(g_psw);
#ifdef __ANDROID__
    cane_touch_draw();   // on-screen controller (no-op unless active)
    cane_second_screen_tick();   // episode picture on the second display
#endif

    if (g_psw != nullptr)
    {
        UpdateUi(&g_ui);
        UpdateGameState(g_clock.dt);

        SetupCm(g_pcm);
        OpenFrame();
        MarkClockTick(&g_clock);
        UpdateCodes();
        UpdateSw(g_psw, g_clock.dt);
#if defined(__ANDROID__) && defined(CANE_DIAG)
        s_diag.updateMs += s_diag.Lap();
#endif

        if (g_fRenderModels == true)
        {
            RenderSw(g_psw, g_pcm);
            RenderUi(&g_ui);
#if defined(__ANDROID__) && defined(CANE_DIAG)
            if (g_caneRaster == 1) glEnable(GL_RASTERIZER_DISCARD);
            if (g_caneRaster == 2) { glEnable(GL_SCISSOR_TEST); glScissor(0, 0, 1, 1); }
#endif
            DrawSw(g_psw, g_pcm);
#if defined(__ANDROID__) && defined(CANE_DIAG)
            if (g_caneRaster == 1) glDisable(GL_RASTERIZER_DISCARD);
            if (g_caneRaster == 2) glDisable(GL_SCISSOR_TEST);
#endif
        }

        if (g_fRenderCollision == true)
		{
            DrawSwCollisionAll(g_pcm);
		}

		if (g_fRenderDialogTriggers == true)
			DrawSwDialogTriggers(g_pcm);

        DrawUi(&g_ui);
    }
#if defined(__ANDROID__) && defined(CANE_DIAG)
    s_diag.renderMs += s_diag.Lap();
    if (s_diag.finishMode == 1)
    {
        // Diagnostic only (adb shell setprop debug.cane.finish 1): serialises CPU and GPU so
        // finishMs is the time the GPU still needed after the CPU finished issuing the frame.
        glFinish();
        s_diag.finishMs += s_diag.Lap();
    }
#endif

    if (g_fMsaa)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gl.fboMSAA);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gl.fbo);
        glBlitFramebuffer(0, 0, g_gl.renderWidth, g_gl.renderHeight,
            0, 0, g_gl.renderWidth, g_gl.renderHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(g_gl.presentX, g_gl.presentY, g_gl.presentWidth, g_gl.presentHeight);

	// Presentation is a hard OpenGL state boundary. World, shadow, particle,
	// and UI passes are allowed to use these states internally, but none of
	// them may influence the default-framebuffer clear or fullscreen composite.
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    {
        glScreenShader.Use();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_gl.fbc);
        glBindVertexArray(g_gl.sao);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
#if defined(__ANDROID__) && defined(CANE_DIAG)
    {
        // Frame capture for flicker analysis: N consecutive presented frames, scaled to 480x270 RGBA.
        static int s_capLeft = 0, s_capSeen = 0;
        static std::vector<unsigned char> s_capBuf;
        static GLuint s_capFbo = 0, s_capTex = 0;
        constexpr int kW = 480, kH = 270;
        if (g_caneCapture != s_capSeen)
        {
            s_capSeen = g_caneCapture;
            if (g_caneCapture > 0)
            {
                s_capLeft = g_caneCapture;
                s_capBuf.clear();
                s_capBuf.reserve(static_cast<size_t>(kW) * kH * 4 * s_capLeft);
            }
        }
        if (s_capLeft > 0)
        {
            if (s_capFbo == 0)
            {
                glGenTextures(1, &s_capTex);
                glBindTexture(GL_TEXTURE_2D, s_capTex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glGenFramebuffers(1, &s_capFbo);
                glBindFramebuffer(GL_FRAMEBUFFER, s_capFbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_capTex, 0);
            }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_capFbo);
            glBlitFramebuffer(g_gl.presentX, g_gl.presentY, g_gl.presentX + g_gl.presentWidth, g_gl.presentY + g_gl.presentHeight,
                0, 0, kW, kH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s_capFbo);
            const size_t off = s_capBuf.size();
            s_capBuf.resize(off + static_cast<size_t>(kW) * kH * 4);
            glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, s_capBuf.data() + off);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            __android_log_print(ANDROID_LOG_INFO, "cane", "[Cap] i=%d bakes=%d cframe=%d", g_caneCapture - s_capLeft, g_caneFrameBakes, g_cframe);
            if (--s_capLeft == 0)
            {
                if (FILE* f = std::fopen("/data/data/org.projectcane.cane/capture.bin", "wb"))
                {
                    std::fwrite(s_capBuf.data(), 1, s_capBuf.size(), f);
                    std::fclose(f);
                }
                __android_log_print(ANDROID_LOG_INFO, "cane", "[Capture] saved %zu frames", s_capBuf.size() / (static_cast<size_t>(kW) * kH * 4));
                s_capBuf.clear();
                s_capBuf.shrink_to_fit();
            }
        }
    }
    if (g_caneCapture < 0)
    {
        // Flicker detector (debug.cane.capture -1): keeps the last three presented frames and, whenever
        // the middle one jumps away from both neighbours that agree with each other, saves the triple
        // to flick_<n>.bin in the app directory (3 x 480x270 RGBA) and logs the score.
        constexpr int kW = 480, kH = 270, kPix = kW * kH;
        static std::vector<unsigned char> s_ring[3];
        static int s_n = 0, s_saved = 0, s_cool = 0;
        static GLuint s_fbo = 0, s_tex = 0;
        if (s_ring[0].empty())
            for (auto& r : s_ring) r.resize(static_cast<size_t>(kPix) * 4);
        if (s_fbo == 0)
        {
            glGenTextures(1, &s_tex);
            glBindTexture(GL_TEXTURE_2D, s_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glGenFramebuffers(1, &s_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_tex, 0);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
        glBlitFramebuffer(g_gl.presentX, g_gl.presentY, g_gl.presentX + g_gl.presentWidth, g_gl.presentY + g_gl.presentHeight,
            0, 0, kW, kH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
        unsigned char* cur = s_ring[s_n % 3].data();
        glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, cur);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        ++s_n;
        if (s_cool > 0) --s_cool;
        if (s_n >= 3)
        {
            const unsigned char* a = s_ring[(s_n - 3) % 3].data();
            const unsigned char* b = s_ring[(s_n - 2) % 3].data();
            int score = 0;
            for (int p = 0; p < kPix; ++p)
            {
                const int ga = a[p * 4 + 1], gb = b[p * 4 + 1], gc = cur[p * 4 + 1];
                if (std::abs(gb - ga) > 40 && std::abs(gc - gb) > 40 && std::abs(gc - ga) < 20)
                    ++score;
            }
            if (score > 150 && s_cool == 0)
            {
                char path[96];
                std::snprintf(path, sizeof(path), "/data/data/org.projectcane.cane/flick_%d.bin", s_saved % 24);
                if (FILE* f = std::fopen(path, "wb"))
                {
                    std::fwrite(a, 1, static_cast<size_t>(kPix) * 4, f);
                    std::fwrite(b, 1, static_cast<size_t>(kPix) * 4, f);
                    std::fwrite(cur, 1, static_cast<size_t>(kPix) * 4, f);
                    std::fclose(f);
                }
                __android_log_print(ANDROID_LOG_INFO, "cane", "[Flicker] #%d frame=%d score=%d bakes=%d", s_saved, s_n, score, g_caneFrameBakes);
                ++s_saved;
                s_cool = 3;
            }
        }
    }
#endif
#if defined(__ANDROID__) && defined(CANE_DIAG)
    g_caneFrameBakes = 0;
#endif
    glfwSwapBuffers(g_gl.window);
#if defined(__ANDROID__) && defined(CANE_DIAG)
    s_diag.presentMs += s_diag.Lap();
#endif
    if (s_framePacer != nullptr)
        s_framePacer->Wait();
#if defined(__ANDROID__) && defined(CANE_DIAG)
    {
        s_diag.waitMs += s_diag.Lap();
        s_diag.frameMs += std::chrono::duration<double, std::milli>(DiagClock::now() - s_diag.frameStart).count();
        ++s_diag.frames;
        const double elapsed = std::chrono::duration<double>(DiagClock::now() - s_diag.windowStart).count();
        if (elapsed >= 1.0 && s_diag.frames > 0)
        {
            const double n = s_diag.frames;
            __android_log_print(ANDROID_LOG_INFO, "cane",
                "[Frame] fps=%.1f total=%.1fms | update=%.1f render(cpu)=%.1f gpuWait(finish)=%.1f present=%.1f pacerWait=%.1f | draws=%d finish=%d",
                n / elapsed, s_diag.frameMs / n, s_diag.updateMs / n, s_diag.renderMs / n, s_diag.finishMs / n,
                s_diag.presentMs / n, s_diag.waitMs / n, g_worldMapCount, s_diag.finishMode);
            {
                // Top GL entry points by CPU time per frame (includes the driver's work inside the call).
                CaneGlProf& pr = cane_prof();
                int order[CGL_COUNT];
                for (int k = 0; k < CGL_COUNT; ++k) order[k] = k;
                std::sort(order, order + CGL_COUNT, [&](int x, int y) { return pr.ns[x] > pr.ns[y]; });
                char line[512]; int len = std::snprintf(line, sizeof(line), "[GL/frame] top CPU ms/frame (calls/frame):");
                double total = 0; for (int k = 0; k < CGL_COUNT; ++k) total += pr.ns[k] / 1e6;
                for (int k = 0; k < 7 && len < (int)sizeof(line) - 60; ++k)
                {
                    const int id = order[k];
                    len += std::snprintf(line + len, sizeof(line) - len, " %s=%.2f(%.0f)", cane_gl_names()[id] + 2, pr.ns[id] / 1e6 / n, pr.cnt[id] / n);
                }
                std::snprintf(line + len, sizeof(line) - len, " | all GL calls %.2f ms/frame", total / n);
                __android_log_print(ANDROID_LOG_INFO, "cane", "%s", line);
                {
                    // Slowest single glBufferSubData calls this second (time, bytes, target, buffer id).
                    CaneBsdRec* top = cane_bsd_top();
                    std::sort(top, top + 6, [](const CaneBsdRec& x, const CaneBsdRec& y) { return x.ns > y.ns; });
                    char b[400]; int bl = std::snprintf(b, sizeof(b), "[BufferSubData] slowest:");
                    for (int k = 0; k < 5; ++k)
                        bl += std::snprintf(b + bl, sizeof(b) - bl, " %.2fms/%lldB/%s/buf%u", top[k].ns / 1e6, top[k].size,
                            top[k].target == 0x8A11 ? "UBO" : top[k].target == 0x8892 ? "VBO" : top[k].target == 0x8893 ? "IBO" : "SSBO", top[k].buffer);
                    __android_log_print(ANDROID_LOG_INFO, "cane", "%s", b);
                    for (int k = 0; k < 6; ++k) top[k] = CaneBsdRec{};
                }
                pr = CaneGlProf{};
            }
            s_diag.updateMs = s_diag.renderMs = s_diag.finishMs = s_diag.presentMs = s_diag.waitMs = s_diag.frameMs = 0;
            s_diag.frames = 0;
            s_diag.windowStart = DiagClock::now();
        }
    }
#endif

    ++g_cframe;
    s_runningFrame = false;
}

#ifdef _WIN32
WNDPROC s_glfwWindowProc = nullptr;
constexpr UINT kRunTrackedWindowFrame = WM_APP + 0x504;
std::atomic_bool s_windowTracking = false;
std::atomic_bool s_windowFrameThreadRunning = false;
HWND s_gameWindow = nullptr;
std::thread s_windowFrameThread;
int s_cutsceneNcDrag = HTNOWHERE;
POINT s_cutsceneNcDragStart{};
RECT s_cutsceneNcWindowStart{};

bool BeginCutsceneNonClientDrag(HWND hwnd, int hitTest)
{
	switch (hitTest)
	{
	case HTCAPTION:
	case HTLEFT:
	case HTRIGHT:
	case HTTOP:
	case HTTOPLEFT:
	case HTTOPRIGHT:
	case HTBOTTOM:
	case HTBOTTOMLEFT:
	case HTBOTTOMRIGHT:
		break;
	default:
		return false;
	}

	s_cutsceneNcDrag = hitTest;
	GetCursorPos(&s_cutsceneNcDragStart);
	GetWindowRect(hwnd, &s_cutsceneNcWindowStart);
	SetCapture(hwnd);
	return true;
}

void UpdateCutsceneNonClientDrag(HWND hwnd)
{
	if (s_cutsceneNcDrag == HTNOWHERE)
		return;

	POINT cursor{};
	GetCursorPos(&cursor);
	const int dx = cursor.x - s_cutsceneNcDragStart.x;
	const int dy = cursor.y - s_cutsceneNcDragStart.y;
	RECT rect = s_cutsceneNcWindowStart;

	if (s_cutsceneNcDrag == HTCAPTION)
	{
		OffsetRect(&rect, dx, dy);
	}
	else
	{
		if (s_cutsceneNcDrag == HTLEFT || s_cutsceneNcDrag == HTTOPLEFT ||
			s_cutsceneNcDrag == HTBOTTOMLEFT)
			rect.left += dx;
		if (s_cutsceneNcDrag == HTRIGHT || s_cutsceneNcDrag == HTTOPRIGHT ||
			s_cutsceneNcDrag == HTBOTTOMRIGHT)
			rect.right += dx;
		if (s_cutsceneNcDrag == HTTOP || s_cutsceneNcDrag == HTTOPLEFT ||
			s_cutsceneNcDrag == HTTOPRIGHT)
			rect.top += dy;
		if (s_cutsceneNcDrag == HTBOTTOM || s_cutsceneNcDrag == HTBOTTOMLEFT ||
			s_cutsceneNcDrag == HTBOTTOMRIGHT)
			rect.bottom += dy;

		constexpr int minimumWidth = 320;
		constexpr int minimumHeight = 240;
		if (rect.right - rect.left < minimumWidth)
		{
			if (s_cutsceneNcDrag == HTLEFT || s_cutsceneNcDrag == HTTOPLEFT ||
				s_cutsceneNcDrag == HTBOTTOMLEFT)
				rect.left = rect.right - minimumWidth;
			else
				rect.right = rect.left + minimumWidth;
		}
		if (rect.bottom - rect.top < minimumHeight)
		{
			if (s_cutsceneNcDrag == HTTOP || s_cutsceneNcDrag == HTTOPLEFT ||
				s_cutsceneNcDrag == HTTOPRIGHT)
				rect.top = rect.bottom - minimumHeight;
			else
				rect.bottom = rect.top + minimumHeight;
		}
	}

	SetWindowPos(hwnd, nullptr, rect.left, rect.top,
		rect.right - rect.left, rect.bottom - rect.top,
		SWP_NOACTIVATE | SWP_NOZORDER);
}

void EndCutsceneNonClientDrag(HWND hwnd)
{
	if (s_cutsceneNcDrag == HTNOWHERE)
		return;
	s_cutsceneNcDrag = HTNOWHERE;
	if (GetCapture() == hwnd)
		ReleaseCapture();
}

void WindowFrameThreadMain()
{
	while (s_windowFrameThreadRunning.load(std::memory_order_acquire))
	{
		if (s_windowTracking.load(std::memory_order_acquire) && s_gameWindow != nullptr)
		{
			DWORD_PTR result = 0;
			SendMessageTimeoutW(s_gameWindow, kRunTrackedWindowFrame, 0, 0,
				SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &result);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
}

LRESULT CALLBACK LiveResizeWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	// DefWindowProc enters a modal tracking loop for caption dragging, resizing,
	// caption buttons, and the system menu. A movie is driven by the main thread,
	// so that loop freezes its video while XAudio continues independently. During
	// an FMV, handle these interactions without entering the modal loop.
	if (FCutscenePlaying())
	{
		if (message == WM_NCLBUTTONDOWN)
		{
			const int hitTest = static_cast<int>(wParam);
			if (BeginCutsceneNonClientDrag(hwnd, hitTest))
				return 0;
			if (hitTest == HTMINBUTTON)
			{
				ShowWindow(hwnd, SW_MINIMIZE);
				return 0;
			}
			if (hitTest == HTMAXBUTTON)
			{
				ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
				return 0;
			}
			if (hitTest == HTCLOSE)
			{
				PostMessageW(hwnd, WM_CLOSE, 0, 0);
				return 0;
			}
			// Do not pass any remaining caption presses (for example the window
			// icon) to DefWindowProc while an FMV is active; they can also start a
			// modal system-menu loop.
			return 0;
		}
		else if (message == WM_NCLBUTTONDBLCLK && wParam == HTCAPTION)
		{
			ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
			return 0;
		}
		else if (message == WM_NCRBUTTONDOWN || message == WM_NCRBUTTONUP ||
			message == WM_NCMBUTTONDOWN || message == WM_NCMBUTTONUP ||
			message == WM_NCXBUTTONDOWN || message == WM_NCXBUTTONUP ||
			message == WM_CONTEXTMENU)
		{
			// The native system menu is modal too. Suppress it only while a movie
			// is active so right-clicking or holding the caption cannot stop video.
			return 0;
		}
		else if (message == WM_MOUSEMOVE && s_cutsceneNcDrag != HTNOWHERE)
		{
			UpdateCutsceneNonClientDrag(hwnd);
			return 0;
		}
		else if (message == WM_LBUTTONUP && s_cutsceneNcDrag != HTNOWHERE)
		{
			EndCutsceneNonClientDrag(hwnd);
			return 0;
		}
		else if (message == WM_CAPTURECHANGED && s_cutsceneNcDrag != HTNOWHERE)
		{
			s_cutsceneNcDrag = HTNOWHERE;
		}
		else if (message == WM_CANCELMODE && s_cutsceneNcDrag != HTNOWHERE)
		{
			EndCutsceneNonClientDrag(hwnd);
			return 0;
		}
	}
	else if (s_cutsceneNcDrag != HTNOWHERE)
	{
		EndCutsceneNonClientDrag(hwnd);
	}

	// Any mouse hold in the non-client area can enter a private Windows tracking
	// loop before GLFW receives another event. Start pumping immediately instead
	// of waiting for WM_ENTERSIZEMOVE or WM_ENTERMENULOOP. This covers caption
	// buttons, title-bar dragging, and holding right-click before the system menu
	// is shown, while still letting GLFW's original procedure own the behavior.
	if (message == WM_NCLBUTTONDOWN ||
		message == WM_NCRBUTTONDOWN ||
		message == WM_NCMBUTTONDOWN)
	{
		s_windowTracking.store(true, std::memory_order_release);
		const LRESULT result = CallWindowProcW(s_glfwWindowProc, hwnd, message, wParam, lParam);
		s_windowTracking.store(false, std::memory_order_release);
		return result;
	}

    switch (message)
    {
        case WM_ENTERSIZEMOVE:
		case WM_ENTERMENULOOP:
		s_windowTracking.store(true, std::memory_order_release);
		break;

        case kRunTrackedWindowFrame:
        if (s_windowTracking.load(std::memory_order_acquire))
        {
            RunGameFrame();
            return 0;
        }
        break;

        case WM_EXITSIZEMOVE:
		case WM_EXITMENULOOP:
		s_windowTracking.store(false, std::memory_order_release);
		break;

        case WM_DESTROY:
		s_windowTracking.store(false, std::memory_order_release);
        break;
    }

    return CallWindowProcW(s_glfwWindowProc, hwnd, message, wParam, lParam);
}

void InstallLiveResizeWindowProc()
{
    HWND hwnd = glfwGetWin32Window(g_gl.window);
    if (hwnd == nullptr)
        return;

	HINSTANCE instance = GetModuleHandleW(nullptr);
	HICON largeIcon = static_cast<HICON>(LoadImageW(instance,
		MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
		GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
	HICON smallIcon = static_cast<HICON>(LoadImageW(instance,
		MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
	if (largeIcon != nullptr)
		SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
	if (smallIcon != nullptr)
		SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));

    s_glfwWindowProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(LiveResizeWindowProc));
	s_gameWindow = hwnd;
	s_windowFrameThreadRunning.store(true, std::memory_order_release);
	s_windowFrameThread = std::thread(WindowFrameThreadMain);
}

void ShutdownLiveResizeWindowProc()
{
	s_windowTracking.store(false, std::memory_order_release);
	s_windowFrameThreadRunning.store(false, std::memory_order_release);
	if (s_windowFrameThread.joinable())
		s_windowFrameThread.join();
}
#endif
}

#if defined(__ANDROID__)
// Native stdout/stderr go to /dev/null on Android, so fatal errors reported
// through fprintf(stderr, ...) are invisible in logcat. Pipe both streams
// into a reader thread and forward complete lines to logcat as tag "cane".
void RedirectStdioToLogcat()
{
	int pipes[2];
	if (pipe(pipes) != 0)
		return;
	dup2(pipes[1], STDOUT_FILENO);
	dup2(pipes[1], STDERR_FILENO);
	close(pipes[1]);
	const int readFd = pipes[0];
	std::thread([readFd]
	{
		char buffer[1024];
		std::string line;
		for (;;)
		{
			const ssize_t count = read(readFd, buffer, sizeof(buffer));
			if (count <= 0)
				break;
			line.append(buffer, static_cast<size_t>(count));
			size_t newline = 0;
			while ((newline = line.find('\n')) != std::string::npos)
			{
				__android_log_print(ANDROID_LOG_INFO, "cane", "%s", line.substr(0, newline).c_str());
				line.erase(0, newline + 1);
			}
			if (line.size() > 4096)
			{
				__android_log_print(ANDROID_LOG_INFO, "cane", "%s", line.c_str());
				line.clear();
			}
		}
		if (!line.empty())
			__android_log_print(ANDROID_LOG_INFO, "cane", "%s", line.c_str());
		close(readFd);
	}).detach();
}
#endif

int RunApplication(const std::vector<std::string>& arguments)
{
#if defined(__ANDROID__)
	RedirectStdioToLogcat();
#endif
    InstallRuntimeErrorHandlers();
    try
    {
		cane::ConfigureRuntime(arguments);
        RefreshContentPackage(cane::DataDirectory());
        LoadGuiProportions();
        Startup();
		#ifdef _WIN32
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
		#endif
		FramePacer framePacer;
		s_framePacer = &framePacer;
#ifdef _WIN32
		InstallLiveResizeWindowProc();
#endif

		while (!glfwWindowShouldClose(g_gl.window) && fQuitGame != true)
		{
			RunGameFrame();
			glfwPollEvents();

		}
		s_framePacer = nullptr;

#ifdef _WIN32
		ShutdownLiveResizeWindowProc();
#endif

		if (g_psw != nullptr)
			DeleteWorld(g_psw);

		ShutdownSound();
		ShutdownControllerInput();
		FreeTvGL();
		g_gl.TerminateGL();
		return EXIT_SUCCESS;
	}
	catch (const std::exception& exception)
	{
		ReportFatalRuntimeError(exception.what());
		return EXIT_FAILURE;
	}
	catch (...)
	{
		ReportFatalRuntimeError("Unknown C++ exception");
		return EXIT_FAILURE;
	}
}

#if defined(_WIN32) && defined(NDEBUG)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	return RunApplication(cane::Arguments(0, nullptr));
}
#else
int main(int argc, char* argv[])
{
#ifdef __ANDROID__
	// Keep stdout unbuffered so lines reach the logcat redirect pipe promptly.
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
#endif
	return RunApplication(cane::Arguments(argc, argv));
}
#endif

void Startup()
{
	// Debug mode is intentionally sampled once. Editing the INI while running
	// takes effect only after restarting the program.
	LoadDebugModeSetting();
    g_gl.InitGL();
    StartupControllerInput();
    glfwSwapInterval(g_fVsync ? 1 : 0);

    std::cout << "Sly Cooper 2002 Sony Computer Entertainment America & Sucker Punch Productions\n";
    SetPhase(PHASE_Startup);

    StartupClock();
    StartupBrx();
    StartupScreen();
    StartupUi();
    StartupFrame();
    StartupTarget();
    StartupJmt();
    StartupPipe();
    StartupHide();
    StartupSaveData(&g_saveData);
    StartupGame();
    StartupSound();
    StartupCodes();
}

bool fQuitGame;
