#!/usr/bin/env python3
"""Keep the documented serial console in sync with the implementation.

README lists every console command the bring-up session may rely on.  A command
that is documented but not implemented (or one that exists but is missing from
`help`) is a dead end on a unit with no debugger, so both directions are checked.
"""

import re
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parent
README = PORT / "README.md"
SHELL = PORT / "main" / "shell_demo.cpp"
BRINGUP = PORT / "scripts" / "bringup_serial.py"


def documented_commands(text: str) -> set[str]:
    block = text.split("serial bring-up console", 1)[1].split("```", 2)[1]
    commands = set()
    for line in block.splitlines():
        head = re.split(r"\s{2,}", line.strip(), maxsplit=1)[0]
        # Only "cmd | alt" (spaced pipe) is a command alternative; "on|off" is
        # an argument list, not two commands.
        for part in head.split(" | "):
            token = part.strip().split()[0] if part.strip() else ""
            if re.fullmatch(r"[a-z][a-z0-9]*", token):
                commands.add(token)
    return commands


def implemented_commands(source: str) -> set[str]:
    return set(re.findall(r'strcmp\(command, "([a-z0-9]+)"\)', source))


class ConsoleDocsTest(unittest.TestCase):
    def setUp(self):
        self.documented = documented_commands(README.read_text(encoding="utf-8"))
        self.source = SHELL.read_text(encoding="utf-8")
        self.implemented = implemented_commands(self.source)

    def test_readme_commands_are_implemented(self):
        missing = sorted(self.documented - self.implemented)
        self.assertEqual(missing, [], f"documented but not implemented: {missing}")

    def test_help_lists_implemented_commands(self):
        help_text = self.source.split("void consoleHelp()", 1)[1].split("}", 1)[0]
        missing = sorted(cmd for cmd in self.implemented - {"help"} if f" {cmd}" not in help_text)
        self.assertEqual(missing, [], f"implemented but absent from help: {missing}")

    def test_bringup_script_commands_are_implemented(self):
        steps = BRINGUP.read_text(encoding="utf-8").split("STEPS = [", 1)[1].split("]", 1)[0]
        commands = set(re.findall(r'\("([a-z0-9]+)",', steps))
        self.assertIn("slpinfo", commands)
        missing = sorted(commands - self.implemented)
        self.assertEqual(missing, [], f"bring-up script uses unknown commands: {missing}")


if __name__ == "__main__":
    unittest.main()
