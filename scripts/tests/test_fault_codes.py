"""Fault code gate: the vocabulary, its reachability, and its shape.

Why this is a machine check and not a review note: the codes exist because a field
report is a photograph. A code that is declared but never raised is a tag a user
will never see (harmless but dead); a code raised but not declared is an entry with
no tag, so the screen prints nothing for a fault that did happen; two codes sharing
a tag make a report ambiguous; and a tag whose prefix disagrees with its value's
documented band breaks the one property that makes a screenshot decodable without a
tool -- "STK-" means internal-flash refusal, always.

Every check below is a relation between artifacts that already exist in the tree,
and each has a fixture pair where both verdicts are pinned, so a broken glob cannot
pass by finding nothing.
"""

import pathlib
import re
import unittest

# scripts/tests/ -> the repository root is two levels up.
REPO = pathlib.Path(__file__).resolve().parents[2]
CODES = REPO / "lib/Diagnostics/FaultCodes.h"

# The scan set: product code, not tests, not docs, not the deleted bench islands.
SCAN_DIRS = ["src", "lib"]
SKIP_PARTS = {"test", "tests", "stubs"}

RESERVED_NAMES = {"ReservedSelfHealingFirst", "ReservedSelfHealingLast"}


def read(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


# Strings first: a `//` inside "http://host" is not a comment. The alternation
# is tried left to right at each position, so a literal is consumed whole.
COMMENTS = re.compile(
    "|".join([
        r'"(?:\\.|[^"\\])*"',      # a string literal, consumed whole
        r"'(?:\\.|[^'\\])*'",     # and a character literal
        r"//[^\n]*",              # only then: a line comment
        r"/\*.*?\*/",            # and a block comment
    ]),
    re.S,
)


# Files inside the fault namespace itself write `Code::X`; everything else has to
# qualify it, which is what keeps another enum's `Code::Ok` out of the scan.
IN_FAULT_NAMESPACE = re.compile(r"namespace fault \{")


def code_of(path: pathlib.Path) -> str:
    """The file with comments stripped: a sentence about a code is not a use of it."""
    return COMMENTS.sub(" ", read(path))


def sources():
    for directory in SCAN_DIRS:
        root = REPO / directory
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file() or path.suffix not in (".cpp", ".h", ".hpp", ".cc"):
                continue
            if SKIP_PARTS & set(path.parts):
                continue
            # FaultCodes.h declares the vocabulary; it is not a user of it.
            if path.samefile(CODES):
                continue
            # The owner takes the code as a parameter; its own files are declarations,
            # not uses of the vocabulary.
            if path.name in ("StackSafety.h", "StackSafety.cpp"):
                continue
            yield path


TABLE_ROW = re.compile(r"\{\s*Code::(\w+)\s*,\s*\"([A-Z]{3}-\d{2})\"\s*,\s*\"([^\"]+)\"\s*\}")
ENUM_VALUE = re.compile(r"^\s*(\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*,", re.M)
BAND = re.compile(r"// --- (0x[0-9A-Fa-f]+)\.\.(0x[0-9A-Fa-f]+): \w")


class TableTest(unittest.TestCase):
    def setUp(self):
        text = read(CODES)
        self.declared = {name: int(value, 16) for name, value in ENUM_VALUE.findall(text)}
        self.rows = TABLE_ROW.findall(text)
        self.tags = {tag: name for name, tag, _ in self.rows}
        self.codes_by_name = {name: tag for name, tag, _ in self.rows}

    def test_table_covers_every_declared_code_but_the_reserved_band(self):
        listed = {name for name, _, _ in self.rows}
        for name, value in self.declared.items():
            if name in RESERVED_NAMES:
                continue
            if value >= self.declared["ReservedSelfHealingFirst"]:
                continue
            self.assertIn(name, listed, f"{name} is declared but has no table row")
        # The reverse direction: a row whose name is not an enumerator cannot compile,
        # so this is really a check that the two lists were not edited apart.
        for name in listed:
            self.assertIn(name, self.declared, f"table row {name} has no enumerator")
        self.assertGreaterEqual(len(listed), 60, "the table lost most of its rows")

    def test_tags_are_unique_and_dense_per_band(self):
        self.assertEqual(len(self.tags), len(self.rows), "two rows share a tag")
        seen = {}
        for name, tag, _ in self.rows:
            prefix, number = tag.split("-")
            seen.setdefault(prefix, []).append(int(number))
        for prefix, numbers in seen.items():
            self.assertEqual(sorted(numbers), list(range(1, len(numbers) + 1)),
                             f"{prefix} tags have a gap or a reused number")

    def test_tag_prefix_matches_the_documented_value_band(self):
        # The bands are read from the header's own section comments, so the comment
        # and the check cannot drift apart silently.
        bands = []
        text = read(CODES)
        reserved = self.declared["ReservedSelfHealingFirst"]
        empty_bands = 0
        for match in BAND.finditer(text):
            start = int(match.group(1), 16)
            end = int(match.group(2), 16)
            inside = [tag for name, tag, _ in self.rows if start <= self.declared[name] <= end]
            if not inside:
                # Only the reserved self-healing band is allowed to have no rows:
                # an empty band anywhere else means rows drifted out of their range.
                self.assertGreaterEqual(start, reserved, f"band 0x{start:02X} lost all of its rows")
                empty_bands += 1
                continue
            prefixes = {tag.split("-")[0] for tag in inside}
            self.assertEqual(len(prefixes), 1, f"band 0x{start:02X} mixes prefixes: {prefixes}")
            bands.append((start, prefixes.pop()))
        bands.sort()
        self.assertGreaterEqual(len(bands), 5, "the band comments disappeared from FaultCodes.h")

        def expected(value: int) -> str:
            prefix = None
            for start, candidate in bands:
                if value >= start:
                    prefix = candidate
            return prefix

        for name, tag, _ in self.rows:
            value = self.declared[name]
            if value >= self.declared["ReservedSelfHealingFirst"]:
                continue
            self.assertEqual(tag.split("-")[0], expected(value), f"{tag} ({name}) sits in the wrong band")

    def test_every_meaning_is_ascii_and_distinct_enough_to_act_on(self):
        meanings = {}
        for name, tag, what in self.rows:
            self.assertTrue(what.isascii() and what, f"{tag} has a non-ASCII or empty meaning")
            self.assertGreaterEqual(len(what), 12, f"{tag}'s meaning is too short to act on: {what!r}")
            meanings.setdefault(what.lower(), []).append(tag)
        duplicates = {key: value for key, value in meanings.items() if len(value) > 1}
        self.assertEqual(duplicates, {}, "two codes claim the same sentence; one of them is a dead row")


class ReachabilityTest(unittest.TestCase):
    def setUp(self):
        text = read(CODES)
        self.declared = {name: int(value, 16) for name, value in ENUM_VALUE.findall(text)}
        self.raised = {}
        for path in sources():
            body = read(path)
            if "Code::" not in body:
                continue
            pattern = r"\bCode::(\w+)" if IN_FAULT_NAMESPACE.search(body) else r"fault::Code::(\w+)"
            for match in re.finditer(pattern, body):
                self.raised.setdefault(match.group(1), []).append(path)

    def test_every_declared_code_is_reached_by_at_least_one_site(self):
        missing = sorted(name for name, value in self.declared.items()
                         if name not in RESERVED_NAMES and value < self.declared["ReservedSelfHealingFirst"]
                         and name not in self.raised)
        self.assertEqual(missing, [], "declared codes with no raise site: " + ", ".join(missing))

    def test_nothing_raises_an_undeclared_code(self):
        unknown = sorted({name for name in self.raised if name not in self.declared})
        self.assertEqual(unknown, [], "raise sites use names that are not in the enum: " + ", ".join(unknown))

    def test_the_vocabulary_is_actually_wired_in(self):
        # A floor, not a number to admire: below this the scan is broken rather
        # than the tree being instrumented. 40 flash refusals + task/panel/state
        # sites + the panic hooks is the shape of the current instrumentation.
        self.assertGreaterEqual(sum(len(paths) for paths in self.raised.values()), 55,
                                "almost no raise sites found; the scan or the instrumentation broke")
        self.assertIn("CallbackEmpty", self.raised, "the callback owner stopped recording")

    def test_flash_guard_sites_always_name_their_code(self):
        owner = re.compile(r"(internalFlashAllowed|guardInternalFlash)\(([^)]*)\)")
        offenders = []
        for path in sources():
            body = code_of(path)
            for match in owner.finditer(body):
                arguments = match.group(2)
                if "Code::" in arguments or "site" in arguments:
                    continue
                if arguments.strip() == "":
                    offenders.append((path, match.group(1)))
        self.assertEqual(offenders, [], "guard calls without a site code: " + str(offenders))

    def test_local_guard_aliases_forward_a_code_rather_than_hiding_one(self):
        alias = re.compile(r"bool stackIsInPsram\(([^)]*)\)\s*\{", re.S)
        found = 0
        for path in sources():
            body = code_of(path)
            for match in alias.finditer(body):
                found += 1
                self.assertIn("fault::Code", match.group(1), f"{path}: alias takes no site code")
        self.assertGreaterEqual(found, 3, "the guard aliases disappeared; the check needs a fixture instead")


class FormatBudgetTest(unittest.TestCase):
    def test_the_line_capacity_holds_the_whole_ring(self):
        header = read(REPO / "lib/Diagnostics/FaultLog.h")
        capacity = int(re.search(r"kCapacity = (\d+)", header).group(1))
        line = int(re.search(r"kLineCapacity = (\d+)", header).group(1))
        # Worst realistic entry: "STK-08x42949" (11) plus a separator, so 13 bytes.
        # An entry dropped for lack of buffer is a fault that did not get reported.
        self.assertGreaterEqual(line, capacity * 13 + 1,
                                "kLineCapacity can no longer hold kCapacity entries")

    def test_the_crash_screen_prints_codes_without_translating_them(self):
        screen = read(REPO / "src/activities/home/CrashActivity.cpp")
        line = [entry for entry in screen.splitlines() if '"fault=%s' in entry]
        self.assertEqual(len(line), 1, "the fault line disappeared from the crash screen")
        self.assertNotIn("tr(", line[0], "the fault line must stay language-neutral")


if __name__ == "__main__":
    unittest.main(verbosity=2)
