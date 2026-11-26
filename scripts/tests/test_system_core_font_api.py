#!/usr/bin/env python3
"""Prove every declared core-font function is defined in *both* preprocessor branches.

Why this exists: `src/SystemCoreFont.cpp` is one `#if/#else` over the whole file, and the
`#else` stub omitted `faceOnSd()` -- which `SdCardFontSystem::loadSupplementFonts()` calls
with no feature gate of its own. `[c3_hardware]` sets `CROSSPOINT_SYSTEM_FONT_PACK` but not
`CROSSPOINT_FONT_PACK_EMBEDDED`, so C3 selects the stub and the call resolves to nothing:
not a missing feature, an undefined reference at link, on the one target nobody has built
this week. `tierLabel()` was missing the same way.

A readpico build cannot catch this -- it compiles the *other* branch -- and the simulator
was already skipped for unrelated reasons, so the defect is invisible from the target people
actually test. The check is therefore structural, and the same asymmetry would be caught for
any future addition to this namespace.

The gate also self-tests against a mutated copy: a checker that passes because it found
nothing to compare would be worse than no checker.
"""
import pathlib
import re
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "src/SystemCoreFont.h"
SOURCE = ROOT / "src/SystemCoreFont.cpp"
CALLER = ROOT / "src/SdCardFontSystem.cpp"
NAMESPACE = "system_core_font"

# `bool name(...)`, `const char* name(...)`, `const uint8_t* name(...)`, `Status name(...)`.
# Two shapes because a declaration ends in `;` and a definition opens a body.
_DEF_HEAD = r"^(?:bool|void|int|uint32_t|Status|const char\s*\*|const uint8_t\s*\*)"
DECL_RE = re.compile(_DEF_HEAD + r"\s+([A-Za-z_]\w*)\s*\([^;]*\)\s*(?:const)?;", re.MULTILINE)
DEF_RE = re.compile(_DEF_HEAD + r"\s+([A-Za-z_]\w*)\s*\([^;{)]*\)\s*(?:const)?\s*\{", re.MULTILINE)


def _namespace_block(text: str) -> str:
    """Body of `namespace system_core_font { ... }` (the header keeps it flat)."""
    start = text.index(f"namespace {NAMESPACE}")
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i]
    raise AssertionError("unterminated namespace in the header")


def declared_api(header_text: str) -> set:
    return set(DECL_RE.findall(_namespace_block(header_text)))


def branch_bodies(source_text: str) -> dict:
    """Split a whole-file `#if/#else/#endif` into {'real': ..., 'stub': ...}."""
    lines = source_text.splitlines()
    opens = [i for i, ln in enumerate(lines) if ln.startswith("#if ")]
    elses = [i for i, ln in enumerate(lines) if ln.startswith("#else")]
    ends = [i for i, ln in enumerate(lines) if ln.startswith("#endif")]
    assert len(opens) == 1, f"expected one whole-file #if, found {len(opens)}"
    assert len(elses) == 1 and len(ends) >= 1, "expected exactly one #else in the stub branch"
    return {
        "real": "\n".join(lines[opens[0] + 1:elses[0]]),
        "stub": "\n".join(lines[elses[0] + 1:ends[0]]),
    }


def defined_api(branch_text: str) -> set:
    return set(DEF_RE.findall(branch_text))


def gaps(header_text: str, source_text: str) -> dict:
    """{branch: sorted(missing)} for every branch that fails to implement the header."""
    declared = declared_api(header_text)
    out = {}
    for name, body in branch_bodies(source_text).items():
        missing = sorted(declared - defined_api(body))
        if missing:
            out[name] = missing
    return out


class TestSystemCoreFontApi(unittest.TestCase):
    maxDiff = None

    def setUp(self):
        self.header = HEADER.read_text()
        self.source = SOURCE.read_text()

    def test_declared_surface_is_the_expected_set(self):
        # A rename or deletion here must be a considered act: the stubs, the callers and the
        # release manifest's kPartitionOffset all read this namespace.
        self.assertEqual(
            declared_api(self.header),
            {
                "carrierPresent", "releaseToSd", "ready", "installResident",
                "residentData", "faceOnSd", "tierLabel", "status", "problemName",
            },
        )

    def test_both_branches_define_everything_declared(self):
        self.assertEqual(
            gaps(self.header, self.source), {},
            "a stub-branch build would fail to link",
        )

    def test_the_stub_defines_nothing_undeclared(self):
        """The stub is a pure API mirror: no private helpers, no stale names.

        Only the stub gets this rule. The real branch legitimately holds file-local helpers
        (`probe`, `fileCrc`, `inflateCarrier`, ...) that are not part of the API, so applying
        the same rule there would just demand an anonymous namespace and prove nothing.
        A stub-only name, by contrast, means someone renamed or dropped a declaration and
        left the fallback behind -- which is how a stub silently stops matching its header.
        """
        declared = declared_api(self.header)
        extra = sorted(defined_api(branch_bodies(self.source)["stub"]) - declared)
        self.assertEqual(extra, [], "the stub branch defines functions the header does not declare")
        # And the rule must actually fire.
        injected = self.source.replace(
            "bool carrierPresent() { return false; }",
            "bool carrierPresent() { return false; }\nbool staleName() { return false; }", 1)
        self.assertEqual(
            sorted(defined_api(branch_bodies(injected)["stub"]) - declared), ["staleName"])

    def test_the_caller_that_makes_this_load_bearing_is_ungated(self):
        """faceOnSd() must be reachable in a stub build, or this whole gate is decoration.

        Pinned, not assumed: if someone wraps the call in a feature `#if`, the stubs become
        dead code and the invariant still holds -- but the comment in the stub explains
        itself by this call site, so the claim must stay true.
        """
        text = CALLER.read_text()
        start = text.index("bool SdCardFontSystem::loadSupplementFonts(")
        call = text.index("faceOnSd()", start)
        self.assertNotIn("#if", text[start:call],
                        "the ungated caller is now gated; re-check whether the stubs are needed")

    def test_gate_detects_a_missing_stub(self):
        """The checker must fail on the exact bug that shipped."""
        with tempfile.TemporaryDirectory() as tmp:
            broken = pathlib.Path(tmp) / "SystemCoreFont.cpp"
            needle = "bool faceOnSd() { return false; }\n"
            self.assertEqual(self.source.count(needle), 1)
            broken.write_text(self.source.replace(needle, ""))
            self.assertEqual(
                gaps(self.header, broken.read_text()),
                {"stub": ["faceOnSd"]},
                "deleting a stub definition must be reported, not waved through",
            )

    def test_gate_detects_a_split_surface(self):
        """The realistic future bug: declared, implemented on the real path, stub forgotten.

        That is exactly how `faceOnSd()` got here -- it was added for the wider-face rule and
        only the branch that readpico compiles was updated.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            header = root / "SystemCoreFont.h"
            source = root / "SystemCoreFont.cpp"
            header.write_text(self.header.replace(
                "bool carrierPresent();",
                "bool carrierPresent();\nbool brandNew();", 1))
            source.write_text(self.source.replace(
                "bool carrierPresent() { return probe(); }",
                "bool carrierPresent() { return probe(); }\nbool brandNew() { return true; }", 1))
            self.assertEqual(
                gaps(header.read_text(), source.read_text()),
                {"stub": ["brandNew"]},
                "a real-branch-only implementation must be reported against the stub",
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
