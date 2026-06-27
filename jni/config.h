#pragma once
#include <string>
#include <vector>
#include <atomic>

namespace cleanfg {

enum class Mode { Auto, Gles, Vulkan };
enum class Method { Blend, Extrapolate };

struct Config {
    std::vector<std::string> target_packages;
    // The actual package name of the current process, set once it is matched in
    // preAppSpecialize. With a multi-app target list we must remember WHICH app
    // we are running inside (the vendor engine connects per package), instead of
    // assuming the first entry of target_packages.
    std::string current_package;
    Mode mode = Mode::Auto;
    Method method = Method::Blend;
    int multiplier = 2;
    // Auto mode: number of main-surface GLES swaps to wait before the EGL path
    // may claim the engine. Gives a Vulkan swapchain (the preferred engine) a
    // chance to appear first, so games that show a short GLES splash before
    // switching to Vulkan still end up on the Vulkan present path. The Vulkan
    // path claims immediately and always wins the tie.
    int warmup_frames = 120;
    int max_fps = 0;              // 0 = auto / OS clamps to panel
    bool elevate_rate = true;     // request 120/144/etc via ANativeWindow_setFrameRate
    bool force_swap_interval_0 = true;
    bool present_bridge = false;  // Stage 1: inject duplicate presents (frame-gen bridge)
    bool debug = false;
    // Stage 2B adaptive blend knobs (tunable live via cleanfg.prop)
    float blend_alpha = 0.5f;       // base blend on static scenes
    float diff_threshold = 0.08f;   // luma-diff motion sensitivity
    float diff_softness = 0.12f;    // smoothstep width above threshold
    float motion_strength = 1.0f;   // 0..1 how hard motion kills blend
    int blur_radius = 0;            // 0 = off; box-blur radius in motion zones
    // --- Stage 2C: motion-compensated interpolation (real frame generation) ---
    bool mc_interp = false;   // enable motion-compensated interpolation (needs present_bridge=1, Vulkan path)
    int  mc_tile = 16;        // flow tile size in px (smaller = finer motion, more GPU)
    int  mc_search = 24;      // max motion search radius in px
    int  mc_levels = 4;       // coarse-to-fine search steps
    float mc_occl = 0.20f;    // occlusion sensitivity: luma-diff above which the vector is distrusted
    bool interop_bench = false;     // Stage 2: run one-shot Vulkan<->GL interop benchmark
    bool extrap_bench = false;      // Stage 2: run one-shot glExtrapolateTex2DQCOM probe
    bool extrap_eval = false;       // Stage 2: objective ME prediction eval (logcat only)
    bool matchesPackage(const char* name) const;
};

extern Config g_config;

// Auto engine arbitration. In Mode::Auto both the EGL and Vulkan present hooks
// are installed; the first present path that actually fires in this process
// "claims" the engine so the other path stays a pass-through. This is how the
// module auto-detects the display technique (GLES vs Vulkan) per app.
//   0 = undecided, 1 = GLES/EGL, 2 = Vulkan
extern std::atomic<int> g_activeEngine;
// Direct file read (root context only, e.g. companion). Fails from app processes
// because /data/adb is not accessible to unprivileged UIDs.
bool loadConfig(const char* path);
// Read+parse config from an fd to EOF. Works for a module-dir openat fd or a
// companion socket fd. Preferred from the app process.
bool loadConfigFromFd(int fd);
}
