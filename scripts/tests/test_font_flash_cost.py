"""Parsing of GNU ld link maps in scripts/font_flash_cost.py.

The tool answers "what does this font asset cost in the shipped image", and its answer is used
to decide whether a header may be deleted. A parser that quietly missed entries would report a
family as free and justify deleting something that is actually linked -- so the fragile part
(the two-line entry GNU ld emits for long section names) is pinned here rather than trusted
because it worked on one big map.
"""

import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

import font_flash_cost as ffc  # noqa: E402

MAP = """Discarded output sections:
/DISCARD/
 *(.rela.*)
 .rodata._ZL99discarded_ubuntu_12Bitmaps
                0x3c000000    0x9999 some/object.o

Memory Configuration

Linker script and memory map

 .rodata._ZL22notoserif_titleBitmaps
                0x3c51eb3c    0x2e9eb .pio/build/x/src/main.cpp.o
 .text.foo
                0x42000000       0x20 .pio/build/x/src/foo.cpp.o
 .rodata._ZL17ubuntu_12_regular
                0x3c600000      0x100 .pio/build/x/src/bar.cpp.o
                0x3c600100       0x50 second line that is not a section
"""


class ParseMapTest(unittest.TestCase):
    def setUp(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'firmware.map'
            path.write_text(MAP, encoding='utf-8')
            self.sections = ffc.parse_map(path)

    def test_wrapped_long_section_names_are_parsed(self):
        entry = self.sections.get('.rodata._ZL22notoserif_titleBitmaps')
        self.assertIsNotNone(entry, 'the two-line entry format is what the whole tool depends on')
        self.assertEqual(entry[0]['bytes'], 0x2E9EB)
        self.assertEqual(entry[0]['object'], '.pio/build/x/src/main.cpp.o')

    def test_single_line_entries_are_parsed_too(self):
        entry = self.sections.get('.rodata._ZL17ubuntu_12_regular')
        self.assertIsNotNone(entry)
        self.assertEqual(entry[0]['bytes'], 0x100)

    def test_code_sections_are_out_of_scope(self):
        """The tool answers "what does this font blob cost", so it tracks data sections only.

        A .text entry must not appear as a family cost, and must not disturb the pending
        section name either -- otherwise a code line between a wrapped data header and its
        size line would silently mis-attribute bytes.
        """
        self.assertNotIn('.text.foo', self.sections)
        self.assertEqual(self.sections['.rodata._ZL22notoserif_titleBitmaps'][0]['bytes'],
                         0x2E9EB, 'a following .text line corrupted the pending data section')

    def test_discarded_sections_are_not_counted(self):
        """0x9999 of ubuntu_12 lives under /DISCARD/ and must not be reported as image cost."""
        rows = {r['family']: r for r in ffc.summarise(self.sections)}
        self.assertEqual(rows['ubuntu_12']['bytes'], 0x100,
                         'a discarded section was counted as flash cost')

    def test_family_patterns_separate_cjk_from_latin(self):
        sections = {
            '.rodata._ZL21notosans_cjk_12Bitmaps': [{'bytes': 961524, 'object': 'a.o'}],
            '.rodata._ZL21notosans_16_boldBitmaps': [{'bytes': 1234, 'object': 'b.o'}],
        }
        rows = {r['family']: r for r in ffc.summarise(sections)}
        self.assertEqual(rows['notosans_cjk (UI bitmaps)']['bytes'], 961524)
        self.assertEqual(rows['notosans (latin bitmaps)']['bytes'], 1234,
                         'the latin pattern swallowed the CJK family')

    def test_missing_map_is_an_error_not_zero(self):
        """A wrong path must fail loudly; reporting "all families free" would be worse."""
        missing = Path(tempfile.gettempdir()) / 'definitely-not-a-map-9f3.map'
        self.assertEqual(ffc.main([str(missing)]), 1)

    def test_a_code_line_does_not_drop_a_pending_data_section(self):
        """GNU ld can list a kept .text entry between a wrapped name and its size line.

        The pending mechanism must survive it, or the bytes land on the wrong family.
        """
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'firmware.map'
            path.write_text(
                ' .text.keep_me\n'
                '                0x42000000       0x20 .pio/build/x/src/keep.cpp.o\n'
                ' .rodata._ZL17ubuntu_12_regular\n'
                ' .text.also_kept\n'
                '                0x3c600000      0x100 .pio/build/x/src/bar.cpp.o\n',
                encoding='utf-8')
            sections = ffc.parse_map(path)
        rows = {r['family']: r for r in ffc.summarise(sections)}
        self.assertEqual(rows['ubuntu_12']['bytes'], 0x100,
                         'an interleaved .text entry lost the pending data section')


if __name__ == '__main__':
    unittest.main()
