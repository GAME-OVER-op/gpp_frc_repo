#pragma once
// Objective, file-free ME quality test: given three real consecutive frames
// F0,F1,F2, predict F2 from (F0,F1) via glExtrapolateTex2DQCOM and log the
// prediction error vs the duplicate-frame baseline |F1-F2|. Also sweeps
// scaleFactor to calibrate the right value. All results go to logcat.
#include <cstdint>
#include <vector>

namespace cleanfg {

void runExtrapEval(std::vector<uint8_t> f0, std::vector<uint8_t> f1,
                   std::vector<uint8_t> f2, uint32_t width, uint32_t height);

}  // namespace cleanfg
