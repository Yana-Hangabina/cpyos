#pragma once
#include <stdint.h>

// HAL-owned request intent; no persistent permission crosses display calls.
//
// TransientUi is for high-frequency interactive surfaces (a keyboard repainting
// per keystroke, a slider repainting per drag frame). It presents with the exact
// same waveform as Normal -- convertRefreshContext maps it to Normal -- but it is
// deliberately excluded from GfxRenderer's periodic GC16 sweep counter, so a
// burst of keystrokes or drag frames cannot land a full black/white flash in the
// middle of the gesture. The surface is short-lived and bounded; the discrete
// Normal pushes that follow it (the screen the keyboard/slider closes into)
// resume the sweep and clear any faint GL16 residue.
enum class DisplayRefreshContext : uint8_t {
  Normal,
  ContinuousReading,
  TextOnlyAntiAliasing,
  ImageReading,
  TransientUi
};
