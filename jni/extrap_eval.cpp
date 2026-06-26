#include "extrap_eval.h"
#include "log.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <thread>
#include <vector>
#include <cstdint>
#include <chrono>
#include <algorithm>

#ifndef GL_APIENTRYP
#define GL_APIENTRYP *
#endif

namespace cleanfg {
namespace {

typedef void (GL_APIENTRYP PFN_glExtrapolateTex2DQCOM)(GLuint src1, GLuint src2,
                                                       GLuint output, GLfloat scaleFactor);

GLuint makeTex(int w, int h, const uint8_t* data) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    return t;
}

double meanAbsDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    if (!n) return -1.0;
    double acc = 0.0; size_t cnt = 0;
    for (size_t i = 0; i + 3 < n; i += 4) {
        for (int c = 0; c < 3; ++c) {
            int d = (int)a[i + c] - (int)b[i + c];
            acc += d < 0 ? -d : d; ++cnt;
        }
    }
    return cnt ? acc / (double)cnt : -1.0;
}

void evalThread(std::vector<uint8_t> f0, std::vector<uint8_t> f1,
                std::vector<uint8_t> f2, uint32_t width, uint32_t height) {
    const int w = (int)width, h = (int)height;
    LOGI("extrap-eval: worker start %dx%d", w, h);

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) {
        LOGE("extrap-eval: eglInitialize failed"); return;
    }
    const EGLint cfgAttrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint nc = 0;
    if (!eglChooseConfig(dpy, cfgAttrs, &cfg, 1, &nc) || nc < 1) {
        LOGE("extrap-eval: eglChooseConfig failed"); return;
    }
    const EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
    const EGLint pb[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE ||
        !eglMakeCurrent(dpy, surf, surf, ctx)) {
        LOGE("extrap-eval: failed to create/make-current EGL context"); return;
    }

    PFN_glExtrapolateTex2DQCOM extrapolate =
        (PFN_glExtrapolateTex2DQCOM)eglGetProcAddress("glExtrapolateTex2DQCOM");
    LOGI("extrap-eval: glExtrapolateTex2DQCOM entrypoint=%p", (void*)extrapolate);

    double dupErr = meanAbsDiff(f1, f2);
    LOGI("extrap-eval: input motion |F0-F1|=%.3f |F1-F2|=%.3f ; baseline duplicate error=%.3f",
         meanAbsDiff(f0, f1), dupErr, dupErr);

    if (extrapolate) {
        GLuint t0 = makeTex(w, h, f0.data());
        GLuint t1 = makeTex(w, h, f1.data());
        GLuint tout = makeTex(w, h, nullptr);
        GLuint fbo = 0; glGenFramebuffers(1, &fbo);
        glFinish();
        std::vector<uint8_t> outBuf((size_t)w * (size_t)h * 4u, 0);
        const float scales[] = { 0.25f, 0.5f, 1.0f, 1.5f, 2.0f };
        double best = 1e18; float bestS = 0.0f;
        for (float s : scales) {
            for (int i = 0; i < 3; ++i) extrapolate(t0, t1, tout, s);
            glFinish();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tout, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                LOGW("extrap-eval: fbo incomplete s=%.2f", s); continue;
            }
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, outBuf.data());
            double e = meanAbsDiff(outBuf, f2);
            double impr = dupErr - e;
            LOGI("extrap-eval: scale=%.2f predErr=%.3f dupErr=%.3f improvement=%.3f (%+.1f%%)",
                 s, e, dupErr, impr, dupErr > 0.0 ? 100.0 * impr / dupErr : 0.0);
            if (e < best) { best = e; bestS = s; }
        }
        GLenum gerr = glGetError();
        if (gerr != GL_NO_ERROR) LOGW("extrap-eval: glGetError = 0x%x", gerr);
        LOGI("extrap-eval: VERDICT best scale=%.2f predErr=%.3f %s baseline by %.1f%%",
             bestS, best, best < dupErr ? "BEATS" : "WORSE-than",
             dupErr > 0.0 ? 100.0 * (dupErr - best) / dupErr : 0.0);
        // Cost on real frames: time the extension at the best scale (like a
        // benchmark, but on real game content instead of synthetic input).
        {
            const int iters = 120;
            std::vector<double> ms; ms.reserve(iters);
            for (int i = 0; i < iters; ++i) {
                auto t0c = std::chrono::steady_clock::now();
                extrapolate(t0, t1, tout, bestS);
                glFinish();
                auto t1c = std::chrono::steady_clock::now();
                ms.push_back(std::chrono::duration<double, std::milli>(t1c - t0c).count());
            }
            std::sort(ms.begin(), ms.end());
            double sum = 0.0; for (double v : ms) sum += v;
            LOGI("extrap-eval: cost @scale=%.2f over %d iters: min=%.3fms p50=%.3fms avg=%.3fms max=%.3fms",
                 bestS, iters, ms.front(), ms[ms.size()/2], sum/(double)iters, ms.back());
        }

        glDeleteTextures(1, &t0);
        glDeleteTextures(1, &t1);
        glDeleteTextures(1, &tout);
        glDeleteFramebuffers(1, &fbo);
    } else {
        LOGE("extrap-eval: glExtrapolateTex2DQCOM unavailable");
    }

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(dpy, surf);
    eglDestroyContext(dpy, ctx);
    LOGI("extrap-eval: complete");
}

}  // namespace

void runExtrapEval(std::vector<uint8_t> f0, std::vector<uint8_t> f1,
                   std::vector<uint8_t> f2, uint32_t width, uint32_t height) {
    std::thread(evalThread, std::move(f0), std::move(f1), std::move(f2), width, height).detach();
}

}  // namespace cleanfg
