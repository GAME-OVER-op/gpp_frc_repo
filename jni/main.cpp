// Zygisk entrypoint for gpp_frc_repo.
#include <sys/types.h>   // dev_t / ino_t, required by zygisk.hpp
#include "zygisk.hpp"
#include "config.h"
#include "log.h"
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>

namespace gpp_frc_repo {
bool installEglHook();
bool installVulkanHook();
}

using namespace gpp_frc_repo;

// Module id is "gpp_frc_repo"; the config lives next to the module.
static const char* kConfigPath = "/data/adb/modules/gpp_frc_repo/gpp_frc_repo.prop";
static const char* kConfigName = "gpp_frc_repo.prop";

class GppFrcRepoModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        // ВАЖНО: конфиг лежит в /data/adb/modules/..., куда непривилегированный
        // процесс приложения ДОСТУПА НЕ ИМЕЕТ (/data/adb — root-only, 0700).
        // Поэтому читаем через root-канал:
        //   1) getModuleDir() — fd каталога модуля, открытый демоном (root);
        //   2) connectCompanion() — fallback, companion-процесс (root) отдаёт файл;
        //   3) прямое чтение (работает только если процесс всё ещё root).
        bool loaded = false;

        int dirFd = api->getModuleDir();
        if (dirFd >= 0) {
            int cfgFd = openat(dirFd, kConfigName, O_RDONLY | O_CLOEXEC);
            if (cfgFd >= 0) {
                loaded = loadConfigFromFd(cfgFd);
                close(cfgFd);
                if (loaded) LOGI("config read via module dir fd");
            } else {
                LOGW("openat(moduleDir, %s) failed: %s", kConfigName, strerror(errno));
            }
        }

        if (!loaded) {
            int compFd = api->connectCompanion();
            if (compFd >= 0) {
                loaded = loadConfigFromFd(compFd);
                close(compFd);
                if (loaded) LOGI("config read via companion");
            }
        }

        if (!loaded) {
            loaded = loadConfig(kConfigPath);  // last resort
        }

        const char* pkg = env->GetStringUTFChars(args->nice_name, nullptr);
        targetProcess = g_config.matchesPackage(pkg);
        if (pkg) {
            if (targetProcess) LOGI("gpp_frc_repo target matched: %s", pkg);
            env->ReleaseStringUTFChars(args->nice_name, pkg);
        }

        if (!targetProcess) {
            // Не наш процесс — выгружаем библиотеку, ничего не трогаем.
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs*) override {
        if (!targetProcess) return;
        LOGI("gpp_frc_repo active in target process");

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

// Companion: работает в root-контексте. Читает конфиг (недоступный из песочницы
// приложения) и потоком отдаёт его байты в сокет, затем закрывает (→ EOF).
static void companionHandler(int fd) {
    FILE* f = fopen(kConfigPath, "re");
    if (!f) return;  // приложение получит EOF сразу → пустой конфиг
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(fd, buf + off, n - off);
            if (w <= 0) { fclose(f); return; }
            off += (size_t)w;
        }
    }
    fclose(f);
}

REGISTER_ZYGISK_MODULE(GppFrcRepoModule)
REGISTER_ZYGISK_COMPANION(companionHandler)
