#pragma once
// Stage 2 diagnostic: capture two REAL consecutive game frames, run the Adreno
// frame-extrapolation extension on them on a worker GL context, and write the
// inputs + extrapolated outputs to disk as PPM for visual inspection. This
// validates ME quality on real content without touching the present cadence.
#include <cstdint>
#include <vector>
#include <string>

namespace cleanfg {

// prev/cur are tightly-packed RGBA8 frames (width*4*height bytes). Runs on a
// detached worker thread; arguments are moved in.
void runExtrapDump(std::vector<uint8_t> prev, std::vector<uint8_t> cur,
                   uint32_t width, uint32_t height, std::string outDir);

}  // namespace cleanfg
