#!/usr/bin/env python3
"""Gate: the crash screen allocates nothing.

The 0.8.4 field report died in Settings with `operator new` failing, rebooted
into CrashActivity, and the crash screen's own std::string / std::vector /
wrappedText() allocations failed in turn -- the recorded context then reads
`screen=Crash stage=enter`, i.e. the second death. A diagnostics screen that
needs the heap cannot report a heap failure.

So CrashActivity builds fixed char arrays in onEnter() and render() only draws.
This gate keeps that true, and keeps the failure-history line the report needs
(fault codes, `new=<bytes>`, heap, screen/stage) from being dropped in a later
rewrite.

Checked over src/activities/home/CrashActivity.{h,cpp}:
  * no std::string / std::vector / std::map / std::unique_ptr members or locals;
  * no allocating containers operations (emplace_back / push_back /
    wrappedText / getLastLogs / getPanicInfo(std::string) );
  * no raw heap use (malloc / calloc / new / make_unique);
  * the context still prints fault codes and the allocation/heap pair.

Mutation to prove the gate: add `std::vector<std::string> lines;` to the header
and it must fail.

Run: python3 -m unittest scripts.tests.test_crash_screen_alloc -v
"""
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
FILES = [
    REPO / "src" / "activities" / "home" / "CrashActivity.h",
    REPO / "src" / "activities" / "home" / "CrashActivity.cpp",
]

FORBIDDEN = [
    (re.compile(r"\bstd::string\b"), "std::string"),
    (re.compile(r"\bstd::vector\b"), "std::vector"),
    (re.compile(r"\bstd::map\b"), "std::map"),
    (re.compile(r"\bunique_ptr\b"), "unique_ptr"),
    (re.compile(r"\bemplace_back\b"), "emplace_back"),
    (re.compile(r"\bpush_back\b"), "push_back"),
    (re.compile(r"\bwrappedText\b"), "wrappedText"),
    (re.compile(r"\bgetLastLogs\b"), "getLastLogs"),
    (re.compile(r"\bgetPanicInfo\s*\("), "getPanicInfo (returns std::string)"),
    (re.compile(r"\bmalloc\s*\(|\bcalloc\s*\(|\brealloc\s*\("), "raw heap allocation"),
    (re.compile(r"(?<![A-Za-z_])new\s+[A-Za-z_(]"), "operator new"),
    (re.compile(r"\bmake_unique\b|\bmakeUnique"), "make_unique"),
]


STRING_LITERAL = re.compile(r'"(?:\\.|[^"\\])*"')


def code_lines(path: pathlib.Path):
    """Lines with comments and string literals stripped.

    The rationale comments quote the old code, and one line prints the literal
    "(new address)" -- matching either would be a false positive, not a finding.
    """
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        stripped = raw.split("//")[0]
        yield number, STRING_LITERAL.sub('""', stripped)


class CrashScreenAllocationsTest(unittest.TestCase):
    def test_crash_screen_does_not_allocate(self):
        problems = []
        for path in FILES:
            for number, line in code_lines(path):
                for pattern, label in FORBIDDEN:
                    if pattern.search(line):
                        problems.append(f"{path.relative_to(REPO)}:{number}: {label}: {line.strip()}")
        self.assertEqual(problems, [],
                         "the crash screen must not allocate:\n" + "\n".join(problems))

    def test_the_report_lines_the_field_needs_are_still_there(self):
        text = (FILES[1]).read_text(encoding="utf-8")
        for token in ('fault::formatLine', '"new=%uB heap=%uK/%uK"', '"screen=%s stage=%s"',
                      'fault::detailOf(fault::Code::PanicBadAlloc)',
                      'HalSystem::getPanicMessage'):
            self.assertIn(token, text, f"the crash context lost {token}")

    def test_fixed_buffers_are_the_only_storage(self):
        header = FILES[0].read_text(encoding="utf-8")
        self.assertIn("char context_[kMaxLines][kLineChars]", header)
        self.assertIn("static constexpr uint8_t kLineChars", header)


if __name__ == "__main__":
    unittest.main(verbosity=2)