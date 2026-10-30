#include <HalDisplay.h>
#include <HalGPIO.h>

#if FREEINK_DEVICE_READPICO
#include <BoardReadPico.h>
#include <EpdiyLcd.h>
#include <HalPowerManager.h>
#include <Logging.h>
#endif

// Global HalDisplay instance
HalDisplay display;

namespace {
uint32_t s_grayScans = 0;
}  // namespace

uint32_t HalDisplay::takeGrayScanCount() {
  const uint32_t out = s_grayScans;
  s_grayScans = 0;
  return out;
}


#define SD_SPI_MISO 7

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

HalDisplay::Controller HalDisplay::getController() const { return BoardConfig::ACTIVE.displayController; }

void HalDisplay::begin(bool seamless) {
  // Set X3-specific panel mode before initializing.
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }
#if FREEINK_DEVICE_READPICO
  // Precondition, not a gate: the SY7636A rails (EN P0.3, VCOM_EN P0.4), the EPD
  // output enable (XOE P0.1) and the PGOOD sense (P0.5) all live behind the
  // FCA9555, so the board must be up before the driver's power hooks can sequence
  // anything (BoardReadPico::epdPowerOn -> read-pico.md 1.4). HalGPIO::begin()
  // brings the expander up first; if it never answered, say so loudly and keep
  // going — the panel will simply stay dark instead of aborting the boot.
  if (!BoardReadPico::ready()) {
    LOG_ERR("DISP", "FCA9555 expander is not up; EPD rails cannot be sequenced");
  }
  // epdiy owns 4bpp front/back/difference buffers in PSRAM (~1.59 MiB here),
  // plus the SDK's B/W base and selector planes. Feed queues, DMA buffers and
  // renderer tasks need internal RAM; track free space and largest blocks.
  LOG_INF("DISP", "PSRAM free=%u maxBlock=%u internal free=%u maxBlock=%u before panel init",
          static_cast<unsigned>(ESP.getFreePsram()), static_cast<unsigned>(ESP.getMaxAllocPsram()),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
#endif
#if FREEINK_DEVICE_MURPHY_M4
  einkDisplay.setMurphyM4Batch(gpio.murphyM4Batch());
#endif

  einkDisplay.begin();

#if FREEINK_DEVICE_READPICO
  // The facade publishes the driver's real geometry (EpdiyLcdDriver::geometry():
  // 1216 x 684 -> 152 bytes/row -> 103,968 bytes for Read Pico). Every consumer
  // must read it from here; see the constants note in HalDisplay.h.
  LOG_INF("DISP", "panel %ux%u (%u bytes/row, %u-byte framebuffer); PSRAM free=%u maxBlock=%u internal free=%u",
          static_cast<unsigned>(getDisplayWidth()), static_cast<unsigned>(getDisplayHeight()),
          static_cast<unsigned>(getDisplayWidthBytes()), static_cast<unsigned>(getBufferSize()),
          static_cast<unsigned>(ESP.getFreePsram()), static_cast<unsigned>(ESP.getMaxAllocPsram()),
          static_cast<unsigned>(ESP.getFreeHeap()));
#endif

  if (seamless) {
    // Defuse the SDK's X3 _x3InitialFullSyncsRemaining counter (no-op on X4)
    // so the first paint isn't promoted to FULL (~770ms). Skips the wakeup-
    // gated requestResync() below for the same reason.
    einkDisplay.skipInitialResync();
    return;
  }
  // Request resync after specific wakeup events to ensure clean display state.
  const auto wakeupReason = gpio.getWakeupReason();
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
      wakeupReason == HalGPIO::WakeupReason::Other) {
    einkDisplay.requestResync();
  }
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

namespace {
EInkDisplay::RefreshContext convertRefreshContext(DisplayRefreshContext context) {
  switch (context) {
    case DisplayRefreshContext::Normal:
      return EInkDisplay::RefreshContext::Normal;
    case DisplayRefreshContext::ContinuousReading:
      return EInkDisplay::RefreshContext::ContinuousReading;
    case DisplayRefreshContext::TextOnlyAntiAliasing:
      return EInkDisplay::RefreshContext::TextOnlyAntiAliasing;
    case DisplayRefreshContext::ImageReading:
      return EInkDisplay::RefreshContext::ImageReading;
    case DisplayRefreshContext::TransientUi:
      // Same waveform as Normal; the distinction lives only in GfxRenderer's
      // sweep counter, which skips TransientUi pushes.
      return EInkDisplay::RefreshContext::Normal;
  }
  return EInkDisplay::RefreshContext::Normal;
}

#if FREEINK_DEVICE_READPICO
// A full-frame scan consumes one line every 16 us. At LOW_POWER_FREQ the two
// line feeders cannot keep that pace on this board (measured ~32 us/line
// combined), so the frame underruns, is reported failed and is retried as a
// clean GC16 -- a visible flash on every push. Interactive turns already run at
// 240 MHz because the input event raises the clock, but background scans do
// not: the standby clock face and the reader's minute clock tick both draw
// while the device is idle at 80 MHz. Holding a power lock for the duration of
// the scan guarantees the feeders run at full speed regardless of who asked
// for the push. The lock is quiet because a concurrent real lock (network
// request) already implies full speed.
struct ScanPowerGuard {
  HalPowerManager::Lock lock{true};
};
#else
struct ScanPowerGuard {
  // User-provided ctor keeps the guard non-trivial, so -Wunused-variable stays quiet.
  ScanPowerGuard() {}
};
#endif
}  // namespace

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen, DisplayRefreshContext context) {
  const ScanPowerGuard power;
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen, convertRefreshContext(context));
}

void HalDisplay::displayBufferAsync(HalDisplay::RefreshMode mode, DisplayRefreshContext context) {
  const ScanPowerGuard power;
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayBufferAsyncNoShadow(convertRefreshMode(mode), convertRefreshContext(context));
}

void HalDisplay::waitRefreshComplete() { einkDisplay.waitRefreshComplete(); }

bool HalDisplay::supportsAsyncRefresh() const { return einkDisplay.supportsAsyncRefresh(); }

HalDisplay::GrayscaleCapabilities HalDisplay::grayscaleCapabilities(GrayscaleMode mode) const {
  return einkDisplay.grayscaleCapabilities(mode);
}

bool HalDisplay::supportsAsyncGrayscaleBase() const { return grayscaleCapabilities().asyncBase; }

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  const ScanPowerGuard power;
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::setInverted(bool inverted) { einkDisplay.setInverted(inverted); }

bool HalDisplay::toggleInverted() { return einkDisplay.toggleInverted(); }

bool HalDisplay::isInverted() const { return einkDisplay.isInverted(); }

void HalDisplay::deepSleep() { einkDisplay.deepSleep(); }

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

void HalDisplay::syncFrameBufferFromActive() { einkDisplay.syncWriteBufferFromActive(); }

uint8_t* HalDisplay::lendFrameBufferStorage(uint32_t* sizeOut) { return einkDisplay.lendBuildStorage(sizeOut); }

void HalDisplay::returnFrameBufferStorage() { einkDisplay.returnBuildStorage(); }

bool HalDisplay::displayGrayscaleBase(GrayscaleMode mode, RefreshMode fallback, bool turnOffScreen) {
  ++s_grayScans;
  const ScanPowerGuard power;
  if (gpio.deviceIsX3() && fallback == HALF_REFRESH) einkDisplay.requestResync();
  return einkDisplay.displayGrayscaleBase(mode, static_cast<EInkDisplay::RefreshMode>(fallback), turnOffScreen);
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOffScreen, DisplayRefreshContext context) {
  ++s_grayScans;
  const ScanPowerGuard power;
  // X3: a HALF fallback means the caller wants a clean base (e.g. the sleep
  // cover, a full-screen swap from arbitrary prior content). Without this, the
  // X3 grayscale base takes its gentle differential happy path and the prior
  // home/reader frame ghosts through the soft aa_pre_bw_mid waveform. Forcing a
  // resync makes displayGrayscaleBase clear first, matching displayBuffer(HALF).
  // The reader's FAST path is deliberately left on the differential path so
  // per-page grayscale stays cheap.
  if (gpio.deviceIsX3() && fallback == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  einkDisplay.displayGrayscaleBase(convertRefreshMode(fallback), turnOffScreen, convertRefreshContext(context));
}

void HalDisplay::preconditionGrayscale() { einkDisplay.preconditionGrayscale(); }

void HalDisplay::preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  einkDisplay.preconditionGrayscale(x, y, w, h);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) {
  ++s_grayScans;
  const ScanPowerGuard power;
  einkDisplay.displayGrayBuffer(turnOffScreen);
}

bool HalDisplay::waitForScanIdle(uint32_t timeoutMs) {
#if FREEINK_DEVICE_READPICO
  const uint32_t start = millis();
  while (freeink::epdiyLcdScanInProgress()) {
    if (millis() - start >= timeoutMs) {
      LOG_ERR("HAL", "present barrier timed out after %ums", static_cast<unsigned>(timeoutMs));
      return false;
    }
    delay(1);
  }
  return true;
#else
  (void)timeoutMs;
  return true;
#endif
}

void HalDisplay::takePanelPushStats(uint32_t* frames, uint64_t* us) {
#if FREEINK_DEVICE_READPICO
  freeink::epdiyLcdTakePushStats(frames, us);
#else
  if (frames != nullptr) *frames = 0;
  if (us != nullptr) *us = 0;
#endif
}

bool HalDisplay::idlePowerTick(const uint32_t nowMs) {
#if FREEINK_DEVICE_READPICO
  if (freeink::epdiyLcdScanInProgress()) return false;
  // No ScanPowerGuard here: this runs on EVERY main-loop pass, and the guard
  // forces 240 MHz. Paired with the loop's idle downclock it produced a 240/80
  // flip every ~60 ms while the device was doing nothing (measured 2026-10-30).
  // The only real work below is dropping the rails 8 s after the last push,
  // which is a few I2C writes and is fine at 80 MHz; the next push has its own
  // guard.
  return einkDisplay.idlePowerTick(nowMs);
#else
  (void)nowMs;
  return false;
#endif
}

bool HalDisplay::cleanWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  const ScanPowerGuard power;
#if FREEINK_DEVICE_READPICO
  if (freeink::epdiyLcdCleanArea(getFrameBuffer(), x, y, w, h)) return true;
  ESP_LOGE("HAL", "area clean refused; falling back to a full clean");
#endif
  einkDisplay.displayBuffer(convertRefreshMode(FULL_REFRESH), false, EInkDisplay::RefreshContext::Normal);
  return false;
}

void HalDisplay::displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen) {
  const ScanPowerGuard power;
  einkDisplay.displayWindow(x, y, w, h, turnOffScreen);
}

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  einkDisplay.writeGrayscalePlaneStrip(lsbPlane ? EInkDisplay::GRAY_PLANE_LSB : EInkDisplay::GRAY_PLANE_MSB, rows,
                                       yStart, numRows);
}

bool HalDisplay::supportsStripGrayscale() const { return grayscaleCapabilities().stripUploads; }

bool HalDisplay::combinesGrayscaleBase() const { return grayscaleCapabilities().base == GrayscaleBase::Combined; }

bool HalDisplay::supportsTextOnlyCombinedBase() const { return einkDisplay.supportsTextOnlyCombinedBase(); }
bool HalDisplay::supportsReaderTransitions() const { return einkDisplay.supportsReaderTransitions(); }
bool HalDisplay::supportsContinuousImageReading() const { return einkDisplay.supportsContinuousImageReading(); }
bool HalDisplay::canUseTextTransition() const { return einkDisplay.canUseTextTransition(); }

void HalDisplay::cancelGrayscale() {
  if (!einkDisplay.combinesGrayscaleBase()) return;
  einkDisplay.abortPostRefresh();
  // Discard staged planes and re-arm the next render without touching the glass.
  einkDisplay.beginDisplayWork();
}

void HalDisplay::setPageTurnDirection(int direction) { einkDisplay.setPageTurnDirection(direction); }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }
uint8_t HalDisplay::getGrayscaleLevels() const { return einkDisplay.getGrayscaleLevels(); }
uint8_t* HalDisplay::beginGrayscale16() { return einkDisplay.beginGrayscale16(); }
uint8_t* HalDisplay::beginGrayscale16From1bpp(const uint8_t* fb) {
  return einkDisplay.beginGrayscale16From1bpp(fb);
}
bool HalDisplay::commitGrayscale16(bool clean) {
  ++s_grayScans;
  const ScanPowerGuard power;
  return einkDisplay.commitGrayscale16(clean);
}
bool HalDisplay::commitGrayscale16Area(const uint8_t* bwProxy, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                       bool clean) {
  ++s_grayScans;
  const ScanPowerGuard power;
  (void)bwProxy;
  return einkDisplay.commitGrayscale16Area(x, y, w, h, clean);
}
bool HalDisplay::commitGrayscale16Areas(const freeink::GrayArea* areas, int count, bool clean) {
  ++s_grayScans;
  const ScanPowerGuard power;
  return einkDisplay.commitGrayscale16Areas(areas, count, clean);
}
void HalDisplay::cancelGrayscale16() { einkDisplay.cancelGrayscale16(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
