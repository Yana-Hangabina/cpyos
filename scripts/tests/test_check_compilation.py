"""``check_compilation.py`` must not write anything into the tree it is checking.

The script replays a real compile command, which is exactly what makes it useful and what
makes it dangerous: the recorded command carries dependency-generation flags, and an object
path that does not exist yet. Left alone, ``-MMD`` combined with ``-o
.../a-Foo.cpp.o`` writes ``a-Foo.d`` into the working directory -- five untracked files in a
tree other sessions are editing, produced by a command whose whole purpose was to *not*
build anything.

The synthetic cases are the regression pin (they run everywhere, including CI on a fresh
clone where no database exists). ``RealDatabaseTest`` re-runs the same guarantee over every
entry of a locally built ``compile_commands.json``, because a hand-written token list cannot
prove the real environment is covered.
"""

import json
import sys
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

import check_compilation  # noqa: E402

ROOT = SCRIPTS.parent


class CommandForTest(unittest.TestCase):
    def build(self, command: str) -> list[str]:
        return check_compilation.command_for(
            {'command': command, 'file': 'src/Foo.cpp', 'directory': str(ROOT)},
            'src/Foo.cpp')

    def test_every_dependency_flag_form_is_dropped(self):
        # Each form the GNU drivers accept: no-value flags, attached values, separate values.
        args = self.build(
            'g++ -std=gnu++20 -Isrc -DFOO=1 -MMD -MD -MG -MP -MV '
            '-MFattached.d -MT attached.o -MQ other -o .pio/build/a-Foo.cpp.o -c src/Foo.cpp')
        self.assertEqual(
            [a for a in args if a.startswith('-M')], [],
            f'dependency flags survive, so a run writes files: {args}')
        self.assertNotIn('attached.d', args)
        self.assertNotIn('attached.o', args)
        self.assertNotIn('other', args)

    def test_object_and_compile_flags_are_dropped(self):
        args = self.build('g++ -Isrc -o out.o -c src/Foo.cpp')
        self.assertNotIn('-o', args)
        self.assertNotIn('out.o', args)
        self.assertNotIn('-c', args)

    def test_real_flags_are_kept_and_syntax_only_is_last(self):
        args = self.build('g++ -std=gnu++20 -Wall -Ilib/hal/include -DPSRAM=1 -MMD '
                          '-o x.o -c src/Foo.cpp')
        for wanted in ('-std=gnu++20', '-Wall', '-Ilib/hal/include', '-DPSRAM=1'):
            self.assertIn(wanted, args, 'stripping must not remove the flags being tested')
        self.assertEqual(args[-2:], ['-fsyntax-only', 'src/Foo.cpp'])

    def test_args_form_behaves_like_command_form(self):
        # Some databases emit "args" instead of "command"; the guarantee must hold there too.
        args = check_compilation.command_for(
            {'args': ['g++', '-MMD', '-MF', 'dep.d', '-o', 'x.o', '-c', 'src/Foo.cpp'],
             'directory': str(ROOT)}, 'src/Foo.cpp')
        self.assertNotIn('-MF', args)
        self.assertNotIn('dep.d', args)
        self.assertNotIn('x.o', args)


class RealDatabaseTest(unittest.TestCase):
    """The guarantee against the flags this tree actually built with.

    Two halves, because checking all 990 entries through ``command_for()`` costs 45 s
    (shlex over ~580 tokens each): the *vocabulary* check is cheap and covers every entry, the
    *behaviour* check is slow and covers a representative few. The vocabulary check compares
    against the script's own constants rather than a list repeated here, so the two cannot
    drift together -- an assertion as narrow as the rule it tests is a false pass.
    """

    @classmethod
    def setUpClass(cls):
        db = ROOT / 'compile_commands.json'
        if not db.is_file():
            raise unittest.SkipTest('no compile_commands.json here (built last environment only)')
        cls.entries = json.loads(db.read_text())

    def test_the_databases_dependency_vocabulary_is_known_to_the_script(self):
        handled = set(check_compilation._DEP_FLAGS_WITHOUT_VALUE)
        valued = check_compilation._DEP_FLAGS_WITH_VALUE
        seen: set[str] = set()
        for entry in self.entries:
            for token in entry['command'].split():
                if token.startswith('-M'):
                    seen.add(token)
        self.assertTrue(seen, 'a database with no -M flags tests nothing here')
        unknown = sorted(t for t in seen if t not in handled and not t.startswith(valued))
        self.assertFalse(
            unknown,
            f'the real build uses dependency flags the script does not strip: {unknown}; '
            f'they would be written next to the object file ({sorted(seen)})')

    def test_stripping_holds_for_the_files_this_plan_edits(self):
        names = ('SystemCoreFont.cpp', 'SdCardFontSystem.cpp', 'Typography.cpp', 'FontExtras.cpp')
        checked = []
        for entry in self.entries:
            if Path(entry['file']).name in names:
                args = check_compilation.command_for(entry, entry['file'])
                offenders = [a for a in args if a.startswith('-M') or a.endswith('.d')]
                self.assertFalse(offenders, f'{entry["file"]} keeps {offenders}')
                checked.append(Path(entry['file']).name)
        self.assertEqual(sorted(checked), sorted(names),
                         'a file this plan touches is not in the last-built environment')


if __name__ == '__main__':
    unittest.main()
