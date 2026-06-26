// Stage 2 probe v2: characterize Adreno glExtrapolateTex2DQCOM on THIS device
// with a TRACKABLE stimulus (a full-frame checkerboard that shifts), and
// quantitatively measure where the extension places the extrapolated content
// for several scaleFactor values. A single bar on black is a degenerate input
// for block-matching motion estimation (aperture problem + featureless
// regions); a checkerboard has features everywhere, so the motion field is
// well-defined.
//
// For each scaleFactor we render prev(offset 0) and cur(offset = motionPx),
// run extrapolate -> out, read out back, and find by SAD which integer pixel
// offset best matches the output. That tells us, in pixels, how far the ME
// moved the content -- i.e. the real meaning of scaleFactor.
//
// Pure GL, no Vulkan/AHB (interop cost measured separately).

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
#include <cstring>
#include <cstdint>

#ifndef GL_APIENTRYP
#define GL_APIENTRYP *
#endif

namespace cleanfg {
namespace {

typedef void (GL_APIENTRYP PFN_glExtrapolateTex2DQCOM)(GLuint src1, GLuint src2,
                                                       GLuint output, GLfloat scaleFactor);

double nowMs() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

const int kTile = 32;   // checkerboard tile size (period = 64px)

// 1 if the checkerboard cell at (x,y) shifted by ox is bright, else 0.
inline int checkBright(int x, int y, int ox) {
    int cx = (x + ox) / kTile;
    int cy = y / kTile;
    return ((cx + cy) & 1) == 0 ? 1 : 0;
}

void genChecker(std::vector<uint8_t>& buf, int w, int h, int ox) {
    buf.resize((size_t)w * (size_t)h * 4u);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t v = checkBright(x, y, ox) ? 255 : 0;
            size_t i = ((size_t)y * (size_t)w + (size_t)x) * 4u;
            buf[i + 0] = v; buf[i + 1] = v; buf[i + 2] = v; buf[i + 3] = 255;
        }
    }
}

GLuint makeTex(int w, int h, const uint8_t* data) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    return t;
}

// SAD between the readback (out) and an ideal checkerboard shifted by ox.
// Subsampled for speed; uses the R channel only.
double sadAtOffset(const std::vector<uint8_t>& out, int w, int h, int ox, int step) {
    double acc = 0.0; long n = 0;
    for (int y = step; y < h - step; y += step) {
        for (int x = step; x < w - step; x += step) {
            int ideal = checkBright(x, y, ox) ? 255 : 0;
            int got = out[((size_t)y * (size_t)w + (size_t)x) * 4u];
            acc += (double)(got > ideal ? got - ideal : ideal - got);
            ++n;
        }
    }
    return n ? acc / (double)n : 1e9;
}

int bestOffset(const std::vector<uint8_t>& out, int w, int h, int maxOff, double* bestSad) {
    int best = -1; double bs = 1e18;
    for (int o = 0; o <= maxOff; ++o) {
        double s = sadAtOffset(out, w, h, o, 4);
        if (s < bs) { bs = s; best = o; }
    }
    if (bestSad) *bestSad = bs;
    return best;
}

void extrapThread(uint32_t width, uint32_t height, int iterations) {
    const int w = (int)width, h = (int)height;
    const int motionPx = 16;
    LOGI("extrap: starting Adreno frame-extrapolation probe v2 %dx%d iters=%d motion=%dpx tile=%d",
         w, h, iterations, motionPx, kTile);

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
        LOGE("extrap: glExtrapolateTex2DQCOM unavailable");
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(dpy, surf); eglDestroyContext(dpy, ctx);
        return;
    }

    std::vector<uint8_t> prevBuf, curBuf;
    genChecker(prevBuf, w, h, 0);
    genChecker(curBuf,  w, h, motionPx);
    GLuint prev = makeTex(w, h, prevBuf.data());
    GLuint cur  = makeTex(w, h, curBuf.data());
    GLuint out  = makeTex(w, h, nullptr);
    GLuint fbo = 0; glGenFramebuffers(1, &fbo);
    glFinish();

    // Sanity references: where do prev/cur themselves best-match? (should be 0 and motionPx)
    {
        double sp = 0, sc = 0;
        int op = bestOffset(prevBuf, w, h, 2 * kTile, &sp);
        int oc = bestOffset(curBuf,  w, h, 2 * kTile, &sc);
        LOGI("extrap: sanity self-match prev->%dpx(sad=%.1f) cur->%dpx(sad=%.1f) [expect 0 and %d]",
             op, sp, oc, sc, motionPx);
    }

    // Characterize scaleFactor semantics.
    const float scales[] = { 0.25f, 0.5f, 1.0f };
    std::vector<uint8_t> outBuf((size_t)w * (size_t)h * 4u, 0);
    for (float sc : scales) {
        for (int i = 0; i < 3; ++i) extrapolate(prev, cur, out, sc);  // warm
        glFinish();
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOGW("extrap: out FBO incomplete for scale=%.2f", sc); continue;
        }
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, outBuf.data());
        double bs = 0;
        int bo = bestOffset(outBuf, w, h, 2 * kTile, &bs);
        LOGI("extrap: scale=%.2f -> matched offset=%dpx (sad=%.1f) [cur=%dpx; pure-copy=%d; ideal-0.5-step=%d]",
             sc, bo, bs, motionPx, motionPx, motionPx + motionPx / 2);
    }
    GLenum gerr = glGetError();
    if (gerr != GL_NO_ERROR) LOGW("extrap: glGetError = 0x%x", gerr);

    // Timing at scale=0.5.
    for (int i = 0; i < 5; ++i) extrapolate(prev, cur, out, 0.5f);
    glFinish();
    std::vector<double> samples; samples.reserve(iterations);
    for (int it = 0; it < iterations; ++it) {
        double t0 = nowMs();
        extrapolate(prev, cur, out, 0.5f);
        glFinish();
        samples.push_back(nowMs() - t0);
    }
    if (!samples.empty()) {
        std::sort(samples.begin(), samples.end());
        double sum = 0; for (double x : samples) sum += x;
        double avg = sum / (double)samples.size();
        double mn = samples.front(), mx = samples.back();
        double p50 = samples[samples.size() / 2];
        LOGI("extrap: RESULT glExtrapolateTex2DQCOM over %zu iters: min=%.3fms p50=%.3fms avg=%.3fms max=%.3fms",
             samples.size(), mn, p50, avg, mx);
        LOGI("extrap: VERDICT %s",
             avg < 2.0 ? "GREAT-ME-cheap" : (avg < 4.0 ? "OK-ME-affordable" : "HEAVY-ME-reconsider"));
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
