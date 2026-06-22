// GPP-FRC standalone harness - Phase 5 (podacha kadrov cherez GPPSession::connect)
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

// ---- FFI tipy ----
using FnFactory    = void*  (*)();
using FnSessCtor   = void   (*)(void* thiz);
using FnConnect    = long   (*)(void* thiz, const std::string* a, const std::string* b,
                                const Sp* outGbp, Sp* inGbp);
using FnSurfCtor   = void   (*)(void* thiz, const Sp* gbp, bool controlledByApp, const Sp* sc);
using FnSurfGetGbp = SpRet  (*)(void* thiz);   // const metod (fallback)
using FnCreateBQ   = void   (*)(Sp* outProd, Sp* outCons, bool consumerIsSurfaceFlinger);
using FnBicCtor    = void   (*)(void* thiz, const Sp* cons, uint64_t usage, int bufCount, bool controlledByApp);

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

// narisovat dvizhushijsya pryamougolnik v RGBA8888 bufer
static void draw_frame(ANativeWindow_Buffer* b, int frame) {
    uint8_t* base = (uint8_t*)b->bits;
    int rx = (frame * 12) % (b->width  > 160 ? b->width  - 160 : 1);
    int ry = (frame * 7)  % (b->height > 160 ? b->height - 160 : 1);
    for (int y = 0; y < b->height; ++y) {
        uint32_t* row = (uint32_t*)(base + (size_t)y * b->stride * 4);
        for (int x = 0; x < b->width; ++x) {
            bool in = (x >= rx && x < rx + 160 && y >= ry && y < ry + 160);
            row[x] = in ? 0xFF00FF00u : 0xFF202020u;
        }
    }
}

int main(int argc, char** argv) {
    LOG("=== GPP-FRC standalone harness (Phase 5) ===");
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

    auto createFactory = sym<FnFactory>(SYM_CREATE_FACTORY);
    auto sessCtor      = sym<FnSessCtor>(SYM_SESS_CTOR);
    auto sessConnect   = sym<FnConnect>(SYM_SESS_CONNECT);
    auto surfCtor      = sym<FnSurfCtor>(SYM_SURFACE_CTOR);
    auto surfGetGbp    = sym<FnSurfGetGbp>(SYM_SURF_GETIGBP);
    auto createBQ      = sym<FnCreateBQ>(SYM_CREATE_BQ);
    auto bicCtor       = sym<FnBicCtor>(SYM_BIC_CTOR);
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

    // -------- S6: obernut vhodnoj producer v Surface --------
    g_stage = 6;
    ANativeWindow* inWin = nullptr;
    if (sigsetjmp(g_jmp, 1) == 0) {
        void* surf = calloc(1, 8192);
        Sp nullBinder;
        surfCtor(surf, &inGbp, /*controlledByApp*/ true, &nullBinder);
        inWin = (ANativeWindow*)surf;   // Surface : ANativeWindow (offset 0)
        LOG("[S6] Surface(inGbp) ctor OK @%p", inWin);
    } else { LOG("[S6] upalo na konstruktore Surface(inGbp)"); return 1; }

    // -------- S6b: proba okna (ne fatalno) --------
    g_stage = 6;
    if (sigsetjmp(g_jmp, 1) == 0) {
        int w = ANativeWindow_getWidth(inWin);
        int h = ANativeWindow_getHeight(inWin);
        int f = ANativeWindow_getFormat(inWin);
        LOG("[S6b] okno do nastrojki: %dx%d format=%d", w, h, f);
    } else { LOG("[S6b] query okna upal (ne fatalno)"); }

    // -------- S6c: setBuffersGeometry (ne fatalno) --------
    g_stage = 6;
    bool geom_ok = false;
    if (sigsetjmp(g_jmp, 1) == 0) {
        int rc = ANativeWindow_setBuffersGeometry(inWin, W, H, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
        LOG("[S6c] setBuffersGeometry rc=%d", rc);
        geom_ok = (rc == 0);
    } else { LOG("[S6c] setBuffersGeometry UPAL (perform-hook). Probuyem dalshe bez nego."); }

    // -------- S7: gnat kadry vo vhodnoj producer (kazhdyj kadr pod gardom) --------
    g_stage = 7;
    int posted = 0, lockfail = 0;
    for (int i = 0; i < frames; ++i) {
        if (sigsetjmp(g_jmp, 1) != 0) {
            LOG("[S7] SIGSEGV/ABRT na kadre %d (lock/post). Stop.", i);
            break;
        }
        ANativeWindow_Buffer buf;
        int rc = ANativeWindow_lock(inWin, &buf, nullptr);
        if (rc != 0) {
            LOG("[S7] lock kadr %d rc=%d", i, rc);
            if (++lockfail >= 5) { LOG("[S7] 5 podryad neudachnyh lock -> stop"); break; }
            usleep(16000); continue;
        }
        lockfail = 0;
        if (posted == 0) LOG("[S7] pervyj lock OK: %dx%d stride=%d format=%d", buf.width, buf.height, buf.stride, buf.format);
        draw_frame(&buf, i);
        ANativeWindow_unlockAndPost(inWin);
        ++posted;
        if (i % 30 == 0) LOG("[S7] otdano kadrov: %d", posted);
        usleep(16000);   // ~60 fps
    }
    LOG("[S7] vsego otdano kadrov: %d", posted);
    LOG("=== gotovo. smotri logcat na stroki dvizhka (FRC/Interpolated/BufferQueue) ===");
    return 0;
}
