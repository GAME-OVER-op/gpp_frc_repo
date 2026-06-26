#pragma once
#include <string>
#include <vector>

namespace cleanfg {

enum class Mode { Auto, Gles, Vulkan };
enum class Method { Blend, Extrapolate };

struct Config {
    std::vector<std::string> target_packages;
    Mode mode = Mode::Auto;
    Method method = Method::Blend;
    int multiplier = 2;
    int max_fps = 0;              // 0 = auto / OS clamps to panel
    bool elevate_rate = true;     // request 120/144/etc via ANativeWindow_setFrameRate
    bool force_swap_interval_0 = true;
    bool debug = false;
    bool matchesPackage(const char* name) const;
};

extern Config g_config;
// Direct file read (root context only, e.g. companion). Fails from app processes
// because /data/adb is not accessible to unprivileged UIDs.
bool loadConfig(const char* path);
// Read+parse config from an fd to EOF. Works for a module-dir openat fd or a
// companion socket fd. Preferred from the app process.
bool loadConfigFromFd(int fd);
}
