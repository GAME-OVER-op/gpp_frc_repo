// Vulkan present path: inline hook of vkQueuePresentKHR.
// Enabled when mode=vulkan, or mode=auto and libvulkan.so is present.
// (The original config had vulkan=2, i.e. a Vulkan present path.)
#include "frame_gen.h"
#include "config.h"
#include "log.h"
#include <dlfcn.h>

// Avoid pulling full vulkan.h; describe only the minimal type we need.
typedef void* VkQueue;
struct VkPresentInfoKHR;
typedef int (*PFN_vkQueuePresentKHR)(VkQueue, const VkPresentInfoKHR*);

namespace cleanfg {

extern void* hookFunction(void* target, void* replacement);

static PFN_vkQueuePresentKHR orig_vkQueuePresentKHR = nullptr;
static FrameContext g_vkCtx;

static int my_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    bool generated = fgOnPresentVulkan((void*)queue, (const void*)info, g_vkCtx);
    if (generated) {
        // TODO: extra present of the generated swapchain image.
    }
    return orig_vkQueuePresentKHR(queue, info);
}

bool installVulkanHook() {
    void* libvk = dlopen("libvulkan.so", RTLD_NOW | RTLD_GLOBAL);
    if (!libvk) { LOGW("libvulkan.so not present, skip vulkan hook"); return false; }
    void* sym = dlsym(libvk, "vkQueuePresentKHR");
    if (!sym) { LOGW("dlsym vkQueuePresentKHR failed"); return false; }
    void* tramp = hookFunction(sym, (void*)my_vkQueuePresentKHR);
    if (!tramp) { LOGE("inline hook vkQueuePresentKHR failed"); return false; }
    orig_vkQueuePresentKHR = (PFN_vkQueuePresentKHR)tramp;
    LOGI("hooked vkQueuePresentKHR @ %p", sym);
    return true;
}

}  // namespace cleanfg
