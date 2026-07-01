#include "frame_gen.h"
#include "display_rate.h"
#include "config.h"
#include "backend_select.h"
#include "log.h"
#include <dlfcn.h>
#include <EGL/egl.h>
#include <android/native_window.h>

namespace gpp_frc_repo {
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
static bool g_loggedMode=false;
static bool g_forcedSwap0=false;
static uint64_t g_swapCalls=0;
static uint64_t g_generatedPresents=0;

static EGLSurface my_eglCreateWindowSurface(EGLDisplay d,EGLConfig c,EGLNativeWindowType win,const EGLint* attr){
    EGLSurface s=orig_eglCreateWindowSurface(d,c,win,attr);
    if(s!=EGL_NO_SURFACE && win) rememberWindowForSurface(s,(void*)win);
    return s;
}
static EGLBoolean my_eglDestroySurface(EGLDisplay d,EGLSurface s){ forgetWindowForSurface(s); return orig_eglDestroySurface(d,s); }
static EGLBoolean my_eglSwapInterval(EGLDisplay d,EGLint interval){
    if(g_config.force_swap_interval_0 && interval>0){ LOGD("eglSwapInterval(%d)->0",interval); interval=0; }
    return orig_eglSwapInterval ? orig_eglSwapInterval(d,interval) : EGL_FALSE;
}
static EGLBoolean my_eglSwapBuffers(EGLDisplay dpy,EGLSurface surface){
    if(g_inside) return orig_eglSwapBuffers(dpy,surface);

    if(!tryActivateBackend(RuntimeBackend::Gles,"eglSwapBuffers")){
        return orig_eglSwapBuffers(dpy,surface);
    }

    g_inside=true;
    g_swapCalls++;
    EGLint w=0,h=0; eglQuerySurface(dpy,surface,EGL_WIDTH,&w); eglQuerySurface(dpy,surface,EGL_HEIGHT,&h);
    if(!g_inited || g_ctx.width!=w || g_ctx.height!=h){
        g_ctx = FrameContext{};
        g_ctx.width=w; g_ctx.height=h;
        fgInitGles(w,h);
        g_inited=true;
        g_loggedMode=false;
    }
    if(g_config.force_swap_interval_0 && orig_eglSwapInterval && !g_forcedSwap0){
        EGLBoolean sr=orig_eglSwapInterval(dpy,0);
        if(g_config.debug) LOGI("gles forced eglSwapInterval(0) in swap path result=%d", (int)sr);
        g_forcedSwap0=true;
    }
    if(g_config.debug && !g_loggedMode){ LOGI("gles debug mode active: %d", g_config.gles_debug_mode); g_loggedMode=true; }
    if(g_config.gles_debug_mode==1){
        float fps=fgMeasuredFps(g_ctx); if(fps>1.f) requestBestFrameRate(surface,fps);
        EGLBoolean r=orig_eglSwapBuffers(dpy,surface);
        g_inside=false; return r;
    }
    fgCaptureCurrentGles(g_ctx);
    float fps=fgMeasuredFps(g_ctx); if(fps>1.f) requestBestFrameRate(surface,fps);
    if(g_config.gles_debug_mode==2){
        EGLBoolean r=orig_eglSwapBuffers(dpy,surface);
        g_inside=false; return r;
    }
    if(g_config.gles_debug_mode==3){
        fgRenderCurrentGles(g_ctx);
    } else if(g_config.gles_debug_mode==4){
        orig_eglSwapBuffers(dpy,surface);
    } else if(g_config.gles_debug_mode==5){
        fgRenderCurrentGles(g_ctx);
    } else if(g_config.multiplier>=2 && fgRenderGeneratedGles(g_ctx)){
        orig_eglSwapBuffers(dpy,surface);
        g_generatedPresents++;
        fgRenderCurrentGles(g_ctx);
    }
    if(g_config.debug && (g_swapCalls==1 || (g_swapCalls % 120)==0)){
        LOGI("gles stats mode=%d appSwaps=%llu generatedPresents=%llu measuredFps=%.1f",
             g_config.gles_debug_mode,
             (unsigned long long)g_swapCalls,
             (unsigned long long)g_generatedPresents,
             fgMeasuredFps(g_ctx));
    }
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
