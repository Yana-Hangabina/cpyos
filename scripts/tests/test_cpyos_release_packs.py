"""build_cpyos_release.py's data-manifest completeness gate.

A release bundle is assembled from build/data_manifest.txt, and every role that row is
missing removes a feature without any symptom at boot: no i18n row means 33 languages
silently fall back to English, no web row means the file-transfer page is a recovery stub,
no core row means ordinary Han in book titles and shelf rows draws boxes. The generator
already refuses to omit the core when it fits, so the bundler refusing to package a
manifest that left one out is the second half of that guarantee -- the half that runs at
release time, against whatever file happens to be on disk.
"""

import contextlib
import hashlib
import inspect
import io
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
ROOT = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))

import build_cpyos_release as bundle  # noqa: E402
import data_manifest  # noqa: E402
import generate_pack_manifest as packgen  # noqa: E402


def render_packs(root: Path) -> str:
    """What scripts/generate_pack_manifest.py would write for this tree."""
    return packgen.render(root)


def write_manifest(root: Path, roles) -> Path:
    build = root / "build"
    build.mkdir(parents=True, exist_ok=True)
    lines = ["# generated", "partition\tspiffs\t0x00df0000\t0x00200000"]
    for index, role in enumerate(roles):
        blob = build / f"{role}.dat"
        blob.write_bytes(role.encode("utf-8") * (16 + index))
        data = blob.read_bytes()
        address = 0xDF0000 + index * 0x1000
        lines.append("\t".join((
            "data", role, f"0x{index * 0x1000:08x}", f"0x{address:08x}",
            str(len(data)), hashlib.sha256(data).hexdigest(), f"build/{role}.dat",
        )))
    path = build / "data_manifest.txt"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


class RequiredDataRolesTest(unittest.TestCase):
    def test_complete_manifest_is_accepted(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_manifest(root, bundle.REQUIRED_DATA_ROLES)
            rows = bundle.read_data_manifest(root)
            self.assertEqual([r["role"] for r in rows], list(bundle.REQUIRED_DATA_ROLES))
            self.assertEqual(rows[0]["offset"], 0xDF0000)

    def test_missing_core_is_refused(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_manifest(root, [r for r in bundle.REQUIRED_DATA_ROLES if r != "core"])
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                bundle.read_data_manifest(root)
            self.assertIn("core", stderr.getvalue())

    def test_every_required_role_is_individually_enforced(self):
        """One omission per run: a loop that only checked the first role would pass this."""
        import tempfile

        for missing in bundle.REQUIRED_DATA_ROLES:
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                write_manifest(root, [r for r in bundle.REQUIRED_DATA_ROLES if r != missing])
                with self.assertRaises(SystemExit, msg=missing):
                    bundle.read_data_manifest(root)

    def test_manifest_partition_must_match_the_image(self):
        """The 0.8.0 failure mode: a manifest for another layout, bundled without complaint.

        Read Pico has two partition tables (the plain one and the XIP one the shipping
        environment uses). Packs written at the wrong base are simply never read, so this
        comparison is the difference between a release that works and one that boots into
        English-only UI and a missing reading font.
        """
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_manifest(root, bundle.REQUIRED_DATA_ROLES)
            self.assertEqual(
                len(bundle.read_data_manifest(root, spiffs=(0xDF0000, 0x200000))), 6)
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                bundle.read_data_manifest(root, spiffs=(0xE70000, 0x180000))
            self.assertIn("describes spiffs", stderr.getvalue())

    def test_a_swapped_blob_is_refused(self):
        """The path column names a tiered core file; replacing its bytes must be caught.

        This is the release-time mirror of the 15.9 hazard: build/cjk_core.dat is an alias
        for whichever tier was generated last, so a bundle assembled after a Read Pico
        build on the same machine could otherwise carry the narrow carrier while its
        manifest still describes the wide one.
        """
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_manifest(root, bundle.REQUIRED_DATA_ROLES)
            (root / "build" / "core.dat").write_bytes(b"someone-else-tier" * 40)
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                bundle.read_data_manifest(root)
            self.assertIn("no longer matches", stderr.getvalue())



class ReleaseContentsTest(unittest.TestCase):
    """The image must actually contain what this release is named for.

    A build of the wrong environment (readpico_nosource, or a checkout without the private
    src/jjwxc) succeeds and boots -- it just has no JJWXC app and no PDF engine. Nothing in
    the version, size or SHA checks would notice, so the bundle step reads the bytes.
    """

    def test_both_markers_present_is_accepted(self):
        blob = b"".join(bundle.REQUIRED_IMAGE_MARKERS)
        bundle.check_release_contents(blob)

    def test_missing_jjwxc_is_refused(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
            bundle.check_release_contents(b"not a CPYOS PDF engine image")
        self.assertIn("VIP chain ok:", stderr.getvalue())

    def test_missing_pdf_loader_is_refused(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
            bundle.check_release_contents(b"VIP chain ok:")
        self.assertIn("PDF engine image", stderr.getvalue())

    def test_markers_exist_in_a_real_image(self):
        """Guard against a check that can never pass because a string was reworded."""
        elf = ROOT / ".pio/build/readpico_pdf_jjwxc/firmware.bin"
        if not elf.is_file():
            self.skipTest("no local build; the marker strings are unverified here")
        blob = elf.read_bytes()
        for marker in bundle.REQUIRED_IMAGE_MARKERS:
            self.assertIn(marker, blob, f"{marker!r} is not in the image anyone built today")



class ManifestMatchesFirmwareConstantsTest(unittest.TestCase):
    """The release manifest must agree with the offsets the firmware actually reads.

    These numbers live in C++ headers (SystemCoreFont.h, I18nPack.h, WebPackFormat.h,
    EmojiPackFormat.h, HyphenationStore.h) because the loader is the truth, and the
    generator parses them out rather than restating them. This test is the other half: it
    checks the generated manifest against the headers on the current tree, so a stale
    build/data_manifest.txt -- left over from another partition table, or from before an
    offset moved -- fails here instead of on a user's reader. The Plus layout has the same
    assertion in ports/metalio_plus/test/test_data_layout.py.
    """

    CONSTANTS = {
        'web': ('src/network/WebPackFormat.h', 'WEB_PACK_PARTITION_OFFSET'),
        'hyphen': ('lib/Epub/Epub/hyphenation/HyphenationStore.h', 'HYPN_PARTITION_OFFSET'),
        'i18n': ('lib/I18n/I18nPack.h', 'I18N_PACK_PARTITION_OFFSET'),
        'emoji': ('src/EmojiPackFormat.h', 'kPartitionOffset'),
        'core': ('src/SystemCoreFont.h', 'kPartitionOffset'),
    }

    def read_constant(self, rel, name):
        text = (ROOT / rel).read_text(encoding='utf-8')
        m = re.search(rf"{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)", text)
        self.assertIsNotNone(m, f"{name} not found in {rel}")
        return int(m.group(1), 0)

    def rows_for(self, env):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'manifest.txt'
            subprocess.run([sys.executable, str(ROOT / 'scripts' / 'generate_data_manifest.py'),
                            '--env', env, '--out', str(out)], check=True,
                           capture_output=True, text=True, cwd=str(ROOT))
            return data_manifest.rows(out)

    def test_shipped_manifest_offsets_match_the_headers(self):
        rows = {r['name']: r for r in self.rows_for('readpico_pdf_jjwxc')}
        for role, (header, const) in self.CONSTANTS.items():
            self.assertIn(role, rows, f"manifest has no {role} row")
            want = self.read_constant(header, const)
            self.assertEqual(rows[role]['offset'], want,
                             f"{role}: manifest says partition offset 0x{rows[role]['offset']:X}, "
                             f"but {header} says 0x{want:X}")

    def test_pinyin_is_at_partition_zero(self):
        # PinyinDb.cpp reads from offset 0 unconditionally, so the manifest cannot choose.
        rows = {r['name']: r for r in self.rows_for('readpico_pdf_jjwxc')}
        self.assertEqual(rows['pinyin']['offset'], 0)

    def test_rows_are_ordered_and_do_not_overlap(self):
        rows = sorted(self.rows_for('readpico_pdf_jjwxc'), key=lambda r: r['offset'])
        for prev, nxt in zip(rows, rows[1:]):
            self.assertLessEqual(prev['offset'] + prev['size'], nxt['offset'],
                                 f"{prev['name']} overlaps {nxt['name']}")
        reserved = 0x80000
        for row in rows:
            end = row['offset'] + row['size']
            self.assertFalse(row['offset'] < reserved < end,
                             f"{row['name']} would overwrite the reserved factory bytes")

    def test_the_two_readpico_layouts_select_different_tiers(self):
        xip = {r['name']: r for r in self.rows_for('readpico_pdf_jjwxc')}['core']
        wide = {r['name']: r for r in self.rows_for('readpico_jjwxc')}['core']
        self.assertGreater(wide['size'], xip['size'])
        self.assertTrue(xip['path'].endswith('cjk_core_3500.dat'), xip['path'])
        self.assertTrue(wide['path'].endswith('cjk_core_8105.dat'), wide['path'])


class PackManifestAgreementTest(unittest.TestCase):
    """The bundle and the OTA pack manifest must describe the same files by the same names.

    This reproduces the tier-rename drift rather than trusting that it cannot happen:
    build/data_manifest.txt names a tiered carrier (``cjk_core_3500.dat``) while
    readpico-packs.txt names the alias a device asks for (``cjk_core.dat``). When the
    bundler derived its published name from the file on disk, it shipped the tiered name,
    every device OTA 404'd on the core, and -- because an app update is still useful without
    its data packs -- the update completed with no reading font. Nothing else in the release
    checks noticed, so the agreement is asserted here.
    """

    def bundle_root(self, tmp: str) -> Path:
        """A manifest whose core row points at a tier-named carrier file.

        That is what generate_data_manifest.py writes since the tier split, so the fixture
        has to look like it: the row's file is ``build/cjk_core_3500.dat`` while the role is
        still ``core``.
        """
        root = Path(tmp)
        write_manifest(root, bundle.REQUIRED_DATA_ROLES)
        build = root / 'build'
        index = list(bundle.REQUIRED_DATA_ROLES).index('core')
        data = (build / 'core.dat').read_bytes()
        (build / 'core.dat').unlink()
        tiered = build / 'cjk_core_3500.dat'
        tiered.write_bytes(data)
        lines = [
            line for line in (build / 'data_manifest.txt').read_text(encoding='utf-8').splitlines()
            if not line.startswith('data\tcore\t')
        ]
        lines.append('\t'.join((
            'data', 'core', f'0x{index * 0x1000:08x}', f'0x{0xDF0000 + index * 0x1000:08x}',
            str(len(data)), hashlib.sha256(data).hexdigest(), f'build/{tiered.name}',
        )))
        (build / 'data_manifest.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
        return root

    def tamper(self, root: Path, asset: str, **changes) -> str:
        """The generated pack manifest with one field of one line rewritten."""
        out = []
        for line in render_packs(root).splitlines():
            if line and not line.startswith('#') and line.split()[0] == asset:
                fields = line.split()
                for index, (key, value) in enumerate(changes.items()):
                    fields[{'offset': 1, 'size': 2, 'sha256': 3}[key]] = value
                line = ' '.join(fields)
            out.append(line)
        return '\n'.join(out) + '\n'

    def test_a_tiered_carrier_is_still_published_under_the_alias(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = self.bundle_root(tmp)
            row = next(r for r in bundle.read_data_manifest(root) if r['role'] == 'core')
            self.assertTrue(row['source'].name.startswith('cjk_core_3500'), row['source'].name)
            self.assertEqual(row['name'], 'cjk_core.dat',
                             'devices fetch this name from readpico-packs.txt')

    def test_generator_and_bundler_agree_on_the_real_tree(self):
        """The pair this repo can actually build today must pass the same check."""
        manifest = ROOT / 'build' / 'data_manifest.txt'
        if not manifest.is_file():
            self.skipTest('no generated data manifest on this tree')
        import generate_pack_manifest as packgen

        rows = bundle.read_data_manifest(ROOT, spiffs=None)
        bundle.check_pack_manifest_agreement(rows, packgen.render(ROOT))
        core = next(r for r in rows if r['role'] == 'core')
        self.assertEqual(core['name'], 'cjk_core.dat')

    def test_a_pack_line_that_was_dropped_is_refused(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = self.bundle_root(tmp)
            rows = bundle.read_data_manifest(root)
            packs = '\n'.join(
                line for line in render_packs(root).splitlines() if not line.startswith('cjk_core.dat')
            )
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                bundle.check_pack_manifest_agreement(rows, packs)
            self.assertIn('never lists it', stderr.getvalue())

    def test_a_stale_size_in_a_pack_line_is_refused(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = self.bundle_root(tmp)
            rows = bundle.read_data_manifest(root)
            packs = self.tamper(root, 'cjk_core.dat', size='999999')
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                bundle.check_pack_manifest_agreement(rows, packs)
            self.assertIn('describes cjk_core.dat', stderr.getvalue())

    def test_a_stale_offset_in_a_pack_line_is_refused(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = self.bundle_root(tmp)
            rows = bundle.read_data_manifest(root)
            packs = self.tamper(root, 'i18n_pack.dat', offset='0x0e6000')
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                bundle.check_pack_manifest_agreement(rows, packs)
            self.assertIn('describes i18n_pack.dat', stderr.getvalue())

    def test_a_pack_line_for_an_unbundled_blob_is_refused(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = self.bundle_root(tmp)
            rows = bundle.read_data_manifest(root)
            packs = render_packs(root) + 'mystery.dat 0x1f0000 10 ' + '0' * 64 + ' 0x200000\n'
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                bundle.check_pack_manifest_agreement(rows, packs)
            self.assertIn('does not carry', stderr.getvalue())

    def test_alias_names_are_the_ones_already_shipped_devices_ask_for(self):
        """Sharing the table stops the two scripts drifting; this stops the table moving.

        Devices resolve ``<pack asset>`` next to ``readpico-ota.json``, and 0.8.0 published
        ``cjk_core.dat`` (verified against the live release asset list). Renaming an alias
        here would be byte-identical on both sides of the check above and still 404 on every
        device, so the names themselves are pinned.
        """
        self.assertEqual(data_manifest.PACK_ASSETS, {
            'pinyin': 'pinyin.dat',
            'web': 'web_pack.dat',
            'hyphen': 'hyphen.dat',
            'i18n': 'i18n_pack.dat',
            'emoji': 'emoji.dat',
            'core': 'cjk_core.dat',
        })
        self.assertEqual(packgen.ASSETS, data_manifest.PACK_ASSETS)
        self.assertEqual(bundle.DATA_FILES, data_manifest.PACK_ASSETS)

    def test_an_unmapped_role_has_no_filename_fallback(self):
        """The bug class: inventing a name from the carrier file instead of refusing."""
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
            bundle.published_asset_name('mystery', Path('build/mystery.dat'))
        self.assertIn('PACK_ASSETS', stderr.getvalue())

    def test_a_malformed_pack_line_is_refused(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = self.bundle_root(tmp)
            rows = bundle.read_data_manifest(root)
            for bad in ('cjk_core.dat 0x15c000 642887', 'a b c d e f'):
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                    bundle.check_pack_manifest_agreement(rows, bad + '\n')
                self.assertIn('malformed', stderr.getvalue())


class OtaLayoutBaselineTest(unittest.TestCase):
    """0.8.1 is meant to be OTA-able, and the previous baseline forbade exactly that.

    The guard compared app1/spiffs against 0x740000/0xE70000 -- the pre-XIP layout that
    0.8.0 replaced. Neither current Read Pico table matches it, so every release looked like
    a migration: only CI's --full-install-only kept the build alive, and the first release
    that wanted an OTA manifest (0.8.1, per the plan) would have been refused by a check
    that no longer described anything real.
    """

    def xip(self):
        csv = bundle.nightly_targets.env_partition_csv(ROOT, bundle.ENVIRONMENT)
        self.assertEqual(csv, 'partitions_readpico_xip.csv',
                         f'{bundle.ENVIRONMENT} now builds {csv}; OTA_LAYOUT and the release '
                         'notes OTA claim have to move together with it')
        return bundle.package_nightly_target.verify_partition_csv(ROOT, csv)

    def test_baseline_is_the_table_this_release_builds(self):
        partitions = self.xip()
        for role, offset in bundle.OTA_LAYOUT.items():
            self.assertEqual(partitions[role][0], offset,
                             f'OTA_LAYOUT says {role} at 0x{bundle.OTA_LAYOUT[role]:x} but '
                             f'{bundle.ENVIRONMENT} builds it at 0x{partitions[role][0]:x}')

    def test_the_shipped_layout_allows_an_ota_manifest(self):
        bundle.check_ota_layout(self.xip(), full_install_only=False)

    def test_both_older_layouts_are_refused_as_migrations(self):
        for label, partitions in (
            ('pre-XIP (0.7.0 and earlier)',
             {'app1': (0x740000, 0x640000), 'spiffs': (0xE70000, 0x180000)}),
            ('the non-XIP readpico table',
             {'app1': (0x670000, 0x660000), 'spiffs': (0xCD0000, 0x320000)}),
        ):
            with self.subTest(label):
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                    bundle.check_ota_layout(partitions, full_install_only=False)
                self.assertIn('moved the partition layout', stderr.getvalue())
                self.assertIn('spiffs', stderr.getvalue())

    def test_a_migration_is_fine_as_a_full_install(self):
        old = {'app1': (0x740000, 0x640000), 'spiffs': (0xE70000, 0x180000)}
        bundle.check_ota_layout(old, full_install_only=True)
        bundle.check_ota_layout(self.xip(), full_install_only=True)

    def test_the_guard_is_actually_called_during_assembly(self):
        """The direct calls above prove nothing about the release path itself.

        Deleting the call from build() keeps every other test here green while a real
        release ships an OTA manifest it should have refused -- the same shape as a
        conditional-compile fix that never made it into the image.
        """
        source = inspect.getsource(bundle.build)
        self.assertIn('check_ota_layout(partitions, full_install_only)', source)
        self.assertIn('check_pack_manifest_agreement(data_rows, packs_text)', source)


class FullInstallClaimTest(unittest.TestCase):
    """--full-install-only hides the layout guard, so it has to be justified.

    0.8.0 published no upgrade manifest at all. With the flag set, check_ota_layout stays
    silent by design, so a release could keep inheriting that choice forever and nobody would
    see a question asked. The gate below only fires in the case where nothing forces the
    flag -- a layout that still matches OTA_LAYOUT, which is exactly what 0.8.1 has.
    """

    def xip(self):
        csv = bundle.nightly_targets.env_partition_csv(ROOT, bundle.ENVIRONMENT)
        return bundle.package_nightly_target.verify_partition_csv(ROOT, csv)

    def test_matching_layout_with_the_flag_and_no_reason_is_refused(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
            bundle.check_full_install_claim(self.xip(), full_install_only=True, reason='')
        self.assertIn('--no-ota-reason', stderr.getvalue())
        self.assertIn('manifest-upgrade.json', stderr.getvalue())

    def test_whitespace_is_not_a_reason(self):
        with self.assertRaises(SystemExit):
            bundle.check_full_install_claim(self.xip(), full_install_only=True, reason='   ')

    def test_a_stated_reason_is_accepted_and_echoed(self):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            bundle.check_full_install_claim(self.xip(), full_install_only=True,
                                            reason='data pack offsets moved')
        self.assertIn('data pack offsets moved', stdout.getvalue())

    def test_a_real_migration_needs_no_second_justification(self):
        old = {'app1': (0x740000, 0x640000), 'spiffs': (0xE70000, 0x180000)}
        bundle.check_full_install_claim(old, full_install_only=True, reason='')

    def test_an_ota_release_is_unaffected(self):
        bundle.check_full_install_claim(self.xip(), full_install_only=False, reason='')

    def test_the_guard_runs_during_assembly(self):
        source = inspect.getsource(bundle.build)
        self.assertIn('check_full_install_claim(partitions, full_install_only, no_ota_reason)',
                      source)

    def test_the_cli_still_builds_its_parser(self):
        """Catches a parser that references an unimported module.

        Importing this script does not execute argparse's defaults, so a NameError on os.environ
        survived an "imports fine" check; --help is the cheapest way to run that code.
        """
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), self.assertRaises(SystemExit):
            bundle.main(['--version', '0.0.0', '--help'])
        self.assertIn('--no-ota-reason', stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
