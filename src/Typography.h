#pragma once

#include <cstddef>
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

// One UI role and the point size the pack binds it to.
struct UiRolePoint {
  int fontId;
  uint8_t pointSize;
};
inline constexpr std::size_t kMaxUiRolePoints = 8;

// The roles the vector pack binds for a scale, in attachment order. A Font
// Extras supplement has to be built at exactly these sizes: attaching one at
// 14pt while the pack draws the row at 10pt renders the rare glyph at the wrong
// size in a second face, which is what a font_extras user notices first.
// Returns 0 on targets that keep their built-in UI faces (there the SD UI-size
// table is what the supplements follow).
std::size_t uiRolePointsFor(Points points, UiRolePoint* out, std::size_t capacity);

// Rebind UI ids to the matching vector faces. No-op when the system font pack
// is absent (ReadPico keeps its built-in faces until an external vector pack is
// available).
void apply(GfxRenderer& renderer, uint8_t scaleIndex);

}  // namespace typography
