"""Check that the dev-only Git version define reaches only the sources that use it."""

import configparser
import importlib.util
import re
import unittest
from pathlib import Path
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / 'git_branch.py'
PROJECT_DIR = SCRIPT.parents[1]
INI = PROJECT_DIR / 'platformio.ini'

# platformio.ini must not smuggle the git-derived version back in as a build flag:
# a flag lives on every compile line, so the whole tree would rebuild on a new sha.
DEV_ENVS = ('default', 'sticky', 'eego_a4', 'murphy_m4', 'waveshare_epaper_397',
            'metalio_eink4')


def load_module():
    spec = importlib.util.spec_from_file_location('git_branch_under_test', SCRIPT)
    module = importlib.util.module_from_spec(spec)
    # Running the module outside SCons takes the validation-mode branch, which only
    # prints the version, so importing it is safe here.
    spec.loader.exec_module(module)
    return module


class _Node:
    def __init__(self, contents, name='source.cpp'):
        self._contents = contents
        self.name = name
        self.compiled_with = []

    def srcnode(self):
        return self

    def get_contents(self):
        return self._contents


class _Compiled:
    def __init__(self, node, defines):
        self.node = node
        self.defines = defines


class _FakeEnv(dict):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.middlewares = []
        self.appended = []

    def AddBuildMiddleware(self, callback):
        self.middlewares.append(callback)

    def Append(self, **kwargs):
        self.appended.append(kwargs)

    def Clone(self):
        return _FakeEnv(**self)

    def Object(self, node):
        return _Compiled(node, self.appended[-1]['CPPDEFINES'])


class GitVersionScopingTest(unittest.TestCase):
    def test_only_sources_using_the_token_get_the_define(self):
        module = load_module()
        env = _FakeEnv(PIOENV='default', PROJECT_DIR=str(SCRIPT.parents[1]))
        module.inject_version(env)

        self.assertEqual(len(env.middlewares), 1)
        middleware = env.middlewares[0]

        user = _Node(b'snprintf(buf, "CrossMux-%s-" CROSSPOINT_VERSION, model);')
        other = _Node(b'int answer() { return 42; }')

        compiled = middleware(env, user)
        self.assertIsInstance(compiled, _Compiled)
        self.assertIn('0.7.0-dev-', compiled.defines[0][1])
        # Unrelated objects keep their identity, so a branch/SHA change cannot
        # invalidate their cached signatures.
        self.assertIs(middleware(env, other), other)
        self.assertEqual(env.appended, [], 'the shared env must not carry the define')

    def test_release_environments_are_untouched(self):
        module = load_module()
        env = _FakeEnv(PIOENV='readpico', PROJECT_DIR=str(SCRIPT.parents[1]))
        module.inject_version(env)
        self.assertEqual(env.middlewares, [])
        self.assertEqual(env.appended, [])

    def test_short_sha_width_is_pinned(self):
        """--short alone grows with the repository; the version length must not."""
        module = load_module()
        seen = []

        def fake_check_output(argv, **_kwargs):
            seen.append(argv)
            return 'abc1234\n'

        with mock.patch.object(module.subprocess, 'check_output', fake_check_output):
            self.assertEqual(module.get_git_short_sha(str(PROJECT_DIR)), 'abc1234')
        self.assertIn('--short=7', seen[-1])

    def test_git_output_is_sanitized_for_a_c_string_literal(self):
        """A branch name with a quote or backslash must not break the define."""
        module = load_module()
        with mock.patch.object(module.subprocess, 'check_output',
                               lambda *_a, **_k: 'feat/"odd\\branch"\n'):
            self.assertEqual(module.get_git_branch(str(PROJECT_DIR)), 'feat/oddbranch')

    def test_no_script_appends_the_version_define_globally(self):
        """Only a Clone() of the build env may receive CPPDEFINES; the shared env must not."""
        global_append = re.compile(r'(?<![A-Za-z_])env\.Append\(CPPDEFINES')
        for path in sorted((PROJECT_DIR / 'scripts').glob('*.py')):
            self.assertIsNone(global_append.search(path.read_text(encoding='utf-8')),
                              f'{path.name} appends CPPDEFINES to the shared env')
        source = SCRIPT.read_text(encoding='utf-8')
        self.assertIn('AddBuildMiddleware', source)
        self.assertIn('version_env.Append(CPPDEFINES', source)

    def test_dev_environments_do_not_carry_a_version_build_flag(self):
        config = configparser.RawConfigParser()
        config.read(INI, encoding='utf-8')
        for env in DEV_ENVS:
            section = f'env:{env}'
            self.assertTrue(config.has_section(section), f'missing [{section}]')
            flags = config.get(section, 'build_flags', fallback='')
            self.assertNotIn('-DCROSSPOINT_VERSION', flags,
                             f'[{section}] must get its version from scripts/git_branch.py')


if __name__ == '__main__':
    unittest.main()
