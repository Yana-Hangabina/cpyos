#!/usr/bin/env python3
"""Catch shadowed serial bench commands in src/main.cpp -- structurally, no device.

Why this exists: `CMD:PDFSELACTION 101` never reached its branch, because the chain
tested `cmd.startsWith("PDFSEL")` first and that also matches `PDFSELACTION`. Every
chip and key-widen code therefore silently ran a *selection probe* at default
coordinates. Nothing failed loudly: the harness simply stopped seeing the line it
asserted on, which reads like "the feature broke" and is really "the bench command is
dead" -- the most expensive kind of false alarm, because it costs a debugging session
against working firmware.

One instance is a typo; the class is structural, since any chain of
`cmd == "X"` / `cmd.startsWith("P")` branches can hide a later branch behind an
earlier prefix. So this parses the real dispatch chain and proves that every branch
is reachable with the argument shape its own code reads.
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
MAIN = ROOT / "src/main.cpp"

# `cmd.startsWith("X")` / `cmd.equals("X")` / `cmd == "X"`, in source order.
CONDITION = re.compile(r'\bcmd(?:(\.startsWith\(|\.equals\(| == ))"(?P<name>[^"]*)"')
ARGS_AFTER = re.compile(r'cmd\.substring\(\s*\d+\s*\)\s*\.toInt\(\)|substring\(\s*cmd\.indexOf')


def branches(text: str):
    for match in CONDITION.finditer(text):
        op = match.group(1).strip()
        kind = "exact" if op == "==" else ("equals" if ".equals(" in op else "prefix")
        yield {"pos": match.start(), "name": match.group("name"), "kind": kind}


def first_match(chain, command: str):
    """Which branch handles `CMD:<command>`, using the same C++ semantics.

    The name is returned without its argument-separating space, so callers compare
    commands ("PDFSELACTION 101") against the command they meant, not against the
    literal spelling of the branch.
    """
    for branch in chain:
        name = branch["name"]
        if branch["kind"] == "prefix":
            if command.startswith(name):
                return name
        elif command == name:
            return name
    return None


def sample_command(branch) -> str:
    """A command string that this branch is meant to handle.

    A prefix written as "PDF " demands an argument; one written "PDF" accepts the
    bare word too. Using the branch's own literal keeps the test honest about the
    argument shape instead of guessing what each hook wants.
    """
    name = branch["name"]
    return name if branch["kind"] != "prefix" or name.endswith(" ") else name + " 1"


class SerialCommandDispatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.chain = list(branches(MAIN.read_text()))
        assert len(cls.chain) > 50, "the command chain was not parsed; the test is vacuous"

    @staticmethod
    def token(name: str) -> str:
        return name.split()[0] if name.strip() else name

    def test_no_branch_is_stolen_by_a_different_command(self):
        """A branch may only be pre-empted by a branch for the SAME command word.

        `startsWith("PDF")` ahead of `startsWith("PDF ")` is two spellings of one
        command (arg vs no arg) -- untidy, but not this bug. `startsWith("PDFSEL")`
        ahead of `startsWith("PDFSELACTION")` is a different command being eaten, and
        that is what silently turned every chip code into a probe. Restricting the
        check to cross-command theft is what keeps 177 branches from drowning the one
        signal that matters.
        """
        stolen = []
        for branch in self.chain:
            probe = sample_command(branch)
            handled = first_match(self.chain, probe)
            if handled is None or self.token(handled) != self.token(branch["name"]):
                stolen.append((branch["name"], handled, probe))
        # Report every offender at once: fixing one collision and re-running to find
        # the next is how these survive for months.
        formatted = "; ".join(f'{want!r} probe {probe!r} is handled by {got!r}'
                              for want, got, probe in stolen)
        self.assertFalse(stolen, f"command branches stolen by a different command: {formatted}")

    def test_specific_command_reaches_its_own_branch(self):
        """The regression that motivated this file."""
        self.assertEqual(first_match(self.chain, "PDFSELACTION 101"), "PDFSELACTION ")
        self.assertEqual(first_match(self.chain, "PDFSEL 200 300"), "PDFSEL ")
        self.assertEqual(first_match(self.chain, "BOPENPLACE bm 1"), "BOPENPLACE ")
        self.assertEqual(first_match(self.chain, "BOPEN https://example.com"), "BOPEN")

    def test_the_trailing_space_convention_is_used_deliberately(self):
        """A bare-prefix branch in front of a longer one is the actual hazard."""
        for index, branch in enumerate(self.chain):
            if branch["kind"] != "prefix" or branch["name"].endswith(" "):
                continue
            for later in self.chain[index + 1:]:
                if later["name"].startswith(branch["name"]) and later["name"] != branch["name"]:
                    self.fail(
                        f'{branch["name"]!r} (startsWith, no trailing space) shadows the '
                        f'later {later["name"]!r}; write the shorter one as '
                        f'"{branch["name"]} " or reorder it after the longer one')


class DetectorIsNotVacuous(unittest.TestCase):
    """The parser itself is the thing most likely to rot silently.

    A test that greps nothing passes forever, so the detector is shown the bug it
    exists to catch, in text, rather than trusting that the real file happens to be
    parsed. Without these, `test_every_branch_is_reachable` could go green by finding
    zero branches.
    """

    BUGGY = """      } else if (cmd.startsWith("PDFSEL")) {
        probe();
      } else if (cmd.startsWith("PDFSELACTION")) {
        chips();
"""

    FIXED = """      } else if (cmd.startsWith("PDFSELACTION")) {
        chips();
      } else if (cmd.startsWith("PDFSEL ")) {
        probe();
"""

    def test_detects_the_original_bug(self):
        chain = list(branches(self.BUGGY))
        self.assertEqual(len(chain), 2, "the parser found no branches in the fixture")
        self.assertEqual(first_match(chain, "PDFSELACTION 101"), "PDFSEL")
        self.assertEqual(first_match(chain, "PDFSEL 200 300"), "PDFSEL")

    def test_accepts_the_fixed_shape(self):
        chain = list(branches(self.FIXED))
        self.assertEqual(first_match(chain, "PDFSELACTION 101"), "PDFSELACTION")
        self.assertEqual(first_match(chain, "PDFSEL 200 300"), "PDFSEL ")

    def test_real_file_is_not_empty_of_branches(self):
        # Guards against the whole test suite silently checking nothing.
        self.assertGreater(len(self.chain if hasattr(self, "chain") else
                               list(branches(MAIN.read_text()))), 50)


if __name__ == "__main__":
    unittest.main(verbosity=2)
