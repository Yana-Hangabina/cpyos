#pragma once
// The host build has no Arduino core. src/BleInput.cpp pulls <Arduino.h> exactly as
// the real SDK's BleKeyboardHost.h does; the test clock lives in HalGPIO.h so the
// two stub headers share one definition instead of disagreeing across test binaries.
#include "HalGPIO.h"
