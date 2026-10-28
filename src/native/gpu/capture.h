/**
 * @file    gpu/capture.h
 * @brief   MGR: read back the presented guest front buffer for the scripted
 *          test harness (MGR_CAPTURE_DIR), standing in for the SDK presenter's
 *          CaptureGuestOutput.
 */
#pragma once

#include <cstdint>
#include <vector>
#include <string>

#include <rex/types.h>

namespace plume {
class RenderCommandList;
}

namespace mgrn::gpu {

struct GuestTexture;
struct VideoState;

struct CapturedFrame {
  u32 width = 0;
  u32 height = 0;
  std::vector<u8> rgba;  // R8 G8 B8 A8, tightly packed rows
};

// Harness thread: asks the next Present to copy its source, and blocks until
// it has (true) or the timeout passes (false).
bool CaptureNextPresent(CapturedFrame &out, u32 timeout_ms);

// Present, with state().mutex held, after recording the present pass:
// records the copy when a capture is pending.
void RecordCaptureCopy(VideoState &s, GuestTexture *source);
// Present, after submitting the list that holds the copy: waits for it and
// hands the pixels to the waiting harness thread.
// Dev: queue a per-draw colour target copy, completed after submission.
void RecordDrawCapture(VideoState &s, GuestTexture *source, const std::string &path);
void FinishCapture(VideoState &s, u32 frame_slot);

} // namespace mgrn::gpu
