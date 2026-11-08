"""Check the wolfSSL settings patch: DEBUG_WOLFSSL is dropped and the patch is idempotent."""

import builtins
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / 'patch_wolfssl.py'


class _Env:
    def __init__(self, project_dir: Path):
        self.project_dir = project_dir

    def subst(self, _value):
        return str(self.project_dir)


def run_patch(project_dir: Path):
    """Execute the pre-script with a fake SCons env."""
    source = SCRIPT.read_text()
    scope = {
        '__file__': str(SCRIPT),
        'Import': lambda _name: None,
        'env': _Env(project_dir),
    }
    original_import = builtins.__import__
    try:
        builtins.__import__ = original_import
        exec(compile(source, str(SCRIPT), 'exec'), scope)  # noqa: S102 - test harness for a build script
    finally:
        builtins.__import__ = original_import


class WolfSslPatchTest(unittest.TestCase):
    def test_debug_trace_is_compiled_out(self):
        with tempfile.TemporaryDirectory() as tmp:
            settings = Path(tmp) / '.pio/libdeps/readpico/Arduino-wolfSSL/src/user_settings.h'
            settings.parent.mkdir(parents=True)
            settings.write_text('#define DEBUG_WOLFSSL\n#define WOLFSSL_ARDUINO\n')

            run_patch(Path(tmp))
            once = settings.read_text()
            self.assertIn('#ifndef FREEINK_WOLFSSL_DEBUG', once)
            self.assertIn('#undef DEBUG_WOLFSSL', once)
            # The guard must come after wolfSSL's own definition, so the
            # #undef actually applies.
            self.assertLess(once.index('#define DEBUG_WOLFSSL'), once.index('#undef DEBUG_WOLFSSL'))

            run_patch(Path(tmp))
            self.assertEqual(settings.read_text(), once, 'the patch must be idempotent')


if __name__ == '__main__':
    unittest.main()
