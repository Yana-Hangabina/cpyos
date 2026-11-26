#!/usr/bin/env python3
"""Add the firmware HAL methods the native simulator shim is missing.

The `crosspoint-simulator` dependency is pinned by PlatformIO and its shim
headers lag the firmware. The simulator env cannot put an include override
ahead of the library dep, so this pre-build script patches the installed shim
headers in place (idempotent, marker `CPYOS_SIM_OVERLAY`).

Only the simulator env runs it; device toolchains never load these files.
Delete the marker blocks (or the whole file) when the dependency pin ships the
same API.
"""
import os

MARK = "// CPYOS_SIM_OVERLAY"


def patch(path, anchor, insert, present=None):
    if not os.path.isfile(path):
        print(f"[sim-shim] skip missing {path}")
        return
    text = open(path, encoding="utf-8").read()
    if present is not None:
        if present in text:
            return
    elif insert.replace("MARK", MARK) in text:
        return
    if anchor not in text:
        print(f"[sim-shim] anchor not found in {path}")
        return
    text = text.replace(anchor, anchor + "\n" + insert.replace("MARK", MARK), 1)
    open(path, "w", encoding="utf-8").write(text)
    print(f"[sim-shim] patched {path}")


def run(project, env_name):
    src = os.path.join(project, ".pio", "libdeps", env_name, "simulator", "src")

    patch(
        os.path.join(src, "BoardConfig.h"),
        "  uint8_t grayscaleLevels = 4;",
        "  // MARK: firmware board geometry for rounded UI corners.\n"
        "  uint8_t cornerRadius = 0;\n"
        "  uint8_t bottomCornerRadius = 0;",
    )
    patch(
        os.path.join(src, "WebServer.h"),
        "class WebServer;",
        "// MARK: firmware middleware hook (request/response tap).\n"
        "class Middleware {\n"
        " public:\n"
        "  using Callback = std::function<bool()>;\n"
        "};",
    )
    patch(
        os.path.join(src, "WebServer.h"),
        "  void addHandler(RequestHandler *handler);",
        "  // MARK: firmware middleware registration.\n"
        "  template <typename F> void addMiddleware(F &&) {}",
    )
    patch(
        os.path.join(src, "WebServer.h"),
        "  long clientContentLength();",
        "  // MARK: response taps the firmware server overrides.\n"
        "  virtual size_t _currentClientWrite(const char *, size_t l) { return l; }\n"
        "  virtual size_t _currentClientWrite_P(const char *, size_t l) { return l; }",
    )
    patch(
        os.path.join(src, "WebSocketsServer.h"),
        "class WebSocketsServer {",
        "#define WEBSOCKETS_SERVER_CLIENT_MAX 4",
    )
    patch(
        os.path.join(src, "WebSocketsServer.h"),
        "  void close();",
        "  // MARK: firmware connection bookkeeping.\n"
        "  bool clientIsConnected(uint8_t) { return false; }\n"
        "  void disconnect(uint8_t) {}",
    )
    patch(
        os.path.join(src, "HalPowerManager.h"),
        "  uint16_t getBatteryPercentage() const;",
        """  // MARK: firmware-only API, stubbed for the host build.
  uint16_t batteryDisplayPercent() const { return getBatteryPercentage(); }
  bool batteryPercentageKnown() const { return true; }
  bool isBatteryCharging() const { return false; }
  bool canStandbyLightSleep(const HalGPIO&) const { return false; }
  enum class LightSleepWakeReason : uint8_t { Timer, PowerButton, Failed };
  LightSleepWakeReason lightSleepFor(uint32_t) const { return LightSleepWakeReason::Timer; }
  static uint8_t lastLightSleepFailure() { return 0; }
  static int lastLightSleepError() { return 0; }
  struct BatteryTelemetry {
    bool valid = false;
    uint16_t millivolts = 0;
    uint16_t socPermille = 0;
    uint8_t chargeState = 0;
  };
  BatteryTelemetry readBatteryTelemetry() const { return {}; }
  const char* debugModeMutexHolderName() const { return "(sim)"; }""",
    )
    patch(
        os.path.join(src, "BoardConfig.h"),
        "  uint8_t grayscaleLevels = 4;",
        "  // MARK: firmware board geometry for rounded UI corners.\n"
        "  uint8_t cornerRadius = 0;\n"
        "  uint8_t bottomCornerRadius = 0;",
    )
    patch(
        os.path.join(src, "WebServer.h"),
        "class WebServer;",
        "// MARK: firmware middleware hook (request/response tap).\n"
        "class Middleware {\n"
        " public:\n"
        "  using Callback = std::function<bool()>;\n"
        "};",
    )
    patch(
        os.path.join(src, "WebServer.h"),
        "  void addHandler(RequestHandler *handler);",
        "  // MARK: firmware middleware registration.\n"
        "  template <typename F> void addMiddleware(F &&) {}",
    )
    patch(
        os.path.join(src, "WebServer.h"),
        "  long clientContentLength();",
        "  // MARK: response taps the firmware server overrides.\n"
        "  virtual size_t _currentClientWrite(const char *, size_t l) { return l; }\n"
        "  virtual size_t _currentClientWrite_P(const char *, size_t l) { return l; }",
    )
    patch(
        os.path.join(src, "WebSocketsServer.h"),
        "class WebSocketsServer {",
        "#define WEBSOCKETS_SERVER_CLIENT_MAX 4",
    )
    patch(
        os.path.join(src, "WebSocketsServer.h"),
        "  void close();",
        "  // MARK: firmware connection bookkeeping.\n"
        "  bool clientIsConnected(uint8_t) { return false; }\n"
        "  void disconnect(uint8_t) {}",
    )
    patch(
        os.path.join(src, "HalPowerManager.h"),
        "    explicit Lock();",
        "    // MARK: `quiet` only suppresses a device-only warning.\n"
        "    explicit Lock(bool) : Lock() {}",
        present="explicit Lock(bool)",
    )
    patch(
        os.path.join(src, "HalSystem.h"),
        "HeapInfo getHeapInfo();",
        "// MARK: firmware-only USB download mode (Plus); not available on host.\n"
        "inline bool supportsUsbDownloadMode() { return false; }\n"
        "inline bool rebootToUsbDownload() { return false; }",
    )
    patch(
        os.path.join(src, "HalDisplay.h"),
        "#pragma once",
        "#include <DisplayRefreshContext.h>\n"
        "namespace freeink {\n"
        "// MARK: gray area repaint descriptor used by the renderer.\n"
        "struct GrayArea { int x = 0, y = 0, w = 0, h = 0; };\n"
        "}  // namespace freeink",
    )
    patch(
        os.path.join(src, "HalDisplay.h"),
        "  void begin(bool seamless);",
        "  // MARK: firmware-only scan barrier (band/area pushes).\n"
        "  bool waitForScanIdle(int = 3000) const { return true; }\n"
        "  const char* getControllerName() const { return \"sim\"; }\n"
        "  // MARK: firmware area clean / refresh-context overloads.\n"
        "  void displayBuffer(RefreshMode, bool, ::DisplayRefreshContext) {}\n"
        "  void cleanWindow(uint16_t, uint16_t, uint16_t, uint16_t) {}",
    )
    patch(
        os.path.join(src, "SecureHttpClient.h"),
        "  int GET() { return http_.GET(); }",
        "  // MARK: response metadata the firmware reads from the shim HTTP client.\n"
        "  bool hasContentLength() const { return false; }\n"
        "  size_t getContentLength() const { return 0; }\n"
        "  int getStatus() const { return 200; }\n"
        "  String getHeader(const char *) const { return String(); }",
    )
    patch(
        os.path.join(src, "WString.h"),
        "  size_t length() const { return s.length(); }",
        "  // MARK: Arduino String API used by the firmware.\n"
        "  bool empty() const { return s.empty(); }",
    )
    patch(
        os.path.join(src, "Print.h"),
        "  virtual size_t write(uint8_t) = 0;",
        "  // MARK: Arduino Print accepts char buffers; the shim only had bytes.\n"
        "  size_t write(const char *s) { return write((const uint8_t *)s, strlen(s)); }\n"
        "  size_t write(const char *b, size_t n) { return write((const uint8_t *)b, n); }",
    )
    patch(
        os.path.join(src, "NetworkClient.h"),
        "  virtual size_t write(uint8_t c) { return write(&c, 1); }",
        "  // MARK: firmware writes String/char buffers through this shim.\n"
        "  virtual size_t write(const char *b, size_t n) { return write((const uint8_t *)b, n); }\n"
        "  size_t write(const String &s) { return write(s.c_str(), s.length()); }\n"
        "  size_t write(const char *b, int n) { return write((const uint8_t *)b, (size_t)n); }",
    )
    patch(
        os.path.join(src, "HalTiltSensor.h"),
        "  bool wasTiltedBack() { return false; }",
        "  // MARK: firmware motion gestures.\n"
        "  bool wasMotionForward() { return false; }\n"
        "  bool wasMotionBack() { return false; }",
    )
    patch(
        os.path.join(src, "HalStorage.h"),
        "  bool ready() const;",
        "  // MARK: firmware-only diagnostics.\n"
        '  const char* debugMutexHolderName() const { return "(sim)"; }',
    )


def replace_once(path, old, new):
    if not os.path.isfile(path):
        print(f"[sim-shim] skip missing {path}")
        return
    text = open(path, encoding="utf-8").read()
    if new in text:
        return
    if old not in text:
        print(f"[sim-shim] replace anchor not found in {path}")
        return
    open(path, "w", encoding="utf-8").write(text.replace(old, new, 1))
    print(f"[sim-shim] replaced in {path}")


def ensure_epdiy_header(src):
    path = os.path.join(src, "EpdiyLcd.h")
    if os.path.exists(path):
        return
    with open(path, "w", encoding="utf-8") as f:
        f.write("""#pragma once
// CPYOS_SIM_OVERLAY: firmware-only epdiy facade; the host renderer is the panel.
namespace freeink {
inline void epdiyLcdSetAaRealDiagonal(bool) {}
}  // namespace freeink
""")
    print(f"[sim-shim] wrote {path}")


def patch_all_simulator_envs(project):
    libdeps = os.path.join(project, ".pio", "libdeps")
    if not os.path.isdir(libdeps):
        print("[sim-shim] no libdeps yet; build once to install the shim")
        return
    for env_name in sorted(os.listdir(libdeps)):
        src = os.path.join(libdeps, env_name, "simulator", "src")
        if os.path.isdir(src):
            run(project, env_name)
            ensure_epdiy_header(src)
            replace_once(
                os.path.join(src, "HalDisplay.cpp"),
                "bool HalDisplay::shouldQuit() const { return quitRequested.load(); }",
                "bool HalDisplay::shouldQuit() const {\n"
                "  // CPYOS_SIM_OVERLAY: a headless capture run must keep looping even\n"
                "  // though SDL cannot create a window.\n"
                "  if (std::getenv(\"CPYOS_SIM_HEADLESS\")) return false;\n"
                "  return quitRequested.load();\n"
                "}",
            )


# PlatformIO runs pre-scripts from the project root; scanning every installed
# simulator env avoids depending on SCons-only variables.
patch_all_simulator_envs(os.getcwd())
