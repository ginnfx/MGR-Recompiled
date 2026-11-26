/**
 * @file    gpu/pipeline/pso_recorder.h
 * @brief   Boot-time replay of the compiled-in PSO residual and
 *          (REBLUE_PSO_CAP builds) the predictor miss capture instrument.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

#include "gpu/pipeline/pipeline_state.h"

namespace mgrn::gpu {

// Called for every PSO bound by a draw. 'builtOnRenderThread' means the draw
// compiled it synchronously: a first-encounter miss neither the compiled-in
// residual nor the predictor covered. Warns once per pipeline, and
// REBLUE_PSO_CAP builds also write the state to
// <cache>/pso_misses_<session>.csv for the cache header generator. Precompiled
// pipelines are never captured, so the dump is exactly what prediction missed.
void RecordPipelineState(const PipelineState &state, u32 renderPassId,
                         bool builtOnRenderThread);

// Precompile the generated pipeline state cache on the background pool,
// deferring each entry until its shaders/decl exist. Call once after the host
// device and main pipeline layout are ready.
void ReplayBootCache();

// Write the pending miss capture now (shutdown). No-op outside REBLUE_PSO_CAP,
// and when nothing new arrived since the last periodic flush.
void FlushPSOCapture();

// mgr_msaa or mgr_transparency_aa changed (or the device was created with MSAA
// on): synchronously compile, in parallel, the scene pipelines built so far at
// the new sample count / alpha-to-coverage state and return when they are
// cached, so the first frames after the change do not compile on demand. Runs
// on the guest thread (D3DDevice_Swap hook and device init), not the render
// thread.
void OnMSAAChanged();

// Called by the pipeline cache for every pipeline it builds: scene pipelines
// are remembered (by content hash) so OnMSAAChanged can precompile their twins.
void NoteBuiltPipeline(const PipelineState &state);

// Called right after creating a guest shader / vertex decl so a deferred replay
// entry waiting on that content hash can resolve and enqueue.
void OnShaderCreated(u64 microcodeHash);
void OnVertexDeclarationCreated(u64 elementHash);

} // namespace mgrn::gpu
