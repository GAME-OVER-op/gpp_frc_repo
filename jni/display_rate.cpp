#include "display_rate.h"
#include "config.h"
#include "log.h"
#include <android/native_window.h>
#include <dlfcn.h>
#include <mutex>
#include <unordered_map>
#include <cmath>

// Frame-rate hint constants. Defined locally so we do not depend on API-30+ enums
// when building against android-26.
#ifndef ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE
#define ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE 1
#endif
#ifndef ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS
#define ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS 1
#endif

namespace cleanfg {

// ANativeWindow_setFrameRate is __INTRODUCED_IN(30); the "with change strategy"
// variant is __INTRODUCED_IN(31). We build against android-26 and must also run on
// older devices, so we resolve these at runtime via dlsym instead of referencing the
// guarded symbols directly (which is both a compile error under API 26 and breaks the
// weak-symbol null check).
using PFN_setFrameRate = int32_t (*)(ANativeWindow*, float, int8_t);
using PFN_setFrameRateWithStrategy = int32_t (*)(ANativeWindow*, float, int8_t, int8_t);
static PFN_setFrameRate g_setFrameRate = nullptr;
static PFN_setFrameRateWithStrategy g_setFrameRateWithStrategy = nullptr;
static std::once_flag g_resolveOnce;

static void resolveFrameRateApis() {
    void* lib = dlopen("libnativewindow.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) lib = dlopen("libandroid.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { LOGW("display_rate: dlopen libnativewindow/libandroid failed: %s", dlerror()); return; }
    g_setFrameRateWithStrategy =
        (PFN_setFrameRateWithStrategy)dlsym(lib, "ANativeWindow_setFrameRateWithChangeStrategy");
    g_setFrameRate = (PFN_setFrameRate)dlsym(lib, "ANativeWindow_setFrameRate");
    LOGI("display_rate: setFrameRate=%p withStrategy=%p",
         (void*)g_setFrameRate, (void*)g_setFrameRateWithStrategy);
}

static std::mutex g_mu;
static std::unordered_map<EGLSurface, ANativeWindow*> g_windows;
static float g_lastRequested = 0.0f;

void rememberWindowForSurface(EGLSurface surface, void* nativeWindow) {
    if (!surface || !nativeWindow) return;
    std::lock_guard<std::mutex> lock(g_mu);
    g_windows[surface] = reinterpret_cast<ANativeWindow*>(nativeWindow);
}

void forgetWindowForSurface(EGLSurface surface) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_windows.erase(surface);
}

float chooseTargetRate(float measuredGameFps) {
    if (measuredGameFps <= 1.0f) return 0.0f;
    // Production 2x path is designed for a stable 120 Hz presentation target.
    // Do not downshift the display request to 60/72/90 when the measured app
    // frame time jitters during capture/generation; that causes exactly the
    // 60-90 oscillation we are trying to avoid. Android/SurfaceFlinger will
    // clamp 120 to the real supported panel mode if needed.
    if (g_config.max_fps == 0 && g_config.multiplier == 2 && measuredGameFps >= 24.0f) {
        return 120.0f;
    }
    // Ignore first unstable samples after hook/context creation. Some games can
    // briefly report hundreds/thousands FPS before their render loop settles.
    if (measuredGameFps > 180.0f && g_config.max_fps == 0) return 120.0f;
    float target = measuredGameFps * (float)g_config.multiplier;
    if (g_config.max_fps > 0 && target > (float)g_config.max_fps) target = (float)g_config.max_fps;
    if (g_config.max_fps == 0 && g_config.multiplier == 2 && target > 108.0f && target < 132.0f) {
        target = 120.0f;
    }
    // Snap to common Android panel modes. OS will still clamp to real supported mode.
    const float modes[] = {60, 72, 90, 96, 120, 144, 165};
    float best = target;
    for (float m : modes) {
        if (m + 0.5f >= target) { best = m; break; }
    }
    if (g_config.max_fps > 0 && best > (float)g_config.max_fps) best = (float)g_config.max_fps;
    return best;
}

void requestBestFrameRate(EGLSurface surface, float measuredGameFps) {
    if (!g_config.elevate_rate) return;
    std::call_once(g_resolveOnce, resolveFrameRateApis);
    if (!g_setFrameRate && !g_setFrameRateWithStrategy) return;
    ANativeWindow* w = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_windows.find(surface);
        if (it == g_windows.end()) return;
        w = it->second;
    }
    float target = chooseTargetRate(measuredGameFps);
    if (target < 30.0f) return;
    if (std::fabs(target - g_lastRequested) < 1.0f) return;
    int r = 0;
    if (g_setFrameRateWithStrategy) {
        r = g_setFrameRateWithStrategy(w, target,
            ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
            ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS);
    } else {
        r = g_setFrameRate(w, target,
            ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
    }
    g_lastRequested = target;
    LOGI("requested display frameRate %.1f for measured game %.1f, result=%d", target, measuredGameFps, r);
}

// Vulkan path: request a fixed target rate directly on a swapchain's
// ANativeWindow (no EGLSurface map). Used by the Vulkan present hook so the
// panel runs at the elevated rate and the generated frames can be shown.
void requestFrameRateForWindow(void* nativeWindow, float targetFps) {
    if (!g_config.elevate_rate) return;
    if (!nativeWindow || targetFps < 30.0f) return;
    std::call_once(g_resolveOnce, resolveFrameRateApis);
    if (!g_setFrameRate && !g_setFrameRateWithStrategy) return;
    ANativeWindow* w = reinterpret_cast<ANativeWindow*>(nativeWindow);
    int r = 0;
    if (g_setFrameRateWithStrategy) {
        r = g_setFrameRateWithStrategy(w, targetFps,
            ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
            ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS);
    } else {
        r = g_setFrameRate(w, targetFps,
            ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
    }
    LOGI("vk: requested display frameRate %.1f result=%d", targetFps, r);
}
}
