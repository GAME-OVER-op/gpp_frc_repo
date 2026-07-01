#include "config.h"
#include "log.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <utility>

namespace cleanfg {

Config g_config;

static void applyInternalDefaultsKeepingTargets(std::vector<std::string>&& targets) {
    g_config = Config{};
    g_config.target_packages = std::move(targets);
    // Production is intentionally self-tuning. cleanfg.prop should only carry
    // target_packages; all legacy/tuning keys are ignored so stale configs cannot
    // force GLES/Vulkan/debug/experimental modes or low-quality parameters.
    g_config.mode = Mode::Auto;
    g_config.method = Method::Blend;
    g_config.multiplier = 2;
    g_config.max_fps = 0;
    g_config.elevate_rate = true;
    g_config.force_swap_interval_0 = true;
    g_config.present_bridge = true;
    g_config.debug = false;
    g_config.blend_alpha = 0.52f;
    g_config.diff_threshold = 0.055f;
    g_config.diff_softness = 0.18f;
    g_config.motion_strength = 0.85f;
    g_config.blur_radius = 1;
    g_config.gles_debug_mode = 0;
    g_config.interop_bench = false;
    g_config.extrap_bench = false;
    g_config.extrap_eval = false;
}

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

// Parse config text (in-memory). Resets g_config to defaults first so repeated
// loads (e.g. fd fallback then file) do not accumulate duplicate targets.
static void parseConfigText(const std::string& content) {
    std::vector<std::string> targets;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        std::string line = content.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? content.size() : nl + 1;

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
                if (!item.empty()) targets.push_back(item);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
    }

    applyInternalDefaultsKeepingTargets(std::move(targets));

    LOGI("config loaded: %zu target(s), internal mode=%d mult=%d max_fps=%d method=%d elevate=%d swap0=%d pbridge=%d debug=%d",
         g_config.target_packages.size(), (int)g_config.mode, g_config.multiplier,
         g_config.max_fps, (int)g_config.method, g_config.elevate_rate,
         g_config.force_swap_interval_0, g_config.present_bridge, g_config.debug);
    LOGI("blend params: alpha=%.3f diff_thr=%.3f diff_soft=%.3f motion=%.3f blur=%d",
         g_config.blend_alpha, g_config.diff_threshold, g_config.diff_softness,
         g_config.motion_strength, g_config.blur_radius);
    LOGI("gles_debug_mode=%d", g_config.gles_debug_mode);
    LOGI("interop_bench=%d", g_config.interop_bench);
    LOGI("extrap_bench=%d", g_config.extrap_bench);
    LOGI("extrap_eval=%d", g_config.extrap_eval);
}

// Read an entire fd to EOF and parse it. Works for both a regular file fd
// (openat on the module dir fd) and a companion socket fd (root process streams
// the file then closes). Returns true if any bytes were read.
bool loadConfigFromFd(int fd) {
    if (fd < 0) return false;
    std::string data;
    char buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) data.append(buf, (size_t)n);
    if (data.empty()) {
        LOGW("config fd produced no data");
        return false;
    }
    parseConfigText(data);
    return true;
}

// Direct file read. Only works from a root context (e.g. the Zygisk companion),
// since /data/adb is not accessible to unprivileged app processes.
bool loadConfig(const char* path) {
    FILE* f = fopen(path, "re");
    if (!f) {
        LOGW("config not found / not readable: %s", path);
        return false;
    }
    std::string data;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.empty()) return false;
    parseConfigText(data);
    return true;
}

}  // namespace cleanfg
