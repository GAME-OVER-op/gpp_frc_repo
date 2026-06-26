#pragma once
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>
#include <cstdint>

namespace cleanfg {
// One-shot Vulkan <-> GL interop overhead benchmark (Stage 2 prototype).
// MUST be called from the present thread (only thread allowed to submit to the
// game's present queue). Spins up a dedicated GL worker thread that owns its own
// EGL context (eglMakeCurrent exactly once) and does only GL work. Logs
// min/avg/max round-trip latency over `iterations` runs, then tears everything
// down. `gdpa` must be the REAL vkGetDeviceProcAddr resolver.
void runInteropBenchmark(VkDevice device, VkPhysicalDevice phys, VkQueue queue,
                         uint32_t queueFamily, uint32_t width, uint32_t height,
                         PFN_vkGetDeviceProcAddr gdpa, int iterations);
}
