#!/usr/bin/env python3
"""Host regression for the two gates in scripts/flash_pdf_xip.py.

esptool only proves bytes landed.  The incident this test guards against is a
flash that verified and then never booted usefully, which the host reported as
success.  classify_boot() must therefore fail loudly on a missing PSRAM heap, a
reset loop, an empty console and a boot that never reaches its first frame --
while still passing a real good boot.  The ROM banner shape here is copied from
device_log/live.log, not invented.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from flash_pdf_xip import classify_boot  # noqa: E402

GOOD = """ESP-ROM:esp32s3-20210327
rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)
[RDP] SC7A20H WHO_AM_I=0x11 ok
[446] [INF] [MAIN] Hardware detect: read_pico
[493] [SD] SDMMC mounted at 40000 kHz (1-bit)
[820] [INF] [SFP] system font pack ready (4-bit coverage)
[1712] [INF] [BOOT] boot 1712ms: boot 43 board 584 sd 632 panel 2722 frame 1712
[2010] [INF] [PDFM] engine loaded
"""

NO_PSRAM = """[283] [ERR] [HYPH] no PSRAM headroom for a 26943 byte trie
[678] [ERR] [I18N] no PSRAM for the 1088-byte pack index
[679] [INF] [I18N] language pack PSRAM allocation failed; 0 language(s) in PSRAM
"""

PANIC = "[518] [INF] [SYS] Dumped panic info to SD card\n"

LOOP = "".join(
    "ESP-ROM:esp32s3-20210327\n"
    "rst:0xc (RTC_SW_CPU_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)\n"
    "load:0x403c8700,len:0xaf4\n"
    "entry 0x403c8898\n"
    for _ in range(4)
)

# Captured verbatim from device_log/live.log on 2026-11-19, when the board's
# PSRAM stopped initialising.  `PSRAM free=0` is a different shape from the
# allocator complaints above, and epdiy's "cache line size is set to 32" appears
# on healthy boots too -- so the first must be a fault and the second must not.
NO_PSRAM_HW = """[702] [INF] [DISP] PSRAM free=0 maxBlock=0 internal free=161760 maxBlock=73716 before panel init
E (701) epdiy: cache line size is set to 32 (< 64B)! This will degrade performance, please update this option in menuconfig.
E (720) EPDiy: Highlevel framebuffer allocation failed
"""

# Same session, a few lines later: the card reader is timing out too.  Two
# independent 3V3 loads failing together is the supply, and the gate has to say
# so, because "rebuild the firmware" is the expensive wrong conclusion here.
SD_TIMEOUT = "E (2589) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107\n"
BOTH_HW = NO_PSRAM_HW + SD_TIMEOUT

HEALTHY_HW = """[796] [INF] [DISP] PSRAM free=7994208 maxBlock=7864308 internal free=164583 maxBlock=106484 before panel init
E (701) epdiy: cache line size is set to 32 (< 64B)! This will degrade performance, please update this option in menuconfig.
[1240] [INF] [DISP] panel 1216x684 (152 bytes/row, 103968 B)
[1712] [INF] [BOOT] boot 1712ms: boot 43 board 584 sd 632 panel 2722 frame 1712
"""

checks = 0
failures = 0


def expect(name, text, must_have, must_not=()):
    global checks, failures
    checks += 1
    faults = classify_boot(text)
    joined = " | ".join(faults)
    bad = [key for key in must_have if key not in joined]
    good = [key for key in must_not if key in joined]
    if faults and must_have == []:
        bad = ["(unexpected faults on a healthy boot)"]
    if bad or good:
        failures += 1
        print(f"FAIL {name}: got [{joined}]")
    else:
        print(f"ok   {name}: [{joined or 'no faults'}]")


expect("healthy boot raises nothing", GOOD, [], ["no PSRAM", "reset loop", "first frame", "panicked"])
expect("missing PSRAM heap is a fault", NO_PSRAM, ["no PSRAM heap"])
expect("panic dump is a fault", PANIC + "frame 1200\n", ["previous boot panicked"])
expect("empty console is a fault", "", ["no serial output"])
expect("whitespace console is a fault", "   \n\r\n", ["no serial output"])
expect("boot without a first frame is a fault", GOOD.split("[BOOT]")[0], ["never reached the first frame"])
expect("reset loop is a fault", LOOP, ["reset loop", "never reached the first frame"])
expect("two resets are not yet a loop", LOOP.split("ESP-ROM")[0] + "ESP-ROM" + LOOP.split("ESP-ROM")[1] + "[BOOT] boot 900ms: x",
       [], ["reset loop"])
expect("PSRAM fault wins over silence", NO_PSRAM, ["no PSRAM heap"], ["no serial output"])
# The hardware/power shape, captured from a real log: `PSRAM free=0` is not the
# allocator's wording, so the first rule alone would have called this boot fine.
expect("PSRAM free=0 is a fault", NO_PSRAM_HW, ["no PSRAM heap", "framebuffer"])
expect("healthy hardware line raises nothing", HEALTHY_HW, [], ["no PSRAM", "framebuffer"])
expect("SDMMC timeout alone is a fault", SD_TIMEOUT + "[BOOT] boot 900ms: x\n", ["SDMMC timed out"])
expect(
    "PSRAM + SDMMC together tells the operator to power-cycle, and names no cause",
    BOTH_HW,
    ["no PSRAM heap", "SDMMC timed out", "power-cycle"],
    ["3V3", "hardware", "supply"],
)
expect(
    "SDMMC timeout without a PSRAM fault does not escalate to the power-cycle advice",
    SD_TIMEOUT + GOOD,
    ["SDMMC timed out"],
    ["power-cycle"],
)
expect(
    "epdiy cache-line noise alone is not a fault",
    "E (701) epdiy: cache line size is set to 32 (< 64B)!\n[BOOT] boot 900ms: x\n",
    [],
    ["no PSRAM", "framebuffer"],
)

print(f"\npdf flash gates: {checks} checks, {failures} failures")
sys.exit(1 if failures else 0)
