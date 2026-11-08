"""Pin the UI role -> point-size table a Font Extras supplement has to mirror.

`typography::uiRolePointsFor()` is the single source of "which UI role is drawn
at which point size". `SdCardFontSystem::loadSupplementFonts()` builds its UI
faces from it precisely so that a rare-script glyph inside a 10pt row is not
drawn from a 14pt face - the concrete defect this table exists to prevent.
"""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def method(source, name):
    start = source.index(name)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def reader_footer_ids():
    """READER_STATUS/ESTIMATE live in SdCardFontSystem.h; read them, never copy."""
    source = (ROOT / 'src/SdCardFontSystem.h').read_text()
    return {name: re.search(rf'inline constexpr int {name} = (0x[0-9A-Fa-f]+);', source).group(1)
            for name in ('READER_STATUS_FONT_ID', 'READER_ESTIMATE_FONT_ID')}


def program(with_pack_bindings):
    """Typography.cpp's two point-table functions, compiled against the real ids."""
    source = (ROOT / 'src/Typography.cpp').read_text()
    prelude = '#include <algorithm>\n#include <cstddef>\n#include <cstdio>\n'
    prelude += '#include "Typography.h"\n#include "fontIds.h"\n'
    prelude += ''.join(f'#define {name} {value}\n' for name, value in reader_footer_ids().items())
    body = method(source, 'Points pointsFor(') + '\n' + method(source, 'std::size_t uiRolePointsFor(')
    # The ids stay in C++ so the expectation is the shipped constant, not a copy.
    check = '''
int main() {
  const typography::Scale scales[] = {typography::Scale::Compact, typography::Scale::Standard,
                                      typography::Scale::Large, typography::Scale::ExtraLarge};
  for (const auto scale : scales) {
    typography::UiRolePoint roles[typography::kMaxUiRolePoints];
    const std::size_t count =
        typography::uiRolePointsFor(typography::pointsFor(scale), roles, typography::kMaxUiRolePoints);
#if EXPECT_ROLES
    if (count != 7) return 1;
    const int expected[] = {SMALL_FONT_ID,      UI_10_FONT_ID,          UI_12_FONT_ID,
                            CONTROL_18_FONT_ID, NOTOSANS_18_FONT_ID,   READER_STATUS_FONT_ID,
                            READER_ESTIMATE_FONT_ID};
    for (std::size_t i = 0; i < count; ++i) {
      if (roles[i].fontId != expected[i]) return 2;
    }
#else
    if (count != 0) return 3;
#endif
    for (std::size_t i = 0; i < count; ++i) std::printf("%d\\n", static_cast<int>(roles[i].pointSize));
  }
  return 0;
}
'''.replace('EXPECT_ROLES', '1' if with_pack_bindings else '0')
    return prelude + 'namespace typography {\n' + body + '\n}  // namespace typography\n' + check


def role_points(with_pack_bindings):
    with tempfile.TemporaryDirectory(prefix='ui-font-roles-') as directory:
        cpp = Path(directory) / 'check.cpp'
        exe = Path(directory) / 'check'
        cpp.write_text(program(with_pack_bindings))
        command = ['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src'), str(cpp),
                   '-o', str(exe)]
        if with_pack_bindings:
            command.insert(1, '-DCROSSPOINT_FONT_PACK_EMBEDDED')
        subprocess.run(command, check=True)
        output = subprocess.check_output([str(exe)], text=True)
    points = [int(line) for line in output.splitlines()]
    return [points[index * 7:(index + 1) * 7] for index in range(len(points) // 7)]


class UiFontRolesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scales = role_points(True)

    def test_every_scale_follows_its_own_points(self):
        # Caption/Body/Title/Title/Display/Caption/Caption, per pointsFor():
        # Compact {8,10,12,14}, Standard {10,12,14,16}, Large {12,14,16,16},
        # ExtraLarge {14,16,16,16}. The SD table's fixed 12/14/16 is the defect:
        # a Compact body row is 10pt, so its supplement must be 10pt too.
        self.assertEqual(self.scales, [
            [8, 10, 12, 12, 14, 8, 8],
            [10, 12, 14, 14, 16, 10, 10],
            [12, 14, 16, 16, 16, 12, 12],
            [14, 16, 16, 16, 16, 14, 14],
        ])

    def test_no_pack_bindings_means_no_roles(self):
        # Targets that keep their built-in UI faces follow the SD size table
        # instead; reporting a role here would move a supplement off that table.
        self.assertEqual(role_points(False), [])


if __name__ == '__main__':
    unittest.main()
