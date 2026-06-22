// GPP-FRC standalone probe — Phase 1
// Цель: в реальном процессе на устройстве загрузить libgppvppgfrcplussession.so,
// разрешить ключевые символы (CreateFactory, GPPSession ctor/connect/createQueue)
// и безопасно дёрнуть CreateFactory(), чтобы подтвердить живой путь.
//
// Запуск на устройстве (root):
//   su -c 'LD_LIBRARY_PATH=/data/adb/modules/gpp_frc_framegen/system/lib64:/system/lib64:/vendor/lib64 \
//           /data/local/tmp/gpp_frc_test --call-factory'
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <android/log.h>

#define TAG "GPP_FRC_TEST"
#define LOG(...) do { __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__); \
                      printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static const char* LIB_CANDIDATES[] = {
    "libgppvppgfrcplussession.so",
    "/data/adb/modules/gpp_frc_framegen/system/lib64/libgppvppgfrcplussession.so",
    "/system/system/lib64/libgppvppgfrcplussession.so",
    "/system/lib64/libgppvppgfrcplussession.so",
};

// Ключевые символы (mangled) — из readelf libgppvppgfrcplussession.so
static const char* SYMS[] = {
    "CreateFactory",
    "_ZN7android10GPPSessionC1Ev",  // GPPSession::GPPSession()
    "_ZN7android10GPPSession7connectERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_RKNS_2spINS_22IGraphicBufferProducerEEEPSC_", // connect(string,string,sp<IGBP>const&,sp<IGBP>*)
    "_ZN7android10GPPSession11createQueueEbPNS_2spINS_22IGraphicBufferProducerEEE", // createQueue(bool, sp<IGBP>)
    "_ZN7android10GPPSession10disconnectEv",
    "_ZN7android12GPPComponent4initERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEEPNS_12IGPPCallbackE",
    "_ZN7android12GPPComponent6configEjjjRKNS_12ConfigParamsEb",
};

int main(int argc, char** argv) {
    LOG("=== GPP-FRC standalone probe (Phase 1) ===");

    void* h = nullptr;
    for (const char* c : LIB_CANDIDATES) {
        h = dlopen(c, RTLD_NOW | RTLD_GLOBAL);
        LOG("dlopen %-70s -> %s", c, h ? "OK" : dlerror());
        if (h) break;
    }
    if (!h) { LOG("FATAL: не удалось загрузить session-либу (см. ошибку линкера выше)"); return 1; }

    int resolved = 0, total = 0;
    for (const char* s : SYMS) {
        void* p = dlsym(h, s);
        ++total; if (p) ++resolved;
        LOG("sym [%s] %.60s%s -> %p", p ? "OK" : "--", s, strlen(s) > 60 ? "..." : "", p);
    }
    LOG("разрешено %d/%d символов", resolved, total);

    void* cf = dlsym(h, "CreateFactory");
    if (cf && argc > 1 && strcmp(argv[1], "--call-factory") == 0) {
        LOG("вызываю CreateFactory() (ожидаем void* фабрики) ...");
        using FT = void* (*)();
        void* r = reinterpret_cast<FT>(cf)();
        LOG("CreateFactory() вернул %p  -> %s", r, r ? "живой путь ОК" : "null (нужны аргументы?)");
    } else {
        LOG("(чтобы дёрнуть фабрику: запусти с флагом --call-factory)");
    }

    LOG("=== done ===");
    return 0;
}
