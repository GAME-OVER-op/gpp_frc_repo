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
#include "blend_comp.h"  // generated at build time (glslangValidator --vn blend_comp_spv)
#include "interop_bench.h"
#include "extrap_bench.h"
#include "extrap_eval.h"
#include <atomic>

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
    PFN_vkCmdCopyBufferToImage  copyBufToImg = nullptr;
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
    PFN_vkCreateImage                 createImage = nullptr;
    PFN_vkGetImageMemoryRequirements  getImgReq = nullptr;
    PFN_vkBindImageMemory             bindImgMem = nullptr;
    PFN_vkCreateImageView             createImageView = nullptr;
    PFN_vkCreateDescriptorSetLayout   createDescSetLayout = nullptr;
    PFN_vkCreateDescriptorPool        createDescPool = nullptr;
    PFN_vkAllocateDescriptorSets      allocDescSets = nullptr;
    PFN_vkUpdateDescriptorSets        updateDescSets = nullptr;
    PFN_vkCreatePipelineLayout        createPipeLayout = nullptr;
    PFN_vkCreateShaderModule          createShaderModule = nullptr;
    PFN_vkCreateComputePipelines      createComputePipelines = nullptr;
    PFN_vkCmdBindPipeline             cmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets       cmdBindDescSets = nullptr;
    PFN_vkCmdDispatch                 cmdDispatch = nullptr;
    PFN_vkCmdPushConstants            cmdPushConst = nullptr;
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
    // blend interpolation (Stage 2A): CPU midpoint of prev+cur frame
    VkBuffer dlBuf = VK_NULL_HANDLE;   // download current frame to host
    VkBuffer ulBuf = VK_NULL_HANDLE;   // upload blended midpoint from host
    VkDeviceMemory dlMem = VK_NULL_HANDLE;
    VkDeviceMemory ulMem = VK_NULL_HANDLE;
    void* dlMapped = nullptr;
    void* ulMapped = nullptr;
    std::vector<uint8_t> prevFrame;
    bool hasPrev = false;
    int blendRowBytes = 0;
    VkDeviceSize blendBytes = 0;
    bool blendReady = false;
    // --- Stage 2B: GPU compute-blend resources ---
    VkImage gPrev = VK_NULL_HANDLE, gCur = VK_NULL_HANDLE, gOut = VK_NULL_HANDLE;
    VkDeviceMemory gPrevMem = VK_NULL_HANDLE, gCurMem = VK_NULL_HANDLE, gOutMem = VK_NULL_HANDLE;
    VkImageView gPrevView = VK_NULL_HANDLE, gCurView = VK_NULL_HANDLE, gOutView = VK_NULL_HANDLE;
    VkDescriptorSetLayout gDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool gDescPool = VK_NULL_HANDLE;
    VkDescriptorSet gDescSet = VK_NULL_HANDLE;
    VkPipelineLayout gPipeLayout = VK_NULL_HANDLE;
    VkPipeline gPipe = VK_NULL_HANDLE;
    VkShaderModule gShader = VK_NULL_HANDLE;
    bool gHasPrev = false;
    bool gpuReady = false;
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
    f.copyBufToImg  = (PFN_vkCmdCopyBufferToImage)G("vkCmdCopyBufferToImage");
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
    f.createImage            = (PFN_vkCreateImage)G("vkCreateImage");
    f.getImgReq              = (PFN_vkGetImageMemoryRequirements)G("vkGetImageMemoryRequirements");
    f.bindImgMem             = (PFN_vkBindImageMemory)G("vkBindImageMemory");
    f.createImageView        = (PFN_vkCreateImageView)G("vkCreateImageView");
    f.createDescSetLayout    = (PFN_vkCreateDescriptorSetLayout)G("vkCreateDescriptorSetLayout");
    f.createDescPool         = (PFN_vkCreateDescriptorPool)G("vkCreateDescriptorPool");
    f.allocDescSets          = (PFN_vkAllocateDescriptorSets)G("vkAllocateDescriptorSets");
    f.updateDescSets         = (PFN_vkUpdateDescriptorSets)G("vkUpdateDescriptorSets");
    f.createPipeLayout       = (PFN_vkCreatePipelineLayout)G("vkCreatePipelineLayout");
    f.createShaderModule     = (PFN_vkCreateShaderModule)G("vkCreateShaderModule");
    f.createComputePipelines = (PFN_vkCreateComputePipelines)G("vkCreateComputePipelines");
    f.cmdBindPipeline        = (PFN_vkCmdBindPipeline)G("vkCmdBindPipeline");
    f.cmdBindDescSets        = (PFN_vkCmdBindDescriptorSets)G("vkCmdBindDescriptorSets");
    f.cmdDispatch            = (PFN_vkCmdDispatch)G("vkCmdDispatch");
    f.cmdPushConst           = (PFN_vkCmdPushConstants)G("vkCmdPushConstants");
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
        // Use the package of THIS process (set at match time). Falls back to the
        // first configured target, then to a sane default, for safety.
        std::string pkg = !g_config.current_package.empty() ? g_config.current_package
                          : (g_config.target_packages.empty() ? std::string("com.miHoYo.GenshinImpact")
                                                              : g_config.target_packages.front());
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

// One-shot diagnostic helper: copy a swapchain image into a tightly-packed
// host RGBA buffer (used by the extrap-eval real-frame test). Reuses the
// capture buffer/fence. Returns false on any failure.
bool captureToHost(VkQueue queue, VkSwapchainKHR swapchain, uint32_t imageIndex,
                   std::vector<uint8_t>& out, uint32_t& wOut, uint32_t& hOut) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto sit = g_swaps.find(swapchain);
    if (sit == g_swaps.end()) return false;
    SwapInfo& sw = sit->second;
    DevFns& f = devFns(sw.device);
    if (!f.ok) return false;
    if (imageIndex >= sw.images.size()) return false;
    if (!ensureCapture(sw, f, 0)) return false;
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
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
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
    if (f.submit(queue, 1, &si, sw.fence) != VK_SUCCESS) return false;
    f.waitFences(sw.device, 1, &sw.fence, VK_TRUE, 200000000ULL);
    size_t size = (size_t)sw.rowBytes * (size_t)sw.extent.height;
    out.resize(size);
    memcpy(out.data(), sw.mapped, size);
    if (isBGRA(sw.format)) {
        uint8_t* p = out.data();
        size_t n = (size_t)sw.extent.width * (size_t)sw.extent.height;
        for (size_t i = 0; i < n; ++i) { uint8_t t = p[i*4]; p[i*4] = p[i*4+2]; p[i*4+2] = t; }
    }
    wOut = sw.extent.width; hOut = sw.extent.height;
    return true;
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

// Lazily build the host-visible buffers used for CPU blend interpolation:
// dlBuf receives the current frame (image->buffer); ulBuf holds the computed
// midpoint (buffer->image); prevFrame keeps the previous frame on the CPU.
bool ensureBlend(SwapInfo& sw, DevFns& f) {
    if (sw.blendReady) return true;
    sw.blendRowBytes = (int)sw.extent.width * 4;
    sw.blendBytes = (VkDeviceSize)sw.blendRowBytes * sw.extent.height;
    auto mkBuf = [&](VkBufferUsageFlags usage, VkBuffer* buf, VkDeviceMemory* mem, void** mapped) -> bool {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = sw.blendBytes; bci.usage = usage; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (f.createBuf(sw.device, &bci, nullptr, buf) != VK_SUCCESS) return false;
        VkMemoryRequirements mr{}; f.getBufReq(sw.device, *buf, &mr);
        int mt = findMemType(f, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt < 0) return false;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size; mai.memoryTypeIndex = (uint32_t)mt;
        if (f.allocMem(sw.device, &mai, nullptr, mem) != VK_SUCCESS) return false;
        f.bindBufMem(sw.device, *buf, *mem, 0);
        return f.mapMem(sw.device, *mem, 0, VK_WHOLE_SIZE, 0, mapped) == VK_SUCCESS;
    };
    if (!mkBuf(VK_BUFFER_USAGE_TRANSFER_DST_BIT, &sw.dlBuf, &sw.dlMem, &sw.dlMapped)) { LOGE("vk: blend dlBuf fail"); return false; }
    if (!mkBuf(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sw.ulBuf, &sw.ulMem, &sw.ulMapped)) { LOGE("vk: blend ulBuf fail"); return false; }
    sw.prevFrame.assign((size_t)sw.blendBytes, 0);
    sw.hasPrev = false;
    sw.blendReady = true;
    LOGI("vk: blend ready %ux%u rowBytes=%d bytes=%llu", sw.extent.width, sw.extent.height,
         sw.blendRowBytes, (unsigned long long)sw.blendBytes);
    return true;
}

// STAGE 2A: synthesize an interpolated frame = midpoint of the previous and
// current frame (CPU 50/50 blend) and present it BEFORE the real current
// frame. Returns true if the app's wait semaphores were consumed by our
// download submit (the caller must then present the real frame WITHOUT them).
// Push-constant block fed to the GPU blend shader (layout must match blend.comp).
struct BlendPC {
    float baseAlpha;       // static-scene blend (0.5 = even)
    float diffThreshold;   // luma-diff below this -> full blend
    float diffSoftness;    // smoothstep width above threshold
    float motionStrength;  // 0..1 scales how hard motion kills the blend
    int32_t blurRadius;    // 0 = off; >0 = box-blur radius in high-motion zones
};

bool ensureGpuBlend(SwapInfo& sw, DevFns& f) {
    if (sw.gpuReady) return true;
    if (!f.createImage || !f.getImgReq || !f.bindImgMem || !f.createImageView ||
        !f.createDescSetLayout || !f.createDescPool || !f.allocDescSets || !f.updateDescSets ||
        !f.createPipeLayout || !f.createShaderModule || !f.createComputePipelines ||
        !f.cmdBindPipeline || !f.cmdBindDescSets || !f.cmdDispatch || !f.cmdPushConst) {
        LOGE("vk: gpu-blend missing device fns"); return false;
    }
    const VkFormat fmt = sw.format;  // matches swapchain (R8G8B8A8_UNORM = 37 on this device)
    auto mkImg = [&](VkImage* img, VkDeviceMemory* mem, VkImageView* view) -> bool {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = fmt;
        ici.extent = {sw.extent.width, sw.extent.height, 1};
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (f.createImage(sw.device, &ici, nullptr, img) != VK_SUCCESS) return false;
        VkMemoryRequirements mr{}; f.getImgReq(sw.device, *img, &mr);
        int mt = findMemType(f, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt < 0) return false;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size; mai.memoryTypeIndex = (uint32_t)mt;
        if (f.allocMem(sw.device, &mai, nullptr, mem) != VK_SUCCESS) return false;
        if (f.bindImgMem(sw.device, *img, *mem, 0) != VK_SUCCESS) return false;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = *img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return f.createImageView(sw.device, &vci, nullptr, view) == VK_SUCCESS;
    };
    if (!mkImg(&sw.gPrev, &sw.gPrevMem, &sw.gPrevView)) { LOGE("vk: gpu-blend prev img fail"); return false; }
    if (!mkImg(&sw.gCur,  &sw.gCurMem,  &sw.gCurView))  { LOGE("vk: gpu-blend cur img fail");  return false; }
    if (!mkImg(&sw.gOut,  &sw.gOutMem,  &sw.gOutView))  { LOGE("vk: gpu-blend out img fail");  return false; }

    VkDescriptorSetLayoutBinding binds[3]{};
    for (int i = 0; i < 3; ++i) {
        binds[i].binding = (uint32_t)i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 3; dlci.pBindings = binds;
    if (f.createDescSetLayout(sw.device, &dlci, nullptr, &sw.gDescLayout) != VK_SUCCESS) { LOGE("vk: gpu-blend desc layout fail"); return false; }

    VkDescriptorPoolSize psz{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &psz;
    if (f.createDescPool(sw.device, &dpci, nullptr, &sw.gDescPool) != VK_SUCCESS) { LOGE("vk: gpu-blend desc pool fail"); return false; }

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = sw.gDescPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &sw.gDescLayout;
    if (f.allocDescSets(sw.device, &dsai, &sw.gDescSet) != VK_SUCCESS) { LOGE("vk: gpu-blend desc set fail"); return false; }

    VkDescriptorImageInfo dii[3]{};
    dii[0].imageView = sw.gPrevView; dii[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    dii[1].imageView = sw.gCurView;  dii[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    dii[2].imageView = sw.gOutView;  dii[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet wds[3]{};
    for (int i = 0; i < 3; ++i) {
        wds[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[i].dstSet = sw.gDescSet; wds[i].dstBinding = (uint32_t)i; wds[i].descriptorCount = 1;
        wds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wds[i].pImageInfo = &dii[i];
    }
    f.updateDescSets(sw.device, 3, wds, 0, nullptr);

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof(blend_comp_spv); smci.pCode = blend_comp_spv;
    if (f.createShaderModule(sw.device, &smci, nullptr, &sw.gShader) != VK_SUCCESS) { LOGE("vk: gpu-blend shader module fail"); return false; }

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)sizeof(BlendPC)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1; plci.pSetLayouts = &sw.gDescLayout;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (f.createPipeLayout(sw.device, &plci, nullptr, &sw.gPipeLayout) != VK_SUCCESS) { LOGE("vk: gpu-blend pipe layout fail"); return false; }

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = sw.gShader;
    cpci.stage.pName = "main";
    cpci.layout = sw.gPipeLayout;
    if (f.createComputePipelines(sw.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &sw.gPipe) != VK_SUCCESS) { LOGE("vk: gpu-blend pipeline fail"); return false; }

    sw.gHasPrev = false;
    sw.gpuReady = true;
    LOGI("vk: gpu-blend ready %ux%u fmt=%d", sw.extent.width, sw.extent.height, (int)sw.format);
    return true;
}

bool injectBlend(VkQueue queue, VkSwapchainKHR swapchain, const VkPresentInfoKHR* info, uint32_t srcIndex) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto sit = g_swaps.find(swapchain);
    if (sit == g_swaps.end()) return false;
    SwapInfo& sw = sit->second;
    DevFns& f = devFns(sw.device);
    if (!f.ok || !f.acquireNextImage || !f.cmdCopyImage || !f.cmdDispatch) return false;
    if (srcIndex >= sw.images.size()) return false;
    if (!ensureInject(sw, f)) return false;
    if (!ensureGpuBlend(sw, f)) return false;

    // Acquire the destination image BEFORE consuming app semaphores, so a
    // timeout here falls back cleanly to a normal single present.
    uint32_t dstIndex = 0;
    f.resetFences(sw.device, 1, &sw.acqFence);
    VkResult ar = f.acquireNextImage(sw.device, swapchain, 34000000ULL, VK_NULL_HANDLE, sw.acqFence, &dstIndex);
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        if (g_config.debug) LOGW("vk: blend acquire rc=%d", (int)ar);
        return false;
    }
    f.waitFences(sw.device, 1, &sw.acqFence, VK_TRUE, 50000000ULL);
    if (dstIndex >= sw.images.size()) return false;

    VkImage src = sw.images[srcIndex];
    VkImage dst = sw.images[dstIndex];

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    auto imgBar = [&](VkImage img, VkImageLayout o, VkImageLayout n,
                      VkAccessFlags sa, VkAccessFlags da,
                      VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img; b.oldLayout = o; b.newLayout = n;
        b.srcAccessMask = sa; b.dstAccessMask = da;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        f.barrier(sw.injCmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    VkImageCopy cp{};
    cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.extent = {sw.extent.width, sw.extent.height, 1};

    f.resetCmd(sw.injCmd, 0);
    f.beginCmd(sw.injCmd, &bi);

    // (1) Copy the freshly-rendered swapchain image (src) into our gCur image.
    imgBar(src, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
           VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    imgBar(sw.gCur, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
           0, VK_ACCESS_TRANSFER_WRITE_BIT,
           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    f.cmdCopyImage(sw.injCmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   sw.gCur, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    imgBar(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
           VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    if (!sw.gHasPrev) {
        // First frame: no history yet. Seed gPrev = cur and present a duplicate.
        imgBar(sw.gCur, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        imgBar(sw.gPrev, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        f.cmdCopyImage(sw.injCmd, sw.gCur, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       sw.gPrev, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        imgBar(sw.gPrev, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        imgBar(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        f.cmdCopyImage(sw.injCmd, sw.gCur, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        imgBar(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        sw.gHasPrev = true;
    } else {
        // gCur -> GENERAL (compute reads), gOut -> GENERAL (compute writes). gPrev already GENERAL.
        imgBar(sw.gCur, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        imgBar(sw.gOut, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
               0, VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        f.cmdBindPipeline(sw.injCmd, VK_PIPELINE_BIND_POINT_COMPUTE, sw.gPipe);
        f.cmdBindDescSets(sw.injCmd, VK_PIPELINE_BIND_POINT_COMPUTE, sw.gPipeLayout, 0, 1, &sw.gDescSet, 0, nullptr);
        BlendPC pc{ g_config.blend_alpha, g_config.diff_threshold, g_config.diff_softness,
                   g_config.motion_strength, g_config.blur_radius };
        f.cmdPushConst(sw.injCmd, sw.gPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)sizeof(BlendPC), &pc);
        uint32_t gx = (sw.extent.width + 7) / 8, gy = (sw.extent.height + 7) / 8;
        f.cmdDispatch(sw.injCmd, gx, gy, 1);
        // gOut -> TRANSFER_SRC, dst -> TRANSFER_DST, copy interpolated midpoint into dst.
        imgBar(sw.gOut, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        imgBar(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        f.cmdCopyImage(sw.injCmd, sw.gOut, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        imgBar(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        // History update: gCur -> gPrev (both currently GENERAL), leave gPrev GENERAL.
        imgBar(sw.gCur, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        imgBar(sw.gPrev, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        f.cmdCopyImage(sw.injCmd, sw.gCur, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       sw.gPrev, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        imgBar(sw.gPrev, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    f.endCmd(sw.injCmd);

    std::vector<VkPipelineStageFlags> waitStages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = info->waitSemaphoreCount;
    si.pWaitSemaphores = info->pWaitSemaphores;
    si.pWaitDstStageMask = info->waitSemaphoreCount ? waitStages.data() : nullptr;
    si.commandBufferCount = 1; si.pCommandBuffers = &sw.injCmd;
    f.resetFences(sw.device, 1, &sw.injFence);
    if (f.submit(queue, 1, &si, sw.injFence) != VK_SUCCESS) { if (g_config.debug) LOGW("vk: blend submit fail"); return false; }
    f.waitFences(sw.device, 1, &sw.injFence, VK_TRUE, 100000000ULL);

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &dstIndex;
    orig_QueuePresent(queue, &pi);

    if (!sw.injLogged) {
        LOGI("vk: gpu-blend interpolation active src=%u dst=%u %ux%u", srcIndex, dstIndex, sw.extent.width, sw.extent.height);
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
    uint32_t origMin = mci.minImageCount;
    if (g_config.present_bridge) {
        mci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        // Need spare images so we can hold one extra in-flight frame for
        // injection without starving the game's own acquire/present loop.
        mci.minImageCount = origMin + 4;
    }
    VkResult r = orig_CreateSwapchain(device, &mci, alloc, out);
    if (r != VK_SUCCESS && g_config.present_bridge) {
        LOGW("vk: swapchain forced create rc=%d (minImages %u->%u); retrying original", (int)r, origMin, mci.minImageCount);
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
        LOGI("vk: swapchain %p %ux%u fmt=%d images=%zu minReq=%u(orig=%u) usage=0x%x(orig=0x%x)", (void*)*out,
             sw.extent.width, sw.extent.height, (int)sw.format, sw.images.size(),
             (unsigned)mci.minImageCount, (unsigned)origMin,
             (unsigned)mci.imageUsage, (unsigned)origUsage);
        g_swaps[*out] = std::move(sw);
    }
    return r;
}

VkResult my_QueuePresent(VkQueue queue, const VkPresentInfoKHR* info) {
    // STAGE 2 one-shot interop benchmark (opt-in via cleanfg.prop interop_bench=1).
    if (info && info->swapchainCount >= 1 && g_config.interop_bench) {
        static std::atomic<bool> benchRan{false};
        bool expected = false;
        if (benchRan.compare_exchange_strong(expected, true)) {
            VkDevice dev = VK_NULL_HANDLE; VkPhysicalDevice phys = VK_NULL_HANDLE;
            uint32_t bw = 0, bh = 0;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                auto sit = g_swaps.find(info->pSwapchains[0]);
                if (sit != g_swaps.end()) {
                    dev = sit->second.device; bw = sit->second.extent.width; bh = sit->second.extent.height;
                    DevFns& bf = devFns(dev); phys = bf.phys;
                }
            }
            if (dev != VK_NULL_HANDLE && bw && bh && pGDPA) {
                runInteropBenchmark(dev, phys, queue, 0, bw, bh, pGDPA, 120);
            }
        }
    }
    // STAGE 2 one-shot Adreno frame-extrapolation probe (opt-in via extrap_bench=1).
    if (info && info->swapchainCount >= 1 && g_config.extrap_bench) {
        static std::atomic<bool> extrapRan{false};
        bool expected = false;
        if (extrapRan.compare_exchange_strong(expected, true)) {
            uint32_t bw = 0, bh = 0;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                auto sit = g_swaps.find(info->pSwapchains[0]);
                if (sit != g_swaps.end()) { bw = sit->second.extent.width; bh = sit->second.extent.height; }
            }
            if (bw && bh) runExtrapBenchmark(bw, bh, 120);
        }
    }
    // STAGE 2 one-shot objective ME evaluation (opt-in via extrap_eval=1).
    // Collects 3 spaced real frames, but only fires once there is real motion
    // between them; predicts F2 from (F0,F1) and logs prediction error vs the
    // duplicate-frame baseline. Pure logcat, no files. Skips injection meanwhile.
    if (info && info->swapchainCount == 1 && g_config.extrap_eval) {
        static std::atomic<int> evalDone{0};
        static std::atomic<int> seen{0};
        static std::vector<uint8_t> f0, f1, f2;
        static uint32_t ew = 0, eh = 0;
        if (!evalDone.load()) {
            int n = seen.fetch_add(1) + 1;
            const int kWarmup = 120, kSample = 5;
            if (n >= kWarmup && (n % kSample) == 0) {
                std::vector<uint8_t> cur; uint32_t w = 0, h = 0;
                if (captureToHost(queue, info->pSwapchains[0], info->pImageIndices[0], cur, w, h)) {
                    if (!f2.empty() && (w != ew || h != eh)) { f0.clear(); f1.clear(); f2.clear(); }
                    ew = w; eh = h;
                    f0 = std::move(f1); f1 = std::move(f2); f2 = std::move(cur);
                    if (!f0.empty() && !f1.empty() && !f2.empty()) {
                        auto mad = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
                            size_t nn = a.size() < b.size() ? a.size() : b.size();
                            double acc = 0.0; size_t c = 0;
                            for (size_t i = 0; i + 4 <= nn; i += 64) { int d = (int)a[i] - (int)b[i]; acc += d < 0 ? -d : d; ++c; }
                            return c ? acc / (double)c : 0.0;
                        };
                        double m01 = mad(f0, f1), m12 = mad(f1, f2);
                        if (m01 >= 2.0 && m12 >= 2.0) {
                            LOGI("extrap-eval: 3 real frames with motion (m01=%.2f m12=%.2f %ux%u); evaluating", m01, m12, ew, eh);
                            runExtrapEval(std::move(f0), std::move(f1), std::move(f2), ew, eh);
                            evalDone.store(1);
                        } else if (g_config.debug) {
                            LOGI("extrap-eval: waiting for motion (m01=%.2f m12=%.2f)", m01, m12);
                        }
                    }
                }
            }
            return orig_QueuePresent(queue, info);
        }
    }
    // Auto-detect: claim the Vulkan engine for this process. If the GLES path
    // already claimed it, the app renders with GLES -> present normally.
    if (g_config.mode == Mode::Auto) {
        int expected = 0; g_activeEngine.compare_exchange_strong(expected, 2);
        if (g_activeEngine.load() != 2) return orig_QueuePresent(queue, info);
    }
    if (info) {
        // STAGE 2A present-bridge: synthesize an interpolated midpoint frame
        // (CPU blend of the previous and current frame) and present it BEFORE
        // the real current frame, for genuinely smoother perceived motion.
        if (g_config.present_bridge && info->swapchainCount == 1) {
            bool consumed = injectBlend(queue, info->pSwapchains[0], info, info->pImageIndices[0]);
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
    // STAGE 2: the game does not enable the device extensions our Vulkan<->GL
    // interop path needs (AHB import + external semaphore fd). Without them,
    // vkGetDeviceProcAddr returns null for those entry points even though the
    // physical device supports them. So we append the required device
    // extensions here, with a safe fallback: if the driver rejects the
    // augmented list we recreate the device exactly as the game asked, so the
    // game can never fail to boot because of us.
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    bool augmented = false;
    if (ci) {
        static const char* kWanted[] = {
            "VK_ANDROID_external_memory_android_hardware_buffer",
            "VK_EXT_queue_family_foreign",
            "VK_KHR_external_semaphore_fd",
        };
        std::vector<const char*> exts;
        for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i)
            exts.push_back(ci->ppEnabledExtensionNames[i]);
        for (const char* w : kWanted) {
            bool have = false;
            for (const char* e : exts) if (e && !strcmp(e, w)) { have = true; break; }
            if (!have) exts.push_back(w);
        }
        if (exts.size() != ci->enabledExtensionCount) {
            VkDeviceCreateInfo ci2 = *ci;
            ci2.enabledExtensionCount = (uint32_t)exts.size();
            ci2.ppEnabledExtensionNames = exts.data();
            r = orig_CreateDevice(phys, &ci2, alloc, out);
            if (r == VK_SUCCESS) {
                augmented = true;
                LOGI("vk: device created with interop extensions enabled (%u total)", ci2.enabledExtensionCount);
            } else {
                LOGW("vk: interop extensions rejected (r=%d); falling back to original device", r);
            }
        }
    }
    if (!augmented) {
        r = orig_CreateDevice(phys, ci, alloc, out);
    }
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
