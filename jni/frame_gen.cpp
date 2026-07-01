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
} g;

static int64_t nowNs(){ timespec ts{}; clock_gettime(CLOCK_MONOTONIC,&ts); return (int64_t)ts.tv_sec*1000000000LL+ts.tv_nsec; }
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
static void saveCommon(GLint& oldProg, GLint& oldTex, GLint& oldFbo, GLint oldVp[4], GLboolean& blend){
    glGetIntegerv(GL_CURRENT_PROGRAM,&oldProg); glGetIntegerv(GL_ACTIVE_TEXTURE,&oldTex); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldFbo); glGetIntegerv(GL_VIEWPORT,oldVp); blend=glIsEnabled(GL_BLEND);
}
static void restoreCommon(GLint oldProg, GLint oldTex, GLint oldFbo, const GLint oldVp[4], GLboolean blend){
    if(blend) glEnable(GL_BLEND); else glDisable(GL_BLEND); glBindFramebuffer(GL_FRAMEBUFFER,oldFbo); glViewport(oldVp[0],oldVp[1],oldVp[2],oldVp[3]); glActiveTexture(oldTex); glUseProgram(oldProg);
}
}

void fgInitGles(int width,int height){
    if(g.ready && g.w==width && g.h==height) return;
    if(g.ready){ glDeleteTextures(2,g.tex); glDeleteFramebuffers(1,&g.fbo); glDeleteProgram(g.program); glDeleteBuffers(1,&g.vbo); glDeleteVertexArrays(1,&g.vao); std::memset(&g,0,sizeof(g)); }
    g.w=width; g.h=height;
    glGenTextures(2,g.tex);
    for(GLuint t: g.tex){ glBindTexture(GL_TEXTURE_2D,t); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE); glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr); }
    glGenFramebuffers(1,&g.fbo); g.program=makeProgram();
    const float verts[] = {-1.f,-1.f, 3.f,-1.f, -1.f,3.f};
    glGenVertexArrays(1,&g.vao); glGenBuffers(1,&g.vbo); glBindVertexArray(g.vao); glBindBuffer(GL_ARRAY_BUFFER,g.vbo); glBufferData(GL_ARRAY_BUFFER,sizeof(verts),verts,GL_STATIC_DRAW); glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,(void*)0); glBindVertexArray(0);
    g.ready = g.program && g.fbo && g.tex[0] && g.tex[1];
    LOGI("fgInitGles %dx%d ready=%d",width,height,g.ready);
}

void fgCaptureCurrentGles(FrameContext& ctx){
    int64_t n=nowNs(); if(ctx.lastPresentNs){ int64_t dt=n-ctx.lastPresentNs; ctx.avgFrameNs = ctx.avgFrameNs? (ctx.avgFrameNs*7+dt)/8 : dt; } ctx.lastPresentNs=n;
    if(!g.ready) return;
    GLint oldProg, oldTex, oldFbo, oldVp[4]; GLboolean blend; saveCommon(oldProg,oldTex,oldFbo,oldVp,blend);
    g.current = 1 - g.current;
    glBindFramebuffer(GL_READ_FRAMEBUFFER,0); glBindTexture(GL_TEXTURE_2D,g.tex[g.current]);
    glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,g.w,g.h);
    restoreCommon(oldProg,oldTex,oldFbo,oldVp,blend);
    ctx.hasHistory = ctx.frameIndex > 0; ctx.frameIndex++;
}

bool fgRenderGeneratedGles(FrameContext& ctx){
    if(!g.ready || !ctx.hasHistory || g_config.multiplier < 2) return false;
    GLint oldProg, oldTex, oldFbo, oldVp[4]; GLboolean blend; saveCommon(oldProg,oldTex,oldFbo,oldVp,blend);
    glDisable(GL_BLEND); glBindFramebuffer(GL_FRAMEBUFFER,0); glViewport(0,0,g.w,g.h); glUseProgram(g.program);
    int prev=1-g.current, cur=g.current;
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,g.tex[prev]); glUniform1i(glGetUniformLocation(g.program,"prevTex"),0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,g.tex[cur]); glUniform1i(glGetUniformLocation(g.program,"curTex"),1);
    glUniform1f(glGetUniformLocation(g.program,"alpha"),0.5f);
    glBindVertexArray(g.vao); glDrawArrays(GL_TRIANGLES,0,3); glBindVertexArray(0); glFlush();
    restoreCommon(oldProg,oldTex,oldFbo,oldVp,blend); return true;
}

bool fgRenderCurrentGles(FrameContext& ctx){
    if(!g.ready) return false;
    GLint oldProg, oldTex, oldFbo, oldVp[4]; GLboolean blend; saveCommon(oldProg,oldTex,oldFbo,oldVp,blend);
    glDisable(GL_BLEND); glBindFramebuffer(GL_FRAMEBUFFER,0); glViewport(0,0,g.w,g.h); glUseProgram(g.program);
    int cur=g.current;
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,g.tex[cur]); glUniform1i(glGetUniformLocation(g.program,"prevTex"),0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,g.tex[cur]); glUniform1i(glGetUniformLocation(g.program,"curTex"),1);
    glUniform1f(glGetUniformLocation(g.program,"alpha"),1.0f);
    glBindVertexArray(g.vao); glDrawArrays(GL_TRIANGLES,0,3); glBindVertexArray(0); glFlush();
    restoreCommon(oldProg,oldTex,oldFbo,oldVp,blend); return true;
}

float fgMeasuredFps(const FrameContext& ctx){ if(ctx.avgFrameNs<=0) return 0.f; return 1000000000.f/(float)ctx.avgFrameNs; }
bool fgOnPresentVulkan(void*, const void*, FrameContext& ctx){ ctx.frameIndex++; return false; }
}
