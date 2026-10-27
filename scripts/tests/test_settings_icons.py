#!/usr/bin/env python3
"""Static audit of the CPY settings rows' leading icons.

`SettingsActivity::rebuildRowItems()` asks `settings_icons::forSetting()` for
every row of the four Settings tabs. When the lookup returns nullptr the row is
drawn without a glyph, and because the list layout only reserves the icon slot
for rows that have one (FreeInkUI `measureListRow`/`list`), the label starts
~58 px further left. A single unmapped row is therefore a visible misalignment,
not a harmless omission.

The audit reads the real sources, so it fails when a setting is added without an
icon or when an existing mapping is deleted:

  * `src/SettingsList.h` - base entries whose category is DISPLAY / READER /
    CONTROLS / SYSTEM, with the Read Pico capability macros evaluated. Rows
    flagged `.withTextSettings()` / `.withReadingStatsSettings()` are excluded:
    they render in their own subpages, which carry no icons.
  * `src/activities/settings/SettingsActivity.cpp` - the dynamically built
    dictionary row and every ACTION row appended to one of the four lists.

The expected set is a *superset* of what Read Pico shows: a row hidden by a
runtime board filter (e.g. a gesture row erased when the panel has touch) still
has to be mapped, so adding a mapping is never conditional on emulation.

Scope: the icons only render on the CPY high-density profile
(`CROSSMUX_UI_PROFILE_HIGH_DPI` + `usesCpyLayout()`), i.e. `[env:readpico]`/
`[env:simulator_readpico]`. Other boards do not compile them in. Board-only rows
outside those four categories (`STR_CUSTOMISE_STATUS_BAR` subpage rows such as
`STR_XTC_STATUS_BAR`, the web-only KOReader/OPDS/clock entries, and Murphy M4's
hardware batch) are not part of this audit because they never take this path.

Run: python3 scripts/tests/test_settings_icons.py
"""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
ICONS_HEADER = ROOT / 'src/components/icons/listIcons48.h'
MAPPING_HEADER = ROOT / 'src/components/icons/settings_icons.h'
SETTINGS_LIST = ROOT / 'src/SettingsList.h'
SETTINGS_ACTIVITY = ROOT / 'src/activities/settings/SettingsActivity.cpp'

# Capability macros for [env:readpico] (platformio.ini) and the BoardConfig
# defaults the Read Pico profile derives from them. Only macros that gate a
# SettingInfo entry need a value here.
READPICO_DEFINES = {
    'FREEINK_DEVICE_READPICO': 1,
    'CROSSMUX_UI_PROFILE_HIGH_DPI': 1,
    'FREEINK_CAP_FRONTLIGHT': 0,
    'FREEINK_CAP_HAPTIC': 0,
    'FREEINK_CAP_WARMLIGHT': 0,
    'FREEINK_CAP_BLE_HID_HOST': 1,
    'FREEINK_DEVICE_MURPHY_M4': 0,
    'CROSSPOINT_CAP_SOUND_FEEDBACK': 0,
    'CROSSPOINT_CAP_BUZZER_FEEDBACK': 1,
    'ENABLE_CHINESE_VERSION': 0,
}

CATEGORY_LISTS = {
    'STR_CAT_DISPLAY': 'display',
    'STR_CAT_READER': 'reader',
    'STR_CAT_CONTROLS': 'controls',
    'STR_CAT_SYSTEM': 'system',
}

# Rows that are deliberately icon-less although they can appear in one of the
# four lists. Keep this empty unless there is a reason that is worth writing
# down: an entry here hides the row from the audit forever.
INTENTIONAL_NO_ICON = {}

LIST_NAMES = ('display', 'reader', 'controls', 'system')


def evaluate(expression, defines):
    """Evaluate a #if expression with the given macro table.

    Literal-only interpreter for the C preprocessor subset the settings list
    uses: integer literals, macro names, `!`, `==`, `!=`, `&&`, `||`, parens.
    A real `eval` is avoided so a malformed or foreign expression can never run
    code.
    """
    text = re.sub(r'defined\s*\(?\s*([A-Za-z_]\w*)\s*\)?',
                  lambda m: '1' if defines.get(m.group(1), 0) else '0', expression)
    tokens = re.findall(r'\d+|[A-Za-z_]\w*|&&|\|\||==|!=|!|\(|\)', text)
    index = 0

    def unary():
        nonlocal index
        token = tokens[index]
        index += 1
        if token == '!':
            return not unary()
        if token == '(':
            value = disjunction()
            if tokens[index] != ')':
                raise AssertionError(f'unbalanced parenthesis in #if: {expression}')
            index += 1
            return value
        if token.isdigit():
            return bool(int(token))
        return bool(defines.get(token, 0))

    def comparison():
        nonlocal index
        left = unary()
        while index < len(tokens) and tokens[index] in ('==', '!='):
            operator = tokens[index]
            index += 1
            right = unary()
            left = (left == right) if operator == '==' else (left != right)
        return left

    def conjunction():
        nonlocal index
        value = comparison()
        while index < len(tokens) and tokens[index] == '&&':
            index += 1
            right = comparison()
            value = value and right
        return value

    def disjunction():
        nonlocal index
        value = conjunction()
        while index < len(tokens) and tokens[index] == '||':
            index += 1
            right = conjunction()
            value = value or right
        return value

    result = disjunction()
    if index != len(tokens):
        raise AssertionError(f'unparsed #if expression: {expression}')
    return result


def preprocess(text, defines):
    """Drop #if branches that are inactive for this macro table (no #include)."""
    kept = []
    stack = []  # [parent active, branch active, any branch taken]
    for line in text.splitlines():
        stripped = line.strip()
        directive = re.match(r'#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', stripped)
        parent_active = stack[-1][1] if stack else True
        if directive:
            kind, rest = directive.group(1), directive.group(2).strip()
            if kind in ('if', 'ifdef', 'ifndef'):
                if kind == 'ifdef':
                    condition = bool(defines.get(rest.split()[0], 0))
                elif kind == 'ifndef':
                    condition = not bool(defines.get(rest.split()[0], 0))
                else:
                    condition = evaluate(rest, defines)
                stack.append([parent_active, parent_active and condition, condition])
            elif kind == 'elif':
                parent, _, taken = stack[-1]
                condition = (not taken) and evaluate(rest, defines)
                stack[-1] = [parent, parent and condition, taken or condition]
            elif kind == 'else':
                parent, _, taken = stack[-1]
                stack[-1] = [parent, parent and not taken, True]
            else:
                stack.pop()
            continue
        if parent_active:
            kept.append(line)
    if stack:
        raise AssertionError('unbalanced #if/#endif in the audited source')
    return '\n'.join(kept)


def setting_calls(text, kind=None):
    """Yield (call text, text following the call) for every `SettingInfo::<kind>(...)`."""
    pattern = re.compile(rf'SettingInfo::{kind}\(' if kind else r'SettingInfo::\w+\(')
    cursor = 0
    while True:
        match = pattern.search(text, cursor)
        if not match:
            return
        start = match.start()
        depth = 0
        end = match.end() - 1  # index of the opening parenthesis
        while end < len(text):
            if text[end] == '(':
                depth += 1
            elif text[end] == ')':
                depth -= 1
                if depth == 0:
                    break
            end += 1
        yield text[start:end + 1], text[end + 1:end + 160]
        cursor = end + 1


def base_rows(defines=READPICO_DEFINES):
    """(list name -> StrIds) for the static entries of the four Settings tabs."""
    source = SETTINGS_LIST.read_text()
    start = source.index('static const std::vector<SettingInfo> baseList')
    end = source.index('return baseList;', start)
    active = preprocess(source[start:end], defines)

    rows = {name: set() for name in LIST_NAMES}
    for call, tail in setting_calls(active):
        name = re.search(r'SettingInfo::\w+\(\s*StrId::(STR_\w+)', call)
        categories = re.findall(r'StrId::(STR_CAT_\w+)', call)
        if not name or not categories:
            continue
        target = CATEGORY_LISTS.get(categories[-1])
        if target is None:
            continue
        # Merged into "Text Settings" / "Reading Stats" subpages: those screens
        # are plain menus without the CPY row glyphs.
        if '.withTextSettings()' in tail or '.withReadingStatsSettings()' in tail:
            continue
        rows[target].add(name.group(1))
    return rows


def dynamic_rows():
    """Rows built by lambdas/actions in SettingsActivity::rebuildSettingsLists()."""
    source = SETTINGS_ACTIVITY.read_text()
    start = source.index('void SettingsActivity::rebuildSettingsLists()')
    end = source.index('void SettingsActivity::onEnter()', start)
    active = preprocess(source[start:end], READPICO_DEFINES)

    rows = {name: set() for name in LIST_NAMES}
    # getSettingsList() inserts buildDictionarySetting() into the Reader list
    # when /dictionaries has at least one entry. That is a runtime condition, so
    # the row has to be treated as visible and mapped like any other row.
    rows['reader'].add('STR_DICTIONARY')
    for statement in active.split(';'):
        target = re.search(r'(display|reader|controls|system)Settings\s*\.', statement)
        if not target:
            continue
        # Any SettingInfo kind appended to a list counts, not just Action: a
        # board-only DynamicEnum (Murphy M4's hardware batch) would land in the
        # same list and needs a glyph exactly the same way.
        for name in re.findall(r'SettingInfo::\w+\(\s*StrId::(STR_\w+)', statement):
            rows[target.group(1)].add(name)
    return rows


def visible_rows():
    rows = base_rows()
    for name, extra in dynamic_rows().items():
        rows[name] |= extra
    return rows


def mappings_from_text(text):
    """StrId -> icon identifier (or None) from the settings_icons switch."""
    body = text[text.index('switch (id)') :]
    mapping = {}
    pending = []
    for line in body.splitlines():
        stripped = line.strip()
        for case in re.finditer(r'case StrId::(STR_\w+)\s*:', stripped):
            pending.append(case.group(1))
        returned = re.search(r'return\s*&\s*(\w+)\s*;', stripped)
        if returned:
            for name in pending:
                mapping[name] = returned.group(1)
            pending.clear()
        if stripped.startswith('default:') or re.match(r'return\s+nullptr\s*;', stripped):
            for name in pending:
                mapping[name] = None
            pending.clear()
    return mapping


def icons_from_text(text):
    """(icon name -> packed byte count, icon name -> (width, height))."""
    bitmaps = {}
    for name, body in re.findall(r'uint8_t\s+(icon_\w+?)_bits\[\]\s*=\s*\{([^}]*)\}', text):
        bitmaps[name] = len(re.findall(r'0x[0-9A-Fa-f]{2}', body))
    sizes = {}
    for name, width, height in re.findall(
            r'freeink::Icon\s+(icon_\w+)\s*=\s*\{\s*(\d+)\s*,\s*(\d+)', text):
        sizes[name] = (int(width), int(height))
    return bitmaps, sizes


def missing_rows(mapping):
    """{list name: sorted StrIds without an icon}, minus the explicit whitelist."""
    return {
        name: sorted(setting for setting in rows
                     if mapping.get(setting) is None and setting not in INTENTIONAL_NO_ICON)
        for name, rows in visible_rows().items()
    }


class SettingsIconsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mapping = mappings_from_text(MAPPING_HEADER.read_text())
        cls.bitmaps, cls.sizes = icons_from_text(ICONS_HEADER.read_text())
        cls.rows = visible_rows()

    def test_parser_sees_the_real_lists(self):
        # Guard against a parser that silently matches nothing: an empty result
        # would make every other assertion pass for the wrong reason.
        minimum = {'display': 10, 'reader': 8, 'controls': 6, 'system': 20}
        for name, count in minimum.items():
            self.assertGreaterEqual(len(self.rows[name]), count,
                                    f'{name}: parsed only {sorted(self.rows[name])}')
        self.assertIn('STR_FAST_PAGE_TURN', self.rows['reader'])
        self.assertIn('STR_DICTIONARY', self.rows['reader'])
        self.assertIn('STR_BUZZER_STRENGTH', self.rows['system'])
        self.assertIn('STR_TEXT_SETTINGS', self.rows['reader'])
        self.assertIn('STR_LANGUAGE', self.rows['system'])
        self.assertGreaterEqual(len(self.mapping), 80)

    def test_every_visible_row_has_an_icon(self):
        missing = missing_rows(self.mapping)
        detail = '; '.join(f'{name}: {ids}' for name, ids in missing.items() if ids)
        self.assertFalse(any(missing.values()),
                         f'settings rows without a leading icon (add them to '
                         f'src/components/icons/settings_icons.h): {detail}')

    def test_mapped_icons_are_48px_lucide_bitmaps(self):
        for setting, icon in sorted(self.mapping.items()):
            if icon is None:
                continue
            with self.subTest(setting=setting, icon=icon):
                self.assertIn(icon, self.sizes, f'{setting} points at an unknown icon')
                self.assertEqual(self.sizes[icon], (48, 48))
                self.assertIn(icon, self.bitmaps, f'{icon} has no bitmap array')
                self.assertEqual(self.bitmaps[icon], 48 * (48 // 8))

    def test_mapping_covers_the_rows_the_audit_found_missing(self):
        # The six rows a static SettingsList.h-only audit reports; pin that they
        # stay mapped so a future refactor cannot quietly drop them again.
        expected = ('STR_BUZZER_FEEDBACK', 'STR_BUZZER_PITCH', 'STR_BUZZER_STRENGTH',
                    'STR_DICTIONARY', 'STR_FAST_PAGE_TURN', 'STR_XTC_STATUS_BAR')
        for setting in expected:
            self.assertIsNotNone(self.mapping.get(setting), setting)

    def test_newly_drawn_glyphs_are_wired(self):
        # A glyph added to the manifest but never referenced would be dead
        # Flash; the audit is the only place that can catch it statically.
        wired = set(self.mapping.values())
        for icon in ('icon_info_48', 'icon_languages_48', 'icon_music_48'):
            self.assertIn(icon, wired, f'{icon} is generated but no row uses it')

    def test_settings_activity_still_consults_the_mapping(self):
        # The whole audit is about rebuildRowItems(); if that call disappears
        # (or moves behind another flag) this test's premise is gone.
        source = SETTINGS_ACTIVITY.read_text()
        self.assertIn('#include "components/icons/settings_icons.h"', source)
        self.assertIn('settings_icons::forSetting(setting.nameId)', source)
        self.assertIn('#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI', source)

    def test_deleting_a_mapping_fails_the_audit(self):
        """Mutation test: remove one mapping and the audit must notice."""
        text = MAPPING_HEADER.read_text()
        mutated = text.replace(
            '    case StrId::STR_SLEEP_SCREEN:\n      return &icon_clock_48;\n', '')
        self.assertNotEqual(mutated, text, 'mutation anchor moved; update this test')
        missing = missing_rows(mappings_from_text(mutated))
        self.assertIn('STR_SLEEP_SCREEN', missing['display'])


if __name__ == '__main__':
    unittest.main()
