// Vulkan present path: capture the game's presented swapchain image, read it
// back into a linear host-visible buffer, and feed it to the vendor frame-gen
// engine (GppEngine -> libgppvppgfrcplussession.so -> vppservice).
//
// STAGE 1 (this milestone): prove the engine accepts real game frames and logs
// "FRC will do Nx interpolation". The engine output is drained off-screen for now;
// on-screen presentation of generated frames is the next milestone.
#include "gpp_engine.h"
#include "config.h"
#include "log.h"
#include "display_rate.h"
#include <dlfcn.h>
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <cstring>

namespace cleanfg {

extern void* hookFunction(void* target, void* replacement);

namespace {

// ---- loader-level fn pointers ----
PFN_vkGetInstanceProcAddr pGIPA = nullptr;
PFN_vkGetDeviceProcAddr   pGDPA = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties pGetMemProps = nullptr;

// device-level fn pointers (resolved once per device)
struct DevFns {
    PFN_vkGetSwapchainImagesKHR getSwapImages = nullptr;
    PFN_vkCreateCommandPool     createPool = nullptr;
    PFN_vkAllocateCommandBuffers allocCmd = nullptr;
    PFN_vkBeginCommandBuffer    beginCmd = nullptr;
    PFN_vkCmdPipelineBarrier    barrier = nullptr;
    PFN_vkCmdCopyImageToBuffer  copyImgToBuf = nullptr;
    PFN_vkEndCommandBuffer      endCmd = nullptr;
    PFN_vkQueueSubmit           submit = nullptr;
    PFN_vkCreateFence           createFence = nullptr;
    PFN_vkWaitForFences         waitFences = nullptr;
    PFN_vkResetFences           resetFences = nullptr;
    PFN_vkResetCommandBuffer    resetCmd = nullptr;
    PFN_vkCreateBuffer          createBuf = nullptr;
    PFN_vkGetBufferMemoryRequirements getBufReq = nullptr;
    PFN_vkAllocateMemory        allocMem = nullptr;
    PFN_vkBindBufferMemory      bindBufMem = nullptr;
    PFN_vkMapMemory             mapMem = nullptr;
    PFN_vkAcquireNextImageKHR   acquireNextImage = nullptr;
    PFN_vkCmdCopyImage          cmdCopyImage = nullptr;
    VkPhysicalDevice            phys = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps{};
    bool ok = false;
};

struct SwapInfo {
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    // capture resources (lazy)
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    int rowBytes = 0;
    bool capReady = false;
    bool engineConnected = false;
    void* window = nullptr;       // ANativeWindow* captured from vkCreateAndroidSurfaceKHR
    bool rateRequested = false;
    // present-bridge (frame injection) resources
    VkCommandBuffer injCmd = VK_NULL_HANDLE;
    VkFence injFence = VK_NULL_HANDLE;
    VkFence acqFence = VK_NULL_HANDLE;
    bool injReady = false;
    bool injLogged = false;
};

std::mutex g_mtx;
std::unordered_map<VkDevice, DevFns> g_devs;
std::unordered_map<VkSwapchainKHR, SwapInfo> g_swaps;
std::unordered_map<VkDevice, VkPhysicalDevice> g_devPhys;
std::unordered_map<VkSurfaceKHR, void*> g_surfWindow;  // VkSurfaceKHR -> ANativeWindow*

PFN_vkCreateSwapchainKHR orig_CreateSwapchain = nullptr;
PFN_vkQueuePresentKHR    orig_QueuePresent = nullptr;
PFN_vkCreateDevice       orig_CreateDevice = nullptr;
PFN_vkCreateAndroidSurfaceKHR orig_CreateAndroidSurface = nullptr;

bool isBGRA(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}

DevFns& devFns(VkDevice dev) {
    auto it = g_devs.find(dev);
    if (it != g_devs.end() && it->second.ok) return it->second;
    DevFns f;
    auto G = [&](const char* n){ return pGDPA ? pGDPA(dev, n) : nullptr; };
    f.getSwapImages = (PFN_vkGetSwapchainImagesKHR)G("vkGetSwapchainImagesKHR");
    f.createPool    = (PFN_vkCreateCommandPool)G("vkCreateCommandPool");
    f.allocCmd      = (PFN_vkAllocateCommandBuffers)G("vkAllocateCommandBuffers");
    f.beginCmd      = (PFN_vkBeginCommandBuffer)G("vkBeginCommandBuffer");
    f.barrier       = (PFN_vkCmdPipelineBarrier)G("vkCmdPipelineBarrier");
    f.copyImgToBuf  = (PFN_vkCmdCopyImageToBuffer)G("vkCmdCopyImageToBuffer");
    f.endCmd        = (PFN_vkEndCommandBuffer)G("vkEndCommandBuffer");
    f.submit        = (PFN_vkQueueSubmit)G("vkQueueSubmit");
    f.createFence   = (PFN_vkCreateFence)G("vkCreateFence");
    f.waitFences    = (PFN_vkWaitForFences)G("vkWaitForFences");
    f.resetFences   = (PFN_vkResetFences)G("vkResetFences");
    f.resetCmd      = (PFN_vkResetCommandBuffer)G("vkResetCommandBuffer");
    f.createBuf     = (PFN_vkCreateBuffer)G("vkCreateBuffer");
    f.getBufReq     = (PFN_vkGetBufferMemoryRequirements)G("vkGetBufferMemoryRequirements");
    f.allocMem      = (PFN_vkAllocateMemory)G("vkAllocateMemory");
    f.bindBufMem    = (PFN_vkBindBufferMemory)G("vkBindBufferMemory");
    f.mapMem        = (PFN_vkMapMemory)G("vkMapMemory");
    f.acquireNextImage = (PFN_vkAcquireNextImageKHR)G("vkAcquireNextImageKHR");
    f.cmdCopyImage  = (PFN_vkCmdCopyImage)G("vkCmdCopyImage");
    auto pit = g_devPhys.find(dev);
    if (pit != g_devPhys.end()) {
        f.phys = pit->second;
        if (pGetMemProps && f.phys) pGetMemProps(f.phys, &f.memProps);
    }
    f.ok = f.getSwapImages && f.createPool && f.copyImgToBuf && f.submit && f.mapMem;
    g_devs[dev] = f;
    return g_devs[dev];
}

int findMemType(const DevFns& f, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < f.memProps.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (f.memProps.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

// Lazily build capture resources (command pool/buffer/memory/fence) for a swapchain.
bool ensureCapture(SwapInfo& sw, DevFns& f, uint32_t queueFamily) {
    if (sw.capReady) return true;
    int rb = (int)sw.extent.width * 4;
    VkDeviceSize size = (VkDeviceSize)rb * sw.extent.height;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily;
    if (f.createPool(sw.device, &pci, nullptr, &sw.pool) != VK_SUCCESS) { LOGE("vk: createPool fail"); return false; }

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = sw.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    if (f.allocCmd(sw.device, &cai, &sw.cmd) != VK_SUCCESS) { LOGE("vk: allocCmd fail"); return false; }

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    f.createFence(sw.device, &fci, nullptr, &sw.fence);

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (f.createBuf(sw.device, &bci, nullptr, &sw.buffer) != VK_SUCCESS) { LOGE("vk: createBuf fail"); return false; }
    VkMemoryRequirements mr{}; f.getBufReq(sw.device, sw.buffer, &mr);
    int mt = findMemType(f, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) { LOGE("vk: no host-visible mem type"); return false; }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = (uint32_t)mt;
    if (f.allocMem(sw.device, &mai, nullptr, &sw.memory) != VK_SUCCESS) { LOGE("vk: allocMem fail"); return false; }
    f.bindBufMem(sw.device, sw.buffer, sw.memory, 0);
    if (f.mapMem(sw.device, sw.memory, 0, VK_WHOLE_SIZE, 0, &sw.mapped) != VK_SUCCESS) { LOGE("vk: mapMem fail"); return false; }
    sw.rowBytes = rb;
    sw.capReady = true;
    LOGI("vk: capture ready %ux%u rowBytes=%d qf=%u", sw.extent.width, sw.extent.height, rb, queueFamily);
    return true;
}

void captureAndSubmit(VkQueue queue, VkSwapchainKHR swapchain, uint32_t imageIndex) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto sit = g_swaps.find(swapchain);
    if (sit == g_swaps.end()) return;
    SwapInfo& sw = sit->second;
    DevFns& f = devFns(sw.device);
    if (!f.ok) return;
    if (imageIndex >= sw.images.size()) return;
    // queue family 0 assumed (Adreno graphics queue); iterate later if needed.
    if (!ensureCapture(sw, f, 0)) return;

    VkImage img = sw.images[imageIndex];
    f.resetCmd(sw.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    f.beginCmd(sw.cmd, &bi);

    VkImageMemoryBarrier toSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSrc.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = img;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.barrier(sw.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.bufferOffset = 0; region.bufferRowLength = 0; region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0,0,0};
    region.imageExtent = {sw.extent.width, sw.extent.height, 1};
    f.copyImgToBuf(sw.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sw.buffer, 1, &region);

    VkImageMemoryBarrier toPresent = toSrc;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    f.barrier(sw.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);
    f.endCmd(sw.cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &sw.cmd;
    f.resetFences(sw.device, 1, &sw.fence);
    if (f.submit(queue, 1, &si, sw.fence) != VK_SUCCESS) return;
    f.waitFences(sw.device, 1, &sw.fence, VK_TRUE, 100000000ULL); // 100ms

    // If source is BGRA, swap into RGBA in place (engine converter expects RGBA).
    if (isBGRA(sw.format)) {
        uint8_t* p = (uint8_t*)sw.mapped;
        size_t n = (size_t)sw.extent.width * sw.extent.height;
        for (size_t i = 0; i < n; ++i) { uint8_t t = p[i*4]; p[i*4] = p[i*4+2]; p[i*4+2] = t; }
    }

    if (!sw.engineConnected) {
        std::string pkg = g_config.target_packages.empty() ? std::string("com.miHoYo.GenshinImpact")
                                                            : g_config.target_packages.front();
        std::string layer = "SurfaceView[" + pkg + "]#0";
        sw.engineConnected = g_engine.connect(pkg, layer, (int)sw.extent.width, (int)sw.extent.height);
    }
    if (sw.engineConnected) {
        // Elevate the panel refresh rate so the generated (interpolated) frames
        // have somewhere to be shown. Genshin caps at 60; request 60*multiplier
        // (or max_fps). The OS clamps the request to a real supported panel mode.
        if (sw.window && !sw.rateRequested) {
            float target = g_config.max_fps > 0 ? (float)g_config.max_fps
                                                : 60.0f * (float)g_config.multiplier;
            requestFrameRateForWindow(sw.window, target);
            sw.rateRequested = true;
        }
        g_engine.submitFrameRGBA(sw.mapped, (int)sw.extent.width, (int)sw.extent.height, sw.rowBytes);
    }
}

// Present-bridge Stage 1: lazily create the command buffer + fences used to
// copy and re-present a frame.
bool ensureInject(SwapInfo& sw, DevFns& f) {
    if (sw.injReady) return true;
    if (sw.pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = 0;
        if (f.createPool(sw.device, &pci, nullptr, &sw.pool) != VK_SUCCESS) { LOGE("vk: inject createPool fail"); return false; }
    }
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = sw.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    if (f.allocCmd(sw.device, &cai, &sw.injCmd) != VK_SUCCESS) { LOGE("vk: inject allocCmd fail"); return false; }
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    f.createFence(sw.device, &fci, nullptr, &sw.injFence);
    f.createFence(sw.device, &fci, nullptr, &sw.acqFence);
    sw.injReady = true;
    LOGI("vk: inject resources ready");
    return true;
}

// Copy the just-presented image (srcIndex) into a freshly acquired swapchain
// image and present that as an EXTRA frame. Returns true if the app's wait
// semaphores were consumed by our submit (the caller must then present the
// real frame WITHOUT those semaphores).
bool injectDuplicate(VkQueue queue, VkSwapchainKHR swapchain, const VkPresentInfoKHR* info, uint32_t srcIndex) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto sit = g_swaps.find(swapchain);
    if (sit == g_swaps.end()) return false;
    SwapInfo& sw = sit->second;
    DevFns& f = devFns(sw.device);
    if (!f.ok || !f.acquireNextImage || !f.cmdCopyImage) return false;
    if (srcIndex >= sw.images.size()) return false;
    if (!ensureInject(sw, f)) return false;

    // Acquire a destination image (CPU-wait via fence).
    uint32_t dstIndex = 0;
    f.resetFences(sw.device, 1, &sw.acqFence);
    VkResult ar = f.acquireNextImage(sw.device, swapchain, 8000000ULL, VK_NULL_HANDLE, sw.acqFence, &dstIndex);
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        if (g_config.debug) LOGW("vk: inject acquire rc=%d", (int)ar);
        return false;
    }
    f.waitFences(sw.device, 1, &sw.acqFence, VK_TRUE, 50000000ULL);
    if (dstIndex >= sw.images.size()) return false;

    VkImage src = sw.images[srcIndex];
    VkImage dst = sw.images[dstIndex];

    f.resetCmd(sw.injCmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    f.beginCmd(sw.injCmd, &bi);

    VkImageMemoryBarrier pre[2] = {};
    pre[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    pre[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pre[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    pre[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    pre[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    pre[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[0].image = src;
    pre[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    pre[1] = pre[0];
    pre[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    pre[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    pre[1].srcAccessMask = 0;
    pre[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pre[1].image = dst;
    f.barrier(sw.injCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, pre);

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffset = {0, 0, 0}; region.dstOffset = {0, 0, 0};
    region.extent = {sw.extent.width, sw.extent.height, 1};
    f.cmdCopyImage(sw.injCmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier post[2] = {};
    post[0] = pre[0];
    post[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    post[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    post[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    post[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    post[0].image = src;
    post[1] = post[0];
    post[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    post[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    post[1].image = dst;
    f.barrier(sw.injCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, post);
    f.endCmd(sw.injCmd);

    // Wait on the app's render-finished semaphores so src is fully drawn. This
    // consumes them, so the real present must not wait on them again.
    std::vector<VkPipelineStageFlags> waitStages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = info->waitSemaphoreCount;
    si.pWaitSemaphores = info->pWaitSemaphores;
    si.pWaitDstStageMask = info->waitSemaphoreCount ? waitStages.data() : nullptr;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &sw.injCmd;
    f.resetFences(sw.device, 1, &sw.injFence);
    if (f.submit(queue, 1, &si, sw.injFence) != VK_SUCCESS) { if (g_config.debug) LOGW("vk: inject submit fail"); return false; }
    f.waitFences(sw.device, 1, &sw.injFence, VK_TRUE, 100000000ULL);

    // Present the duplicate; the real frame follows (caller).
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &dstIndex;
    orig_QueuePresent(queue, &pi);

    if (!sw.injLogged) {
        LOGI("vk: present-bridge active src=%u dst=%u %ux%u", srcIndex, dstIndex, sw.extent.width, sw.extent.height);
        sw.injLogged = true;
    }
    return true;
}

// ---- hooks ----
VkResult my_CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                            const VkAllocationCallbacks* alloc, VkSwapchainKHR* out) {
    // Present-bridge needs to copy INTO swapchain images, so force TRANSFER
    // usage. (Capture already proved TRANSFER_SRC works; we add TRANSFER_DST.)
    VkSwapchainCreateInfoKHR mci = *ci;
    VkImageUsageFlags origUsage = mci.imageUsage;
    if (g_config.present_bridge)
        mci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkResult r = orig_CreateSwapchain(device, &mci, alloc, out);
    if (r != VK_SUCCESS && g_config.present_bridge) {
        LOGW("vk: swapchain forced-usage create rc=%d; retrying with original usage", (int)r);
        r = orig_CreateSwapchain(device, ci, alloc, out);
    }
    if (r == VK_SUCCESS && out && *out) {
        std::lock_guard<std::mutex> lk(g_mtx);
        DevFns& f = devFns(device);
        SwapInfo sw;
        sw.device = device; sw.format = ci->imageFormat; sw.extent = ci->imageExtent;
        { auto wit = g_surfWindow.find(ci->surface); if (wit != g_surfWindow.end()) sw.window = wit->second; }
        if (f.getSwapImages) {
            uint32_t n = 0; f.getSwapImages(device, *out, &n, nullptr);
            sw.images.resize(n);
            if (n) f.getSwapImages(device, *out, &n, sw.images.data());
        }
        LOGI("vk: swapchain %p %ux%u fmt=%d images=%zu usage=0x%x(orig=0x%x)", (void*)*out,
             sw.extent.width, sw.extent.height, (int)sw.format, sw.images.size(),
             (unsigned)mci.imageUsage, (unsigned)origUsage);
        g_swaps[*out] = std::move(sw);
    }
    return r;
}

VkResult my_QueuePresent(VkQueue queue, const VkPresentInfoKHR* info) {
    if (info) {
        // STAGE 1 present-bridge: inject a duplicate frame to double the present
        // rate, proving extra frames can be pushed through the game's own
        // swapchain (root FPS tool should read ~2x). Stage 2 replaces the
        // duplicate source with the engine's interpolated output.
        if (g_config.present_bridge && info->swapchainCount == 1) {
            bool consumed = injectDuplicate(queue, info->pSwapchains[0], info, info->pImageIndices[0]);
            if (consumed) {
                VkPresentInfoKHR real = *info;
                real.waitSemaphoreCount = 0;
                real.pWaitSemaphores = nullptr;
                return orig_QueuePresent(queue, &real);
            }
        } else {
            for (uint32_t i = 0; i < info->swapchainCount; ++i) {
                captureAndSubmit(queue, info->pSwapchains[i], info->pImageIndices[i]);
            }
        }
    }
    return orig_QueuePresent(queue, info);
}

VkResult my_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                         const VkAllocationCallbacks* alloc, VkDevice* out) {
    VkResult r = orig_CreateDevice(phys, ci, alloc, out);
    if (r == VK_SUCCESS && out && *out) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_devPhys[*out] = phys;
    }
    return r;
}

// Capture the ANativeWindow backing each Android Vulkan surface so we can later
// elevate the panel refresh rate on the matching swapchain.
VkResult my_CreateAndroidSurface(VkInstance inst, const VkAndroidSurfaceCreateInfoKHR* ci,
                                 const VkAllocationCallbacks* alloc, VkSurfaceKHR* out) {
    VkResult r = orig_CreateAndroidSurface(inst, ci, alloc, out);
    if (r == VK_SUCCESS && out && *out && ci) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_surfWindow[*out] = (void*)ci->window;
        LOGI("vk: android surface %p window=%p", (void*)*out, (void*)ci->window);
    }
    return r;
}

// --- Interceptor for vkGet*ProcAddr ---------------------------------------
// Unity/Genshin resolves Vulkan entry points through vkGetInstanceProcAddr /
// vkGetDeviceProcAddr and calls the driver dispatch directly, bypassing the
// exported libvulkan.so symbols (which is why hooking the exported
// vkQueuePresentKHR/vkCreateSwapchainKHR was inert). So we hook the two
// Get*ProcAddr resolvers and hand back our wrappers for the entry points we
// care about. pGIPA/pGDPA always hold the REAL resolvers.
PFN_vkVoidFunction VKAPI_PTR my_GetDeviceProcAddr(VkDevice dev, const char* name) {
    if (!name) return nullptr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)my_GetDeviceProcAddr;
    PFN_vkVoidFunction real = pGDPA ? pGDPA(dev, name) : nullptr;
    if (!real) return nullptr;
    if (!strcmp(name, "vkQueuePresentKHR")) {
        orig_QueuePresent = (PFN_vkQueuePresentKHR)real;
        LOGI("vk: intercepted vkQueuePresentKHR via gdpa");
        return (PFN_vkVoidFunction)my_QueuePresent;
    }
    if (!strcmp(name, "vkCreateSwapchainKHR")) {
        orig_CreateSwapchain = (PFN_vkCreateSwapchainKHR)real;
        LOGI("vk: intercepted vkCreateSwapchainKHR via gdpa");
        return (PFN_vkVoidFunction)my_CreateSwapchain;
    }
    return real;
}

PFN_vkVoidFunction VKAPI_PTR my_GetInstanceProcAddr(VkInstance inst, const char* name) {
    if (!name) return nullptr;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)my_GetInstanceProcAddr;
    PFN_vkVoidFunction real = pGIPA ? pGIPA(inst, name) : nullptr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) {
        if (real) pGDPA = (PFN_vkGetDeviceProcAddr)real;
        return (PFN_vkVoidFunction)my_GetDeviceProcAddr;
    }
    if (!strcmp(name, "vkCreateDevice")) {
        if (real) orig_CreateDevice = (PFN_vkCreateDevice)real;
        return (PFN_vkVoidFunction)my_CreateDevice;
    }
    if (!strcmp(name, "vkCreateAndroidSurfaceKHR")) {
        if (real) orig_CreateAndroidSurface = (PFN_vkCreateAndroidSurfaceKHR)real;
        return (PFN_vkVoidFunction)my_CreateAndroidSurface;
    }
    if (!real) return nullptr;
    if (!strcmp(name, "vkCreateSwapchainKHR")) {
        orig_CreateSwapchain = (PFN_vkCreateSwapchainKHR)real;
        return (PFN_vkVoidFunction)my_CreateSwapchain;
    }
    if (!strcmp(name, "vkQueuePresentKHR")) {
        orig_QueuePresent = (PFN_vkQueuePresentKHR)real;
        return (PFN_vkVoidFunction)my_QueuePresent;
    }
    return real;
}

}  // anonymous namespace

bool installVulkanHook() {
    void* libvk = dlopen("libvulkan.so", RTLD_NOW | RTLD_GLOBAL);
    if (!libvk) { LOGW("libvulkan.so not present, skip vulkan hook"); return false; }
    pGetMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)dlsym(libvk, "vkGetPhysicalDeviceMemoryProperties");

    void* gipa = dlsym(libvk, "vkGetInstanceProcAddr");
    void* gdpa = dlsym(libvk, "vkGetDeviceProcAddr");
    // REAL resolvers (must never point at our wrappers).
    pGIPA = (PFN_vkGetInstanceProcAddr)gipa;
    pGDPA = (PFN_vkGetDeviceProcAddr)gdpa;

    bool any = false;
    // PRIMARY: intercept the proc-address resolvers so we catch the
    // driver-dispatched entry points the engine actually calls.
    if (gipa) {
        void* t = hookFunction(gipa, (void*)my_GetInstanceProcAddr);
        if (t) { pGIPA = (PFN_vkGetInstanceProcAddr)t; any = true; }
    }
    if (gdpa) {
        void* t = hookFunction(gdpa, (void*)my_GetDeviceProcAddr);
        if (t) { pGDPA = (PFN_vkGetDeviceProcAddr)t; any = true; }
    }
    // BELT-AND-SUSPENDERS: also hook the exported entry points, in case the app
    // calls them directly. Harmless if never invoked.
    void* createDev  = dlsym(libvk, "vkCreateDevice");
    void* createSwap = dlsym(libvk, "vkCreateSwapchainKHR");
    void* present    = dlsym(libvk, "vkQueuePresentKHR");
    void* createAndroidSurf = dlsym(libvk, "vkCreateAndroidSurfaceKHR");
    if (createAndroidSurf) { void* t = hookFunction(createAndroidSurf, (void*)my_CreateAndroidSurface); if (t && !orig_CreateAndroidSurface) orig_CreateAndroidSurface = (PFN_vkCreateAndroidSurfaceKHR)t; }
    if (createDev)  { void* t = hookFunction(createDev,  (void*)my_CreateDevice);    if (t && !orig_CreateDevice)    orig_CreateDevice    = (PFN_vkCreateDevice)t; }
    if (createSwap) { void* t = hookFunction(createSwap, (void*)my_CreateSwapchain); if (t && !orig_CreateSwapchain) orig_CreateSwapchain = (PFN_vkCreateSwapchainKHR)t; }
    if (present)    { void* t = hookFunction(present,    (void*)my_QueuePresent);    if (t && !orig_QueuePresent)    orig_QueuePresent    = (PFN_vkQueuePresentKHR)t; }

    if (any) LOGI("vk: proc-addr interception installed (gipa=%p gdpa=%p)", gipa, gdpa);
    else LOGE("vk: failed to install proc-addr interception");
    return any;
}

}  // namespace cleanfg
