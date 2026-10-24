/**
 * @file    gpu/depth_probe.h
 * @brief   MGR dev: read back a depth target at chosen points of the draw-dump
 *          frame and log value statistics, to see what the depth test compares
 *          against.
 */
#pragma once

#include <rex/types.h>

namespace mgrn::gpu {

struct GuestTexture;
struct VideoState;

// With state().mutex held and the command list open: records a copy of ds's
// depth aspect. Up to 16 per frame; later calls are ignored.
void RecordDepthProbeLocked(VideoState &s, GuestTexture *ds, const char *tag);

// Present, after submitting the frame's list: when probes were recorded,
// waits for the list and logs [depth-probe] statistics for each.
void FinishDepthProbes(VideoState &s, u32 frame_slot);

} // namespace mgrn::gpu
