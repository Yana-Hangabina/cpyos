#!/usr/bin/env python3
"""Drift guard: a scratch destination must never be staged twice.

`HttpDownloader::downloadToFile()` stages the body at `<dest>.part` and renames
it into place. That is right for a caller that names its real destination and
wrong for one that already picked a scratch name, verifies the file, and
publishes it itself -- the latter gets `<name>.part.part`, an extra FAT rename,
and a failure that surfaces as a generic storage error. 0.8.2 shipped exactly
that for font packs, plugins and AirPage images.

The rule is now expressible at the call site (`Staging::CallerOwned`), so this
check reads every call and rejects a scratch destination that did not say so.
It carries two synthetic fixtures -- the broken shape and the fixed shape -- so a
parser that silently matches nothing cannot report a clean tree.
"""

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CALL = re.compile(r"downloadToFile\s*\(")
SCRATCH_HINTS = (".part", "partpath", "part_path", "tmppath", "tmp_path")

SIGNATURE = (
    "url",
    "dest",
    "progress",
    "cancelFlag",
    "username",
    "password",
    "headers",
    "downgradeRedirectsToHttp",
    "timeoutMs",
    "staging",
)


def split_arguments(text: str) -> list[str]:
    """Split a call's argument list on top-level commas."""
    args: list[str] = []
    depth = 0
    current = ""
    in_string = False
    escape = False
    for ch in text:
        if in_string:
            current += ch
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
            current += ch
        elif ch in "([{":
            depth += 1
            current += ch
        elif ch in ")]}":
            depth -= 1
            current += ch
        elif ch == "," and depth == 0:
            args.append(current.strip())
            current = ""
        else:
            current += ch
    if current.strip():
        args.append(current.strip())
    return args


def find_calls(source: str) -> list[tuple[str, str]]:
    """Return (destination, rest-of-call) for every downloadToFile(...) found."""
    found: list[tuple[str, str]] = []
    for match in CALL.finditer(source):
        open_index = source.index("(", match.start())
        depth = 0
        for i in range(open_index, len(source)):
            ch = source[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    args = split_arguments(source[open_index + 1 : i])
                    if len(args) >= 2:
                        found.append((args[1], ",".join(args[2:])))
                    break
    return found


def looks_scratch(destination: str, constants: dict[str, str]) -> bool:
    value = destination.strip()
    lowered = value.lower()
    if value in constants:
        lowered = constants[value].lower()
    return any(hint in lowered for hint in SCRATCH_HINTS)


def collect_scratch_bindings(source: str) -> set[str]:
    """Names bound to something that ends in `.part`, e.g. `temp = target + ".part"`.

    A variable name alone is not evidence -- `temp` could hold a final path. The
    assignment is, so the guard reads it instead of guessing from the name.
    """
    names: set[str] = set()
    for match in re.finditer(r"(\w+)\s*=\s*[^;\n]*\"[^\"]*\.part\"[^;\n]*;", source):
        names.add(match.group(1))
    return names


def collect_string_constants(source: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for match in re.finditer(r"(?:constexpr|const)\s+(?:char\s*\*\s*const\s+|const\s*char\s*\*)?\w+\s*\[\s*\]\s*=\s*\"([^\"]*)\"", source):
        name = source[: match.start()].split()[-1]
        out[str(name)] = match.group(1)
    for match in re.finditer(r"constexpr\s+const\s+char\*\s+(\w+)\s*=\s*\"([^\"]*)\"", source):
        out[match.group(1)] = match.group(2)
    return out


def violations(source: str) -> list[str]:
    constants = collect_string_constants(source)
    scratch_names = collect_scratch_bindings(source)
    bad: list[str] = []
    for destination, rest in find_calls(source):
        value = destination.strip()
        if value in scratch_names:
            scratch = True
        else:
            scratch = looks_scratch(destination, constants)
        if not scratch:
            continue
        if "CallerOwned" in rest:
            continue
        bad.append(destination)
    return bad


class TestDownloadStaging(unittest.TestCase):
    def setUp(self) -> None:
        # Positive control: the parser must see the real calls, not nothing.
        self.sources = sorted(REPO.glob("src/**/*.cpp")) + sorted(REPO.glob("ports/**/*.cpp"))
        self.total = 0
        for path in self.sources:
            self.total += len(find_calls(path.read_text(encoding="utf-8", errors="replace")))

    def test_parser_finds_the_real_calls(self) -> None:
        self.assertGreaterEqual(self.total, 6, "the scan matched no real call sites")

    def test_no_scratch_path_is_staged_twice(self) -> None:
        offenders: list[str] = []
        for path in self.sources:
            for destination in violations(path.read_text(encoding="utf-8", errors="replace")):
                offenders.append(f"{path.relative_to(REPO)}: {destination}")
        self.assertEqual([], offenders, "scratch destinations must pass Staging::CallerOwned")

    def test_broken_shape_is_reported(self) -> None:
        broken = (
            'constexpr const char* kDownloadPartPath = "/AirPage/latest.bmp.part";\n'
            '  const std::string temp = target + ".part";\n'
            "  auto r = HttpDownloader::downloadToFile(url, temp, progress, cancel, \"\", \"\", {}, false, 20000);\n"
            "  auto r = HttpDownloader::downloadToFile(url, kDownloadPartPath);\n"
        )
        self.assertEqual(["temp", "kDownloadPartPath"], violations(broken))

    def test_fixed_shape_is_allowed(self) -> None:
        fixed = (
            'constexpr const char* kDownloadPartPath = "/AirPage/latest.bmp.part";\n'
            '  const std::string temp = target + ".part";\n'
            "  auto r = HttpDownloader::downloadToFile(url, temp, progress, cancel, \"\", \"\", {}, false, 20000,\n"
            "        HttpDownloader::Staging::CallerOwned);\n"
            "  auto r = HttpDownloader::downloadToFile(url, \"/fonts/Face.ttf\", progress);\n"
        )
        self.assertEqual([], violations(fixed))

    def test_signature_order_is_documented(self) -> None:
        # The new parameter must stay last, or a positional caller silently
        # changes what `timeoutMs` means.
        header = (REPO / "src/network/HttpDownloader.h").read_text(encoding="utf-8")
        block = header[header.index("static DownloadError downloadToFile") :]
        block = block[: block.index(");")]
        self.assertIn("int timeoutMs = 0, Staging staging = Staging::InPlace", " ".join(block.split()))
        self.assertEqual("staging", SIGNATURE[-1])


if __name__ == "__main__":
    unittest.main()
