#pragma once

class GfxRenderer;

// Small high-contrast badge used to tell standby/light-sleep/deep-sleep apart
// when the user supplies custom artwork (or when a background cache keeps the
// device awake). Drawn in logical UI coordinates; the global dark mode inverts
// it together with the rest of the frame.
void drawSleepStateBadge(const GfxRenderer& renderer, const char* text, bool topLeft = false,
                         bool bottomLeft = false);
