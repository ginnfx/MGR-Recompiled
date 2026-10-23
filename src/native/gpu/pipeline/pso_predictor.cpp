/**
 * @file    gpu/pipeline/pso_predictor.cpp
 * @brief   MGR: load-time PSO prediction is not implemented. re:Blue's
 *          predictor walks Blue Dragon's model/technique tables; MGR keeps the
 *          interface so the recorder links, and every query reports "not
 *          predicted".
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/pipeline/pso_predictor.h"

namespace mgrn::gpu {

void OnModelTechniqueKnown(u32, bool) {}
void OnLoadModelCreated(u32, u32) {}
void OnLoadBegin(u32) {}
void OnLoadEnd() {}
void OnDeclRegistered(u32, u8) {}
void ReemitPredictions() {}
bool IsPairPredicted(u64, u64) { return false; }
std::string DescribePair(u64, u64) { return {}; }

} // namespace mgrn::gpu
