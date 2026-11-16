#!/usr/bin/env python3
"""Make ESP-IDF 6.1 safe with CMake 4.x property files.

`__build_write_properties()` writes every build property into a quoted string
without escaping inner quotes. `IDF_VER="v6.1"` is one such property, so the
generated build_properties.temp.cmake contains `"…IDF_VER="v6.1""`, which
CMake 4.x reports as a syntax warning and IDF's component validation turns into
a hard error on a fresh configure.

Run once per ESP-IDF checkout:

    python3 ports/metalio_plus/scripts/patch_idf_cmake_quotes.py $IDF_PATH
"""

import sys
from pathlib import Path

MARKER = "Values may legitimately contain quotes"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    build_cmake = Path(sys.argv[1]) / "tools" / "cmake" / "build.cmake"
    text = build_cmake.read_text()
    if MARKER in text:
        print(f"already patched: {build_cmake}")
        return 0
    old = '''        idf_build_get_property(val ${property})
        set(build_properties_text "${build_properties_text}\\nset(${property} \\"${val}\\")")'''
    new = '''        idf_build_get_property(val ${property})
        # Values may legitimately contain quotes (IDF_VER="v6.1"); escape them
        # or the generated file is syntactically invalid (CMake 4.x warns).
        string(REPLACE "\\"" "\\\\\\"" escaped_val "${val}")
        set(build_properties_text "${build_properties_text}\\nset(${property} \\"${escaped_val}\\")")'''
    if old not in text:
        print(f"unexpected build.cmake content in {build_cmake}", file=sys.stderr)
        return 1
    build_cmake.write_text(text.replace(old, new))
    print(f"patched: {build_cmake}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
