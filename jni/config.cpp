#include "config.h"
#include "log.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace cleanfg {

Config g_config;
static bool g_loaded = false;

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

bool Config::matchesPackage(const char* name) const {
    if (!name) return false;
    if (target_packages.empty()) return false;  // ничего не хукаем по умолчанию
    for (const auto& p : target_packages) {
        if (p == "*" || p == "all") return true;
        if (p == name) return true;
    }
    return false;
}

bool loadConfig(const char* path) {
    if (g_loaded) return true;
    g_loaded = true;
    FILE* f = fopen(path, "re");
    if (!f) {
        LOGW("config not found: %s", path);
        return false;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(s.substr(0, eq));
        std::string val = trim(s.substr(eq + 1));

        if (key == "target_packages") {
            size_t start = 0;
            while (start < val.size()) {
                size_t comma = val.find(',', start);
                std::string item = trim(val.substr(
                    start, comma == std::string::npos ? std::string::npos : comma - start));
                if (!item.empty()) g_config.target_packages.push_back(item);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else if (key == "mode") {
            if (val == "gles") g_config.mode = Mode::Gles;
            else if (val == "vulkan") g_config.mode = Mode::Vulkan;
            else g_config.mode = Mode::Auto;
        } else if (key == "multiplier") {
            g_config.multiplier = atoi(val.c_str());
            if (g_config.multiplier < 1) g_config.multiplier = 1;
        } else if (key == "method") {
            g_config.method = (val == "extrapolate" || val == "reproject") ? Method::Extrapolate : Method::Blend;
        } else if (key == "max_fps") {
            g_config.max_fps = atoi(val.c_str());
            if (g_config.max_fps < 0) g_config.max_fps = 0;
        } else if (key == "elevate_rate") {
            g_config.elevate_rate = (val == "1" || val == "true");
        } else if (key == "force_swap_interval_0") {
            g_config.force_swap_interval_0 = (val == "1" || val == "true");
        } else if (key == "debug") {
            g_config.debug = (val == "1" || val == "true");
        }
    }
    fclose(f);
    LOGI("config loaded: %zu target(s), mode=%d mult=%d max_fps=%d method=%d elevate=%d swap0=%d debug=%d",
         g_config.target_packages.size(), (int)g_config.mode, g_config.multiplier,
         g_config.max_fps, (int)g_config.method, g_config.elevate_rate,
         g_config.force_swap_interval_0, g_config.debug);
    return true;
}

}  // namespace cleanfg
