#!/usr/bin/env python3
"""Gate: operator-new tracing is wired, and wired under this target's ABI.

0.8.4's report was `PNC-04` with a PC that symbolizes to the new_handler itself,
so the allocation size and site stayed unknown. lib/Diagnostics/NewTrace.cpp
records the size through `--wrap`ped operator new; this gate keeps the two sides
from drifting apart, and pins the ABI trap that already cost one build:

    on 32-bit xtensa size_t is `unsigned int`, so the symbols are `_Znwj` /
    `_Znaj`. Wrapping `_Znwm` / `_Znam` (the 64-bit spelling) links clean and
    records nothing at all -- verified against the 0.8.5 ELF (`nm | grep Znw`
    shows `_Znwj`). A silent no-op instrument is worse than none: the next crash
    report would look instrumented and still say nothing.

Checked:
  1. every `__wrap__<sym>` defined by NewTrace.cpp has `-Wl,--wrap=<sym>`;
  2. every `--wrap=` in platformio.ini that belongs to new-trace is defined;
     (the memory ledger's wraps are separate and already gated by
     test_memory_ledger_wiring.py);
  3. the wrap flags sit in the same env block as `-DCROSSPOINT_NEW_TRACE=1`, and
     that block is the readpico hardware block every readpico env inherits;
  4. the wrapped operator-new names use the 32-bit `j` mangling, not `m`.

Run: python3 -m unittest scripts.tests.test_new_trace_wiring -v
"""
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SOURCE = REPO / "lib" / "Diagnostics" / "NewTrace.cpp"
INI = REPO / "platformio.ini"

DEFINED = re.compile(r'__wrap__(\w+)\s*\(')
WRAPPED = re.compile(r'--wrap=(\w+)')


def defined_symbols() -> set[str]:
    text = SOURCE.read_text(encoding="utf-8")
    # Only the guarded block defines wrappers; the string appears in comments too,
    # so strip line comments before matching.
    code = "\n".join(line.split("//")[0] for line in text.splitlines())
    # `__wrap__Znwj` defines the symbol `_Znwj`: the underscore that ends the
    # shim's prefix is also the symbol's first character.
    return {"_" + name for name in DEFINED.findall(code)}


def ini_text() -> str:
    return INI.read_text(encoding="utf-8")


def section_containing(anchor: str) -> str:
    """The [section] block that contains `anchor`."""
    lines = ini_text().splitlines()
    start = None
    for index, line in enumerate(lines):
        if anchor in line:
            start = index
            break
    if start is None:
        return ""
    begin = start
    while begin > 0 and not lines[begin].startswith("["):
        begin -= 1
    end = start
    while end + 1 < len(lines) and not lines[end + 1].startswith("["):
        end += 1
    return "\n".join(lines[begin:end + 1])


class NewTraceWiringTest(unittest.TestCase):
    def test_every_defined_wrapper_has_a_link_flag(self):
        symbols = defined_symbols()
        self.assertTrue(symbols, "NewTrace.cpp no longer defines any wrapper; the size recording is dead")
        for symbol in symbols:
            self.assertIn(f"--wrap={symbol}", ini_text(),
                          f"{symbol} is defined but not wrapped; the compiler's `new` bypasses it")

    def test_every_new_trace_wrap_is_defined(self):
        symbols = defined_symbols()
        traces = set(WRAPPED.findall(ini_text())) & {"_Znwj", "_Znaj", "_Znwm", "_Znam"}
        self.assertEqual(traces, symbols,
                         f"platformio.ini wraps {sorted(traces)} but the source defines {sorted(symbols)}")

    def test_macro_and_flags_live_in_the_inherited_block(self):
        block = section_containing("-DCROSSPOINT_NEW_TRACE=1")
        self.assertTrue(block, "-DCROSSPOINT_NEW_TRACE=1 is not set anywhere; the recorder compiles to a no-op")
        self.assertIn("[readpico_hardware]", block,
                      "the macro must live in the block every readpico env inherits")
        self.assertIn("--wrap=_Znwj", block)
        self.assertIn("--wrap=_Znaj", block)

    def test_operator_new_names_use_the_thirty_two_bit_abi(self):
        symbols = defined_symbols()
        self.assertIn("_Znwj", symbols, "wrapped operator new must be _Znwj on this ABI")
        self.assertIn("_Znaj", symbols, "wrapped operator new[] must be _Znaj on this ABI")
        self.assertNotIn("_Znwm", symbols, "_Znwm is the 64-bit spelling: it links and records nothing here")


if __name__ == "__main__":
    unittest.main(verbosity=2)