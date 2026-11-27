#!/usr/bin/env python3
"""Gate: product activity pushes must not allocate with a throwing `new`.

Under -fno-exceptions a failed `operator new` reaches the new_handler and aborts
(PNC-04). A tap on a tile that opens an activity is a user action, not a crash
site: `pushActivityWith<T>(...)` allocates with makeUniqueNoThrow and reports
STR-08 instead, leaving the caller on the current screen.

This scans src/ and ports/ for the old spelling. (The helper itself calls
pushActivity(std::move(...)) and is excluded by the pattern.)

Run: python3 -m unittest scripts.tests.test_activity_push_alloc -v
"""
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SKIP = {'.pio', 'build_verify', 'build_shell', 'build_verify2', 'managed_components', '.cache'}
PATTERN = re.compile(r'pushActivity\s*\(\s*std::make_unique')


def offenders():
    found = []
    for root in ('src', 'ports'):
        for path in sorted((REPO / root).rglob('*')):
            if path.suffix not in ('.cpp', '.h', '.c'):
                continue
            if any(part in SKIP for part in path.parts):
                continue
            for number, line in enumerate(path.read_text(encoding='utf-8', errors='ignore').splitlines(), 1):
                if PATTERN.search(line.split('//')[0]):
                    found.append(f'{path.relative_to(REPO)}:{number}: {line.strip()}')
    return found


class ActivityPushAllocTest(unittest.TestCase):
    def test_no_throwing_activity_push(self):
        found = offenders()
        self.assertEqual(found, [], 'pushes must go through pushActivityWith:\n' + '\n'.join(found))

    def test_helper_exists_and_is_no_throw(self):
        header = (REPO / 'src' / 'activities' / 'ActivityManager.h').read_text(encoding='utf-8')
        self.assertIn('T* pushActivityWith(', header)
        body = header[header.index('T* pushActivityWith('):]
        body = body[:body.index('return raw;')]
        self.assertIn('makeUniqueNoThrow<T>', body)
        self.assertIn('fault::raise(fault::Code::ActivityCreateFailed', body)


if __name__ == '__main__':
    unittest.main(verbosity=2)
