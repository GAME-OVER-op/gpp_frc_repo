#pragma once
#include <cstdint>

namespace cleanfg {
// Stage 2 probe: measure glExtrapolateTex2DQCOM cost on this device and verify
// it produces a sane extrapolated frame. Pure GL (own EGL worker context), no
// Vulkan/AHB involved -- isolates the motion-engine cost from interop cost.
void runExtrapBenchmark(uint32_t width, uint32_t height, int iterations);
}
