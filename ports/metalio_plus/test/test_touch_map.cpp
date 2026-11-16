// Host tests for the portrait touch coordinate mapping.
#include "touch_map.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
// Inverse used by GfxRenderer::tapToLogical for portrait on this panel.
float logicalX(float ny) { return 683.0f - ny * 683.0f; }
float logicalY(float nx) { return nx * 1215.0f; }
}  // namespace

int main() {
    using plus_touch::portraitToPanel;
    float nx = 0;
    float ny = 0;

    portraitToPanel(0, 0, nx, ny);
    check(std::fabs(nx - 0.0f) < 1e-6f && std::fabs(ny - 1.0f) < 1e-6f, "top-left -> (0,1)");
    check(std::fabs(logicalX(ny) - 0.0f) < 0.01f && std::fabs(logicalY(nx) - 0.0f) < 0.01f,
          "top-left round-trips to (0,0)");

    portraitToPanel(683, 0, nx, ny);
    check(std::fabs(logicalX(ny) - 683.0f) < 0.01f && std::fabs(logicalY(nx) - 0.0f) < 0.01f,
          "top-right round-trips to (683,0)");

    portraitToPanel(0, 1215, nx, ny);
    check(std::fabs(logicalX(ny) - 0.0f) < 0.01f && std::fabs(logicalY(nx) - 1215.0f) < 0.01f,
          "bottom-left round-trips to (0,1215)");

    portraitToPanel(683, 1215, nx, ny);
    check(std::fabs(logicalX(ny) - 683.0f) < 0.01f && std::fabs(logicalY(nx) - 1215.0f) < 0.01f,
          "bottom-right round-trips to (683,1215)");

    portraitToPanel(341, 607, nx, ny);
    check(std::fabs(logicalX(ny) - 341.0f) < 0.6f && std::fabs(logicalY(nx) - 607.0f) < 0.6f,
          "centre round-trips");

    portraitToPanel(-10, -10, nx, ny);
    check(nx == 0.0f && ny == 1.0f, "negative clamps");
    portraitToPanel(9999, 9999, nx, ny);
    check(nx == 1.0f && ny == 0.0f, "oversize clamps");

    if (failures == 0) std::printf("touch map: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
