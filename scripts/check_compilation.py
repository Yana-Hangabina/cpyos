#!/usr/bin/env python3
"""Compile-check single files with the real firmware flags, without running PlatformIO.

Why this exists: a full ``pio run`` needs a writable ``~/.platformio`` and, whenever an
environment's ``custom_sdkconfig`` or ``build_flags`` changed since the last build, it
reinstalls and rebuilds the Arduino core -- minutes of work and a peak of several GB of
disk (§24 of AGENTS.md). It also deletes other environments' build directories and can
collide with another session's build (§38.2/§38.3). A single translation unit needs none
of that: ``compile_commands.json`` already records the exact compiler, include path and
defines of the environment that produced it, so re-running one entry with ``-fsyntax-only``
answers "does this file still compile in the shipping configuration" in about a second.

What it does NOT prove, and where the plan's own lesson applies (§11 rule 10):

  * it does not link, so a symbol declared but never defined passes;
  * the database belongs to whichever environment was built last -- the header line it
    prints shows the compiler and a few defines, and that is the only thing separating
    ``readpico_pdf_jjwxc`` from a non-PDF build here;
  * a file that was never compiled (new source, not yet in ``platformio.ini``'s scope) is
    reported as an error rather than skipped, because a silent skip reads as a pass.

Usage:
  python3 scripts/check_compilation.py src/SystemCoreFont.cpp src/SdCardFontSystem.cpp
  python3 scripts/check_compilation.py --db ports/metalio_plus/build_shell/compile_commands.json src/foo.cpp

``compiledb`` writes the database to the repository root, not under ``.pio/build/<env>/``;
that root file is the default, and it describes whichever environment was built last.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import shlex
import subprocess
import sys

# Defines worth echoing back: they identify the configuration the flags came from without
# dumping two hundred tokens.
INTERESTING = (
    'CROSSPOINT_SYSTEM_FONT_PACK',
    'CROSSPOINT_FONT_PACK_EMBEDDED',
    'CROSSPOINT_PDF_XIP',
    'CROSSPOINT_MUPDF',
    'CROSSPOINT_I18N_PACK',
    'CROSSPOINT_BOOKSOURCE_JJWXC',
    'CROSSPOINT_SERIAL_BENCH',
    'BOARD_HAS_PSRAM',
    'CROSSMUX_UI_PROFILE_HIGH_DPI',
    'ENABLE_CHINESE_VERSION',
)


def fail(message: str) -> None:
    print(f'error: {message}', file=sys.stderr)
    raise SystemExit(1)


def load(db: pathlib.Path) -> list[dict]:
    if not db.is_file():
        fail(f'no {db}; run `pio run -t compiledb -e <env>` once, or pass --db')
    try:
        entries = json.loads(db.read_text(encoding='utf-8'))
    except json.JSONDecodeError as exc:
        fail(f'{db} is not valid JSON: {exc}')
    if not isinstance(entries, list) or not entries:
        fail(f'{db} contains no compile entries')
    return entries


def describe(entries: list[dict]) -> str:
    first = entries[0]
    tokens = shlex.split(first.get('command', '')) or first.get('args', [])
    compiler = pathlib.Path(tokens[0]).name if tokens else '?'
    flags = {t for t in tokens if any(t.startswith('-D' + key) for key in INTERESTING)}
    return f'compiler={compiler}, {len(entries)} entries, defines: ' + (
        ' '.join(sorted(flag[2:] for flag in flags)) or '(none of the interesting set)')


# Flags that make the compiler write something next to the object file. A syntax check must
# not create files in a tree other sessions are working in, so every one of them is dropped,
# in both the attached (``-MFout.d``) and separate (``-MF out.d``) forms.
#
# This is not defensive padding: ``-MMD`` was once missing from this list, and the entry it
# came from had ``-o .../a-SystemCoreFont.cpp.o``, so gcc derived the depfile name from the
# object and wrote ``a-SystemCoreFont.d`` into the repository root -- five untracked files in
# a shared tree, none of which looked connected to this script.
_DEP_FLAGS_WITHOUT_VALUE = frozenset({'-MD', '-MMD', '-MG', '-MP', '-MV'})
_DEP_FLAGS_WITH_VALUE = ('-MF', '-MJ', '-MT', '-MQ')


def command_for(entry: dict, source: str) -> list[str]:
    tokens = shlex.split(entry['command']) if 'command' in entry else list(entry['args'])
    args: list[str] = []
    skip = False
    for token in tokens:
        if skip:
            skip = False
            continue
        if token == '-o':          # no object file: syntax-only needs no output
            skip = True
            continue
        if token == '-c':
            continue
        if token in _DEP_FLAGS_WITHOUT_VALUE:
            continue
        if token.startswith(_DEP_FLAGS_WITH_VALUE):
            # -MFpath stays dropped as-is; a bare -MF means the next token is its path.
            skip = len(token) == 3
            continue
        args.append(token)
    args += ['-fsyntax-only', source]
    return args


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('files', nargs='+', help='sources to check, repo-relative or absolute')
    parser.add_argument('--db', type=pathlib.Path, default=pathlib.Path('compile_commands.json'))
    parser.add_argument('--quiet', action='store_true', help='only report failures')
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parent.parent
    db = args.db if args.db.is_absolute() else root / args.db
    entries = load(db)
    if not args.quiet:
        print(f'compile db: {db.relative_to(root) if db.is_relative_to(root) else db}  ({describe(entries)})')

    failed: list[str] = []
    missing: list[str] = []
    for source in args.files:
        matches = [e for e in entries
                   if e['file'].replace('\\', '/').endswith(source.replace('\\', '/'))]
        if not matches:
            missing.append(source)
            print(f'error: {source} is not in {db.name}; it has never been compiled by that '
                  'environment (a new file needs a real build, this cannot stand in for one)',
                  file=sys.stderr)
            continue
        entry = matches[0]
        directory = pathlib.Path(entry.get('directory', str(root)))
        result = subprocess.run(command_for(entry, source), cwd=directory,
                                capture_output=True, text=True)
        label = source.split('/')[-1]
        if result.returncode == 0:
            if not args.quiet:
                print(f'{label}: OK')
            continue
        failed.append(source)
        print(f'{label}: FAILED (exit {result.returncode})')
        for line in (result.stderr or result.stdout).strip().splitlines()[:25]:
            print(f'    {line}')
    if missing:
        fail('not in the compile database: ' + ', '.join(missing))
    if failed:
        fail('compile check failed for ' + ', '.join(failed))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
