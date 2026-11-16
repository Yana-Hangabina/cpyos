#pragma once

namespace freeink {

// A desktop simulator cannot inspect a physical e-ink controller.
inline bool applyXteinkDisplayController() { return false; }

}  // namespace freeink
