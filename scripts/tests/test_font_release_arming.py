#!/usr/bin/env python3
"""Gate: a font release for memory must go through font_reclaim (and arm restore).

The 0.8.4 field report had two halves: the network reclaim was never reverted
(fixed by arming), and five other screens released fonts by hand with no second
half at all, so Home stayed on the 1,526-codepoint subset after visiting them.

This pins the invariant at the source: outside the owner and the sites that
reload synchronously for their own reason, nobody calls
SdCardFontSystem::releaseLoadedFont()/releaseSdFamilies() directly.

Run: python3 -m unittest scripts.tests.test_font_release_arming -v
"""
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
PATTERN = re.compile(r'sdFontSystem\.(releaseLoadedFont|releaseSdFamilies)\s*\(')

# Who may release directly, and why.
ALLOWED = {
    'src/SdCardFontSystem.cpp': 'the owner',
    'src/FontReclaim.cpp': 'the wrapper itself',
    'src/NetworkStartup.cpp': 'prepare() releases and arms in the same function',
    'src/activities/reader/EpubReaderActivity.cpp': 'font-change reload: releases then rebuilds inline',
    'src/activities/settings/TextSettingsActivity.cpp': 'preview budget: releases/reloads per enter/exit',
    'src/activities/reader/PdfReaderActivity.cpp': 'MuPDF owns the memory while a document is open; onExit() reloads',
}


def offenders():
    found = []
    for path in sorted((REPO / 'src').rglob('*.cpp')):
        rel = str(path.relative_to(REPO))
        if rel in ALLOWED:
            continue
        for number, line in enumerate(path.read_text(encoding='utf-8', errors='ignore').splitlines(), 1):
            if PATTERN.search(line.split('//')[0]):
                found.append(f'{rel}:{number}: {line.strip()}')
    return found


class FontReleaseArmingTest(unittest.TestCase):
    def test_only_the_wrapper_and_reload_sites_release_directly(self):
        found = offenders()
        self.assertEqual(found, [],
                         'use font_reclaim::releaseForMemory() (arms the restore) instead:\n' + '\n'.join(found))

    def test_the_wrapper_arms(self):
        text = (REPO / 'src' / 'FontReclaim.cpp').read_text(encoding='utf-8')
        self.assertIn('NetworkStartup::rearmRenderMemoryRestore()', text)


if __name__ == '__main__':
    unittest.main(verbosity=2)
