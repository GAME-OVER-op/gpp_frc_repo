#include "gpp_engine.h"
#include "log.h"
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <android/dlext.h>

namespace cleanfg {

// ---- sp<T> opaque wrapper (matches Phase 15 ABI: passed by pointer) ----
namespace {
struct Sp { void* p = nullptr; };

// ---- exact mangled symbol names (engine + libgui), from Phase 15 ----
const char* SYM_CREATE_FACTORY = "CreateFactory";
const char* SYM_SESS_CTOR      = "_ZN7android10GPPSessionC1Ev";
const char* SYM_SESS_CONNECT   = "_ZN7android10GPPSession7connectERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_RKNS_2spINS_22IGraphicBufferProducerEEEPSC_";
const char* SYM_VT_SESSION     = "_ZTVN7android10GPPSessionE";
const char* SYM_VT_PRODUCER    = "_ZTVN7android11GPPProducerE";
const char* SYM_CREATE_BQ      = "_ZN7android11BufferQueue17createBufferQueueEPNS_2spINS_22IGraphicBufferProducerEEEPNS1_INS_22IGraphicBufferConsumerEEEb";
const char* SYM_BIC_CTOR       = "_ZN7android18BufferItemConsumerC1ERKNS_2spINS_22IGraphicBufferConsumerEEEmib";
const char* SYM_BITEM_CTOR     = "_ZN7android10BufferItemC1Ev";
const char* SYM_BITEM_DTOR     = "_ZN7android10BufferItemD1Ev";
const char* SYM_BIC_ACQUIRE    = "_ZN7android18BufferItemConsumer13acquireBufferEPNS_10BufferItemElb";
const char* SYM_BIC_RELEASE    = "_ZN7android18BufferItemConsumer13releaseBufferERKNS_10BufferItemERKNS_2spINS_5FenceEEE";
const char* SYM_CB_SETSIZE     = "_ZN7android12ConsumerBase20setDefaultBufferSizeEjj";
const char* SYM_CB_SETFMT      = "_ZN7android12ConsumerBase22setDefaultBufferFormatEi";
const char* SYM_GPPP_CONNECT   = "_ZN7android11GPPProducer7connectERKNS_2spINS_17IProducerListenerEEEibPNS_22IGraphicBufferProducer17QueueBufferOutputE";
const char* SYM_GPPP_DEQUEUE   = "_ZN7android11GPPProducer13dequeueBufferEPiPNS_2spINS_5FenceEEEjjimPmPNS_22FrameEventHistoryDeltaE";
const char* SYM_GPPP_REQUEST   = "_ZN7android11GPPProducer13requestBufferEiPNS_2spINS_13GraphicBufferEEE";
const char* SYM_GPPP_QUEUE     = "_ZN7android11GPPProducer11queueBufferEiRKNS_22IGraphicBufferProducer16QueueBufferInputEPNS1_17QueueBufferOutputE";
const char* SYM_GPPP_CANCEL    = "_ZN7android11GPPProducer12cancelBufferEiRKNS_2spINS_5FenceEEE";
const char* SYM_GPPP_SETMAXDEQ = "_ZN7android11GPPProducer25setMaxDequeuedBufferCountEi";
const char* SYM_GPPP_SETTIMEOUT= "_ZN7android11GPPProducer17setDequeueTimeoutEl";
const char* SYM_GPPP_SETASYNC  = "_ZN7android11GPPProducer12setAsyncModeEb";
const char* SYM_GPPP_ALLOWALLOC= "_ZN7android11GPPProducer15allowAllocationEb";
const char* SYM_GB_LOCK        = "_ZN7android13GraphicBuffer4lockEjRKNS_4RectEPPvPiS6_";
const char* SYM_GB_UNLOCK      = "_ZN7android13GraphicBuffer6unlockEv";
const char* SYM_REGION_CTOR    = "_ZN7android6RegionC1Ev";
const char* SYM_REGION_DTOR    = "_ZN7android6RegionD1Ev";
const char* SYM_FENCE_NOF      = "_ZN7android5Fence8NO_FENCEE";
const char* SYM_INCSTRONG      = "_ZNK7android7RefBase9incStrongEPKv";

// ---- FFI signatures (from Phase 15) ----
using FnFactory    = void* (*)();
using FnSessCtor   = void  (*)(void*);
using FnConnect    = long  (*)(void*, const std::string*, const std::string*, const Sp*, Sp*);
using FnCreateBQ   = void  (*)(Sp*, Sp*, bool);
using FnBicCtor    = void  (*)(void*, const Sp*, uint64_t, int, bool);
using FnCBSize     = int   (*)(void*, uint32_t, uint32_t);
using FnCBFmt      = int   (*)(void*, int);
using FnGppConnect = int   (*)(void*, const Sp*, int, bool, void*);
using FnGppDequeue = int   (*)(void*, int*, Sp*, uint32_t, uint32_t, int, uint64_t, uint64_t*, void*);
using FnGppRequest = int   (*)(void*, int, Sp*);
using FnGppQueue   = int   (*)(void*, int, const void*, void*);
using FnGppCancel  = int   (*)(void*, int, const Sp*);
using FnSetInt     = int   (*)(void*, int);
using FnSetLong    = int   (*)(void*, long);
using FnSetBool    = int   (*)(void*, bool);
using FnGbLock     = int   (*)(void*, uint32_t, const void*, void**, int*, int*);
using FnGbUnlock   = int   (*)(void*);
using FnRegionCtor = void  (*)(void*);
using FnIncStrong  = void  (*)(const void*, const void*);
using FnBItemCtor  = void  (*)(void*);
using FnBItemDtor  = void  (*)(void*);
using FnAcquire    = int   (*)(void*, void*, long, bool);
using FnRelease    = int   (*)(void*, const void*, const Sp*);

// NV12-Venus input format/usage (frc-mc needs Venus extradata region).
const int      IN_FMT   = 0x7FA30C04;  // HAL_PIXEL_FORMAT_YCbCr_420_SP_VENUS
const uint64_t IN_USAGE = 0x10033;     // SW_RD|SW_WR | HW_VIDEO_ENCODER
inline int alignTo(int v, int a) { return (v + a - 1) & ~(a - 1); }

// resolved symbols
struct Syms {
    FnFactory    factory = nullptr;
    FnSessCtor   sessCtor = nullptr;
    FnConnect    sessConnect = nullptr;
    FnCreateBQ   createBQ = nullptr;
    FnBicCtor    bicCtor = nullptr;
    FnCBSize     cbSetSize = nullptr;
    FnCBFmt      cbSetFmt = nullptr;
    FnGppConnect gppConnect = nullptr;
    FnGppDequeue gppDequeue = nullptr;
    FnGppRequest gppRequest = nullptr;
    FnGppQueue   gppQueue = nullptr;
    FnGppCancel  gppCancel = nullptr;
    FnSetInt     setMaxDeq = nullptr;
    FnSetLong    setTimeout = nullptr;
    FnSetBool    setAsync = nullptr;
    FnSetBool    allowAlloc = nullptr;
    FnGbLock     gbLock = nullptr;
    FnGbUnlock   gbUnlock = nullptr;
    FnRegionCtor regionCtor = nullptr;
    FnRegionCtor regionDtor = nullptr;
    FnIncStrong  incStrong = nullptr;
    FnBItemCtor  bItemCtor = nullptr;
    FnBItemDtor  bItemDtor = nullptr;
    FnAcquire    acquire = nullptr;
    FnRelease    release = nullptr;
    void* vtProducer = nullptr;
} S;

void* g_lib = nullptr;
template <class T> T sym(const char* n) {
    void* p = dlsym(g_lib, n);
    if (!p) p = dlsym(RTLD_DEFAULT, n);
    if (!p) LOGW("dlsym miss: %.60s", n);
    return reinterpret_cast<T>(p);
}

const char* LIB_CANDIDATES[] = {
    "libgppvppgfrcplussession.so",
    "/system/lib64/libgppvppgfrcplussession.so",
    "/system_ext/lib64/libgppvppgfrcplussession.so",
    "/vendor/lib64/libgppvppgfrcplussession.so",
    "/odm/lib64/libgppvppgfrcplussession.so",
    "/data/adb/modules/gamespace/system/lib64/libgppvppgfrcplussession.so",
};
const char* NS_CANDIDATES[] = { "sphal", "vndk", "vndk_product", "default", "system" };
typedef struct android_namespace_t* (*FnGetNs)(const char*);

// ---- custom linker namespace (cross-partition load) ----------------------
// The engine lib lives in /system/lib64 but DT_NEEDED-depends on a /vendor HAL
// lib (vendor.qti.hardware.vpp-V1-ndk.so). No stock namespace can see BOTH
// partitions: 'default'/'system' can open the /system lib but are forbidden
// from resolving the vendor dependency, while 'sphal' can resolve vendor libs
// but refuses the /system path. Solution: build our own SHARED, non-isolated
// namespace whose search path spans both partitions, and link it to the
// platform namespaces so transitive VNDK-SP / SP-HAL deps resolve with their
// proper allowlists.
constexpr uint64_t NS_TYPE_REGULAR  = 0;
constexpr uint64_t NS_TYPE_ISOLATED = 1;
constexpr uint64_t NS_TYPE_SHARED   = 2;
typedef struct android_namespace_t* (*FnCreateNs)(
    const char* name, const char* ld_library_path,
    const char* default_library_path, uint64_t type,
    const char* permitted_when_isolated_path, struct android_namespace_t* parent);
typedef bool (*FnLinkNs)(struct android_namespace_t* from,
                         struct android_namespace_t* to,
                         const char* shared_libs_sonames);

// Search path covering every partition the engine + its deps may live in.
const char* GPP_NS_PATHS =
    "/system/lib64:/system_ext/lib64:/vendor/lib64:/vendor/lib64/hw:"
    "/odm/lib64:/odm/lib64/hw:/vendor/lib64/egl:/data/adb/modules/gamespace/system/lib64";
// Vendor/HAL sonames our custom ns is allowed to pull from the sphal namespace.
const char* GPP_SPHAL_ALLOW =
    "vendor.qti.hardware.vpp-V1-ndk.so:vendor.qti.hardware.vpp-V1-ndk_platform.so:"
    "vendor.qti.hardware.vpp-V1.0-ndk.so:libvppclient.so:libvpphcp.so:libvpp.so:"
    "libqdMetaData.so:libgralloctypes.so:android.hardware.graphics.mapper@4.0.so";
// Core/VNDK sonames shared down from the default & vndk namespaces.
const char* GPP_CORE_ALLOW =
    "libc.so:libdl.so:libm.so:liblog.so:libc++.so:libstdc++.so:libz.so:"
    "libutils.so:libcutils.so:libbase.so:libbinder.so:libbinder_ndk.so:"
    "libui.so:libgui.so:libnativewindow.so:libsync.so:libhardware.so:"
    "libhidlbase.so:libvndksupport.so";

// Build (once) a permissive namespace that can see system + vendor at once.
struct android_namespace_t* createGppNamespace(bool first) {
    static struct android_namespace_t* cached = nullptr;
    static bool tried = false;
    if (tried) return cached;
    tried = true;

    FnCreateNs createNs = (FnCreateNs)dlsym(RTLD_DEFAULT, "android_create_namespace");
    FnLinkNs   linkNs   = (FnLinkNs)dlsym(RTLD_DEFAULT, "android_link_namespaces");
    FnGetNs    getNs    = (FnGetNs)dlsym(RTLD_DEFAULT, "android_get_exported_namespace");
    if (!createNs) { if (first) LOGW("gpp: android_create_namespace unavailable"); return nullptr; }

    // SHARED so already-loaded core libs (libc/libdl/...) from the app namespace
    // are reused instead of double-loaded. parent=nullptr -> caller's namespace.
    cached = createNs("cleanfg_gpp", GPP_NS_PATHS, GPP_NS_PATHS,
                      NS_TYPE_SHARED, nullptr, nullptr);
    if (!cached) { if (first) LOGW("gpp: android_create_namespace failed"); return nullptr; }

    if (linkNs) {
        struct android_namespace_t* sphal = getNs ? getNs("sphal") : nullptr;
        struct android_namespace_t* def   = getNs ? getNs("default") : nullptr;
        struct android_namespace_t* vndk  = getNs ? getNs("vndk") : nullptr;
        if (sphal && !linkNs(cached, sphal, GPP_SPHAL_ALLOW) && first)
            LOGW("gpp: link cleanfg_gpp->sphal failed");
        if (def)  linkNs(cached, def,  GPP_CORE_ALLOW);
        if (vndk) linkNs(cached, vndk, GPP_CORE_ALLOW);
    }
    LOGI("gpp: custom namespace 'cleanfg_gpp' created (%p)", (void*)cached);
    return cached;
}

// Try to load the engine lib through the custom cross-partition namespace.
void* loadEngineViaCustomNs(bool first) {
    struct android_namespace_t* ns = createGppNamespace(first);
    if (!ns) return nullptr;
    android_dlextinfo info; memset(&info, 0, sizeof(info));
    info.flags = ANDROID_DLEXT_USE_NAMESPACE;
    info.library_namespace = ns;

    // Pre-load the vendor dependency first for a precise failure point. Once it
    // is resolved inside this namespace, the engine's DT_NEEDED is satisfied by
    // soname without re-searching the (forbidden-for-vendor) /system paths.
    const char* deps[] = {
        "vendor.qti.hardware.vpp-V1-ndk.so",
        "vendor.qti.hardware.vpp-V1-ndk_platform.so",
        "vendor.qti.hardware.vpp-V1.0-ndk.so",
    };
    for (const char* d : deps) {
        void* dep = android_dlopen_ext(d, RTLD_NOW | RTLD_GLOBAL, &info);
        if (dep) { LOGI("gpp: preloaded vendor dep %s", d); break; }
        if (first) { const char* e = dlerror(); LOGW("gpp: dep '%s': %.120s", d, e ? e : "?"); }
    }

    for (const char* c : LIB_CANDIDATES) {
        if (c[0] != '/') continue;  // namespace load needs an absolute path
        void* h = android_dlopen_ext(c, RTLD_NOW | RTLD_GLOBAL, &info);
        if (h) { LOGI("gpp: dlopen_ext ok via custom ns: %s", c); return h; }
        if (first) { const char* e = dlerror(); LOGW("gpp: custom ns '%s': %.120s", c, e ? e : "?"); }
    }
    return nullptr;
}

// Find RefBase subobject inside a virtually-derived object (Phase 15 logic).
void* findRefBase(void* obj) {
    for (int O = 0; O <= 512; O += 8) {
        char* rb = (char*)obj + O;
        void* mRefs = *(void**)(rb + 8);
        uintptr_t m = (uintptr_t)mRefs;
        if (m < 0x10000 || (m & 0x7)) continue;
        for (int mb = 8; mb <= 16; mb += 8) {
            void* mBase = *(void**)((char*)mRefs + mb);
            if (mBase == (void*)rb) return (void*)rb;
        }
    }
    return nullptr;
}

// ---- output drain thread (acquire+release so the engine can emit frames) ----
void*    d_bic = nullptr;
FnAcquire d_acq = nullptr; FnRelease d_rel = nullptr;
FnBItemCtor d_ctor = nullptr; FnBItemDtor d_dtor = nullptr;
void*    d_noFence = nullptr;
volatile int d_run = 0; volatile int d_count = 0;
pthread_t d_tid = 0; bool d_started = false;

void* drainThread(void*) {
    while (d_run) {
        char item[512]; memset(item, 0, sizeof(item));
        if (d_ctor) d_ctor(item);
        int rc = -1;
        if (d_acq && d_bic) rc = d_acq(d_bic, item, 0L, false);
        if (rc == 0) {
            Sp relFence; relFence.p = d_noFence;
            if (d_rel) d_rel(d_bic, item, &relFence);
            ++d_count;
        }
        if (d_dtor) d_dtor(item);
        usleep(2000);
    }
    return nullptr;
}
}  // anonymous namespace

GppEngine g_engine;
int GppEngine::framesDrained() const { return d_count; }

bool GppEngine::resolveSymbols() {
    S.factory    = sym<FnFactory>(SYM_CREATE_FACTORY);
    S.sessCtor   = sym<FnSessCtor>(SYM_SESS_CTOR);
    S.sessConnect= sym<FnConnect>(SYM_SESS_CONNECT);
    S.createBQ   = sym<FnCreateBQ>(SYM_CREATE_BQ);
    S.bicCtor    = sym<FnBicCtor>(SYM_BIC_CTOR);
    S.cbSetSize  = sym<FnCBSize>(SYM_CB_SETSIZE);
    S.cbSetFmt   = sym<FnCBFmt>(SYM_CB_SETFMT);
    S.gppConnect = sym<FnGppConnect>(SYM_GPPP_CONNECT);
    S.gppDequeue = sym<FnGppDequeue>(SYM_GPPP_DEQUEUE);
    S.gppRequest = sym<FnGppRequest>(SYM_GPPP_REQUEST);
    S.gppQueue   = sym<FnGppQueue>(SYM_GPPP_QUEUE);
    S.gppCancel  = sym<FnGppCancel>(SYM_GPPP_CANCEL);
    S.setMaxDeq  = sym<FnSetInt>(SYM_GPPP_SETMAXDEQ);
    S.setTimeout = sym<FnSetLong>(SYM_GPPP_SETTIMEOUT);
    S.setAsync   = sym<FnSetBool>(SYM_GPPP_SETASYNC);
    S.allowAlloc = sym<FnSetBool>(SYM_GPPP_ALLOWALLOC);
    S.gbLock     = sym<FnGbLock>(SYM_GB_LOCK);
    S.gbUnlock   = sym<FnGbUnlock>(SYM_GB_UNLOCK);
    S.regionCtor = sym<FnRegionCtor>(SYM_REGION_CTOR);
    S.regionDtor = sym<FnRegionCtor>(SYM_REGION_DTOR);
    S.incStrong  = sym<FnIncStrong>(SYM_INCSTRONG);
    S.bItemCtor  = sym<FnBItemCtor>(SYM_BITEM_CTOR);
    S.bItemDtor  = sym<FnBItemDtor>(SYM_BITEM_DTOR);
    S.acquire    = sym<FnAcquire>(SYM_BIC_ACQUIRE);
    S.release    = sym<FnRelease>(SYM_BIC_RELEASE);
    S.vtProducer = dlsym(g_lib, SYM_VT_PRODUCER);
    void* nf = dlsym(g_lib, SYM_FENCE_NOF);
    if (!nf) nf = dlsym(RTLD_DEFAULT, SYM_FENCE_NOF);
    noFence_ = nf ? *(void**)nf : nullptr;
    return S.sessCtor && S.sessConnect && S.createBQ && S.gppDequeue && S.gppQueue;
}

bool GppEngine::init() {
    if (ready_) return true;
    static int attempts = 0;
    static bool gaveUp = false;
    if (gaveUp) return false;
    ++attempts;
    const bool first = (attempts == 1);

    // 0) PRIMARY: custom cross-partition namespace. The engine lib is in
    //    /system/lib64 but needs a /vendor HAL lib; only a namespace that can
    //    see both partitions at once can satisfy that DT_NEEDED.
    g_lib = loadEngineViaCustomNs(first);
    if (g_lib) { lib_ = g_lib; }

    // 1) plain dlopen in the app default namespace
    if (!g_lib)
    for (const char* c : LIB_CANDIDATES) {
        g_lib = dlopen(c, RTLD_NOW | RTLD_GLOBAL);
        if (g_lib) { LOGI("gpp: dlopen ok: %s", c); break; }
        if (first) { const char* e = dlerror(); LOGW("gpp: dlopen('%s'): %.140s", c, e ? e : "?"); }
    }

    // 2) android_dlopen_ext via exported linker namespaces (vendor/system HAL libs
    //    are blocked from the app default namespace; sphal/vndk can reach them).
    if (!g_lib) {
        FnGetNs getNs = (FnGetNs)dlsym(RTLD_DEFAULT, "android_get_exported_namespace");
        if (!getNs && first) LOGW("gpp: android_get_exported_namespace unavailable");
        if (getNs) {
            for (const char* nsn : NS_CANDIDATES) {
                struct android_namespace_t* ns = getNs(nsn);
                if (!ns) { if (first) LOGW("gpp: ns '%s' missing", nsn); continue; }
                android_dlextinfo info;
                memset(&info, 0, sizeof(info));
                info.flags = ANDROID_DLEXT_USE_NAMESPACE;
                info.library_namespace = ns;
                for (const char* c : LIB_CANDIDATES) {
                    if (c[0] != '/') continue;  // namespace load needs absolute path
                    g_lib = android_dlopen_ext(c, RTLD_NOW | RTLD_GLOBAL, &info);
                    if (g_lib) { LOGI("gpp: dlopen_ext ok ns=%s: %s", nsn, c); break; }
                    if (first) { const char* e = dlerror(); LOGW("gpp: dlopen_ext ns=%s '%s': %.120s", nsn, c, e ? e : "?"); }
                }
                if (g_lib) break;
            }
        }
    }

    if (!g_lib) {
        if (first) LOGE("gpp: engine lib not found (paths+namespaces exhausted)");
        if (attempts >= 4) { gaveUp = true; LOGE("gpp: giving up on engine lib load"); }
        return false;
    }
    dlopen("libgui.so", RTLD_NOW | RTLD_GLOBAL);
    dlopen("libui.so",  RTLD_NOW | RTLD_GLOBAL);
    if (!resolveSymbols()) { LOGE("gpp: key symbols unresolved"); gaveUp = true; return false; }
    ready_ = true;
    LOGI("gpp: engine ready");
    return true;
}

bool GppEngine::connect(const std::string& pkg, const std::string& layer, int w, int h) {
    if (!ready_ && !init()) return false;
    if (connected_) return true;
    w_ = w; h_ = h;

    if (S.factory) { void* f = S.factory(); LOGI("gpp: CreateFactory=%p", f); }

    // --- output producer via BufferQueue::createBufferQueue ---
    Sp outGbp, outCons;
    S.createBQ(&outGbp, &outCons, false);
    LOGI("gpp: createBufferQueue prod=%p cons=%p", outGbp.p, outCons.p);
    if (!outGbp.p) { LOGE("gpp: no output producer"); return false; }
    if (outCons.p && S.bicCtor) {
        bic_ = calloc(1, 16384);
        S.bicCtor(bic_, &outCons, 0, 8, false);
        if (S.incStrong) {
            void* rb = findRefBase(bic_);
            if (rb) S.incStrong(rb, rb);
            LOGI("gpp: BIC=%p refbase=%p", bic_, rb);
        }
        if (S.cbSetSize) S.cbSetSize(bic_, (uint32_t)w_, (uint32_t)h_);
        if (S.cbSetFmt)  S.cbSetFmt(bic_, IN_FMT);
    }

    // --- GPPSession + connect ---
    session_ = calloc(1, 32768);
    S.sessCtor(session_);
    Sp inGbp;
    std::string p = pkg, l = layer;
    long rc = S.sessConnect(session_, &p, &l, &outGbp, &inGbp);
    LOGI("gpp: GPPSession::connect rc=%ld inProducer=%p", rc, inGbp.p);
    if (!inGbp.p) { LOGE("gpp: connect returned no input producer"); return false; }
    inProducer_ = inGbp.p;

    // --- GPPProducer::connect(API_CPU) ---
    if (S.gppConnect) {
        char qbo[256]; memset(qbo, 0, sizeof(qbo));
        Sp nullListener;
        int cr = S.gppConnect(inProducer_, &nullListener, 2 /*NATIVE_WINDOW_API_CPU*/, true, qbo);
        LOGI("gpp: GPPProducer::connect rc=%d", cr);
    }
    if (S.allowAlloc) S.allowAlloc(inProducer_, true);
    if (S.setAsync)   S.setAsync(inProducer_, true);
    if (S.setMaxDeq)  S.setMaxDeq(inProducer_, 6);
    if (S.setTimeout) S.setTimeout(inProducer_, 50000000L);

    // --- start output drain ---
    d_bic = bic_; d_acq = S.acquire; d_rel = S.release;
    d_ctor = S.bItemCtor; d_dtor = S.bItemDtor; d_noFence = noFence_;
    if (d_acq && d_rel && d_ctor && d_dtor && d_bic) {
        d_run = 1;
        if (pthread_create(&d_tid, nullptr, drainThread, nullptr) == 0) d_started = true;
        else d_run = 0;
    }
    LOGI("gpp: connected (drain=%d) %dx%d pkg=%s", d_started, w_, h_, pkg.c_str());
    connected_ = true;
    return true;
}

bool GppEngine::submitFrameRGBA(const void* rgba, int w, int h, int strideBytes) {
    if (!connected_ || !S.gppDequeue) return false;
    int W = w_, H = h_;
    int slot = -1; Sp fence; uint64_t age = 0;
    int rc = S.gppDequeue(inProducer_, &slot, &fence, (uint32_t)W, (uint32_t)H, IN_FMT, IN_USAGE, &age, nullptr);
    if (rc < 0 || slot < 0 || slot >= 64) {
        if (++deqFail_ >= 5) { LOGW("gpp: 5 dequeue fails, pausing"); deqFail_ = 0; }
        return false;
    }
    deqFail_ = 0;
    if ((rc & 1) || !gbslot_[slot]) {
        if (S.gppRequest) { Sp gb; S.gppRequest(inProducer_, slot, &gb); gbslot_[slot] = gb.p; }
    }
    void* gb = gbslot_[slot];
    if (!gb) return false;

    // lock the NV12 buffer and convert RGBA->NV12 (BT.601 limited)
    if (S.gbLock) {
        int lockRect[4] = { 0, 0, W, H };
        void* vaddr = nullptr; int obpp = 0, obps = 0;
        int lr = S.gbLock(gb, (uint32_t)IN_USAGE, lockRect, &vaddr, &obpp, &obps);
        if (lr == 0 && vaddr) {
            int yStride = (obps > 0) ? obps : W;
            int scan    = alignTo(H, 32);
            uint8_t* dstY  = (uint8_t*)vaddr;
            uint8_t* dstUV = dstY + (size_t)yStride * scan;
            const uint8_t* src = (const uint8_t*)rgba;
            int cw = (w < W) ? w : W;
            int ch = (h < H) ? h : H;
            for (int y = 0; y < ch; ++y) {
                const uint8_t* sp = src + (size_t)y * strideBytes;
                uint8_t* yr = dstY + (size_t)y * yStride;
                for (int x = 0; x < cw; ++x) {
                    int r = sp[x*4+0], g = sp[x*4+1], b = sp[x*4+2];
                    yr[x] = (uint8_t)(((66*r + 129*g + 25*b + 128) >> 8) + 16);
                }
            }
            for (int y = 0; y < ch; y += 2) {
                const uint8_t* sp = src + (size_t)y * strideBytes;
                uint8_t* uvr = dstUV + (size_t)(y/2) * yStride;
                for (int x = 0; x < cw; x += 2) {
                    int r = sp[x*4+0], g = sp[x*4+1], b = sp[x*4+2];
                    uvr[x+0] = (uint8_t)(((-38*r - 74*g + 112*b + 128) >> 8) + 128); // U
                    uvr[x+1] = (uint8_t)(((112*r - 94*g - 18*b + 128) >> 8) + 128); // V
                }
            }
            if (S.gbUnlock) S.gbUnlock(gb);
        } else {
            if (S.gbUnlock && lr == 0) S.gbUnlock(gb);
        }
    }

    // queue the filled buffer (crop = full frame, fence = NO_FENCE)
    if (!useCancel_ && S.gppQueue) {
        char qbi[256]; memset(qbi, 0, sizeof(qbi));
        *(int64_t*)(qbi + 0)   = 0;          // timestamp
        *(int*)(qbi + 8)       = 1;          // isAutoTimestamp
        *(int*)(qbi + 12)      = 0;          // dataSpace
        *(int*)(qbi + 16)      = 0;          // crop.left
        *(int*)(qbi + 20)      = 0;          // crop.top
        *(int*)(qbi + 24)      = W;          // crop.right
        *(int*)(qbi + 28)      = H;          // crop.bottom
        *(int*)(qbi + 32)      = 0;          // scalingMode
        *(uint32_t*)(qbi + 36) = 0;          // transform
        *(uint32_t*)(qbi + 40) = 0;          // stickyTransform
        *(void**)(qbi + 48)    = noFence_;   // fence sp<Fence>=NO_FENCE
        if (S.regionCtor) S.regionCtor(qbi + 56);
        char qbo[256]; memset(qbo, 0, sizeof(qbo));
        int qr = S.gppQueue(inProducer_, slot, qbi, qbo);
        if (S.regionDtor) S.regionDtor(qbi + 56);
        if (qr < 0) { LOGW("gpp: queueBuffer rc=%d -> cancel mode", qr); useCancel_ = true; }
        else ++posted_;
    }
    if (useCancel_ && S.gppCancel) { Sp nf; nf.p = noFence_; S.gppCancel(inProducer_, slot, &nf); }
    return true;
}

void GppEngine::stop() {
    if (d_started) { d_run = 0; pthread_join(d_tid, nullptr); d_started = false; }
    connected_ = false;
    LOGI("gpp: stopped (posted=%d drained=%d)", posted_, d_count);
}

}  // namespace cleanfg
