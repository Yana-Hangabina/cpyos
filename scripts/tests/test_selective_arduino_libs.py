"""Keep the selective Arduino library trim safe: no BT switch, no used library dropped."""

import configparser
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Arduino core wrappers this firmware never includes: storage, network,
# provisioning and video. Bluetooth is deliberately absent from this list.
TRIMMED_LIBRARIES = (
    "ArduinoOTA",
    "EEPROM",
    "ESP_Video",
    "Ethernet",
    "FFat",
    "HTTPUpdate",
    "LittleFS",
    "Matter",
    "NetBIOS",
    "OpenThread",
    "PPP",
    "SD",
    "SD_MMC",
    "SPIFFS",
    "Ticker",
    "WiFiProv",
    "Zigbee",
)

# Anything that would turn Bluetooth off breaks the BLE HID host on every
# hardware target, so it must never appear in this tree.
FORBIDDEN = (
    "CONFIG_BT_ENABLED=n",
    "CONFIG_BT_BLUEDROID_ENABLED=y",
    "CONFIG_ARDUINO_SELECTIVE_BLE=n",
    "CONFIG_ARDUINO_SELECTIVE_BluetoothSerial=n",
    "CONFIG_ARDUINO_SELECTIVE_SimpleBLE=n",
)

INCLUDE_PATTERN = re.compile(r'#\s*include\s*[<"]([^>"]+)[>"]')


def resolve(config, section, key):
    value = config[section].get(key)
    if value is None:
        for parent in config[section].get("extends", "").split(","):
            if parent.strip():
                inherited = resolve(config, parent.strip(), key)
                if inherited:
                    return inherited
        return ""
    return re.sub(r"\$\{([^{}]+)\.([^{}.]+)\}", lambda match: resolve(config, *match.groups()) or "", value)


class SelectiveArduinoLibrariesTest(unittest.TestCase):
    def setUp(self):
        self.config = configparser.ConfigParser(interpolation=None)
        self.config.read(ROOT / "platformio.ini")

    def test_base_profile_trims_only_the_unused_libraries(self):
        sdkconfig = resolve(self.config, "firmware_tuned", "custom_sdkconfig")
        self.assertIn("CONFIG_ARDUINO_SELECTIVE_COMPILATION=y", sdkconfig)
        for library in TRIMMED_LIBRARIES:
            with self.subTest(library=library):
                self.assertIn(f"CONFIG_ARDUINO_SELECTIVE_{library}=n", sdkconfig)

    def test_bluetooth_is_never_turned_off(self):
        for path in [ROOT / "platformio.ini", *(ROOT / "scripts").rglob("*.py")]:
            if path.name == "test_selective_arduino_libs.py":
                continue
            text = path.read_text(errors="ignore")
            for marker in FORBIDDEN:
                with self.subTest(path=str(path.relative_to(ROOT)), marker=marker):
                    self.assertNotIn(marker, text)
        # Both Bluetooth host profiles still enable the controller.
        for section in ("s3_ble_controller", "c3_ble"):
            with self.subTest(section=section):
                self.assertIn("CONFIG_BT_ENABLED=y", resolve(self.config, section, "custom_sdkconfig"))

    def test_trimmed_headers_are_not_included_anywhere(self):
        roots = [ROOT / "src", ROOT / "lib", ROOT / "freeink-sdk" / "libs"]
        headers = {f"{library}.h" for library in TRIMMED_LIBRARIES}
        headers.add("esp_video.hpp")
        offenders = []
        for root in roots:
            if not root.is_dir():
                continue
            for path in root.rglob("*"):
                if path.suffix not in (".c", ".cc", ".cpp", ".h", ".hh", ".hpp") or not path.is_file():
                    continue
                try:
                    text = path.read_text(errors="ignore")
                except OSError:
                    continue
                for include in INCLUDE_PATTERN.findall(text):
                    name = include.rsplit("/", 1)[-1]
                    if name in headers:
                        offenders.append(f"{path.relative_to(ROOT)}: {include}")
        self.assertEqual(offenders, [], "Re-enable the Arduino wrapper or stop including it")

    def test_zigbee_components_stay_out_of_the_core(self):
        removed = resolve(self.config, "firmware_tuned", "custom_component_remove")
        self.assertIn("espressif/esp-zboss-lib", removed)
        self.assertIn("espressif/esp-zigbee-lib", removed)


if __name__ == "__main__":
    unittest.main()
