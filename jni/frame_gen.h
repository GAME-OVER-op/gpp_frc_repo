#pragma once
#include <EGL/egl.h>
#include <cstdint>

namespace cleanfg {
struct FrameContext {
    int width = 0;
    int height = 0;
    uint64_t frameIndex = 0;
    int64_t lastPresentNs = 0;
    int64_t avgFrameNs = 0;
    bool hasHistory = false;
};
void fgInitGles(int width, int height);
bool fgRenderGeneratedGles(FrameContext& ctx);
bool fgRenderCurrentGles(FrameContext& ctx);
void fgCaptureCurrentGles(FrameContext& ctx);
float fgMeasuredFps(const FrameContext& ctx);
bool fgOnPresentVulkan(void* queue, const void* pPresentInfo, FrameContext& ctx);
}
