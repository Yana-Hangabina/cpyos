#!/usr/bin/env python3
"""Guard the project-wide license and the JJWXC support notice."""

from __future__ import annotations

import hashlib
import os
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / "ports" / "metalio_plus"
CODE_SUFFIXES = {".c", ".h", ".cc", ".cpp", ".hpp", ".py", ".sh", ".cmake", ".txt"}
PERSONAL_STUDY_MARK = "LicenseRef-Kiikoread-Personal-Study"
PERSONAL_STUDY_HEADER = re.compile(
    r"SPDX-License-Identifier:\s*" + re.escape(PERSONAL_STUDY_MARK))


class LicenseScopeTest(unittest.TestCase):
    def test_project_license_is_custom_and_noncommercial(self) -> None:
        text = (ROOT / "LICENSE").read_text(encoding="utf-8")
        self.assertIn("CPYOS PROJECT LICENSE 1.0", text)
        self.assertIn("not an open-source license", text.lower())
        self.assertIn("Commercial Use", text)
        self.assertIn("JJWXC Component", text)

    def test_mit_notice_is_preserved(self) -> None:
        path = ROOT / "LICENSES" / "MIT.txt"
        self.assertTrue(path.is_file())
        self.assertIn("MIT License", path.read_text(encoding="utf-8"))

    def test_jjwxc_notice_states_the_boundaries(self) -> None:
        text = (ROOT / "NOTICE-JJWXC.md").read_text(encoding="utf-8")
        self.assertIn("不侵入", text)
        self.assertIn("著作权法", text)
        self.assertIn("设备绑定密钥", text)
        self.assertIn("未购买", text)

    def test_readmes_link_the_license_and_notice(self) -> None:
        for name in ("README.md", "README.en.md"):
            text = (ROOT / name).read_text(encoding="utf-8")
            self.assertIn("LICENSE", text)
            self.assertIn("NOTICE-JJWXC.md", text)
            self.assertIn("CPYOS Project License 1.0", text)

    def test_third_party_notices_keep_the_split(self) -> None:
        text = (ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        self.assertIn("CPYOS Project License 1.0", text)
        self.assertIn("JJWXC component", text)
        self.assertIn("Metalio E-INK4 Plus", text)
        self.assertIn("LICENSES/MIT.txt", text)

    def test_plus_managed_components_are_attributed(self) -> None:
        # The Plus port links stacks that are not vendored: they must still be
        # named here, or the distributed firmware would carry unattributed code.
        text = (ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        for name in ("esp_tinyusb", "TinyUSB", "NimBLE", "esp_http_server"):
            self.assertIn(name, text)

    def test_personal_study_is_acknowledged_not_adopted(self) -> None:
        # The user asked to take inspiration from the Personal Study license.
        # The notices must say so while making clear no text was copied and the
        # binding terms are only the CPYOS license.
        notices = (ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        self.assertIn("drafted independently", notices)
        self.assertIn("does not adopt that license", notices)
        license_text = (ROOT / "LICENSE").read_text(encoding="utf-8")
        self.assertNotIn("Personal Study", license_text)

    def test_plus_vendored_components_are_attributed(self) -> None:
        # Every third-party component directory compiled into a Plus image needs
        # an entry; first-party cpyos_* components are covered by the project and
        # upstream CrossMux notices.
        vendored = {
            "ArduinoJson": "ArduinoJson",
            "elf_loader": "elf_loader",
            "expat_lib": "Expat",
            "freeink_font": "FreeType",
            "imgcodecs": "JPEGDEC",
            "iot_eth": "iot_eth",
            "pubsubclient": "PubSubClient",
            "qrcode": "QRCode",
            "uart_uhci": "uart_uhci",
            "uzlib_lib": "uzlib",
        }
        text = (ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        components = ROOT / "ports" / "metalio_plus" / "components"
        for directory, notice in vendored.items():
            self.assertTrue((components / directory).is_dir(), directory)
            self.assertIn(notice, text, directory)
        for path in components.iterdir():
            if path.is_dir() and not path.name.startswith("cpyos_"):
                self.assertIn(path.name, vendored, f"unattributed component {path.name}")


    def test_ported_kiikoread_files_keep_their_apache_notice(self) -> None:
        # Apache-2.0 requires the notice to travel with the derived files.
        ported = [
            "lib/Pinyin/PinyinIme.h",
            "lib/Pinyin/PinyinTables.h",
            "lib/hal/ShakePageTurnGate.h",
            "freeink-sdk/libs/display/EpdiyLcd/src/e0470_page_turn/e0470_page_turn.c",
            "freeink-sdk/libs/display/EpdiyLcd/src/e0470_page_turn/e0470_page_turn.h",
            "freeink-sdk/libs/display/EpdiyLcd/src/e0470/e0470_epaper_waveform.c",
            "freeink-sdk/libs/display/EpdiyLcd/src/e0470/include/e0470_epaper_waveform.h",
            "freeink-sdk/libs/display/EpdiyLcd/src/EpdiyLcd.cpp",
        ]
        for relative in ported:
            path = ROOT / relative
            self.assertTrue(path.is_file(), relative)
            text = path.read_text(encoding="utf-8", errors="replace")
            self.assertIn("Apache-2.0", text, relative)
            self.assertTrue("read-pico-reader" in text or "kiikoread" in text, relative)
        notices = (ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        for work in ("water-ripple page-turn engine", "pinyin search", "shake page-turn",
                     "gamma cover LUT"):
            self.assertIn(work, notices)

    def test_port_contains_no_personal_study_code(self) -> None:
        # The Metalio Plus port is an independent implementation; no file may
        # carry the non-OSI kiikoread Personal-Study header.
        offenders = []
        for path in PORT.rglob("*"):
            if not path.is_file() or path.suffix not in CODE_SUFFIXES:
                continue
            if "build" in path.parts or "managed_components" in path.parts:
                continue
            if PERSONAL_STUDY_HEADER.search(path.read_bytes()[:4096].decode("utf-8", "replace")):
                offenders.append(path.relative_to(ROOT).as_posix())
        self.assertEqual(offenders, [])

    def test_no_kiikoread_file_is_copied_verbatim(self) -> None:
        # Optional local check: the reference checkout is not part of the repo.
        # Vendored upstream code under boards/metalio47/src/components is not
        # kiikoread-original and is excluded; everything else must differ.
        tree = Path(os.environ.get(
            "KIIKOREAD_RC110_DIR",
            ROOT.parents[1] / ".scratch" / "metalio-research" / "rpr-rc110"))
        if not tree.is_dir():
            self.skipTest(f"no kiikoread checkout at {tree}")
        protected = set()
        personal_study = 0
        for base in ("boards/metalio47", "components/metalio_hw"):
            for path in (tree / base).rglob("*"):
                if not path.is_file():
                    continue
                rel = path.relative_to(tree).as_posix()
                if rel.startswith("boards/metalio47/src/components/"):
                    continue  # vendored upstream display code
                protected.add(hashlib.sha256(path.read_bytes()).hexdigest())
                if PERSONAL_STUDY_MARK in path.read_bytes()[:4096].decode("utf-8", "replace"):
                    personal_study += 1
        self.assertGreaterEqual(personal_study, 11)
        port_hashes = {
            hashlib.sha256(path.read_bytes()).hexdigest()
            for path in PORT.rglob("*")
            if path.is_file() and "build" not in path.parts and "managed_components" not in path.parts
        }
        self.assertFalse(protected & port_hashes,
                         "a kiikoread file was copied verbatim into the port")


if __name__ == "__main__":
    unittest.main()
