#pragma once
// GPPSession client for the vendor frame-generation engine
// (libgppvppgfrcplussession.so -> vppservice). Ported from the proven
// gpp_frc Phase 15 harness, with ALL network code removed and without
// installing global signal handlers (unsafe inside the game process).
//
// Pipeline (all symbols resolved via dlsym; libgui is not linked):
//   CreateFactory -> BufferQueue::createBufferQueue (output) ->
//   BufferItemConsumer(+incStrong) -> GPPSession::connect(pkg,layer,out,&in) ->
//   GPPProducer::connect(API_CPU) -> per-frame dequeue/request/lock/copy/queue,
//   with a background drain thread on the output consumer.
#include <cstdint>
#include <string>

namespace cleanfg {

class GppEngine {
public:
    // dlopen the engine library and resolve all symbols. Idempotent.
    bool init();
    // Bind a game package+layer to the engine. w/h are the engine input frame
    // dimensions (the size of frames we will submit). Starts the drain thread.
    bool connect(const std::string& pkg, const std::string& layer, int w, int h);
    // Submit one source frame in RGBA8888 (strideBytes = row stride of src).
    // Internally converts to NV12-Venus and runs one dequeue->lock->copy->queue.
    // Safe to call from the present hook; returns false on fatal pipeline error.
    bool submitFrameRGBA(const void* rgba, int w, int h, int strideBytes);
    void stop();

    bool ready()        const { return ready_; }
    bool connected()    const { return connected_; }
    int  framesPosted() const { return posted_; }
    int  framesDrained() const;

private:
    bool resolveSymbols();
    void* lib_ = nullptr;
    bool  ready_ = false;
    bool  connected_ = false;
    bool  useCancel_ = false;
    int   posted_ = 0;
    int   deqFail_ = 0;
    int   w_ = 0, h_ = 0;

    void* session_   = nullptr;   // GPPSession object (calloc'd)
    void* inProducer_ = nullptr;  // sp<IGraphicBufferProducer>.p (GPPProducer)
    void* bic_       = nullptr;   // BufferItemConsumer object (calloc'd)
    void* noFence_   = nullptr;   // sp<Fence> Fence::NO_FENCE value
    void* gbslot_[64] = {};       // cached GraphicBuffer per slot
};

extern GppEngine g_engine;

}  // namespace cleanfg
