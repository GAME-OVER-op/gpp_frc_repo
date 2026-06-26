#include "display_rate.h"
#include "config.h"
#include "log.h"
#include <android/native_window.h>
#include <mutex>
#include <unordered_map>
#include <cmath>

#ifndef ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE
#define ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE 1
#endif
#ifndef ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS
#define ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS 1
#endif

extern "C" int ANativeWindow_setFrameRate(ANativeWindow* window, float frameRate, int8_t compatibility, int8_t changeFrameRateStrategy) __attribute__((weak));

namespace cleanfg {
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
    float target = measuredGameFps * (float)g_config.multiplier;
    if (g_config.max_fps > 0 && target > (float)g_config.max_fps) target = (float)g_config.max_fps;
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
    if (!ANativeWindow_setFrameRate) return;
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
    int r = ANativeWindow_setFrameRate(w, target,
        ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
        ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS);
    g_lastRequested = target;
    LOGI("requested display frameRate %.1f for measured game %.1f, result=%d", target, measuredGameFps, r);
}
}
