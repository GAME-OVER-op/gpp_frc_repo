#include "extrap_dump.h"
#include "log.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <thread>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdint>

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

bool writePPM(const std::string& path, const uint8_t* rgba, int w, int h) {
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) { LOGW("extrap-dump: cannot open %s", path.c_str()); return false; }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row((size_t)w * 3u);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = rgba + (size_t)y * (size_t)w * 4u;
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite(row.data(), 1, row.size(), fp);
    }
    fclose(fp);
    return true;
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

void dumpThread(std::vector<uint8_t> prev, std::vector<uint8_t> cur,
                uint32_t width, uint32_t height, std::string outDir) {
    const int w = (int)width, h = (int)height;
    LOGI("extrap-dump: worker start %dx%d dir=%s", w, h, outDir.c_str());

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) {
        LOGE("extrap-dump: eglInitialize failed"); return;
    }
    const EGLint cfgAttrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint nc = 0;
    if (!eglChooseConfig(dpy, cfgAttrs, &cfg, 1, &nc) || nc < 1) {
        LOGE("extrap-dump: eglChooseConfig failed"); return;
    }
    const EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
    const EGLint pb[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE ||
        !eglMakeCurrent(dpy, surf, surf, ctx)) {
        LOGE("extrap-dump: failed to create/make-current EGL context"); return;
    }

    PFN_glExtrapolateTex2DQCOM extrapolate =
        (PFN_glExtrapolateTex2DQCOM)eglGetProcAddress("glExtrapolateTex2DQCOM");
    LOGI("extrap-dump: glExtrapolateTex2DQCOM entrypoint=%p", (void*)extrapolate);

    writePPM(outDir + "cleanfg_prev.ppm", prev.data(), w, h);
    writePPM(outDir + "cleanfg_cur.ppm",  cur.data(),  w, h);
    LOGI("extrap-dump: input motion meanAbsDiff(cur,prev)=%.2f (0=>frames identical, no motion captured)",
         meanAbsDiff(cur, prev));

    GLuint tPrev = makeTex(w, h, prev.data());
    GLuint tCur  = makeTex(w, h, cur.data());
    GLuint tOut  = makeTex(w, h, nullptr);
    GLuint fbo = 0; glGenFramebuffers(1, &fbo);
    glFinish();

    if (extrapolate) {
        const float scales[] = { 0.5f, 1.0f };
        std::vector<uint8_t> outBuf((size_t)w * (size_t)h * 4u, 0);
        for (float s : scales) {
            for (int i = 0; i < 3; ++i) extrapolate(tPrev, tCur, tOut, s);
            glFinish();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tOut, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, outBuf.data());
                char name[64];
                snprintf(name, sizeof(name), "cleanfg_out_s%03d.ppm", (int)(s * 100.0f));
                writePPM(outDir + name, outBuf.data(), w, h);
                LOGI("extrap-dump: scale=%.2f meanAbsDiff(out,cur)=%.2f (0=>pure copy of cur, >0=>ME moved content)",
                     s, meanAbsDiff(outBuf, cur));
            } else {
                LOGW("extrap-dump: out FBO incomplete for scale=%.2f", s);
            }
        }
        GLenum gerr = glGetError();
        if (gerr != GL_NO_ERROR) LOGW("extrap-dump: glGetError = 0x%x", gerr);
    } else {
        LOGE("extrap-dump: glExtrapolateTex2DQCOM unavailable");
    }

    glDeleteTextures(1, &tPrev);
    glDeleteTextures(1, &tCur);
    glDeleteTextures(1, &tOut);
    glDeleteFramebuffers(1, &fbo);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(dpy, surf);
    eglDestroyContext(dpy, ctx);
    LOGI("extrap-dump: complete (wrote cleanfg_prev/cur/out_s050/out_s100.ppm to %s)", outDir.c_str());
}

}  // namespace

void runExtrapDump(std::vector<uint8_t> prev, std::vector<uint8_t> cur,
                   uint32_t width, uint32_t height, std::string outDir) {
    std::thread(dumpThread, std::move(prev), std::move(cur), width, height, std::move(outDir)).detach();
}

}  // namespace cleanfg
