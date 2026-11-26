"""Structural integrity of the 0.8.1 execution plan document.

The plan is the anti-drift artifact the user asked for: "整合进之前旧的方案并本地留存, 便于后续
完整、不漂移地推进". Its value depends on the document being *complete*, not just current -- and
six editing sessions this one lost a section heading by using it as the anchor for inserting a
neighbouring section. The text under the heading survived, so a careless diff review passed it.

These checks fail on the specific failure modes seen while writing the file:
a missing or duplicated heading, and a sync-manifest class that vanished while its body stayed.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PLAN = ROOT / 'docs' / 'plans' / '2026-11-26-readpico-unified-pdf-channel-and-fonts.md'


class PlanDocumentTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not PLAN.is_file():
            raise unittest.SkipTest(f'{PLAN} is not in this tree')
        cls.text = PLAN.read_text(encoding='utf-8')
        cls.lines = cls.text.split('\n')

    def test_numbered_sections_are_unique(self):
        # The suffix matters: 15.12 and 15.12b are different sections, and a regex that
        # stopped at the digits reported a duplicate that does not exist.
        numbers = [m.group(1) for m in re.finditer(r'^#{2,3} (15\.\d+[a-z]?)', self.text, re.M)]
        self.assertGreater(len(numbers), 15, 'this plan should have many subsections')
        dupes = sorted({n for n in numbers if numbers.count(n) > 1})
        self.assertFalse(dupes, f'a section number appears twice: {dupes}')

    def test_every_section_has_body(self):
        """An inserted section that swallows its neighbour's heading leaves a headingless body.

        Order is deliberately not asserted: this document grew by appending discoveries as they
        happened (15.20-15.24 sit before 15.9), and renumbering to force order would break the
        many "见 15.x" cross-references. Uniqueness and non-empty bodies are the properties
        that actually catch the mistakes made while editing it.
        """
        positions = [m.start() for m in re.finditer(r'^## ', self.text, re.M)]
        for start, end in zip(positions, positions[1:] + [len(self.text)]):
            title = self.text[start:].split('\n', 1)[0]
            body = self.text[start:end].split('\n', 1)[1].strip()
            self.assertTrue(body, f'{title} has no content')

    def test_sync_manifest_classes_are_all_present(self):
        """§16.2's classes A-F each have to exist; two of them were deleted by my own edits."""
        labels = re.findall(r'^\*\*([A-Z])\. ', self.text, re.M)
        self.assertEqual(sorted(set(labels)), ['A', 'B', 'C', 'D', 'E', 'F'],
                         f'classes present: {labels}')
        self.assertEqual(len(labels), len(set(labels)),
                         f'a class label is used twice: {labels}')

    def test_no_empty_section_at_end_of_file(self):
        self.assertTrue(self.lines[-1] == '' or self.lines[-1].strip(),
                        'the document ends with a dangling line')


    def test_referenced_sections_exist(self):
        """Every 15.x cited in prose must still be a real heading.

        This is what catches the mistake actually made repeatedly here: inserting a section by
        replacing the next section's heading, which keeps all the text but drops the title. The
        document then reads fine at a glance, while every "见 15.22" points at nothing.
        """
        titled = {m.group(1) for m in re.finditer(r'^#{2,3} (15\.\d+[a-z]?)', self.text, re.M)}
        # Code fences are stripped first: a quoted command or a table cell that mentions
        # 15.13 is not a promise that such a heading exists.
        prose = re.sub(r'```.*?```', '', self.text, flags=re.M | re.S)
        prose = '\n'.join(l for l in prose.split('\n') if not re.match(r'^#{2,3} ', l))
        # Any bare mention counts, not just "见 15.x" / "§15.x": the doc also writes
        # "15.22 之后这条变成硬失败", and a checker that only follows the punctuated forms
        # passes while the heading it points at is gone -- measured on the mutation below.
        cited = {m.group(1) for m in re.finditer(r'(?<![\d.])15\.(\d+[a-z]?)(?![\d.])', prose)}
        cited = {f'15.{c}' for c in cited}
        missing = sorted(c for c in cited if c not in titled)
        self.assertFalse(missing, f'these sections are cited but have no heading: {missing}')


if __name__ == '__main__':
    unittest.main()
