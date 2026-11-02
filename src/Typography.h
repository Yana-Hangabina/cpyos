#pragma once

#include <cstdint>

class GfxRenderer;

// One type scale for both chrome and user content. The scale is applied by
// rebinding existing UI font IDs to the family's point sizes; call sites keep
// asking for Body/Title/Caption through the familiar UI_10/UI_12/SMALL ids, so
// a title in the library and a title in a system page cannot drift apart.
namespace typography {

enum class Role : uint8_t { Caption, Body, Title, Display };
enum class Scale : uint8_t { Compact = 0, Standard = 1, Large = 2, ExtraLarge = 3, Count = 4 };

struct Points {
  uint8_t caption;
  uint8_t body;
  uint8_t title;
  uint8_t display;
};

Points pointsFor(Scale scale);

// Rebind UI ids to the matching vector faces. No-op when the system font pack
// is absent (ReadPico keeps its built-in faces until an external vector pack is
// available).
void apply(GfxRenderer& renderer, uint8_t scaleIndex);

}  // namespace typography
