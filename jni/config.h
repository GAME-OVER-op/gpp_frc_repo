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
    bool present_bridge = false;  // Stage 1: inject duplicate presents (frame-gen bridge)
    bool debug = false;
    // Single system generation path: adaptive blend with current-frame
    // reactivity and cheap motion-softening. These are kept tunable only while
    // we tune picture quality; later they can become internal constants.
    float blend_alpha = 0.52f;      // base blend on static scenes
    float diff_threshold = 0.055f;  // luma-diff motion sensitivity
    float diff_softness = 0.18f;    // smoothstep width above threshold
    float motion_strength = 0.85f;  // 0..1 how hard motion kills blend
    int blur_radius = 1;            // 0 = off; box-blur radius in motion zones
    // GLES diagnostic modes for compatibility debugging:
    // 0=framegen, 1=passthrough, 2=capture_only, 3=draw_only,
    // 4=double_present, 5=current_only.
    int gles_debug_mode = 0;
    bool interop_bench = false;     // Stage 2: run one-shot Vulkan<->GL interop benchmark
    bool extrap_bench = false;      // Stage 2: run one-shot glExtrapolateTex2DQCOM probe
    bool extrap_eval = false;       // Stage 2: objective ME prediction eval (logcat only)
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
