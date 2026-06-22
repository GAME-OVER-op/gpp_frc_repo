// GPP-FRC standalone harness - Phase 7 (podacha kadrov cherez GPPSession::connect)
//
// Otlichie ot Phase 2: vyhodnoj IGraphicBufferProducer beryom NE iz Surface
// AImageReader (privedenie ukazatelya padalo na Android 15/SDK36 -> S3 SIGSEGV),
// a napryamuyu cherez BufferQueue::createBufferQueue (chistyj ABI: rezultaty
// cherez out-parametry sp<>*, bez vozvrata struktury po znacheniyu i bez kastov).
// K vyhodnomu konsumeru veshaem BufferItemConsumer, chtoby ochered ne perepolnyalas
// i dvizhok mog otdat neskolko kadrov.
//
// Uspeh = v logcat poyavlyayutsya logi generacii kadrov:
//   "create BufferQueue done", "FRC will do Nx interpolation", "Send the Non-Interpolated frame".
//
// Vsyo vneshnee tyanetsya cherez dlopen/dlsym (libgui ne linkuetsya).
// sp<T> predstavlen kak {void* p}; vozvrat sp po znacheniyu - cherez sret-tryuk (SpRet).

#include <dlfcn.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/hardware_buffer.h>
#include <media/NdkImageReader.h>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#define TAG "GPP_FRC_TEST"
#define LOG(...) do { __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__); \
                      printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

// ---- opakovka sp<T> ----
struct Sp { void* p = nullptr; };          // peredayotsya po ssylke/ukazatelyu
struct SpRet { void* p; ~SpRet() {} };     // netrivialnyj dtor => vozvrat cherez pamyat (x8)

// ---- tochnye mangled-imena (engine) ----
static const char* SYM_CREATE_FACTORY = "CreateFactory";
static const char* SYM_SESS_CTOR      = "_ZN7android10GPPSessionC1Ev";
static const char* SYM_SESS_CONNECT   = "_ZN7android10GPPSession7connectERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_RKNS_2spINS_22IGraphicBufferProducerEEEPSC_";
static const char* SYM_VT_SESSION     = "_ZTVN7android10GPPSessionE";
static const char* SYM_VT_COMPONENT   = "_ZTVN7android12GPPComponentE";
static const char* SYM_VT_PRODUCER    = "_ZTVN7android11GPPProducerE";

// ---- tochnye mangled-imena (libgui) ----
static const char* SYM_SURFACE_CTOR   = "_ZN7android7SurfaceC1ERKNS_2spINS_22IGraphicBufferProducerEEEbRKNS1_INS_7IBinderEEE";
static const char* SYM_SURF_GETIGBP   = "_ZNK7android7Surface25getIGraphicBufferProducerEv";
static const char* SYM_CREATE_BQ      = "_ZN7android11BufferQueue17createBufferQueueEPNS_2spINS_22IGraphicBufferProducerEEEPNS1_INS_22IGraphicBufferConsumerEEEb";
static const char* SYM_BIC_CTOR       = "_ZN7android18BufferItemConsumerC1ERKNS_2spINS_22IGraphicBufferConsumerEEEmib";

// ---- Phase 7: pryamoe upravlenie GPPProducer + nastrojka razreshenia ----
static const char* SYM_CB_SETSIZE  = "_ZN7android12ConsumerBase20setDefaultBufferSizeEjj";
static const char* SYM_CB_SETFMT   = "_ZN7android12ConsumerBase22setDefaultBufferFormatEi";
static const char* SYM_GPPP_CONNECT= "_ZN7android11GPPProducer7connectERKNS_2spINS_17IProducerListenerEEEibPNS_22IGraphicBufferProducer17QueueBufferOutputE";
static const char* SYM_GPPP_DEQUEUE= "_ZN7android11GPPProducer13dequeueBufferEPiPNS_2spINS_5FenceEEEjjimPmPNS_22FrameEventHistoryDeltaE";
static const char* SYM_GPPP_REQUEST= "_ZN7android11GPPProducer13requestBufferEiPNS_2spINS_13GraphicBufferEEE";
static const char* SYM_GPPP_QUEUE  = "_ZN7android11GPPProducer11queueBufferEiRKNS_22IGraphicBufferProducer16QueueBufferInputEPNS1_17QueueBufferOutputE";
static const char* SYM_GPPP_CANCEL = "_ZN7android11GPPProducer12cancelBufferEiRKNS_2spINS_5FenceEEE";
static const char* SYM_GB_LOCK     = "_ZN7android13GraphicBuffer4lockEjRKNS_4RectEPPvPiS6_"; // lock(usage, Rect const&, void**, int*, int*)
static const char* SYM_GB_UNLOCK   = "_ZN7android13GraphicBuffer6unlockEv";
static const char* SYM_REGION_CTOR = "_ZN7android6RegionC1Ev";
static const char* SYM_REGION_DTOR = "_ZN7android6RegionD1Ev";

// ---- FFI tipy ----
using FnFactory    = void*  (*)();
using FnSessCtor   = void   (*)(void* thiz);
using FnConnect    = long   (*)(void* thiz, const std::string* a, const std::string* b,
                                const Sp* outGbp, Sp* inGbp);
using FnSurfCtor   = void   (*)(void* thiz, const Sp* gbp, bool controlledByApp, const Sp* sc);
using FnSurfGetGbp = SpRet  (*)(void* thiz);   // const metod (fallback)
using FnCreateBQ   = void   (*)(Sp* outProd, Sp* outCons, bool consumerIsSurfaceFlinger);
using FnBicCtor    = void   (*)(void* thiz, const Sp* cons, uint64_t usage, int bufCount, bool controlledByApp);
using FnCBSize     = int    (*)(void* thiz, uint32_t w, uint32_t h);
using FnCBFmt      = int    (*)(void* thiz, int fmt);
using FnGppConnect = int    (*)(void* thiz, const Sp* listener, int api, bool controlledByApp, void* qbo);
using FnGppDequeue = int    (*)(void* thiz, int* slot, Sp* fence, uint32_t w, uint32_t h, int fmt, uint64_t usage, uint64_t* age, void* outTs);
using FnGppRequest = int    (*)(void* thiz, int slot, Sp* gbOut);
using FnGppQueue   = int    (*)(void* thiz, int slot, const void* qbi, void* qbo);
using FnGppCancel  = int    (*)(void* thiz, int slot, const Sp* fence);
using FnGbLock     = int    (*)(void* thiz, uint32_t usage, const void* rect, void** vaddr, int* obpp, int* obps);
using FnGbUnlock   = int    (*)(void* thiz);
using FnRegionCtor = void   (*)(void* thiz);

// ---- krash-gard ----
static sigjmp_buf g_jmp;
static volatile int g_stage = 0;
static void on_sig(int s) {
    __android_log_print(ANDROID_LOG_ERROR, TAG, "!!! signal %d na stadii %d", s, g_stage);
    printf("!!! signal %d na stadii %d\n", s, g_stage); fflush(stdout);
    siglongjmp(g_jmp, 1);
}

static void* g_lib = nullptr;
template <class T> static T sym(const char* n) {
    void* p = dlsym(g_lib, n);
    if (!p) p = dlsym(RTLD_DEFAULT, n);
    LOG("  dlsym %.50s%s -> %p", n, strlen(n) > 50 ? "..." : "", p);
    return reinterpret_cast<T>(p);
}

static const char* LIB_CANDIDATES[] = {
    "libgppvppgfrcplussession.so",
    "/data/adb/modules/gpp_frc_framegen/system/lib64/libgppvppgfrcplussession.so",
    "/system/system/lib64/libgppvppgfrcplussession.so",
    "/system/lib64/libgppvppgfrcplussession.so",
};

static const int W = 1280, H = 720;

// narisovat dvizhushijsya pryamougolnik v RGBA8888 bufer (stride v pikselyah)
static void draw_raw(void* bits, int w, int h, int stride, int frame) {
    uint8_t* base = (uint8_t*)bits;
    int rx = (frame * 12) % (w > 160 ? w - 160 : 1);
    int ry = (frame * 7)  % (h > 160 ? h - 160 : 1);
    for (int y = 0; y < h; ++y) {
        uint32_t* row = (uint32_t*)(base + (size_t)y * stride * 4);
        for (int x = 0; x < w; ++x) {
            bool in = (x >= rx && x < rx + 160 && y >= ry && y < ry + 160);
            row[x] = in ? 0xFF00FF00u : 0xFF202020u;
        }
    }
}

int main(int argc, char** argv) {
    LOG("=== GPP-FRC standalone harness (Phase 7) ===");
    signal(SIGSEGV, on_sig);
    signal(SIGABRT, on_sig);
    signal(SIGBUS,  on_sig);

    int frames = (argc > 1) ? atoi(argv[1]) : 150;
    if (frames < 10) frames = 10;
    LOG("kadrov k podache: %d, razreshenie: %dx%d", frames, W, H);

    // -------- S1: zagruzka liby --------
    g_stage = 1;
    for (const char* c : LIB_CANDIDATES) {
        g_lib = dlopen(c, RTLD_NOW | RTLD_GLOBAL);
        LOG("[S1] dlopen %-60s -> %s", c, g_lib ? "OK" : dlerror());
        if (g_lib) break;
    }
    if (!g_lib) { LOG("FATAL: net session-liby"); return 1; }
    // podtyanut libgui v globalnyj scope (na sluchaj, esli ne podtyanulas tranzitivno)
    dlopen("libgui.so", RTLD_NOW | RTLD_GLOBAL);
    dlopen("libui.so", RTLD_NOW | RTLD_GLOBAL);

    auto createFactory = sym<FnFactory>(SYM_CREATE_FACTORY);
    auto sessCtor      = sym<FnSessCtor>(SYM_SESS_CTOR);
    auto sessConnect   = sym<FnConnect>(SYM_SESS_CONNECT);
    auto surfCtor      = sym<FnSurfCtor>(SYM_SURFACE_CTOR);
    auto surfGetGbp    = sym<FnSurfGetGbp>(SYM_SURF_GETIGBP);
    auto createBQ      = sym<FnCreateBQ>(SYM_CREATE_BQ);
    auto bicCtor       = sym<FnBicCtor>(SYM_BIC_CTOR);
    auto cbSetSize     = sym<FnCBSize>(SYM_CB_SETSIZE);
    auto cbSetFmt      = sym<FnCBFmt>(SYM_CB_SETFMT);
    auto gppConnect    = sym<FnGppConnect>(SYM_GPPP_CONNECT);
    auto gppDequeue    = sym<FnGppDequeue>(SYM_GPPP_DEQUEUE);
    auto gppRequest    = sym<FnGppRequest>(SYM_GPPP_REQUEST);
    auto gppQueue      = sym<FnGppQueue>(SYM_GPPP_QUEUE);
    auto gppCancel     = sym<FnGppCancel>(SYM_GPPP_CANCEL);
    auto gbLock        = sym<FnGbLock>(SYM_GB_LOCK);
    auto gbUnlock      = sym<FnGbUnlock>(SYM_GB_UNLOCK);
    auto regionCtor    = sym<FnRegionCtor>(SYM_REGION_CTOR);
    auto regionDtor    = sym<FnRegionCtor>(SYM_REGION_DTOR);
    void* vtSession    = dlsym(g_lib, SYM_VT_SESSION);
    void* vtComponent  = dlsym(g_lib, SYM_VT_COMPONENT);
    void* vtProducer   = dlsym(g_lib, SYM_VT_PRODUCER);
    LOG("[S1] vtable GPPSession=%p GPPComponent=%p GPPProducer=%p", vtSession, vtComponent, vtProducer);

    if (!sessCtor || !sessConnect || !surfCtor) {
        LOG("FATAL: ne vse klyuchevye simvoly razresheny"); return 1;
    }

    // identificirovat klass obyekta po vptr
    auto idclass = [&](void* obj) -> const char* {
        if (!obj) return "null";
        void* vptr = *(void**)obj;
        auto near = [&](void* vt){ return vt && (uintptr_t)vptr >= (uintptr_t)vt && (uintptr_t)vptr < (uintptr_t)vt + 0x400; };
        if (near(vtSession))   return "GPPSession";
        if (near(vtComponent)) return "GPPComponent";
        if (near(vtProducer))  return "GPPProducer";
        return "?(drugoj)";
    };

    // -------- S2: CreateFactory --------
    g_stage = 2;
    if (sigsetjmp(g_jmp, 1) == 0) {
        if (createFactory) {
            void* f = createFactory();
            LOG("[S2] CreateFactory() = %p  klass=%s", f, idclass(f));
        }
    } else { LOG("[S2] upalo na CreateFactory"); }

    // -------- S3: vyhodnoj producer cherez BufferQueue::createBufferQueue --------
    g_stage = 3;
    Sp outGbp;          // vyhodnoj IGraphicBufferProducer (dlya connect)
    Sp outCons;         // vyhodnoj IGraphicBufferConsumer
    void* bic = nullptr;
    if (sigsetjmp(g_jmp, 1) == 0) {
        if (createBQ) {
            createBQ(&outGbp, &outCons, false);
            LOG("[S3] createBufferQueue -> prod=%p cons=%p", outGbp.p, outCons.p);
            if (outCons.p && bicCtor) {
                bic = calloc(1, 16384);   // ConsumerBase+BufferItemConsumer, s zapasom
                bicCtor(bic, &outCons, /*usage*/0, /*bufCount*/8, /*controlledByApp*/false);
                LOG("[S3] BufferItemConsumer sozdan @%p (drenazh ocheredi)", bic);
                if (cbSetSize) { int r1 = cbSetSize(bic, (uint32_t)W, (uint32_t)H); LOG("[S3b] setDefaultBufferSize(%d,%d) rc=%d", W, H, r1); }
                if (cbSetFmt)  { int r2 = cbSetFmt(bic, 1 /*RGBA_8888*/);            LOG("[S3b] setDefaultBufferFormat(RGBA_8888) rc=%d", r2); }
            }
        }
    } else { LOG("[S3] upalo na createBufferQueue/BufferItemConsumer"); }

    // fallback: staryj put cherez AImageReader (esli createBufferQueue nedostupen)
    if (!outGbp.p) {
        if (sigsetjmp(g_jmp, 1) == 0) {
            AImageReader* reader = nullptr; ANativeWindow* outWin = nullptr;
            media_status_t ms = AImageReader_new(W, H, AIMAGE_FORMAT_RGBA_8888, 8, &reader);
            LOG("[S3-fb] AImageReader_new -> %d, reader=%p", ms, reader);
            if (reader) {
                AImageReader_getWindow(reader, &outWin);
                if (outWin && surfGetGbp) { SpRet r = surfGetGbp((void*)outWin); outGbp.p = r.p; }
                LOG("[S3-fb] outGbp=%p", outGbp.p);
            }
        } else { LOG("[S3-fb] fallback tozhe upal"); }
    }
    if (!outGbp.p) { LOG("FATAL: net vyhodnogo producer"); return 1; }

    // -------- S4: sozdat GPPSession --------
    g_stage = 4;
    void* session = nullptr;
    if (sigsetjmp(g_jmp, 1) == 0) {
        session = calloc(1, 32768);
        sessCtor(session);
        LOG("[S4] GPPSession sozdan @%p  klass=%s", session, idclass(session));
    } else { LOG("[S4] upalo na konstruktore GPPSession"); return 1; }

    // -------- S5: connect --------
    g_stage = 5;
    Sp inGbp;
    if (sigsetjmp(g_jmp, 1) == 0) {
        std::string pkg   = "com.tencent.tmgp.sgame";
        std::string layer = "SurfaceView[com.tencent.tmgp.sgame/test]#0";
        long rc = sessConnect(session, &pkg, &layer, &outGbp, &inGbp);
        LOG("[S5] connect() rc=%ld  inGbp(IGraphicBufferProducer*)=%p klass=%s", rc, inGbp.p, idclass(inGbp.p));
        if (inGbp.p) {
            void* vptr = *(void**)inGbp.p;
            LOG("[S5] inGbp vptr=%p (BufferQueueProducer => standartnyj; GPPProducer => kastomnyj)", vptr);
        }
    } else { LOG("[S5] upalo na connect()"); return 1; }
    if (!inGbp.p) { LOG("FATAL: connect ne vernul vhodnoj producer"); return 1; }

    // -------- S5b: dampnut vnutrennie ukazateli GPPProducer --------
    // Iz disasm: query() chitaet inner-producer iz this+0x30, dequeueBuffer iz this+0x20.
    // Esli oni nulevye/dikie -> connect ne dovyazal konvejer (rc=1).
    g_stage = 5;
    if (sigsetjmp(g_jmp, 1) == 0) {
        unsigned char* o = (unsigned char*)inGbp.p;
        void* in20  = *(void**)(o + 0x20);
        void* cons28= *(void**)(o + 0x28);
        void* out30 = *(void**)(o + 0x30);
        int   w38   = *(int*)(o + 0x38);
        int   h3c   = *(int*)(o + 0x3c);
        LOG("[S5b] GPPProducer inner: in(+0x20)=%p cons(+0x28)=%p out(+0x30)=%p  cached %dx%d",
            in20, cons28, out30, w38, h3c);
        if (!in20 || !out30) LOG("[S5b] VNIMANIE: inner-producer NULL -> connect ne dovyazal konvejer (nuzhen VPP HAL/configstore)");
    } else { LOG("[S5b] damp inner upal"); }

    // -------- S6: podklyuchitsya k vhodnomu producer NAPRYAMUYU (bez Surface) --------
    // Surface-obyortka padala na NDK-dispetche (perform/query hooks). Gonim kadry
    // pryamo v GPPProducer ego sobstvennymi metodami (connect/dequeue/request/queue).
    g_stage = 6;
    bool connected = false;
    if (sigsetjmp(g_jmp, 1) == 0) {
        if (gppConnect) {
            char qbo[256]; memset(qbo, 0, sizeof(qbo));
            Sp nullListener;
            int rc = gppConnect(inGbp.p, &nullListener, 2 /*NATIVE_WINDOW_API_CPU*/, true, qbo);
            uint32_t qw = *(uint32_t*)(qbo + 0), qh = *(uint32_t*)(qbo + 4);
            LOG("[S6] GPPProducer::connect(API_CPU) rc=%d  qbo w=%u h=%u", rc, qw, qh);
            connected = (rc >= 0);
        } else { LOG("[S6] net simvola GPPProducer::connect"); }
    } else { LOG("[S6] GPPProducer::connect UPAL"); }

    // -------- S7: dequeue -> request -> lock -> draw -> queue (per-kadr pod gardom) --------
    g_stage = 7;
    void* gbslot[64]; memset(gbslot, 0, sizeof(gbslot));
    int posted = 0, deqfail = 0; bool use_cancel = false;
    const uint64_t USAGE = 0x33; // SW_READ_OFTEN | SW_WRITE_OFTEN
    for (int i = 0; i < frames; ++i) {
        if (sigsetjmp(g_jmp, 1) != 0) { LOG("[S7] signal na kadre %d -> stop", i); break; }
        if (!gppDequeue) { LOG("[S7] net dequeueBuffer"); break; }
        int slot = -1; Sp fence; uint64_t age = 0;
        int rc = gppDequeue(inGbp.p, &slot, &fence, (uint32_t)W, (uint32_t)H, 1 /*RGBA_8888*/, USAGE, &age, nullptr);
        if (i == 0 || rc < 0) LOG("[S7] dequeueBuffer kadr %d -> rc=%d slot=%d", i, rc, slot);
        if (rc < 0 || slot < 0 || slot >= 64) {
            if (++deqfail >= 5) { LOG("[S7] 5 neudachnyh dequeue -> stop"); break; }
            usleep(16000); continue;
        }
        deqfail = 0;
        if ((rc & 1 /*BUFFER_NEEDS_REALLOCATION*/) || !gbslot[slot]) {
            if (gppRequest) { Sp gb; int rr = gppRequest(inGbp.p, slot, &gb); gbslot[slot] = gb.p;
                if (i == 0) LOG("[S7] requestBuffer slot=%d rc=%d gb=%p", slot, rr, gb.p); }
        }
        void* gb = gbslot[slot];
        if (!gb) { LOG("[S7] net GraphicBuffer dlya slota %d", slot); break; }
        // ANativeWindowBuffer: baza android_native_base_t = 56 bajt (magic,version,reserved[4],incRef,decRef)
        // => width@56, height@60, stride@64, format@68 (piksel-stride)
        int gw = *(int*)((char*)gb + 56), gh = *(int*)((char*)gb + 60);
        int gstride = *(int*)((char*)gb + 64), gfmt = *(int*)((char*)gb + 68);
        if (i == 0) LOG("[S7] GraphicBuffer %dx%d stride=%d fmt=%d", gw, gh, gstride, gfmt);
        int dw = (gw > 0 && gw <= 4096) ? gw : W;
        int dh = (gh > 0 && gh <= 4096) ? gh : H;
        int dstride = (gstride >= dw && gstride <= dw + 4096) ? gstride : dw;
        void* vaddr = nullptr;
        if (gbLock) {
            int lockRect[4] = { 0, 0, dw, dh };   // Rect{left,top,right,bottom}
            int obpp = 0, obps = 0;
            int lr = gbLock(gb, (uint32_t)USAGE, lockRect, &vaddr, &obpp, &obps);
            if (i == 0) LOG("[S7] GraphicBuffer::lock rc=%d vaddr=%p bpp=%d", lr, vaddr, obpp);
            if (lr == 0 && vaddr) { draw_raw(vaddr, dw, dh, dstride, i); if (gbUnlock) gbUnlock(gb); }
        }
        if (!use_cancel && gppQueue) {
            // sobrat QueueBufferInput; crop - po realnym razmeram bufera (gw/gh), inache W/H
            int cw = (gw > 0 && gw <= 4096) ? gw : W;
            int ch = (gh > 0 && gh <= 4096) ? gh : H;
            // helper: odna popytka queueBuffer s zadannym crop (cl,ct,cr,cb)
            auto try_queue = [&](int cl, int ct, int cr, int cb) -> int {
                char qbi[256]; memset(qbi, 0, sizeof(qbi));
                *(int64_t*)(qbi + 0)   = 0;      // timestamp
                *(int*)(qbi + 8)       = 1;      // isAutoTimestamp
                *(int*)(qbi + 12)      = 0;      // dataSpace UNKNOWN
                *(int*)(qbi + 16)      = cl;     // crop.left
                *(int*)(qbi + 20)      = ct;     // crop.top
                *(int*)(qbi + 24)      = cr;     // crop.right
                *(int*)(qbi + 28)      = cb;     // crop.bottom
                *(int*)(qbi + 32)      = 0;      // scalingMode FREEZE
                *(uint32_t*)(qbi + 36) = 0;      // transform
                *(uint32_t*)(qbi + 40) = 0;      // stickyTransform
                *(void**)(qbi + 48)    = nullptr;// fence sp<>
                if (regionCtor) regionCtor(qbi + 56);   // surfaceDamage Region
                char qboQ[256]; memset(qboQ, 0, sizeof(qboQ));
                int r = gppQueue(inGbp.p, slot, qbi, qboQ);
                if (regionDtor) regionDtor(qbi + 56);
                return r;
            };
            int qr = try_queue(0, 0, cw, ch);
            if (qr < 0 && i == 0) {
                LOG("[S7] queueBuffer crop(0,0,%d,%d) rc=%d -> probuyu drugie varianty crop", cw, ch, qr);
                int qr2 = try_queue(0, 0, 0, 0);                 // pustoj crop = ves bufer
                LOG("[S7]   crop EMPTY(0,0,0,0) rc=%d", qr2);
                int qr3 = try_queue(0, 0, W, H);                 // zaproshennyj W/H
                LOG("[S7]   crop(0,0,%d,%d) rc=%d", W, H, qr3);
                if (qr2 >= 0) qr = qr2; else if (qr3 >= 0) qr = qr3;
            }
            if (i == 0) LOG("[S7] queueBuffer slot=%d itog rc=%d", slot, qr);
            if (qr < 0) { if (i == 0) LOG("[S7] queueBuffer oshibka -> perehozhu na cancelBuffer"); use_cancel = true; }
            else ++posted;
        }
        if (use_cancel && gppCancel) { Sp nf; gppCancel(inGbp.p, slot, &nf); }
        if (i % 30 == 0) LOG("[S7] kadr %d (otdano=%d, cancel=%d)", i, posted, use_cancel ? 1 : 0);
        usleep(16000);
    }
    LOG("[S7] vsego otdano kadrov dvizhku: %d", posted);
    LOG("=== gotovo. smotri logcat: FRC/Interpolated/MotionEngine/BufferQueue ===");
    return 0;
}
