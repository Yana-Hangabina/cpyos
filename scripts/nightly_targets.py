"""Canonical firmware target mapping and artifact naming."""

from pathlib import Path


TARGETS = {
    'xteink_x4': {
        'deviceSlug': 'xteink',
        'models': ['xteink_x3', 'xteink_x4'],
        'boardTag': 'x4',
        'chip': 'ESP32-C3',
        'chipId': 0x0005,
        'environments': {'stable': 'gh_release', 'nightly': 'gh_release_rc'},
        'supportedChannels': ['stable', 'nightly'],
        'fullInstall': False,
        'partitionsCsv': 'partitions.csv',
        # No-PSRAM targets stream the scalable CJK face from spiffs; the full
        # install writes it in the same pass as the app, so nobody has to copy
        # a font by hand after flashing.
        'dataAssets': (('system_font', 'system_font.dat', 'spiffs', 0),),
    },
    'sticky': {
        'deviceSlug': 'sticky',
        'models': ['sticky'],
        'boardTag': 'sticky',
        'chip': 'ESP32-S3',
        'chipId': 0x0009,
        'environments': {'stable': 'sticky-gh_release', 'nightly': 'sticky_nightly'},
        'supportedChannels': ['stable', 'nightly'],
        'fullInstall': True,
    },
    'xteink_x4_pro': {
        'deviceSlug': 'x4pro',
        'models': ['xteink_x4_pro'],
        'boardTag': 'x4pro',
        'chip': 'ESP32-S3',
        'chipId': 0x0009,
        'environments': {'nightly': 'x4pro_nightly'},
        'supportedChannels': ['nightly'],
        'fullInstall': True,
    },
    'm5stack_paper_mono': {
        'deviceSlug': 'papermono',
        'models': ['m5stack_paper_mono'],
        'boardTag': 'papermono',
        'chip': 'ESP32-S3',
        'chipId': 0x0009,
        'environments': {'nightly': 'papermono_nightly'},
        'supportedChannels': ['nightly'],
        'fullInstall': True,
    },
    'eego_a4': {
        'deviceSlug': 'eego-a4',
        'models': ['eego_a4'],
        'boardTag': 'eego_a4',
        'chip': 'ESP32-S3',
        'chipId': 0x0009,
        'environments': {'nightly': 'eego_a4_nightly'},
        'supportedChannels': ['nightly'],
        'fullInstall': True,
        # This target used to declare partitions_readpico.csv ("7.19 MiB slots,
        # shared with Read Pico") while [env:eego_a4_nightly] never selected that
        # table, so the full-install package flashed a partition map the firmware
        # was not built against and the app0 fit check measured the wrong slot.
        # Bigger slots are an env change plus device verification, not a declaration
        # here; partition_layout_conflicts() now refuses the two drifting apart.
        'partitionsCsv': 'partitions.csv',
    },
    'murphy_m4': {
        'deviceSlug': 'murphy-m4',
        'models': ['murphy_m4'],
        'boardTag': 'murphy_m4',
        'chip': 'ESP32-S3',
        'chipId': 0x0009,
        'environments': {'nightly': 'murphy_m4_nightly'},
        'supportedChannels': ['nightly'],
        'fullInstall': True,
    },
    'waveshare_epaper_397': {
        'deviceSlug': 'waveshare-epaper-397',
        'models': ['waveshare_epaper_397'],
        'boardTag': 'waveshare_epaper_397',
        'chip': 'ESP32-S3',
        'chipId': 0x0009,
        'environments': {'nightly': 'waveshare_epaper_397_nightly'},
        'supportedChannels': ['nightly'],
        'fullInstall': True,
    },
    'metalio_eink4': {
        'deviceSlug': 'metalio-eink4',
        'models': ['metalio_eink4'],
        'boardTag': 'metalio_eink4',
        'chip': 'ESP32-S3',
        'chipId': 0x0009,
        'environments': {'nightly': 'metalio_eink4_nightly'},
        'supportedChannels': ['nightly'],
        'fullInstall': True,
        # Same correction as eego_a4: [env:metalio_eink4_nightly] builds against
        # the shared 6.25 MiB table, so that is what the package must describe.
        'partitionsCsv': 'partitions.csv',
    },
    'readpico': {
        'deviceSlug': 'readpico',
        'models': ['readpico', 'read_pico'],
        'boardTag': 'readpico',
        'chip': 'ESP32-S3',
        'chipId': 0x0009,
        'environments': {'nightly': 'readpico_nightly'},
        'supportedChannels': ['nightly'],
        'fullInstall': True,
        # This target packages [env:readpico_nightly], which extends
        # readpico_hardware and therefore builds against
        # partitions_readpico.csv (6.375 MiB slots, no pdfmod partition). The XIP
        # layout belongs to env:readpico_pdf/_pdf_jjwxc; moving this nightly onto
        # that env is what would change the value here, and partition_layout_conflicts()
        # below fails loudly if the two ever disagree.
        'partitionsCsv': 'partitions_readpico.csv',
    },
}

FLAVOR_TOKENS = {'global': 'global', 'zh-CN': 'cn'}
CHANNELS = ('stable', 'nightly')


def targets_for(channel):
    if channel not in CHANNELS:
        raise KeyError(channel)
    return {target_id: target for target_id, target in TARGETS.items() if channel in target['supportedChannels']}


def environment_for(target_id, channel, flavor):
    if flavor not in FLAVOR_TOKENS:
        raise KeyError(flavor)
    return TARGETS[target_id]['environments'][channel]


def version_for(base_version, target_id, channel, flavor, short_sha):
    target = TARGETS[target_id]
    if channel == 'stable':
        return base_version
    if channel != 'nightly':
        raise KeyError(channel)
    parts = [base_version]
    if target_id != 'xteink_x4':
        parts.append(target['deviceSlug'])
    if flavor not in FLAVOR_TOKENS:
        raise KeyError(flavor)
    return f"{'-'.join(parts)}-rc+{short_sha[:7]}"


def asset_name(target_id, source_name):
    target = TARGETS[target_id]
    return f"{target['deviceSlug']}-{source_name}"


def manifest_name(target_id, flavor):
    target = TARGETS[target_id]
    return f"{target['deviceSlug']}-{FLAVOR_TOKENS[flavor]}-manifest.json"


PARTITION_KEY = 'board_build.partitions'
DEFAULT_PARTITIONS_CSV = 'partitions.csv'


def _ini_sections(root):
    """Every ini section, with platformio.local.ini layered on top.

    PlatformIO reads the local file through [platformio] extra_configs and lets it
    override the repo file, so a gate that only read platformio.ini could disagree
    with what the machine actually builds.
    """
    import configparser
    import pathlib

    parser = configparser.ConfigParser(interpolation=None, strict=False)
    for name in ('platformio.ini', 'platformio.local.ini'):
        path = pathlib.Path(root) / name
        if path.is_file():
            parser.read(path, encoding='utf-8')
    return {section: dict(parser.items(section)) for section in parser.sections()}


def _section(name, sections):
    if name in sections:
        return name
    prefixed = name if name.startswith('env:') else f'env:{name}'
    return prefixed if prefixed in sections else None


def _lookup(name, key, sections, trail):
    section = _section(name, sections)
    if section is None or section in trail:
        return None
    body = sections[section]
    if body.get(key, '').strip():
        return body[key].strip()
    parents = [part.strip() for part in body.get('extends', '').split(',') if part.strip()]
    # Later parents win in PlatformIO, so ask them in reverse declaration order.
    for parent in reversed(parents):
        found = _lookup(parent, key, sections, trail + (section,))
        if found is not None:
            return found
    return None


def env_partition_csv(root, env, sections=None):
    """The partition table [env:env] really builds, following `extends`."""
    sections = _ini_sections(root) if sections is None else sections
    return _lookup(env, PARTITION_KEY, sections, ())


def partition_layout_conflicts(root, targets=None):
    """[] when every derivable env table agrees with the target's declared partitionsCsv.

    A target carries the full-install segment map and the data-partition base, so a
    declaration that disagrees with the built image describes a flash layout the
    firmware does not use: packs land where nothing reads them and the device boots
    anyway. That is a silent failure, which is why it is checked here instead of on
    a user's reader.

    An environment whose extends chain declares no table at all is not a conflict:
    with nothing to compare against, the declaration is the only statement of intent,
    and refusing there would reject synthetic packaging roots and any env that relies
    on PlatformIO's default rather than an explicit line.
    """
    problems = []
    sections = _ini_sections(root)
    for target_id, target in (TARGETS if targets is None else targets).items():
        declared = target.get('partitionsCsv', DEFAULT_PARTITIONS_CSV)
        for channel, env in target.get('environments', {}).items():
            actual = env_partition_csv(root, env, sections)
            if actual is not None and actual != declared:
                problems.append(
                    f'{target_id}/{channel}: target declares {declared} but '
                    f'[env:{env}] builds {actual}')
    return problems


def environment_from_firmware(path):
    """The PlatformIO environment an artifact was built by, or None.

    [.pio/build/<env>/firmware.bin] carries it, and a release pipeline that verifies
    a PDF build while packaging the nightly env needs the table of the image it was
    handed, not of whichever channel a target id happens to describe. Returning None
    for any other layout keeps callers honest: they must not guess.
    """
    parts = Path(str(path)).parts
    for index in range(len(parts) - 2):
        if parts[index] == '.pio' and parts[index + 1] == 'build':
            return parts[index + 2]
    return None


def partitions_csv_for(root, target_id=None, channel=None, firmware=None, flavor='global', target=None):
    """The partition table that applies, derived from the env instead of re-declared.

    Resolution order, most specific first: the environment named by `firmware`, then
    the target's environment for `channel`, then the target's explicit partitionsCsv
    (an override/assertion for devices whose release intentionally differs).
    """
    sections = _ini_sections(root)
    for env in (environment_from_firmware(firmware) if firmware else None,
                environment_for(target_id, channel, flavor) if target_id and channel else None):
        if not env:
            continue
        derived = env_partition_csv(root, env, sections)
        if derived is not None:
            return derived
    body = TARGETS.get(target_id or '', {}) if target is None else target
    return body.get('partitionsCsv', DEFAULT_PARTITIONS_CSV)


def matrix(channel):
    return {
        'include': [
            {
                'targetId': target_id,
                'deviceSlug': target['deviceSlug'],
                'environment': environment_for(target_id, channel, 'global'),
            }
            for target_id, target in targets_for(channel).items()
        ]
    }
