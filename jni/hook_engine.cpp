// Abstraction over an inline hooker.
// Default (and what the CI workflow uses): Dobby, statically linked, so the
// whole module is a single self-contained .so with no extra runtime deps.
// Optionally ShadowHook can be used with -DUSE_SHADOWHOOK=ON.
#include "log.h"
#include <mutex>

namespace cleanfg {

#if defined(USE_SHADOWHOOK)
#include "shadowhook.h"
static std::once_flag g_init;
static void ensureInit() {
    std::call_once(g_init, [] {
        int r = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
        if (r != 0) LOGE("shadowhook_init failed: %d", r);
        else LOGI("shadowhook initialized");
    });
}
void* hookFunction(void* target, void* replacement) {
    ensureInit();
    void* origin = nullptr;
    void* stub = shadowhook_hook_func_addr(target, replacement, &origin);
    if (!stub) {
        LOGE("shadowhook hook failed: %s", shadowhook_to_errmsg(shadowhook_get_errno()));
        return nullptr;
    }
    return origin;
}
#else  // Dobby (default)
#include <dobby.h>
void* hookFunction(void* target, void* replacement) {
    void* origin = nullptr;
    if (DobbyHook(target, (dobby_dummy_func_t)replacement, (dobby_dummy_func_t*)&origin) != 0) {
        LOGE("DobbyHook failed @ %p", target);
        return nullptr;
    }
    return origin;
}
#endif

}  // namespace cleanfg
