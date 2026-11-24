#!/usr/bin/env python3
"""The private JJWXC component must stay wired for local dev and out of git."""

import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CMAKE = ROOT / "ports" / "metalio_plus" / "components" / "cpyos_app" / "CMakeLists.txt"
README = ROOT / "ports" / "metalio_plus" / "README.md"


class JjwxcWiringTest(unittest.TestCase):
    def setUp(self):
        self.cmake = CMAKE.read_text(encoding="utf-8")

    def test_opt_in_is_guarded_and_fails_loudly(self):
        self.assertIn("if(DEFINED ENV{CPYOS_PLUS_JJWXC})", self.cmake)
        self.assertIn("FATAL_ERROR", self.cmake)
        self.assertIn("CPYOS_JJWXC_DIR}", self.cmake)
        self.assertIn("app/*.cpp", self.cmake)
        self.assertIn("protocol/src/*.cpp", self.cmake)
        self.assertIn("CROSSPOINT_BOOKSOURCE_JJWXC=1", self.cmake)
        self.assertIn("IS_DIRECTORY", self.cmake)

    def test_default_build_does_not_require_the_private_tree(self):
        # The guarded block must come after the early return and must not be an
        # unconditional glob (a missing tree is only fatal when opted in).
        head = self.cmake.split("set(CPYOS_JJWXC_DIR", 1)[0]
        self.assertIn("if(NOT DEFINED ENV{CPYOS_ACTIVITY_PROBE})", head)
        self.assertNotIn("CPYOS_PLUS_JJWXC)", head.split("idf_component_register(", 1)[0])

    def test_private_tree_is_gitignored(self):
        check = subprocess.run(
            ["git", "-C", str(ROOT), "check-ignore", "-q", "src/jjwxc/app/JjwxcActivity.cpp"],
            capture_output=True, text=True, check=False)
        self.assertEqual(check.returncode, 0, "src/jjwxc must stay gitignored")

    def test_readme_documents_local_auto_enable(self):
        text = README.read_text(encoding="utf-8")
        self.assertIn("auto-enabled", text)
        self.assertIn("CPYOS_PLUS_JJWXC=0", text)
        self.assertIn("6,462,992", text)


if __name__ == "__main__":
    unittest.main()