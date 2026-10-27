#!/usr/bin/env python3
"""Settings ListItem::actionValue must carry the DISPLAY ROW index, not the setting index.

rebuildRowItems() inserts a group header into rowItems_/rowSettingIndex_
whenever the group changes, so the display row index and the index into
*currentSettings diverge as soon as a category has more than one group.
FreeInkUI reports ListItem::actionValue as the tap's event.value;
UiTabListActivity::onRowAction turns it into ring = value + 1 and
SettingsActivity::toggleCurrentSetting resolves settingIndexAtRow(ring - 1).
Writing the setting index into actionValue therefore activates a row above or
below the tapped one (field report: tapping "Language" opened Plugins).

Two layers:
  1. Static checks over the real sources: actionValue is captured from
     rowItems_.size() after the header push (never from the setting index),
     headers stay -1 and are skipped by the SDK's hit registration, and the
     dispatch chain keeps value == display row.
  2. A model of that dispatch chain replayed on a multi-group category,
     including the Language/Plugins layout from the field report. The old
     (setting index) mapping must FAIL the same assertions, so the test is
     proven sensitive instead of vacuous.

Run: python3 test/settings_rows/test_settings_row_mapping.py

Device verification (after the main session builds/flashes):
  1. Settings -> System tab, tap "Language" (Time & Language group, just above
     About): the language picker must open. Before the fix this row opened
     Plugins (the row above it).
  2. Tap "Plugins": the plugin catalog must open.
  3. In every tab, tap the first row under each group header and confirm the
     opened page matches the tapped label; tap a group header itself and
     confirm nothing happens.
  4. Keyboard/membrane keys: Up/Down must still skip headers (not selectable)
     and Confirm must open the highlighted row's page.
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]


def read(relative_path):
    return (ROOT / relative_path).read_text(encoding="utf-8")


def find_matching(text, opening_index):
    """Return the index of the brace closing text[opening_index] == '{'.

    Ignores braces inside line/block comments and string/char literals.
    """
    assert text[opening_index] == "{"
    depth = 0
    index = opening_index
    mode = None  # None | line | block | string | char
    while index < len(text):
        char = text[index]
        if mode is None:
            if char == "/" and text[index + 1 : index + 2] == "/":
                mode = "line"
                index += 2
                continue
            if char == "/" and text[index + 1 : index + 2] == "*":
                mode = "block"
                index += 2
                continue
            if char == '"':
                mode = "string"
                index += 1
                continue
            if char == "'":
                mode = "char"
                index += 1
                continue
            if char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return index
        elif mode == "line":
            if char == "\n":
                mode = None
        elif mode == "block":
            if char == "*" and text[index + 1 : index + 2] == "/":
                mode = None
                index += 2
                continue
        else:
            if char == "\\":
                index += 2
                continue
            if (mode == "string" and char == '"') or (mode == "char" and char == "'"):
                mode = None
        index += 1
    raise AssertionError("unbalanced braces")


def function_body(text, signature_pattern):
    match = re.search(signature_pattern, text)
    assert match, f"missing function: {signature_pattern}"
    opening = text.index("{", match.end())
    return text[opening : find_matching(text, opening) + 1]


def block_after(text, pattern):
    match = re.search(pattern, text)
    assert match, f"missing block: {pattern}"
    opening = text.index("{", match.end())
    return text[match.start() : find_matching(text, opening) + 1]


# --- 1. Production source: actionValue is the display row --------------------

settings_cpp = read("src/activities/settings/SettingsActivity.cpp")
rebuild = function_body(settings_cpp, r"void SettingsActivity::rebuildRowItems\(\)\s*")

header_block = block_after(rebuild, r"if \(group\.label != currentGroup\)\s*")
for fragment in (
    "header.isHeader = true;",
    "header.actionValue = -1;",
    "rowItems_.push_back(header);",
    "rowSettingIndex_.push_back(-1);",
):
    assert fragment in header_block, f"group header lost '{fragment}'"

row_index = re.search(r"const auto row = static_cast<int16_t>\(rowItems_\.size\(\)\);", rebuild)
assert row_index, "rebuildRowItems no longer captures the display row from rowItems_.size()"

action_assignments = list(re.finditer(r"item\.actionValue\s*=\s*([^;]+);", rebuild))
assert len(action_assignments) == 1, (
    "expected exactly one item.actionValue assignment in rebuildRowItems, found "
    f"{len(action_assignments)}"
)
assert action_assignments[0].group(1).strip() == "row", (
    "item.actionValue must be the display row index 'row', got "
    f"'{action_assignments[0].group(1).strip()}'"
)
assert row_index.start() < action_assignments[0].start()

header_end = rebuild.index(header_block) + len(header_block)
assert row_index.start() > header_end, (
    "the display row index must be captured AFTER a header may have been inserted"
)

loop = re.search(r"for \(const uint16_t index : order\)\s*", rebuild)
assert loop, "rebuildRowItems no longer iterates the grouped setting order"
loop_start = loop.start()
loop_end = find_matching(rebuild, rebuild.index("{", loop.end()))

push_item = rebuild.index("rowItems_.push_back(item);")
push_setting = rebuild.index("rowSettingIndex_.push_back(static_cast<int16_t>(index));")
assert loop_start < push_item < push_setting < loop_end, (
    "rowItems_ and rowSettingIndex_ must be appended in the same loop iteration, "
    "rowItems_ first"
)
assert "push_back" not in rebuild[push_item + len("rowItems_.push_back(item);") : push_setting], (
    "rowSettingIndex_ must be pushed immediately after its ListItem"
)

# --- 2. Production source: the value -> ring -> setting dispatch chain --------

ui_list_cpp = read("src/activities/UiListActivity.cpp")
assert "if (event.value < 0 || event.value >= self->listCount()) return;" in ui_list_cpp, (
    "rowActionTrampoline no longer bounds-checks event.value against listCount()"
)

ui_tab_list_cpp = read("src/activities/UiTabListActivity.cpp")
assert "activeNav().selected = event.value + 1;" in ui_tab_list_cpp, (
    "UiTabListActivity::onRowAction no longer maps value -> ring as value + 1"
)
assert "activateIndex(event.value);" in ui_tab_list_cpp, (
    "UiTabListActivity::onRowAction no longer forwards the row value to activateIndex"
)

assert "settingIndexAtRow(ringPos() - 1)" in settings_cpp, (
    "toggleCurrentSetting no longer resolves the setting from the ring position"
)
setting_index_body = function_body(settings_cpp, r"int SettingsActivity::settingIndexAtRow\(const int row\) const\s*")
assert "if (row < 0 || row >= static_cast<int>(rowSettingIndex_.size())) return -1;" in setting_index_body, (
    "settingIndexAtRow lost its bounds check against rowSettingIndex_"
)

list_h = read("freeink-sdk/libs/ui/FreeInkUI/include/components/lists/list.h")
header_hit_guard = list_h.index("if (item.isHeader) {")
header_hit_end = find_matching(list_h, list_h.index("{", header_hit_guard))
assert "continue;" in list_h[header_hit_guard:header_hit_end], "header rows must skip the row body"
row_hit = list_h.index("frame.hit(hit, props.action, item.actionValue, props.inputMask, hitState);")
assert header_hit_guard < row_hit, "header rows must not register a touch hit"


# --- 3. Model: replay the dispatch chain on multi-group categories -----------

def build_rows(display_entries):
    """Return (row_to_setting, row_action_values) for the given display order.

    display_entries: (group_label, setting_index) in the order the sorted
    settings are appended. A new label inserts a header row before its rows.
    """
    row_to_setting = []
    action_values = []
    current_group = None
    for label, setting_index in display_entries:
        if label != current_group:
            row_to_setting.append(-1)
            action_values.append(-1)  # header.actionValue = -1
            current_group = label
        row_to_setting.append(setting_index)
        action_values.append(len(row_to_setting) - 1)  # item.actionValue = rowItems_.size()
    return row_to_setting, action_values


def click(action_value, row_to_setting):
    """Mirror rowActionTrampoline -> onRowAction -> toggleCurrentSetting."""
    if action_value < 0 or action_value >= len(row_to_setting):
        return None
    ring = action_value + 1  # UiTabListActivity::onRowAction
    row = ring - 1  # toggleCurrentSetting
    if row < 0 or row >= len(row_to_setting):
        return None
    setting = row_to_setting[row]
    return None if setting < 0 else setting


def simulate(display_entries, value_source):
    row_to_setting, action_values = build_rows(display_entries)
    if value_source == "setting":
        # The pre-fix bug: actionValue = index into *currentSettings.
        action_values = [
            -1 if setting < 0 else setting for setting in row_to_setting
        ]
    activated = [click(value, row_to_setting) for value in action_values]
    return row_to_setting, action_values, activated


def check_scenario(name, display_entries, expected_mismatches_for_old):
    row_to_setting, action_values, activated = simulate(display_entries, "row")
    settings = [setting for setting in row_to_setting if setting >= 0]
    assert len(settings) == len(set(settings)), f"{name}: duplicate setting rows"

    for row, expected in enumerate(row_to_setting):
        if expected < 0:
            assert activated[row] is None, f"{name}: header row {row} activated setting {activated[row]}"
        else:
            assert activated[row] == expected, (
                f"{name}: tapping row {row} activated {activated[row]}, expected setting {expected}"
            )
            assert action_values[row] == row, f"{name}: row {row} carries actionValue {action_values[row]}"

    old_row_to_setting, old_values, old_activated = simulate(display_entries, "setting")
    mismatches = sum(
        1
        for row, expected in enumerate(old_row_to_setting)
        if expected >= 0 and old_activated[row] != expected
    )
    assert mismatches == expected_mismatches_for_old, (
        f"{name}: the old setting-index mapping produced {mismatches} wrong rows, "
        f"expected {expected_mismatches_for_old}"
    )
    return row_to_setting, old_row_to_setting, old_activated


# 1) One group: still one leading header, so the old mapping is already off by
#    one for every row (tap row N opened row N-1's setting). Extra headers in
#    multi-group tabs compound the offset, which is how it reached "Language
#    opens Plugins".
check_scenario("single group", [("Only", index) for index in range(5)], 5)

# 2) Cross-group layout mirroring the System tab: settings are appended per
#    action/rule, then stable-sorted by group rank for display. The last group
#    is Time & Language, so its rows sit far below their setting indexes.
system_display_order = [
    ("Power", 0),
    ("Network", 2),
    ("Network", 3),
    ("Network", 4),
    ("Storage", 5),
    ("Storage", 6),
    ("Storage", 7),
    ("Storage", 8),
    ("Apps", 9),
    ("Apps", 10),
    ("Apps", 11),
    ("Apps", 12),
    ("Apps", 13),
    ("Apps", 14),  # Plugins
    ("Apps", 15),
    ("TimeLanguage", 16),
    ("TimeLanguage", 17),  # Language
    ("About", 18),
]
row_to_setting, old_row_to_setting, old_activated = check_scenario(
    "system tab cross-group", system_display_order, expected_mismatches_for_old=18
)
language_row = row_to_setting.index(17)
plugins_setting = 14
assert old_activated[language_row] == plugins_setting, (
    "the modelled pre-fix layout must reproduce the field report "
    "(tapping Language opened Plugins)"
)

# 3) Many headers, settings deliberately not in row order: every click must
#    still reach its own row.
check_scenario(
    "many groups",
    [("A", 7), ("A", 0), ("B", 5), ("C", 2), ("C", 9), ("C", 1), ("D", 8), ("E", 3)],
    expected_mismatches_for_old=8,
)

print("settings row mapping: static checks on SettingsActivity/UiListActivity/UiTabListActivity/list.h passed")
print(
    f"settings row mapping: {len(row_to_setting)} modelled System rows, "
    f"Language row {language_row} -> setting {row_to_setting[language_row]} "
    f"(old mapping opened setting {old_activated[language_row]} = Plugins)"
)
print("settings row mapping: old setting-index mapping fails as expected; new row mapping is consistent")
print("Device checks: tap Language/Plugins and the first row of each group; headers do nothing; keys skip headers")
