#pragma once

#include <GfxRenderer.h>
#include <PanelInsets.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "components/ToastQueue.h"
#include "components/UITheme.h"

// The one transient acknowledgement in the system: a black capsule at the
// bottom of the screen, one line, auto-dismissed. It is not a dialog -- it has
// no actions, it never covers the whole page, and it never carries an error the
// user has to read (errors go to a dialog or a state page).
//
// The host owns presentation: paint() draws into the current framebuffer and
// the caller presents once, exactly like every other overlay.
class Toast : public ToastQueue {
 public:
  // bottomInset is the air between the capsule and the safe area's bottom edge;
  // hosts with a key-hint strip pass enough to clear it.
  void paint(const GfxRenderer& renderer, const int fontId, const int bottomInset) const {
    if (text().empty()) return;
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
    const int maxWidth = safe.width - 96;
    // Truncate to what the capsule can hold: a long path or plugin message must
    // not draw past the black pill (it used to).
    const std::string shown = renderer.truncatedText(fontId, text().c_str(), maxWidth);
    const int textW = renderer.getTextWidth(fontId, shown.c_str());
    const int textH = renderer.getLineHeight(fontId);
    constexpr int kPillH = 56;
    const int pillW = std::min(safe.width - 48, textW + 56);
    const int pillY = safe.y + safe.height - kPillH - bottomInset;
    // The capsule's bottom edge is the row that can reach the panel's rounded
    // corners; keep its ends out of that arc. A centred pill of at most
    // safe.width - 48 already clears a 24 px cut, and radius 0 gives cut 0, so
    // the clamp never moves a square panel's toast.
    const int pillDepth = std::max(0, safe.y + safe.height - (pillY + kPillH));
    const int pillCut = panel_insets::cornerCut(UITheme::getInstance().getBottomCornerRadius(), pillDepth);
    const int minPillX = safe.x + pillCut;
    const int maxPillX = safe.x + safe.width - pillW - pillCut;
    const int pillX = std::clamp(safe.x + (safe.width - pillW) / 2, minPillX, std::max(minPillX, maxPillX));
    renderer.fillRoundedRect(pillX, pillY, pillW, kPillH, kPillH / 2, Color::Black);
    renderer.drawText(fontId, pillX + (pillW - textW) / 2, pillY + (kPillH - textH) / 2, shown.c_str(), false);
  }
};
