"""The allocation ledger is either wired or it is nothing.

`-DCROSSPOINT_MEMORY_LEDGER=1` compiles the __wrap_ definitions; the linker flags
are what make the allocator call them. Each half alone is silent: without the
macro the wrappers do not exist, and without the flags the wrappers exist but no
call reaches them -- either way `CMD:MEMLEDGER` prints an empty table that reads
like an idle device, which is the exact failure shape this module exists to
remove.

The suite also checks the reverse direction: a --wrap= flag naming a symbol with
no __wrap_ definition would leave every allocation of that kind calling
__real_<sym> through a wrapper that does not exist, i.e. a link error far from
its cause.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
INI = REPO / "platformio.ini"
LEDGER = REPO / "lib" / "Memory" / "MemoryLedger.cpp"
MACRO = "-DCROSSPOINT_MEMORY_LEDGER=1"


def build_flag_blocks(text: str) -> list[tuple[str, list[str]]]:
    """[(section, [flag lines])] for every build_flags block in the ini."""
    blocks: list[tuple[str, list[str]]] = []
    section = ""
    current: list[str] | None = None
    for line in text.splitlines():
        if line.startswith("[") and line.endswith("]"):
            if current is not None:
                blocks.append((section, current))
                current = None
            section = line.strip("[]")
        elif line.startswith("build_flags"):
            if current is not None:
                blocks.append((section, current))
            current = []
        elif current is not None:
            if line.strip() == "" or re.match(r"^\w", line):
                blocks.append((section, current))
                current = None
                if line.strip() == "":
                    continue
            else:
                current.append(line.strip())
    if current is not None:
        blocks.append((section, current))
    return blocks


def wrapped_symbols(flags: list[str]) -> set[str]:
    found: set[str] = set()
    for flag in flags:
        if not flag.startswith("-Wl,--wrap="):
            continue
        for part in flag.split(","):
            if part.startswith("--wrap="):
                found.add(part[len("--wrap="):])
    return found


def defined_wrappers(text: str) -> set[str]:
    """The __wrap_ entry points the ledger actually defines, read off the
    definitions rather than a hand-kept list that can drift from them."""
    return {m for m in re.findall(r"^(?:void|void\*) __wrap_(\w+)", text, re.M)}


class LedgerWiring(unittest.TestCase):
    """The pairing rule is checked against the ledger's own symbols only: the tree
    wraps panic_abort and xTaskCreatePinnedToCore for unrelated reasons, and a gate
    that demanded the macro beside those would reject code that has nothing to do
    with memory accounting."""

    def setUp(self) -> None:
        self.ini = INI.read_text(encoding="utf-8")
        self.cpp = LEDGER.read_text(encoding="utf-8")
        self.blocks = build_flag_blocks(self.ini)
        self.symbols = defined_wrappers(self.cpp)

    def test_definitions_exist(self) -> None:
        # The real tree must parse into something: renaming the file or the macro
        # would otherwise turn this whole gate green by finding nothing.
        self.assertIn(MACRO, self.ini, "no env defines the ledger macro")
        self.assertTrue(self.symbols, "MemoryLedger.cpp defines no __wrap_ functions")
        self.assertIn("malloc", self.symbols)
        self.assertIn("heap_caps_aligned_alloc", self.symbols)

    def test_macro_and_ledger_wraps_travel_together(self) -> None:
        wired_anywhere = False
        for section, flags in self.blocks:
            has_macro = MACRO in flags
            ledger_wraps = wrapped_symbols(flags) & self.symbols
            if not has_macro and not ledger_wraps:
                continue
            self.assertTrue(
                has_macro,
                f"[{section}] wraps allocator entry points without defining the ledger macro: "
                f"{sorted(ledger_wraps)}",
            )
            self.assertEqual(
                ledger_wraps,
                self.symbols,
                f"[{section}] defines the ledger macro but wraps only part of the allocator: "
                f"missing {sorted(self.symbols - ledger_wraps)}",
            )
            wired_anywhere = True
        self.assertTrue(wired_anywhere, "no build_flags block wires the ledger at all")

    def test_no_wrapper_is_left_unreachable(self) -> None:
        used: set[str] = set()
        for _, flags in self.blocks:
            used |= wrapped_symbols(flags)
        unreachable = sorted(self.symbols - used)
        self.assertEqual(unreachable, [], f"__wrap_ definitions no linker flag reaches: {unreachable}")

    def test_bad_shapes_are_detected(self) -> None:
        good = "[env:x]\nbuild_flags =\n  " + MACRO + "\n  -Wl,--wrap=malloc,--wrap=free\n"
        blocks = build_flag_blocks(good)
        self.assertEqual(len(blocks), 1)
        self.assertEqual(wrapped_symbols(blocks[0][1]), {"malloc", "free"})
        # Macro without flags: nothing is recorded, and the dump would read as an
        # idle device.
        macro_only = build_flag_blocks("[env:x]\nbuild_flags =\n  " + MACRO + "\n")
        self.assertEqual(wrapped_symbols(macro_only[0][1]) & {"malloc", "free"}, set())
        # Flags without the macro: the wrappers are not compiled, so the linker
        # resolves __real_* and no accounting happens.
        flags_only = build_flag_blocks("[env:x]\nbuild_flags =\n  -Wl,--wrap=malloc\n")
        self.assertNotIn(MACRO, flags_only[0][1])
        # A partial wrap list is the third silent shape: malloc tracked, free not,
        # so the live total only ever grows.
        partial = wrapped_symbols(build_flag_blocks(good)[0][1]) & {"malloc", "free", "heap_caps_malloc"}
        self.assertNotEqual(partial, {"malloc", "free", "heap_caps_malloc"})
        # An unrelated wrap must not be pulled into the pairing rule.
        other = build_flag_blocks("[env:y]\nbuild_flags =\n  -Wl,--wrap=panic_abort\n")
        self.assertEqual(wrapped_symbols(other[0][1]) & {"malloc", "free"}, set())


if __name__ == "__main__":
    unittest.main()
