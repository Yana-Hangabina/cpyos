#!/usr/bin/env python3
"""Pin the Settings information architecture to the group table.

`SettingsActivity::groupForSetting()` decides which card a row lands in inside
its tab. A row that matches nothing falls into the catch-all "Other" card at the
bottom of the page, one card away from the settings it belongs to -- UI scale
used to sit there, three cards away from UI density and font size, and so did
the two refresh rows, Wi-Fi idle timeout and the USB download row.

The 2026-11-26 IA pass gave every one of those rows a named group. This test
pins that assignment, and proves it is sensitive: deleting one mapping must make
the same analysis report it. Textbook "presence in a function" checks go green
for the wrong reasons when they look at the whole file, so the body is extracted
by brace matching first and a mutation is run in-memory.

Run: python3 scripts/tests/test_settings_groups.py
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SETTINGS_ACTIVITY = ROOT / "src/activities/settings/SettingsActivity.cpp"
SETTINGS_HUB = ROOT / "src/activities/settings/SettingsHubActivity.cpp"

# Rows whose absence from the group table is a visible regression: each one
# would otherwise land in the "Other" card of its tab.
PINNED_FIELDS = (
    "&CS::uiScalePercent",
    "&CS::uiFontScale",
    "&CS::uiDensity",
    "&CS::periodicFullRefresh",
    "&CS::readerRefreshPages",
    "&CS::wifiIdleTimeoutSeconds",
    "&CS::sideButtonFollowOrientation",
    "&CS::networkType",
    "&CS::screenInverted",
    # jjwxcSignInReminder is deliberately category-less: it is a JJWXC-app
    # preference owned by the sign-in calendar, not a System settings row.
    "&CS::vocabularyHistoryEnabled",
    "&CS::sleepScreenCoverMode",
)
PINNED_ACTIONS = (
    "SettingAction::UsbDownloadMode",
    "SettingAction::ChargingStatus",
    "SettingAction::CellularStatus",
    "SettingAction::FontsHub",
    "SettingAction::UpdatesHub",
    "SettingAction::TapZones",
    "SettingAction::Bluetooth",
    "SettingAction::StandbyImage",
)


def function_body(text, signature):
    """Body of the function (braces included), ignoring comments/strings."""
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 0
    index = opening
    mode = None
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
                    return text[opening : index + 1]
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
    raise AssertionError("unbalanced braces in groupForSetting")


def missing_rows(body, tokens):
    return [token for token in tokens if token not in body]


def main():
    text = SETTINGS_ACTIVITY.read_text(encoding="utf-8")
    body = function_body(text, "SettingsGroup groupForSetting(const SettingInfo& setting)")

    # Positive control: the unmodified table must cover every pinned row.
    assert not missing_rows(body, PINNED_FIELDS), (
        "rows without a named group (they fall into 'Other'): " + ", ".join(missing_rows(body, PINNED_FIELDS))
    )
    assert not missing_rows(body, PINNED_ACTIONS), (
        "actions without a named group: " + ", ".join(missing_rows(body, PINNED_ACTIONS))
    )

    # Mutation: deleting one mapping must be reported by the same analysis, so a
    # future removal cannot quietly pass.
    victim = "&CS::uiScalePercent"
    mutated = body.replace(victim, "&CS::removedUiScalePercent", 1)
    assert victim not in mutated, "mutation did not land in the body"
    assert victim in missing_rows(mutated, PINNED_FIELDS), "mutation survived the check"

    # The two hubs must keep their doors and their contents.
    hub = SETTINGS_HUB.read_text(encoding="utf-8")
    for token in (
        "Kind::TextSettings",
        "STR_TEXT_SETTINGS",
        "STR_MANAGE_FONTS",
        "STR_MANAGE_FONT_EXTRAS",
        "STR_MANAGE_DICTIONARIES",
        "Kind::VocabularyToggle",
        "STR_VIEW_VOCABULARY",
        "Kind::CheckUpdates",
        "STR_SD_FIRMWARE_UPDATE",
        "Kind::PdfEngine",
        "Kind::PlusResources",
    ):
        assert token in hub, f"hub page lost: {token}"
    assert "STR_MANAGE_FONTS, SettingAction::FontsHub" in text, "Reader tab lost the fonts hub door"
    assert "STR_UPDATE, SettingAction::UpdatesHub" in text, "System tab lost the updates hub door"

    print("settings groups: every pinned row has a named group; hub wiring intact; mutation detected")


if __name__ == "__main__":
    main()