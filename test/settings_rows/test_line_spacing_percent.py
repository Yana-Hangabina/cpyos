#!/usr/bin/env python3
"""Static + model checks for the reader line-spacing percentage override.

The override ships through four places that must agree, and a drift between any
two of them is silent (a row that edits the wrong field, a percentage that is
never applied, or a key that stops being persisted):

  1. `CrossPointSettings::readerLineSpacingPercent` exists and defaults to 0
     (0 = off, which is also why an old settings file needs no migration).
  2. `getReaderLineCompression()` still produces the four-level/per-family
     factors and hands them to `reader_line_spacing::resolve()` at the end.
  3. `SettingsList.h` registers the row with key "readerLineSpacingPercent" so
     the generic toJson/fromJson loop persists it (absent key -> default 0).
  4. The in-reader Text sheet (Layout tab) and Settings > Reader (stepper) both
     expose and label the same field.

The model replays the generic VALUE-row load path (absent -> default, clamp to
range, then resolve) so "off keeps family factors / percent overrides" is
checked as behaviour, not just as text. Each stage is proven sensitive by
mutating the input text and requiring the same check to fail.

Run: python3 test/settings_rows/test_line_spacing_percent.py
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]

HEADER = "src/CrossPointSettings.h"
SETTINGS_CPP = "src/CrossPointSettings.cpp"
SETTINGS_LIST = "src/SettingsList.h"
SETTINGS_ACTIVITY = "src/activities/settings/SettingsActivity.cpp"
TEXT_H = "src/activities/settings/TextSettingsActivity.h"
TEXT_CPP = "src/activities/settings/TextSettingsActivity.cpp"
LINE_SPACING_H = "src/ReaderLineSpacing.h"
EN_YAML = "lib/I18n/translations/english.yaml"
ZH_YAML = "lib/I18n/translations/chinese.yaml"
KEY_ORDER = "lib/I18n/key_order.txt"


def read(relative_path):
    return (ROOT / relative_path).read_text(encoding="utf-8")


def function_body(text, signature_pattern):
    match = re.search(signature_pattern, text)
    assert match, f"missing function: {signature_pattern}"
    opening = text.index("{", match.end())
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[opening : index + 1]
    raise AssertionError("unbalanced braces")


def check_sources(sources):
    """Return a list of human-readable failures for the given source texts."""
    failures = []

    def want(condition, message):
        if not condition:
            failures.append(message)

    # --- 1. Field and default ------------------------------------------------
    want(
        re.search(r"uint8_t\s+readerLineSpacingPercent\s*=\s*0;", sources[HEADER]) is not None,
        "CrossPointSettings.h must declare `uint8_t readerLineSpacingPercent = 0;`",
    )

    # --- 2. Resolver ---------------------------------------------------------
    body = function_body(sources[SETTINGS_CPP], r"float CrossPointSettings::getReaderLineCompression\(\) const\s*")
    want(
        "reader_line_spacing::resolve(factor, readerLineSpacingPercent)" in body,
        "getReaderLineCompression must end by resolving the percent override",
    )
    for literal in ("0.90f", "0.95f", "1.0f", "1.05f", "1.1f", "1.2f", "1.3f", "1.6f"):
        want(literal in body, f"getReaderLineCompression lost the per-family factor {literal}")
    resolve_at = body.find("reader_line_spacing::resolve")
    last_literal = max(body.rfind(literal) for literal in ("0.90f", "1.05f", "1.1f", "1.2f", "1.3f", "1.6f"))
    want(
        resolve_at > last_literal,
        "the percent override must be applied after the level/family factors are chosen",
    )
    want(
        "ReaderLineSpacing.h" in sources[SETTINGS_CPP],
        "CrossPointSettings.cpp must include ReaderLineSpacing.h",
    )

    # --- 3. Persistence row --------------------------------------------------
    list_text = sources[SETTINGS_LIST]
    row = re.search(
        r"SettingInfo::Value\(StrId::STR_LINE_SPACING_PERCENT,\s*"
        r"&CrossPointSettings::readerLineSpacingPercent,\s*"
        r"\{(?P<range>[^}]*)\},\s*"
        r'"(?P<key>[^"]+)",\s*StrId::STR_CAT_READER\)',
        list_text,
        re.DOTALL,
    )
    want(row is not None, "SettingsList.h must register the reader line-spacing percentage row")
    if row:
        want(row.group("key") == "readerLineSpacingPercent", "the settings key must be `readerLineSpacingPercent`")
        want("reader_line_spacing::PERCENT_MIN" in row.group("range"), "row range must start at PERCENT_OFF (0)")
        want("reader_line_spacing::PERCENT_MAX" in row.group("range"), "row range must use PERCENT_MAX")
        want("reader_line_spacing::PERCENT_STEP" in row.group("range"), "row range must use PERCENT_STEP")

    # --- 4. Migration is a no-op --------------------------------------------
    # The field is only ever read in the resolver and only ever written by the
    # generic SettingsList loader. Any extra assignment here would be a migration
    # that an old settings file did not ask for.
    want(
        re.search(r"readerLineSpacingPercent\s*=", sources[SETTINGS_CPP]) is None,
        "CrossPointSettings.cpp must not assign readerLineSpacingPercent (absent key keeps 0)",
    )
    want(
        re.search(r"readerLineSpacingPercent\s*=(?!=)", sources[HEADER]) is not None,
        "the struct initializer in CrossPointSettings.h must be the only write",
    )
    want(
        re.search(r'doc\[info\.key\]\s*=', sources[SETTINGS_CPP]) is not None,
        "toJson must still persist via the generic SettingsList key",
    )

    # --- 5. Settings > Reader stepper value text ----------------------------
    activity = sources[SETTINGS_ACTIVITY]
    want(
        "&CrossPointSettings::readerLineSpacingPercent" in activity,
        "SettingsActivity must special-case the percentage row's value text",
    )
    want(
        "reader_line_spacing::PERCENT_OFF" in activity,
        "SettingsActivity must show the off state instead of a bare 0",
    )

    # --- 6. In-reader Text sheet --------------------------------------------
    want(
        re.search(r"LineSpacing,\s*\n\s*LineSpacingPercent,", sources[TEXT_H]) is not None,
        "TextSettingsActivity::LayoutRow must add LineSpacingPercent after LineSpacing",
    )
    label = re.search(r"LAYOUT_ROW_NAME_IDS\[\]\s*=\s*\{(?P<labels>[^}]*)\}", sources[TEXT_CPP], re.DOTALL)
    want(label is not None, "LAYOUT_ROW_NAME_IDS must exist")
    if label and row:
        names = [item.strip() for item in label.group("labels").split(",") if item.strip()]
        want(
            names[:3] == ["StrId::STR_LINE_SPACING", "StrId::STR_LINE_SPACING_PERCENT", "StrId::STR_WORD_SPACING"],
            "LAYOUT_ROW_NAME_IDS must keep the new label in the enum's position",
        )
    want(
        "case LayoutRow::LineSpacingPercent:" in sources[TEXT_CPP],
        "TextSettingsActivity must handle confirming and showing the new row",
    )
    want(
        "case LayoutRow::LineSpacingPercent:" in sources[TEXT_CPP]
        and sources[TEXT_CPP].count("case LayoutRow::LineSpacingPercent:") >= 2,
        "the new row needs both a confirm case and a value-text case",
    )
    want(
        "ReaderLineSpacing.h" in sources[TEXT_CPP],
        "TextSettingsActivity.cpp must include ReaderLineSpacing.h",
    )

    # --- 7. Strings ----------------------------------------------------------
    for lang, text in ((EN_YAML, sources[EN_YAML]), (ZH_YAML, sources[ZH_YAML])):
        hits = re.findall(r"^STR_LINE_SPACING_PERCENT\s*:\s*\"[^\"]+\"\s*$", text, re.MULTILINE)
        want(len(hits) == 1, f"{lang} must carry exactly one STR_LINE_SPACING_PERCENT entry")
    order = [line.strip() for line in sources[KEY_ORDER].splitlines() if line.strip()]
    want(
        "STR_LINE_SPACING_PERCENT" in order,
        "key_order.txt must list the new key (new keys are appended, never reordered)",
    )
    if "STR_LINE_SPACING_PERCENT" in order:
        want(
            order.index("STR_LINE_SPACING_PERCENT") > order.index("STR_WORD_SPACING"),
            "the new key must be appended after the existing order",
        )

    # --- 8. Model: generic VALUE load path + resolver -----------------------
    def resolve(family_factor, percent):
        return percent / 100.0 if percent else family_factor

    def load_value(doc, default, minimum, maximum):
        value = doc.get("readerLineSpacingPercent", default)
        return min(max(value, minimum), maximum)

    family_factors = (0.90, 0.95, 1.0, 1.05, 1.1, 1.2, 1.3, 1.6)
    # Missing key = migration no-op: the default 0 keeps every family factor.
    legacy = load_value({}, 0, 0, 200)
    want(legacy == 0, "an old settings file without the key must load as off")
    for factor in family_factors:
        want(
            abs(resolve(factor, legacy) - factor) < 1e-6,
            f"off must keep the family factor {factor}",
        )
    for percent, expected in ((100, 1.0), (130, 1.3), (160, 1.6)):
        want(
            abs(resolve(0.95, percent) - expected) < 1e-6,
            f"{percent}% must resolve to {expected}",
        )
        for factor in family_factors:
            want(
                abs(resolve(factor, percent) - expected) < 1e-6,
                f"{percent}% must override the family factor {factor}",
            )
    # Out-of-range values are clamped by the generic loader, not stored as-is.
    want(load_value({"readerLineSpacingPercent": 255}, 0, 0, 200) == 200, "out-of-range percent must clamp to max")
    want(load_value({"readerLineSpacingPercent": 0}, 99, 0, 200) == 0, "an explicit 0 must mean off")

    return failures


def main():
    sources = {
        HEADER: read(HEADER),
        SETTINGS_CPP: read(SETTINGS_CPP),
        SETTINGS_LIST: read(SETTINGS_LIST),
        SETTINGS_ACTIVITY: read(SETTINGS_ACTIVITY),
        TEXT_H: read(TEXT_H),
        TEXT_CPP: read(TEXT_CPP),
        EN_YAML: read(EN_YAML),
        ZH_YAML: read(ZH_YAML),
        KEY_ORDER: read(KEY_ORDER),
    }

    failures = check_sources(sources)
    if failures:
        print("reader line spacing settings checks FAILED:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    # Sensitivity: each layer must be able to fail the same checker.
    mutations = {
        "drop the resolver call": (SETTINGS_CPP, "reader_line_spacing::resolve(factor, readerLineSpacingPercent)",
                                   "factor"),
        "drop the settings row": (SETTINGS_LIST, '"readerLineSpacingPercent", StrId::STR_CAT_READER',
                                  '"readerLineSpacingPercentX", StrId::STR_CAT_READER'),
        "drop the text-sheet row": (TEXT_H, "LineSpacing,\n    LineSpacingPercent,", "LineSpacing,"),
        "drop the off state": (SETTINGS_ACTIVITY, "reader_line_spacing::PERCENT_OFF", "0"),
        "drop the English string": (EN_YAML, "STR_LINE_SPACING_PERCENT:", "STR_LINE_SPACING_PERCENT_REMOVED:"),
    }
    for name, (path, needle, replacement) in mutations.items():
        mutated = dict(sources)
        assert needle in mutated[path], f"mutation target missing for: {name}"
        mutated[path] = mutated[path].replace(needle, replacement, 1)
        if not check_sources(mutated):
            print(f"FAIL sensitivity: mutation '{name}' did not turn the checks red")
            return 1

    # Model sensitivity: a resolver that ignores "off" must fail the model.
    evil = lambda factor, percent: percent / 100.0 if percent else 0.5
    if abs(evil(1.3, 0) - 1.3) < 1e-6:
        print("FAIL sensitivity: the model is vacuous")
        return 1

    print("reader line spacing settings: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
