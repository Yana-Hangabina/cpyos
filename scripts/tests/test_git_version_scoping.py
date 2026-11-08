"""Check that the dev-only Git version define reaches only the sources that use it."""

import importlib.util
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / 'git_branch.py'


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


if __name__ == '__main__':
    unittest.main()
