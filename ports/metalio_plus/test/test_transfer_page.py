#!/usr/bin/env python3
"""The native transfer page must stay wired without linking the servers."""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
COMPONENT = ROOT / "ports" / "metalio_plus" / "components" / "cpyos_app"
MAIN = ROOT / "ports" / "metalio_plus" / "main"
MANAGER = ROOT / "src" / "activities" / "ActivityManager.cpp"


class TransferPageTest(unittest.TestCase):
    def test_activity_lives_with_the_activity_layer(self):
        for name in ("plus_transfer_activity.h", "plus_transfer_activity.cpp",
                     "plus_transfer_hooks.h", "plus_transfer_hooks.cpp"):
            self.assertTrue((COMPONENT / name).is_file(), name)

    def test_activity_uses_hooks_not_the_servers(self):
        source = (COMPONENT / "plus_transfer_activity.cpp").read_text(encoding="utf-8")
        self.assertIn("plus_transfer::start()", source)
        self.assertIn("plus_transfer::stop()", source)
        self.assertIn("plus_transfer::status()", source)
        self.assertIn("plus_transfer::usbStart()", source)
        self.assertNotIn('#include "web_transfer.h"', source)
        self.assertNotIn('#include "ws_upload.h"', source)

    def test_shell_registers_the_hooks(self):
        source = (MAIN / "shell_demo.cpp").read_text(encoding="utf-8")
        self.assertIn("plus_transfer::setHooks(", source)
        self.assertIn("ws_upload::status()", source)
        self.assertIn("usb_msc::start()", source)

    def test_manager_dispatches_to_the_page_on_this_port(self):
        source = MANAGER.read_text(encoding="utf-8")
        self.assertIn('#include "plus_transfer_activity.h"', source)
        # Both the File Transfer and Join Network entries land on the page.
        self.assertGreaterEqual(source.count("replaceActivityWith<PlusTransferActivity>()"), 2)
        self.assertNotIn("File transfer is not available on this port yet", source)


if __name__ == "__main__":
    unittest.main()
