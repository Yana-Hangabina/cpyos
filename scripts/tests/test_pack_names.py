#!/usr/bin/env python3
"""The device-facing pack list must name the files the bundle actually carries.

`readpico-packs.txt` is what OtaUpdater reads to decide which blob to download and
where to write it.  It used to name a stable alias (`cjk_core.dat`) while the bundle
shipped the tiered carrier (`cjk_core_3500.dat`); the download then missed, and because
data packs are optional the updater skipped it and booted without a reading core.  The
0.8.1 release carried exactly that mismatch, which is why this is a test and not a
comment.
"""
from __future__ import annotations

import hashlib
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import data_manifest  # noqa: E402
import generate_pack_manifest  # noqa: E402


def blob(path: Path, payload: bytes) -> tuple[int, str]:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    return len(payload), hashlib.sha256(payload).hexdigest()


class PackNames(unittest.TestCase):
    def repo(self, core_name: str = "cjk_core_3500.dat", role: str = "core"):
        tmp = Path(tempfile.mkdtemp())
        build = tmp / "build"
        rows = []
        offset = 0
        for name, relative, payload in (
            ("pinyin", "build/pinyin.dat", b"p" * 32),
            ("web", "build/web_pack.dat", b"w" * 24),
            ("hyphen", "build/hyphen.dat", b"h" * 40),
            ("i18n", "build/i18n_pack.dat", b"i" * 16),
            ("emoji", "build/emoji.dat", b"e" * 12),
            (role, f"build/{core_name}", b"c" * 48),
        ):
            size, digest = blob(tmp / relative, payload)
            rows.append(f"data\t{name}\t0x{offset:06x}\t0x{0xE70000 + offset:06x}\t{size}\t{digest}\t{relative}")
            offset += size
        manifest = build / "data_manifest.txt"
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text(
            "# test fixture\n"
            f"partition\tspiffs\t0xe70000\t0x{0xE70000 + offset + 0x1000:x}\n" +
            "\n".join(rows) + "\n",
            encoding="utf-8")
        return tmp

    def assets(self, repo: Path) -> list[str]:
        text = generate_pack_manifest.render(repo)
        return [line.split()[0] for line in text.splitlines() if line and not line.startswith("#")]

    def test_pack_list_names_the_published_files(self):
        names = self.assets(self.repo())
        self.assertIn("cjk_core_3500.dat", names)
        self.assertNotIn("cjk_core.dat", names,
                         "the pack list named an alias the bundle does not ship")
        self.assertIn("i18n_pack.dat", names)

    def test_no_rename_table_survives(self):
        # A role must map to itself; a second truth is how the alias drifted.
        for role in ("pinyin", "web", "hyphen", "i18n", "emoji", "core"):
            self.assertEqual(data_manifest.asset(role), role)

    def test_unknown_role_is_refused(self):
        with self.assertRaises(SystemExit):
            generate_pack_manifest.render(self.repo(role="mystery"))


if __name__ == "__main__":
    unittest.main()
