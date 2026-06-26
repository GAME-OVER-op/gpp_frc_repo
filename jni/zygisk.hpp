// Минимальный Zygisk C++ API (вырезка из официального zygisk-module-sample).
// При реальной сборке замените на актуальный zygisk.hpp из
// https://github.com/topjohnwu/zygisk-module-sample (версия под ваш Magisk/KernelSU).
#pragma once
#include <jni.h>

namespace zygisk {

struct AppSpecializeArgs {
    jint&      uid;
    jint&      gid;
    jintArray& gids;
    jint&      runtime_flags;
    jobjectArray& rlimits;
    jint&      mount_external;
    jstring&   se_info;
    jstring&   nice_name;
    jstring&   instruction_set;
    jstring&   app_data_dir;
    // … остальные поля см. официальный хедер.
};

struct ServerSpecializeArgs { /* … */ };

enum Option { DLCLOSE_MODULE_LIBRARY = 1, FORCE_DENYLIST_UNMOUNT = 2 };

struct Api {
    void setOption(Option opt);
    // … connectCompanion(), pltHookRegister(), и т.д. см. официальный хедер.
};

struct ModuleBase {
    virtual void onLoad(Api*, JNIEnv*) {}
    virtual void preAppSpecialize(AppSpecializeArgs*) {}
    virtual void postAppSpecialize(const AppSpecializeArgs*) {}
    virtual void preServerSpecialize(ServerSpecializeArgs*) {}
    virtual void postServerSpecialize(const ServerSpecializeArgs*) {}
};

}  // namespace zygisk

#define REGISTER_ZYGISK_MODULE(clazz)    /* см. официальный хедер */
#define REGISTER_ZYGISK_COMPANION(func)  /* см. официальный хедер */
