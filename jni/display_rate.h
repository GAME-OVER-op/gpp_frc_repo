#pragma once
#include <EGL/egl.h>

namespace gpp_frc_repo {
void rememberWindowForSurface(EGLSurface surface, void* nativeWindow);
void forgetWindowForSurface(EGLSurface surface);
void requestBestFrameRate(EGLSurface surface, float measuredGameFps);
void requestFrameRateForWindow(void* nativeWindow, float targetFps);
float chooseTargetRate(float measuredGameFps);
}
