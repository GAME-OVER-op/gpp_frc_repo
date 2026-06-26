// Vulkan <-> GL interop overhead benchmark (Stage 2 prototype).
//
// Isolates the cost of the AHardwareBuffer + sync_fd round-trip BEFORE we
// commit to the full Adreno Motion Engine pipeline. Per iteration:
//   1. (present thread) Vulkan clears an AHB-backed VkImage, submits signalling
//      a VkSemaphore exported to a SYNC_FD (VK_KHR_external_semaphore_fd).
//   2. The fd is handed to the GL worker thread.
//   3. (worker thread) imports it via eglCreateSyncKHR(EGL_SYNC_NATIVE_FENCE
//      _ANDROID), eglWaitSyncKHR (GPU-side wait), reads the AHB-backed texture
//      (glReadPixels 1px) to prove the data arrived, then exports its own GL
//      fence to a SYNC_FD via eglDupNativeFenceFDANDROID.
//   4. (present thread) imports that fd into a VkSemaphore and waits on it,
//      closing the loop back to Vulkan.
// We time the whole round-trip with CLOCK_MONOTONIC and log min/avg/max/p50.
//
// Threading matches the shipping decision: Vulkan submits stay on the present
// thread (only thread allowed to touch the game's present queue); the GL
// context lives on a dedicated worker thread (eglMakeCurrent exactly once).

#include "interop_bench.h"
#include "config.h"
#include "log.h"

#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <algorithm>
#include <ctime>
#include <unistd.h>

namespace cleanfg {
namespace {

double nowMs() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

// ------------------------- GL worker thread -------------------------------
struct Worker {
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    // request: a vulkan SYNC_FD for the worker to wait on (ownership passed in)
    int  reqFd = -1;
    bool reqPending = false;
    bool quit = false;
    // response: a GL SYNC_FD for vulkan to wait on (ownership passed back)
    int  respFd = -1;
    bool respReady = false;
    // init handshake
    bool initDone = false;
    bool initOk = false;

    AHardwareBuffer* ahb = nullptr;
    uint32_t width = 0, height = 0;

    // EGL/GL objects (owned by the worker thread)
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLImageKHR eglImg = EGL_NO_IMAGE_KHR;
    GLuint tex = 0, fbo = 0;

    // resolved extension entry points
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC getClientBuffer = nullptr;
    PFNEGLCREATEIMAGEKHRPROC   createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC  destroyImage = nullptr;
    PFNEGLCREATESYNCKHRPROC    createSync = nullptr;
    PFNEGLDESTROYSYNCKHRPROC   destroySync = nullptr;
    PFNEGLWAITSYNCKHRPROC      waitSync = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dupFenceFd = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture2D = nullptr;
};

bool workerInitGl(Worker* w) {
    w->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (w->dpy == EGL_NO_DISPLAY) { LOGE("bench: eglGetDisplay failed"); return false; }
    if (!eglInitialize(w->dpy, nullptr, nullptr)) { LOGE("bench: eglInitialize failed"); return false; }

    const EGLint cfgAttrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint nCfg = 0;
    if (!eglChooseConfig(w->dpy, cfgAttrs, &cfg, 1, &nCfg) || nCfg < 1) {
        LOGE("bench: eglChooseConfig failed"); return false;
    }
    const EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    w->ctx = eglCreateContext(w->dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
    if (w->ctx == EGL_NO_CONTEXT) { LOGE("bench: eglCreateContext failed"); return false; }
    const EGLint pbAttrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    w->surf = eglCreatePbufferSurface(w->dpy, cfg, pbAttrs);
    if (w->surf == EGL_NO_SURFACE) { LOGE("bench: eglCreatePbufferSurface failed"); return false; }
    if (!eglMakeCurrent(w->dpy, w->surf, w->surf, w->ctx)) { LOGE("bench: eglMakeCurrent failed"); return false; }

    w->getClientBuffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    w->createImage  = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    w->destroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    w->createSync   = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    w->destroySync  = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    w->waitSync     = (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
    w->dupFenceFd   = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    w->imageTargetTexture2D = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!w->getClientBuffer || !w->createImage || !w->createSync || !w->waitSync ||
        !w->dupFenceFd || !w->imageTargetTexture2D) {
        LOGE("bench: required EGL/GL extension entry points missing (AHB/native-fence interop unsupported)");
        return false;
    }

    // Import the AHB into a GL texture via EGLImage.
    EGLClientBuffer cb = w->getClientBuffer(w->ahb);
    if (!cb) { LOGE("bench: eglGetNativeClientBufferANDROID returned null"); return false; }
    const EGLint imgAttrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    w->eglImg = w->createImage(w->dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, imgAttrs);
    if (w->eglImg == EGL_NO_IMAGE_KHR) { LOGE("bench: eglCreateImageKHR failed"); return false; }

    glGenTextures(1, &w->tex);
    glBindTexture(GL_TEXTURE_2D, w->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    w->imageTargetTexture2D(GL_TEXTURE_2D, (GLeglImageOES)w->eglImg);

    glGenFramebuffers(1, &w->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, w->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, w->tex, 0);
    GLenum fbs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbs != GL_FRAMEBUFFER_COMPLETE) {
        LOGW("bench: FBO incomplete (0x%x) - will still time the sync path", fbs);
    }
    LOGI("bench: GL worker ready (AHB %ux%u imported as GL texture)", w->width, w->height);
    return true;
}

void workerLoop(Worker* w) {
    bool ok = workerInitGl(w);
    {
        std::lock_guard<std::mutex> lk(w->m);
        w->initOk = ok; w->initDone = true;
    }
    w->cv.notify_all();
    if (!ok) return;

    uint8_t px[4];
    for (;;) {
        int vkFd;
        {
            std::unique_lock<std::mutex> lk(w->m);
            w->cv.wait(lk, [&]{ return w->reqPending || w->quit; });
            if (w->quit) break;
            vkFd = w->reqFd; w->reqFd = -1; w->reqPending = false;
        }
        // Wait on the Vulkan-produced fence on the GPU timeline, then touch the
        // texture, then produce a GL fence for Vulkan to wait on.
        if (vkFd >= 0) {
            EGLint sAttrs[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, vkFd, EGL_NONE };
            EGLSyncKHR s = w->createSync(w->dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, sAttrs);
            if (s != EGL_NO_SYNC_KHR) {
                w->waitSync(w->dpy, s, 0);   // server-side (GPU) wait, no CPU block
                w->destroySync(w->dpy, s);   // consumes/owns vkFd
            } else {
                close(vkFd);
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, w->fbo);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);  // proves data arrived
        EGLSyncKHR outS = w->createSync(w->dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
        glFlush();
        int outFd = -1;
        if (outS != EGL_NO_SYNC_KHR) {
            outFd = w->dupFenceFd(w->dpy, outS);
            w->destroySync(w->dpy, outS);
        }
        {
            std::lock_guard<std::mutex> lk(w->m);
            w->respFd = outFd; w->respReady = true;
        }
        w->cv.notify_all();
    }

    // teardown
    if (w->fbo) glDeleteFramebuffers(1, &w->fbo);
    if (w->tex) glDeleteTextures(1, &w->tex);
    if (w->eglImg != EGL_NO_IMAGE_KHR && w->destroyImage) w->destroyImage(w->dpy, w->eglImg);
    eglMakeCurrent(w->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (w->surf != EGL_NO_SURFACE) eglDestroySurface(w->dpy, w->surf);
    if (w->ctx != EGL_NO_CONTEXT) eglDestroyContext(w->dpy, w->ctx);
}

// ------------------------- Vulkan side ------------------------------------
struct VkFns {
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkAllocateMemory allocMem = nullptr;
    PFN_vkFreeMemory freeMem = nullptr;
    PFN_vkBindImageMemory bindImgMem = nullptr;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getAhbProps = nullptr;
    PFN_vkCreateCommandPool createPool = nullptr;
    PFN_vkDestroyCommandPool destroyPool = nullptr;
    PFN_vkAllocateCommandBuffers allocCmd = nullptr;
    PFN_vkBeginCommandBuffer beginCmd = nullptr;
    PFN_vkEndCommandBuffer endCmd = nullptr;
    PFN_vkResetCommandBuffer resetCmd = nullptr;
    PFN_vkCmdPipelineBarrier barrier = nullptr;
    PFN_vkCmdClearColorImage clearColor = nullptr;
    PFN_vkQueueSubmit submit = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkWaitForFences waitFences = nullptr;
    PFN_vkResetFences resetFences = nullptr;
    PFN_vkCreateSemaphore createSem = nullptr;
    PFN_vkDestroySemaphore destroySem = nullptr;
    PFN_vkGetSemaphoreFdKHR getSemFd = nullptr;
    PFN_vkImportSemaphoreFdKHR importSemFd = nullptr;
    bool ok = false;
};

VkFns resolveVk(VkDevice dev, PFN_vkGetDeviceProcAddr G) {
    VkFns f;
    auto R = [&](const char* n){ return G ? G(dev, n) : (PFN_vkVoidFunction)nullptr; };
    f.createImage   = (PFN_vkCreateImage)R("vkCreateImage");
    f.destroyImage  = (PFN_vkDestroyImage)R("vkDestroyImage");
    f.allocMem      = (PFN_vkAllocateMemory)R("vkAllocateMemory");
    f.freeMem       = (PFN_vkFreeMemory)R("vkFreeMemory");
    f.bindImgMem    = (PFN_vkBindImageMemory)R("vkBindImageMemory");
    f.getAhbProps   = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)R("vkGetAndroidHardwareBufferPropertiesANDROID");
    f.createPool    = (PFN_vkCreateCommandPool)R("vkCreateCommandPool");
    f.destroyPool   = (PFN_vkDestroyCommandPool)R("vkDestroyCommandPool");
    f.allocCmd      = (PFN_vkAllocateCommandBuffers)R("vkAllocateCommandBuffers");
    f.beginCmd      = (PFN_vkBeginCommandBuffer)R("vkBeginCommandBuffer");
    f.endCmd        = (PFN_vkEndCommandBuffer)R("vkEndCommandBuffer");
    f.resetCmd      = (PFN_vkResetCommandBuffer)R("vkResetCommandBuffer");
    f.barrier       = (PFN_vkCmdPipelineBarrier)R("vkCmdPipelineBarrier");
    f.clearColor    = (PFN_vkCmdClearColorImage)R("vkCmdClearColorImage");
    f.submit        = (PFN_vkQueueSubmit)R("vkQueueSubmit");
    f.createFence   = (PFN_vkCreateFence)R("vkCreateFence");
    f.destroyFence  = (PFN_vkDestroyFence)R("vkDestroyFence");
    f.waitFences    = (PFN_vkWaitForFences)R("vkWaitForFences");
    f.resetFences   = (PFN_vkResetFences)R("vkResetFences");
    f.createSem     = (PFN_vkCreateSemaphore)R("vkCreateSemaphore");
    f.destroySem    = (PFN_vkDestroySemaphore)R("vkDestroySemaphore");
    f.getSemFd      = (PFN_vkGetSemaphoreFdKHR)R("vkGetSemaphoreFdKHR");
    f.importSemFd   = (PFN_vkImportSemaphoreFdKHR)R("vkImportSemaphoreFdKHR");
    f.ok = f.createImage && f.allocMem && f.bindImgMem && f.getAhbProps &&
           f.createPool && f.allocCmd && f.barrier && f.clearColor && f.submit &&
           f.createFence && f.waitFences && f.createSem && f.getSemFd && f.importSemFd;
    return f;
}

}  // namespace

void runInteropBenchmark(VkDevice device, VkPhysicalDevice phys, VkQueue queue,
                         uint32_t queueFamily, uint32_t width, uint32_t height,
                         PFN_vkGetDeviceProcAddr gdpa, int iterations) {
    (void)phys;
    LOGI("bench: starting Vulkan<->GL interop benchmark %ux%u iters=%d", width, height, iterations);

    VkFns f = resolveVk(device, gdpa);
    if (!f.ok) {
        LOGE("bench: Vulkan AHB/external-semaphore entry points unavailable - device likely lacks "
             "VK_ANDROID_external_memory_android_hardware_buffer or VK_KHR_external_semaphore_fd");
        return;
    }

    // All handles declared up front so the single cleanup lambda can release
    // whatever was created, regardless of where we bail out (no goto).
    AHardwareBuffer* ahb = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore semExport = VK_NULL_HANDLE;
    VkSemaphore semImport = VK_NULL_HANDLE;

    auto cleanup = [&]{
        if (semImport) f.destroySem(device, semImport, nullptr);
        if (semExport) f.destroySem(device, semExport, nullptr);
        if (fence) f.destroyFence(device, fence, nullptr);
        if (pool) f.destroyPool(device, pool, nullptr);
        if (mem) f.freeMem(device, mem, nullptr);
        if (image) f.destroyImage(device, image, nullptr);
        if (ahb) AHardwareBuffer_release(ahb);
        LOGI("bench: resources released");
    };

    // 1) Allocate the shared AHardwareBuffer (usable by both Vulkan and GL).
    AHardwareBuffer_Desc d{};
    d.width = width; d.height = height; d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    if (AHardwareBuffer_allocate(&d, &ahb) != 0 || !ahb) {
        LOGE("bench: AHardwareBuffer_allocate failed"); ahb = nullptr; cleanup(); return;
    }

    // 2) Import the AHB into a VkImage + VkDeviceMemory.
    VkAndroidHardwareBufferPropertiesANDROID ahbProps{};
    ahbProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    if (f.getAhbProps(device, ahb, &ahbProps) != VK_SUCCESS) {
        LOGE("bench: vkGetAndroidHardwareBufferPropertiesANDROID failed"); cleanup(); return;
    }
    int memTypeIdx = -1;
    for (uint32_t i = 0; i < 32; ++i) if (ahbProps.memoryTypeBits & (1u << i)) { memTypeIdx = (int)i; break; }
    if (memTypeIdx < 0) { LOGE("bench: no memory type for AHB"); cleanup(); return; }

    VkExternalMemoryImageCreateInfo emici{};
    emici.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    emici.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &emici;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = { width, height, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (f.createImage(device, &ici, nullptr, &image) != VK_SUCCESS) {
        LOGE("bench: vkCreateImage(AHB) failed"); image = VK_NULL_HANDLE; cleanup(); return;
    }

    VkImportAndroidHardwareBufferInfoANDROID importAhb{};
    importAhb.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    importAhb.buffer = ahb;
    VkMemoryDedicatedAllocateInfo ded{};
    ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded.image = image;
    ded.pNext = &importAhb;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &ded;
    mai.allocationSize = ahbProps.allocationSize;
    mai.memoryTypeIndex = (uint32_t)memTypeIdx;
    if (f.allocMem(device, &mai, nullptr, &mem) != VK_SUCCESS) {
        LOGE("bench: vkAllocateMemory(import AHB) failed"); mem = VK_NULL_HANDLE; cleanup(); return;
    }
    if (f.bindImgMem(device, image, mem, 0) != VK_SUCCESS) {
        LOGE("bench: vkBindImageMemory failed"); cleanup(); return;
    }

    // 3) Command pool + buffer + fence + export/import semaphores.
    VkCommandPoolCreateInfo pci{}; pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = queueFamily;
    if (f.createPool(device, &pci, nullptr, &pool) != VK_SUCCESS) { LOGE("bench: createPool fail"); pool = VK_NULL_HANDLE; cleanup(); return; }
    VkCommandBufferAllocateInfo cai{}; cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    if (f.allocCmd(device, &cai, &cmd) != VK_SUCCESS) { LOGE("bench: allocCmd fail"); cleanup(); return; }
    VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    f.createFence(device, &fci, nullptr, &fence);

    VkExportSemaphoreCreateInfo esci{}; esci.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    esci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo sci{}; sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO; sci.pNext = &esci;
    if (f.createSem(device, &sci, nullptr, &semExport) != VK_SUCCESS) { LOGE("bench: createSem(export) fail"); semExport = VK_NULL_HANDLE; cleanup(); return; }
    VkSemaphoreCreateInfo sci2{}; sci2.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (f.createSem(device, &sci2, nullptr, &semImport) != VK_SUCCESS) { LOGE("bench: createSem(import) fail"); semImport = VK_NULL_HANDLE; cleanup(); return; }

    // 4) Start the GL worker thread and wait for its init handshake.
    Worker w;
    w.ahb = ahb; w.width = width; w.height = height;
    w.th = std::thread(workerLoop, &w);
    {
        std::unique_lock<std::mutex> lk(w.m);
        w.cv.wait(lk, [&]{ return w.initDone; });
    }
    if (!w.initOk) {
        LOGE("bench: GL worker init failed - aborting benchmark");
        { std::lock_guard<std::mutex> lk(w.m); w.quit = true; } w.cv.notify_all();
        w.th.join();
        cleanup(); return;
    }

    std::vector<double> samples; samples.reserve(iterations);
    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    bool firstClear = true;
    for (int it = 0; it < iterations; ++it) {
        double t0 = nowMs();

        // --- Vulkan: clear AHB image, submit signalling semExport ---
        f.resetCmd(cmd, 0);
        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        f.beginCmd(cmd, &bi);
        VkImageMemoryBarrier toDst{}; toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDst.oldLayout = firstClear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcAccessMask = 0; toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = image; toDst.subresourceRange = range;
        f.barrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
        VkClearColorValue col{}; col.float32[0] = 0.1f * (float)(it % 10);
        col.float32[1] = 0.2f; col.float32[2] = 0.3f; col.float32[3] = 1.0f;
        f.clearColor(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &range);
        VkImageMemoryBarrier toGen{}; toGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGen.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; toGen.dstAccessMask = 0;
        toGen.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; toGen.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGen.image = image; toGen.subresourceRange = range;
        f.barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGen);
        f.endCmd(cmd);
        firstClear = false;

        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &semExport;
        f.resetFences(device, 1, &fence);
        if (f.submit(queue, 1, &si, fence) != VK_SUCCESS) { LOGE("bench: submit(clear) fail @%d", it); break; }

        // export the signal as a sync_fd
        int vkFd = -1;
        VkSemaphoreGetFdInfoKHR gfi{}; gfi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
        gfi.semaphore = semExport; gfi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        if (f.getSemFd(device, &gfi, &vkFd) != VK_SUCCESS) { LOGW("bench: getSemaphoreFd fail @%d", it); vkFd = -1; }

        // --- hand off to GL worker, wait for its GL fence back ---
        {
            std::lock_guard<std::mutex> lk(w.m);
            w.reqFd = vkFd; w.reqPending = true; w.respReady = false;
        }
        w.cv.notify_all();
        int glFd;
        {
            std::unique_lock<std::mutex> lk(w.m);
            w.cv.wait(lk, [&]{ return w.respReady; });
            glFd = w.respFd; w.respFd = -1; w.respReady = false;
        }

        // --- Vulkan: import GL fence and wait on it (close the loop) ---
        if (glFd >= 0) {
            VkImportSemaphoreFdInfoKHR ifi{}; ifi.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
            ifi.semaphore = semImport; ifi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            ifi.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT; ifi.fd = glFd;
            if (f.importSemFd(device, &ifi) == VK_SUCCESS) {
                VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                VkSubmitInfo wsi{}; wsi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                wsi.waitSemaphoreCount = 1; wsi.pWaitSemaphores = &semImport; wsi.pWaitDstStageMask = &waitStage;
                f.resetFences(device, 1, &fence);
                if (f.submit(queue, 1, &wsi, fence) == VK_SUCCESS)
                    f.waitFences(device, 1, &fence, VK_TRUE, 1000000000ULL);
            } else {
                close(glFd);
                f.waitFences(device, 1, &fence, VK_TRUE, 1000000000ULL);
            }
        } else {
            f.waitFences(device, 1, &fence, VK_TRUE, 1000000000ULL);
        }

        samples.push_back(nowMs() - t0);
    }

    // stop worker
    { std::lock_guard<std::mutex> lk(w.m); w.quit = true; } w.cv.notify_all();
    w.th.join();

    if (!samples.empty()) {
        std::sort(samples.begin(), samples.end());
        double sum = 0; for (double x : samples) sum += x;
        double avg = sum / (double)samples.size();
        double mn = samples.front();
        double mx = samples.back();
        double p50 = samples[samples.size() / 2];
        LOGI("bench: RESULT interop round-trip over %zu iters: min=%.3fms p50=%.3fms avg=%.3fms max=%.3fms",
             samples.size(), mn, p50, avg, mx);
        LOGI("bench: VERDICT %s (avg<3ms good, >6ms too costly)",
             avg < 3.0 ? "GOOD-interop-cheap-proceed-to-Adreno-Motion-Engine"
                       : (avg < 6.0 ? "MARGINAL-needs-optimization" : "TOO-COSTLY-rethink-transport"));
    } else {
        LOGE("bench: no samples collected");
    }

    cleanup();
    LOGI("bench: benchmark complete");
}

}  // namespace cleanfg
