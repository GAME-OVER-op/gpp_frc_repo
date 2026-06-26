#pragma once
#include <EGL/egl.h>

namespace cleanfg {
void rememberWindowForSurface(EGLSurface surface, void* nativeWindow);
void forgetWindowForSurface(EGLSurface surface);
void requestBestFrameRate(EGLSurface surface, float measuredGameFps);
void requestFrameRateForWindow(void* nativeWindow, float targetFps);
float chooseTargetRate(float measuredGameFps);
}
