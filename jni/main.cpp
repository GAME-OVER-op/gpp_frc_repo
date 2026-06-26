// Zygisk entrypoint. Реализует zygisk_module_entry / zygisk_companion_entry,
// как в оригинальном liblybfghook.so, но без сети.
#include "zygisk.hpp"
#include "config.h"
#include "log.h"
#include <unistd.h>
#include <cstring>

namespace cleanfg {
bool installEglHook();
bool installVulkanHook();
}

using namespace cleanfg;

static const char* kConfigPath = "/data/adb/modules/cleanfg/cleanfg.prop";

class CleanFgModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        // Решаем, целевой ли это процесс, ПО ИМЕНИ ПАКЕТА.
        loadConfig(kConfigPath);

        const char* pkg = env->GetStringUTFChars(args->nice_name, nullptr);
        targetProcess = g_config.matchesPackage(pkg);
        if (pkg) env->ReleaseStringUTFChars(args->nice_name, pkg);

        if (!targetProcess) {
            // Не наш процесс — выгружаем библиотеку, ничего не трогаем.
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs*) override {
        if (!targetProcess) return;
        LOGI("cleanfg active in target process");

        bool ok = false;
        switch (g_config.mode) {
            case Mode::Gles:   ok = installEglHook(); break;
            case Mode::Vulkan: ok = installVulkanHook(); break;
            case Mode::Auto:
            default:
                ok = installVulkanHook();   // если есть libvulkan
                ok = installEglHook() || ok;
                break;
        }
        if (!ok) LOGW("no present hook installed");
    }

private:
    zygisk::Api* api = nullptr;
    JNIEnv* env = nullptr;
    bool targetProcess = false;
};

// Companion: работает в root-контексте. Здесь только локальные файлы, без сети.
static void companionHandler(int fd) {
    // При необходимости отдавать приложению файлы/конфиг, недоступные
    // из sandbox. Сейчас — заглушка.
    (void)fd;
}

REGISTER_ZYGISK_MODULE(CleanFgModule)
REGISTER_ZYGISK_COMPANION(companionHandler)
