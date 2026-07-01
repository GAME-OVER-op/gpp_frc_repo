#include "backend_select.h"
#include "config.h"
#include "log.h"
#include <atomic>
#include <time.h>

namespace cleanfg {
namespace {

std::atomic<int> g_backend{(int)RuntimeBackend::Unknown};
std::atomic<int64_t> g_lastVkCandidateNs{0};

static int64_t nowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static const int64_t g_processStartNs = nowNs();

bool forcedAllows(RuntimeBackend backend) {
    if (g_config.mode == Mode::Vulkan) return backend == RuntimeBackend::Vulkan;
    if (g_config.mode == Mode::Gles) return backend == RuntimeBackend::Gles;
    return true;
}

} // namespace

const char* backendName(RuntimeBackend backend) {
    switch (backend) {
        case RuntimeBackend::Gles: return "gles";
        case RuntimeBackend::Vulkan: return "vulkan";
        default: return "unknown";
    }
}

void noteVulkanCandidate() {
    g_lastVkCandidateNs.store(nowNs(), std::memory_order_relaxed);
}

bool hasRecentVulkanCandidate(int windowMs) {
    int64_t t = g_lastVkCandidateNs.load(std::memory_order_relaxed);
    if (t <= 0) return false;
    return (nowNs() - t) < (int64_t)windowMs * 1000000LL;
}

RuntimeBackend activeBackend() {
    return (RuntimeBackend)g_backend.load(std::memory_order_acquire);
}

bool isBackendActive(RuntimeBackend backend) {
    return activeBackend() == backend;
}

bool isBackendBlocked(RuntimeBackend backend) {
    RuntimeBackend active = activeBackend();
    return active != RuntimeBackend::Unknown && active != backend;
}

bool tryActivateBackend(RuntimeBackend backend, const char* reason) {
    if (!forcedAllows(backend)) return false;

    // In auto mode, do not let early Android UI/EGL work steal the backend from
    // a Vulkan renderer. Vulkan present is decisive and can activate instantly;
    // GLES activates after a short startup grace or when no Vulkan candidate has
    // appeared recently.
    if (g_config.mode == Mode::Auto && backend == RuntimeBackend::Gles) {
        const int64_t ageMs = (nowNs() - g_processStartNs) / 1000000LL;
        if (ageMs < 1200 || hasRecentVulkanCandidate(3000)) return false;
    }

    int expected = (int)RuntimeBackend::Unknown;
    int desired = (int)backend;
    if (g_backend.compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
        LOGI("auto backend selected: %s (%s)", backendName(backend), reason ? reason : "present");
        return true;
    }
    return expected == desired;
}

} // namespace cleanfg
