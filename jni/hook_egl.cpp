#include "frame_gen.h"
#include "display_rate.h"
#include "config.h"
#include "log.h"
#include <dlfcn.h>
#include <EGL/egl.h>
#include <android/native_window.h>
#include <unordered_map>

namespace cleanfg {
extern void* hookFunction(void* target, void* replacement);
using PFN_eglSwapBuffers = EGLBoolean (*)(EGLDisplay, EGLSurface);
using PFN_eglSwapInterval = EGLBoolean (*)(EGLDisplay, EGLint);
using PFN_eglCreateWindowSurface = EGLSurface (*)(EGLDisplay,EGLConfig,EGLNativeWindowType,const EGLint*);
using PFN_eglDestroySurface = EGLBoolean (*)(EGLDisplay,EGLSurface);
static PFN_eglSwapBuffers orig_eglSwapBuffers=nullptr;
static PFN_eglSwapInterval orig_eglSwapInterval=nullptr;
static PFN_eglCreateWindowSurface orig_eglCreateWindowSurface=nullptr;
static PFN_eglDestroySurface orig_eglDestroySurface=nullptr;
static FrameContext g_ctx;
static bool g_inited=false;
static bool g_inside=false;
// Surfaces created via eglCreateWindowSurface. Only these real on-screen
// surfaces are frame-gen candidates; pbuffer/pixmap surfaces are ignored.
static std::unordered_map<EGLSurface,bool> g_winSurfaces;
// Main-surface selection: a process can swap several window surfaces (UI overlay,
// small auxiliary surfaces, etc.). We interpolate only the largest one so we
// don't waste work or corrupt small auxiliary surfaces.
static EGLSurface g_mainSurface=EGL_NO_SURFACE;
static int g_mainArea=0;

static EGLSurface my_eglCreateWindowSurface(EGLDisplay d,EGLConfig c,EGLNativeWindowType win,const EGLint* attr){
    EGLSurface s=orig_eglCreateWindowSurface(d,c,win,attr);
    if(s!=EGL_NO_SURFACE){ g_winSurfaces[s]=true; if(win) rememberWindowForSurface(s,(void*)win); }
    return s;
}
static EGLBoolean my_eglDestroySurface(EGLDisplay d,EGLSurface s){
    g_winSurfaces.erase(s);
    if(s==g_mainSurface){ g_mainSurface=EGL_NO_SURFACE; g_mainArea=0; }
    forgetWindowForSurface(s);
    return orig_eglDestroySurface(d,s);
}
static EGLBoolean my_eglSwapInterval(EGLDisplay d,EGLint interval){
    if(g_config.force_swap_interval_0 && interval>0){ LOGD("eglSwapInterval(%d)->0",interval); interval=0; }
    return orig_eglSwapInterval ? orig_eglSwapInterval(d,interval) : EGL_FALSE;
}
static EGLBoolean my_eglSwapBuffers(EGLDisplay dpy,EGLSurface surface){
    if(g_inside) return orig_eglSwapBuffers(dpy,surface);

    // (A) Main-surface filter: interpolate only the largest on-screen window
    // surface. Ignore non-window surfaces and smaller auxiliary surfaces.
    if(g_winSurfaces.find(surface)==g_winSurfaces.end())
        return orig_eglSwapBuffers(dpy,surface);
    EGLint qw=0,qh=0; eglQuerySurface(dpy,surface,EGL_WIDTH,&qw); eglQuerySurface(dpy,surface,EGL_HEIGHT,&qh);
    int area=qw*qh;
    if(surface!=g_mainSurface){
        // Promote only if clearly larger (>12.5%) than the current main, so
        // transient tiny surfaces never steal the slot.
        if(area > g_mainArea + (g_mainArea>>3)){
            g_mainSurface=surface; g_mainArea=area;
            LOGI("gles: main surface -> %p (%dx%d)",(void*)surface,qw,qh);
        }else{
            return orig_eglSwapBuffers(dpy,surface);
        }
    }else if(area>g_mainArea){
        g_mainArea=area;
    }
    // Main surface changed size (rotation/resize) -> rebuild the frame-gen context.
    if(g_inited && (qw!=g_ctx.width || qh!=g_ctx.height)){
        LOGI("gles: main surface resized %dx%d -> %dx%d",g_ctx.width,g_ctx.height,qw,qh);
        g_ctx.width=qw; g_ctx.height=qh; fgInitGles(qw,qh);
    }

    // (B) Auto-detect with warmup: let the Vulkan path (preferred) claim first.
    // Only after warmup_frames real swaps of the main surface does GLES claim,
    // and only if Vulkan hasn't taken the engine in the meantime.
    if(g_config.mode==Mode::Auto){
        int eng=g_activeEngine.load();
        if(eng==2) return orig_eglSwapBuffers(dpy,surface);   // Vulkan won
        if(eng==0){
            static int warm=0;
            if(warm<g_config.warmup_frames){ ++warm; return orig_eglSwapBuffers(dpy,surface); }
            int expected=0; g_activeEngine.compare_exchange_strong(expected,1);
            if(g_activeEngine.load()!=1) return orig_eglSwapBuffers(dpy,surface); // Vulkan grabbed it
            LOGI("auto: GLES engine claimed after warmup (%d frames)",warm);
        }
    }

    g_inside=true;
    if(!g_inited){ g_ctx.width=qw; g_ctx.height=qh; fgInitGles(qw,qh); g_inited=true; }
    fgCaptureCurrentGles(g_ctx);
    float fps=fgMeasuredFps(g_ctx); if(fps>1.f) requestBestFrameRate(surface,fps);
    if(g_config.multiplier>=2 && fgRenderGeneratedGles(g_ctx)) orig_eglSwapBuffers(dpy,surface);
    EGLBoolean r=orig_eglSwapBuffers(dpy,surface);
    g_inside=false; return r;
}

bool installEglHook(){
    void* libegl=dlopen("libEGL.so",RTLD_NOW|RTLD_GLOBAL); if(!libegl){LOGE("dlopen libEGL.so failed: %s",dlerror()); return false;}
    void* swap=dlsym(libegl,"eglSwapBuffers"); if(!swap){LOGE("dlsym eglSwapBuffers failed"); return false;}
    void* tramp=hookFunction(swap,(void*)my_eglSwapBuffers); if(!tramp){LOGE("hook eglSwapBuffers failed"); return false;} orig_eglSwapBuffers=(PFN_eglSwapBuffers)tramp;
    if(void* p=dlsym(libegl,"eglCreateWindowSurface")){ if(void* t=hookFunction(p,(void*)my_eglCreateWindowSurface)) orig_eglCreateWindowSurface=(PFN_eglCreateWindowSurface)t; }
    if(void* p=dlsym(libegl,"eglDestroySurface")){ if(void* t=hookFunction(p,(void*)my_eglDestroySurface)) orig_eglDestroySurface=(PFN_eglDestroySurface)t; }
    if(void* p=dlsym(libegl,"eglSwapInterval")){ if(void* t=hookFunction(p,(void*)my_eglSwapInterval)) orig_eglSwapInterval=(PFN_eglSwapInterval)t; }
    LOGI("EGL hooks installed"); return true;
}
}
