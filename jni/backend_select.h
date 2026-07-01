#pragma once

namespace cleanfg {

enum class RuntimeBackend {
    Unknown = 0,
    Gles = 1,
    Vulkan = 2,
};

// Vulkan swapchain/surface creation is a strong hint that the game will present
// through Vulkan soon. GLES waits briefly in auto mode so an Android UI/EGL
// surface cannot steal the backend before the real Vulkan renderer starts.
void noteVulkanCandidate();
bool hasRecentVulkanCandidate(int windowMs = 3000);

RuntimeBackend activeBackend();
bool isBackendActive(RuntimeBackend backend);
bool isBackendBlocked(RuntimeBackend backend);
bool tryActivateBackend(RuntimeBackend backend, const char* reason);
const char* backendName(RuntimeBackend backend);

}
