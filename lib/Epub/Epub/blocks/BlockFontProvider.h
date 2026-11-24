#pragma once

// Resolves a block's CSS font scale to a renderer font id. The EPUB layer has no
// knowledge of the device's font storage (system pack, SD families, embedded book
// fonts), so the reader injects an implementation; leaving it null keeps every block
// on the section's reader font, which is the pre-M2 behaviour host tests rely on.
class BlockFontProvider {
 public:
  virtual ~BlockFontProvider() = default;

  // `scale` is relative to the section's reader font (1.0 = the reader size).
  // Returns a stable, serialisable renderer font id, or 0 when no scaled face can
  // be resident (the caller then keeps the section font for that block).
  virtual int resolveBlockFont(float scale) = 0;
};
