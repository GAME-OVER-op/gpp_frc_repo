#include "frame_gen.h"
#include "config.h"
#include "log.h"
#include <GLES3/gl3.h>
#include <time.h>
#include <cmath>
#include <cstring>

namespace cleanfg {
namespace {
struct GlState {
    int w=0,h=0;
    GLuint tex[2]{};
    GLuint fbo=0, program=0, vao=0, vbo=0;
    int current=0;
    bool ready=false;
    bool loggedRender=false;
    bool loggedCaptureInfo=false;
    uint64_t captureErrors=0;
} g;

struct SavedGl {
    GLint program=0;
    GLint activeTex=GL_TEXTURE0;
    GLint tex0=0;
    GLint tex1=0;
    GLint drawFbo=0;
    GLint readFbo=0;
    GLint viewport[4]{};
    GLint vao=0;
    GLint arrayBuf=0;
    GLboolean blend=GL_FALSE;
    GLboolean depth=GL_FALSE;
    GLboolean stencil=GL_FALSE;
    GLboolean scissor=GL_FALSE;
    GLboolean cull=GL_FALSE;
    GLboolean dither=GL_FALSE;
    GLboolean colorMask[4]{GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE};
    GLboolean depthMask=GL_TRUE;
};

static int64_t nowNs(){ timespec ts{}; clock_gettime(CLOCK_MONOTONIC,&ts); return (int64_t)ts.tv_sec*1000000000LL+ts.tv_nsec; }

static void drainGlErrors(){ while(glGetError()!=GL_NO_ERROR){} }

static GLuint compile(GLenum type,const char* src){
    GLuint s=glCreateShader(type); glShaderSource(s,1,&src,nullptr); glCompileShader(s);
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok); if(!ok){ char log[512]{}; glGetShaderInfoLog(s,511,nullptr,log); LOGE("shader compile failed: %s",log); glDeleteShader(s); return 0;} return s;
}

static GLuint makeProgram(){
    const char* vs = "#version 300 es\nprecision mediump float;layout(location=0) in vec2 p;out vec2 uv;void main(){uv=(p+1.0)*0.5;gl_Position=vec4(p,0,1);}";
    const char* fs = "#version 300 es\nprecision mediump float;in vec2 uv;out vec4 o;uniform sampler2D prevTex;uniform sampler2D curTex;uniform float alpha;void main(){vec4 a=texture(prevTex,uv);vec4 b=texture(curTex,uv);o=mix(a,b,alpha);}";
    GLuint v=compile(GL_VERTEX_SHADER,vs), f=compile(GL_FRAGMENT_SHADER,fs); if(!v||!f) return 0;
    GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p); glDeleteShader(v); glDeleteShader(f);
    GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok); if(!ok){ char log[512]{}; glGetProgramInfoLog(p,511,nullptr,log); LOGE("program link failed: %s",log); glDeleteProgram(p); return 0;} return p;
}

static void saveGl(SavedGl& s){
    glGetIntegerv(GL_CURRENT_PROGRAM,&s.program);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&s.activeTex);
    glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D,&s.tex0);
    glActiveTexture(GL_TEXTURE1); glGetIntegerv(GL_TEXTURE_BINDING_2D,&s.tex1);
    glActiveTexture(s.activeTex);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&s.drawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&s.readFbo);
    glGetIntegerv(GL_VIEWPORT,s.viewport);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&s.vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&s.arrayBuf);
    s.blend=glIsEnabled(GL_BLEND);
    s.depth=glIsEnabled(GL_DEPTH_TEST);
    s.stencil=glIsEnabled(GL_STENCIL_TEST);
    s.scissor=glIsEnabled(GL_SCISSOR_TEST);
    s.cull=glIsEnabled(GL_CULL_FACE);
    s.dither=glIsEnabled(GL_DITHER);
    glGetBooleanv(GL_COLOR_WRITEMASK,s.colorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK,&s.depthMask);
}

static void restoreGl(const SavedGl& s){
    if(s.blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if(s.depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if(s.stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if(s.scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if(s.cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if(s.dither) glEnable(GL_DITHER); else glDisable(GL_DITHER);
    glColorMask(s.colorMask[0],s.colorMask[1],s.colorMask[2],s.colorMask[3]);
    glDepthMask(s.depthMask);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,s.readFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,s.drawFbo);
    glViewport(s.viewport[0],s.viewport[1],s.viewport[2],s.viewport[3]);
    glBindVertexArray((GLuint)s.vao);
    glBindBuffer(GL_ARRAY_BUFFER,(GLuint)s.arrayBuf);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,(GLuint)s.tex0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,(GLuint)s.tex1);
    glActiveTexture(s.activeTex);
    glUseProgram((GLuint)s.program);
}

static void setupFullscreenDraw(){
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glDepthMask(GL_FALSE);
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    glViewport(0,0,g.w,g.h);
    glUseProgram(g.program);
}

static bool renderBlend(int prev, int cur, float alpha){
    SavedGl s; saveGl(s);
    setupFullscreenDraw();
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,g.tex[prev]); glUniform1i(glGetUniformLocation(g.program,"prevTex"),0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,g.tex[cur]);  glUniform1i(glGetUniformLocation(g.program,"curTex"),1);
    glUniform1f(glGetUniformLocation(g.program,"alpha"),alpha);
    glBindVertexArray(g.vao);
    GLenum before=glGetError(); (void)before;
    glDrawArrays(GL_TRIANGLES,0,3);
    GLenum err=glGetError();
    glBindVertexArray(0);
    glFlush();
    restoreGl(s);
    if(err!=GL_NO_ERROR){ if(g_config.debug) LOGW("gles render err=0x%x",err); return false; }
    if(g_config.debug && !g.loggedRender){ LOGI("gles frame-gen draw active %dx%d",g.w,g.h); g.loggedRender=true; }
    return true;
}
}

void fgInitGles(int width,int height){
    if(g.ready && g.w==width && g.h==height) return;
    if(g.ready){ glDeleteTextures(2,g.tex); glDeleteFramebuffers(1,&g.fbo); glDeleteProgram(g.program); glDeleteBuffers(1,&g.vbo); glDeleteVertexArrays(1,&g.vao); std::memset(&g,0,sizeof(g)); }
    g.w=width; g.h=height;
    SavedGl s; saveGl(s);
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(2,g.tex);
    for(GLuint t: g.tex){ glBindTexture(GL_TEXTURE_2D,t); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE); glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr); }
    glGenFramebuffers(1,&g.fbo); g.program=makeProgram();
    const float verts[] = {-1.f,-1.f, 3.f,-1.f, -1.f,3.f};
    glGenVertexArrays(1,&g.vao); glGenBuffers(1,&g.vbo); glBindVertexArray(g.vao); glBindBuffer(GL_ARRAY_BUFFER,g.vbo); glBufferData(GL_ARRAY_BUFFER,sizeof(verts),verts,GL_STATIC_DRAW); glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,(void*)0); glBindVertexArray(0);
    g.ready = g.program && g.fbo && g.tex[0] && g.tex[1] && glGetError()==GL_NO_ERROR;
    restoreGl(s);
    LOGI("fgInitGles %dx%d ready=%d",width,height,g.ready);
}

void fgCaptureCurrentGles(FrameContext& ctx){
    int64_t n=nowNs(); if(ctx.lastPresentNs){ int64_t dt=n-ctx.lastPresentNs; ctx.avgFrameNs = ctx.avgFrameNs? (ctx.avgFrameNs*7+dt)/8 : dt; } ctx.lastPresentNs=n;
    if(!g.ready) return;
    SavedGl s; saveGl(s);
    g.current = 1 - g.current;

    // Asphalt 8 on Adreno exposes the SurfaceView through a compressed/special
    // default framebuffer. glCopyTexSubImage2D from FBO 0 can return
    // GL_INVALID_OPERATION there. Prefer a GLES3 framebuffer blit into our
    // texture; this also has a chance to resolve MSAA default buffers.
    drainGlErrors();
    glBindFramebuffer(GL_READ_FRAMEBUFFER,0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,g.fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,g.tex[g.current],0);
    GLenum fbStatus=glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    GLenum err=GL_NO_ERROR;
    if(fbStatus==GL_FRAMEBUFFER_COMPLETE){
        glBlitFramebuffer(0,0,g.w,g.h,0,0,g.w,g.h,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        err=glGetError();
    } else {
        err=GL_INVALID_FRAMEBUFFER_OPERATION;
    }

    // Fallback to the older direct copy if blit is not accepted by the driver.
    if(err!=GL_NO_ERROR){
        drainGlErrors();
        glBindFramebuffer(GL_READ_FRAMEBUFFER,0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D,g.tex[g.current]);
        glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,g.w,g.h);
        err=glGetError();
    }
    restoreGl(s);

    if(g_config.debug && !g.loggedCaptureInfo){
        GLint rb=0, db=0, samples=0, red=0, green=0, blue=0, alpha=0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&rb);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&db);
        glGetIntegerv(GL_SAMPLES,&samples);
        glGetIntegerv(GL_RED_BITS,&red); glGetIntegerv(GL_GREEN_BITS,&green); glGetIntegerv(GL_BLUE_BITS,&blue); glGetIntegerv(GL_ALPHA_BITS,&alpha);
        LOGI("gles capture path fbStatus=0x%x err=0x%x restoredReadFbo=%d restoredDrawFbo=%d samples=%d rgbaBits=%d/%d/%d/%d",
             fbStatus,err,rb,db,samples,red,green,blue,alpha);
        g.loggedCaptureInfo=true;
    }
    if(err!=GL_NO_ERROR){
        g.captureErrors++;
        if(g_config.debug && (g.captureErrors<=12 || (g.captureErrors%120)==0)) LOGW("gles capture err=0x%x count=%llu",err,(unsigned long long)g.captureErrors);
        return;
    }
    ctx.hasHistory = ctx.frameIndex > 0; ctx.frameIndex++;
}

bool fgRenderGeneratedGles(FrameContext& ctx){
    if(!g.ready || !ctx.hasHistory || g_config.multiplier < 2) return false;
    int prev=1-g.current, cur=g.current;
    return renderBlend(prev,cur,0.5f);
}

bool fgRenderCurrentGles(FrameContext& ctx){
    (void)ctx;
    if(!g.ready) return false;
    int cur=g.current;
    return renderBlend(cur,cur,1.0f);
}

float fgMeasuredFps(const FrameContext& ctx){ if(ctx.avgFrameNs<=0) return 0.f; return 1000000000.f/(float)ctx.avgFrameNs; }
bool fgOnPresentVulkan(void*, const void*, FrameContext& ctx){ ctx.frameIndex++; return false; }
}
