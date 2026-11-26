// Guest-side texture swap for a retail UI sprite. The retail Options UI's controller
// diagram (outer UI node 9, widget vtable 0x821CBF14) samples the first texture of
// the shared core.wtb: 512x512 tiled DXT5 (see research notes 2026-11-26). Writing
// our own blocks over that texture's guest memory changes what the sprite shows;
// the original bytes are kept and put back when the page closes.
#pragma once

#include "mgr_picture_codec.h"

#include <cstdint>
#include <vector>

namespace mgr_options_sprite {

class SpriteTexture {
 public:
  // `widget` is the sprite widget's guest address. Returns false (and attaches
  // nothing) unless its texture is a loaded 512x512 DXT5 WTB texture.
  bool Attach(uint8_t* base, uint32_t widget);
  // Overwrites the blocks inside `region` with `rgba` (512x512, straight alpha).
  void Write(uint8_t* base, const std::vector<uint8_t>& rgba, mgr_pictures::Region region);
  // Puts the original blocks of every written region back and forgets the texture.
  // If the WTB buffer no longer carries its magic (freed or reused memory) nothing
  // is written and the saved state is dropped.
  void Restore(uint8_t* base);
  // Drops the saved state without touching guest memory.
  void Forget();
  bool Attached() const { return data_ != 0; }

 private:
  void DumpTexture(const char* path) const;

  uint32_t buffer_ = 0;  // WTB buffer; its magic proves the texture memory is still ours
  uint32_t data_ = 0;
  std::vector<uint8_t> original_;
  std::vector<mgr_pictures::Region> written_;
};

}  // namespace mgr_options_sprite
