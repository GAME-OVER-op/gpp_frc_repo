// Stage 2 probe: isolate the cost (and correctness) of the Adreno hardware
// frame extrapolation extension glExtrapolateTex2DQCOM on THIS device.
//
// Pure GL: spins up its own EGL worker context (eglMakeCurrent once), creates
// three RGBA8 textures (prev / cur / out) at swapchain resolution, paints a
// synthetic moving bar (prev: bar at X; cur: bar at X+shift => horizontal
// motion), then loops glExtrapolateTex2DQCOM(prev, cur, out, 0.5) and times it.
// Finally it reads back one row of the output and reports the centroid of the
// bright bar so we can confirm the predicted bar landed BETWEEN cur and the
// motion direction (i.e. the motion engine actually moved it).
//
// No Vulkan / AHB here on purpose: the interop round-trip cost is already
// measured separately. Total per-frame budget ~= interop + this.

#include "extrap_bench.h"
#include "config.h"
#include "log.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <thread>
#include <vector>
#include <algorithm>
#include <ctime>

#ifndef GL_APIENTRYP
#define GL_APIENTRYP *
#endif

namespace cleanfg {
namespace {

// Defined by GL_QCOM_frame_extrapolation. We declare our own typedef (distinct
// name) so we do not depend on the NDK gl2ext.h version exposing it.
typedef void (GL_APIENTRYP PFN_glExtrapolateTex2DQCOM)(GLuint src1, GLuint src2,
                                                       GLuint output, GLfloat scaleFactor);

double nowMs() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

GLuint makeTex(int w, int h) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    return t;
}

// Paint a solid black frame with a white vertical bar at [barX, barX+barW).
void paintBar(GLuint fbo, GLuint tex, int w, int h, int barX, int barW) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(barX, 0, barW, h);
    glClearColor(1.f, 1.f, 1.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

// Weighted centroid (in x) of the red channel along the middle scanline.
double barCentroid(GLuint fbo, GLuint tex, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return -1.0;
    std::vector<uint8_t> row((size_t)w * 4u, 0);
    glReadPixels(0, h / 2, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, row.data());
    double sum = 0.0, wsum = 0.0;
    for (int x = 0; x < w; ++x) {
        double v = (double)row[(size_t)x * 4u];  // R
        sum += v; wsum += v * (double)x;
    }
    return sum > 0.0 ? wsum / sum : -1.0;
}

void extrapThread(uint32_t width, uint32_t height, int iterations) {
    const int w = (int)width, h = (int)height;
    LOGI("extrap: starting Adreno frame-extrapolation probe %dx%d iters=%d", w, h, iterations);

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) {
        LOGE("extrap: eglInitialize failed"); return;
    }
    const EGLint cfgAttrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint nCfg = 0;
    if (!eglChooseConfig(dpy, cfgAttrs, &cfg, 1, &nCfg) || nCfg < 1) {
        LOGE("extrap: eglChooseConfig failed"); return;
    }
    const EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
    const EGLint pbAttrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbAttrs);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE ||
        !eglMakeCurrent(dpy, surf, surf, ctx)) {
        LOGE("extrap: failed to create/make-current EGL context"); return;
    }

    // Confirm the extension is exposed in OUR context, then resolve the entry.
    bool advertised = false;
    GLint nExt = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &nExt);
    for (GLint i = 0; i < nExt; ++i) {
        const char* e = (const char*)glGetStringi(GL_EXTENSIONS, i);
        if (e && !strcmp(e, "GL_QCOM_frame_extrapolation")) { advertised = true; break; }
    }
    PFN_glExtrapolateTex2DQCOM extrapolate =
        (PFN_glExtrapolateTex2DQCOM)eglGetProcAddress("glExtrapolateTex2DQCOM");
    LOGI("extrap: GL_QCOM_frame_extrapolation advertised=%d entrypoint=%p",
         advertised ? 1 : 0, (void*)extrapolate);
    if (!extrapolate) {
        LOGE("extrap: glExtrapolateTex2DQCOM unavailable in this context - cannot use HW extrapolation");
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(dpy, surf); eglDestroyContext(dpy, ctx);
        return;
    }

    GLuint fbo = 0; glGenFramebuffers(1, &fbo);
    GLuint prev = makeTex(w, h);
    GLuint cur  = makeTex(w, h);
    GLuint out  = makeTex(w, h);

    const int barW = 64;
    const int prevX = w / 4;
    const int shift = 16;          // horizontal motion between prev and cur
    const int curX  = prevX + shift;
    paintBar(fbo, prev, w, h, prevX, barW);
    paintBar(fbo, cur,  w, h, curX,  barW);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFinish();

    // warm-up (first calls compile/allocate internal ME state)
    for (int i = 0; i < 5; ++i) { extrapolate(prev, cur, out, 0.5f); }
    glFinish();
    GLenum gerr = glGetError();
    if (gerr != GL_NO_ERROR) LOGW("extrap: glGetError after warm-up = 0x%x", gerr);

    std::vector<double> samples; samples.reserve(iterations);
    for (int it = 0; it < iterations; ++it) {
        double t0 = nowMs();
        extrapolate(prev, cur, out, 0.5f);
        glFinish();                 // resolve GPU work so timing is real
        samples.push_back(nowMs() - t0);
    }

    // Correctness: where did the predicted bar land?
    double cPrev = barCentroid(fbo, prev, w, h);
    double cCur  = barCentroid(fbo, cur,  w, h);
    double cOut  = barCentroid(fbo, out,  w, h);
    double expectCentroid = (double)curX + (double)barW / 2.0 + 0.5 * (double)shift;
    LOGI("extrap: bar centroid prev=%.1f cur=%.1f predicted=%.1f (ideal~%.1f, motion=%dpx scale=0.5)",
         cPrev, cCur, cOut, expectCentroid, shift);

    if (!samples.empty()) {
        std::sort(samples.begin(), samples.end());
        double sum = 0; for (double x : samples) sum += x;
        double avg = sum / (double)samples.size();
        double mn = samples.front(), mx = samples.back();
        double p50 = samples[samples.size() / 2];
        LOGI("extrap: RESULT glExtrapolateTex2DQCOM over %zu iters: min=%.3fms p50=%.3fms avg=%.3fms max=%.3fms",
             samples.size(), mn, p50, avg, mx);
        LOGI("extrap: VERDICT %s (ME budget; pair with ~%.1fms interop upper-bound)",
             avg < 2.0 ? "GREAT-ME-cheap" : (avg < 4.0 ? "OK-ME-affordable" : "HEAVY-ME-reconsider"),
             1.93);
    } else {
        LOGE("extrap: no samples collected");
    }

    glDeleteTextures(1, &prev);
    glDeleteTextures(1, &cur);
    glDeleteTextures(1, &out);
    glDeleteFramebuffers(1, &fbo);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(dpy, surf);
    eglDestroyContext(dpy, ctx);
    LOGI("extrap: probe complete");
}

}  // namespace

void runExtrapBenchmark(uint32_t width, uint32_t height, int iterations) {
    std::thread(extrapThread, width, height, iterations).detach();
}

}  // namespace cleanfg
