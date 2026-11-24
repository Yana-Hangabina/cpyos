#!/usr/bin/env python3
"""The module header is the only source of truth for the ABI the two images agree on.

Why this is a test and not a comment: `scripts/build_pdf_module.py` carried
`ABI = 2  # must match kPdfModuleAbiVersion`, and when the ABI went to 3 for the
paragraph-split op the literal stayed behind.  The device would have refused the pair
(the app compares both the image header and the filled ops table), which is the loud
outcome -- but the same shape of duplication in the *other* direction is silent.  So this
asserts there is no literal left to drift, and that the fields the bump is about are where
both sides think they are.
"""
from __future__ import annotations

import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HEADER = os.path.join(ROOT, "src", "pdf", "PdfModuleApi.h")
HOST_HEADER = os.path.join(ROOT, "src", "pdf", "PdfModuleHost.h")
CHECKER = os.path.join(ROOT, "src", "pdf", "PdfModuleHeaderCheck.h")
BUILDER = os.path.join(ROOT, "scripts", "build_pdf_module.py")
ENTRY = os.path.join(ROOT, "src", "pdf", "PdfModuleEntry.cpp")
RUNTIME = os.path.join(ROOT, "src", "pdf", "PdfModuleRuntime.cpp")


def read(path: str) -> str:
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def constant(text: str, name: str) -> int:
    match = re.search(rf"{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)[uU]?", text)
    assert match, f"{name} missing from the header"
    return int(match.group(1), 0)


def struct_fields(text: str, struct: str) -> list[str]:
    body = re.search(rf"struct {struct}\s*\{{(.*?)\n\}};", text, re.S)
    assert body, f"struct {struct} not found"
    names = []
    for line in body.group(1).splitlines():
        line = line.strip()
        if not line or line.startswith("//"):
            continue
        # A function pointer's identifier is INSIDE the parens (`void (*name)(...)`);
        # anything else names itself just before the ';'.  Reading "the identifier before
        # (" instead would return the return type and quietly make this test compare
        # `void` to a field name -- which is exactly how it first failed.
        match = re.search(r"\(\*(\w+)\)", line) or re.search(r"(\w+)\s*;", line)
        if match:
            names.append(match.group(1))
    return names


class TestPdfModuleAbi(unittest.TestCase):
    def setUp(self) -> None:
        self.header = read(HEADER)
        self.builder = read(BUILDER)

    # builder constant -> the identifier it must be read from in the header
    HEADER_NAMES = {
        "MAGIC": "kPdfModuleMagic",
        "VERSION": "kPdfModuleVersion",
        "ABI": "kPdfModuleAbiVersion",
        "HOST_API_MAJOR": "kPdfModuleHostApiMajor",
        "HOST_API_MINOR": "kPdfModuleHostApiMinor",
    }

    def test_builder_does_not_duplicate_the_header_constants(self) -> None:
        for name, header_name in self.HEADER_NAMES.items():
            literal = re.search(rf"^{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)", self.builder, re.M)
            self.assertIsNone(
                literal,
                f"scripts/build_pdf_module.py hardcodes {name}={literal and literal.group(1)}; "
                f"derive it from PdfModuleApi.h with _parse_header_int() so it cannot drift",
            )
            self.assertIn(f'_parse_header_int("{header_name}")', self.builder)

    def test_abi_bumped_for_the_paragraph_split_op(self) -> None:
        self.assertGreaterEqual(
            constant(self.header, "kPdfModuleAbiVersion"), 4,
            "ABI 4 is the host-table entry signature; older engines must be refused",
        )

    def test_new_op_is_last_in_the_table_and_both_sides_use_it(self) -> None:
        fields = struct_fields(self.header, "PdfModuleOps")
        # Appending is the compatibility rule: a module that predates a field leaves it
        # null in the app's zero-initialised table, so a new op must never be inserted.
        self.assertEqual(fields[-1], "set_paragraph_split")
        self.assertIn("ops->set_paragraph_split = &modSetParagraphSplit;", read(ENTRY))
        self.assertIn("table->set_paragraph_split", read(os.path.join(ROOT, "src", "pdf",
                                                                     "PdfDocumentXip.cpp")))

    def test_both_sides_actually_compare_the_abi(self) -> None:
        runtime = read(RUNTIME)
        entry = read(ENTRY)
        # An ABI number nobody compares is a comment.
        self.assertIn("g_ops.structSize = sizeof(PdfModuleOps)", runtime)
        self.assertRegex(runtime, r"g_ops\.abi\s*!=\s*kPdfModuleAbiVersion")
        self.assertRegex(read(CHECKER), r"h\.abi\s*!=\s*kPdfModuleAbiVersion")
        self.assertIn("ops->abi = kPdfModuleAbiVersion;", entry)
        self.assertRegex(entry, r"ops->structSize\s*<\s*sizeof\(PdfModuleOps\)")

    def test_host_table_is_append_only_and_versioned(self) -> None:
        host = read(HOST_HEADER)
        fields = struct_fields(host, "PdfModuleHost")
        # New services go at the end; the loader passes structSize and the module
        # reads only what fits, so inserting a field would shift every later slot.
        self.assertEqual(fields[-1], "font_cache_close")
        self.assertIn("kPdfModuleHostApiMajor", host)
        self.assertIn('_parse_header_int("kPdfModuleHostApiMajor")', self.builder)
        entry = read(ENTRY)
        self.assertIn("pdfmod_entry(const PdfModuleHost* host", entry)
        self.assertIn("(host->abi >> 16) != kPdfModuleHostApiMajor", entry)

    def test_compatibility_is_decided_by_class_not_build_id(self) -> None:
        checker = read(CHECKER)
        header = read(HEADER)
        self.assertIn("h.hostApi", checker)
        self.assertIn("h.layoutKey", checker)
        self.assertIn("kPdfModuleLayoutKey", checker)
        # buildId exists for provenance; nothing in the predicate may compare it.
        self.assertIn("buildId", header)
        self.assertNotIn("buildId", checker)

    def test_loader_zero_initialises_the_table(self) -> None:
        # The null-check pattern for optional trailing ops depends on this: without it a
        # module that predates a field would leave garbage where the app reads a pointer.
        self.assertRegex(read(RUNTIME), r"PdfModuleOps g_ops\{\}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
