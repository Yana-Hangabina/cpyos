#!/usr/bin/env python3
"""Exercise the shipped theme registry: enum shape, picker labels, migration of
legacy stored ids, font binding and the OOM fallback, by compiling the real
extracted methods against stubs."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def method(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 0
    for end in range(opening, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[start:end+1]
    raise ValueError(marker)

settings = (ROOT / 'src/CrossPointSettings.h').read_text()
ui = (ROOT / 'src/components/UITheme.cpp').read_text()
ui_header = (ROOT / 'src/components/UITheme.h').read_text()
labels = (ROOT / 'src/SettingsList.h').read_text()
enum = re.search(r'enum UI_THEME \{.*?\};', settings, re.S).group()
uses_modern = re.search(r'bool usesModernLayout\(\) const \{[^}]*\}', ui_header, re.S).group()
program = r'''
#include <cassert>
#include <memory>
#include <vector>
#include <iterator>
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
bool oom = false;
struct CrossPointSettings { @ENUM@ uint8_t uiTheme=CPY; } SETTINGS;
struct HalMemory { struct Heap {size_t totalBytes;}; static Heap getPsramHeap(){return {0u};} };
struct ThemeMetrics {};
struct BaseTheme {virtual ~BaseTheme()=default;};
struct UpstreamStyleTheme:BaseTheme{}; struct CpyTheme:BaseTheme{};
namespace BaseMetrics {const ThemeMetrics values{};}
namespace UpstreamStyleMetrics {const ThemeMetrics values{};}
namespace CpyMetrics {const ThemeMetrics values{};}
template<class T> std::unique_ptr<T> makeUniqueNoThrow(){return oom?nullptr:std::make_unique<T>();}
struct Font {const int* data; Font(const int* p=nullptr):data(p){} }; using EpdFont=Font;
const int ubuntu_10_regular=1, ubuntu_10_bold=2, ubuntu_12_regular=3, ubuntu_12_bold=4;
const int notosans_18_regular=5, notosans_18_bold=6, fallback=7;
@FONT_BINDINGS@
Font ui18RegularFont{}, ui18BoldFont{};
Font offlineReaderFont{&fallback};
struct UITheme {
 BaseTheme fallbackTheme; std::unique_ptr<BaseTheme> ownedTheme;
 BaseTheme* currentTheme=nullptr; const ThemeMetrics* currentMetrics=nullptr;
 CrossPointSettings::UI_THEME currentType=CrossPointSettings::UI_THEME::CPY; bool metricsValid=true;
 @USESMODERN@
 void setTheme(CrossPointSettings::UI_THEME); void reload();
};
@METHODS@
int main(){
 // CPY is the only theme and takes slot 0, so every stored legacy id (old
 // INX=0 included) migrates for free; the loader clamp and setTheme both
 // funnel unknown ids to CPY.
 static_assert(CrossPointSettings::CPY==0 && CrossPointSettings::UI_THEME_COUNT==1);
 { SETTINGS.uiTheme=CrossPointSettings::CPY; UITheme theme; theme.reload();
   assert(theme.currentType==CrossPointSettings::CPY);
   assert(theme.currentMetrics==&CpyMetrics::values);
   assert(ui18RegularFont.data==&fallback && ui18BoldFont.data==&fallback); }
 // Legacy stored ids (old INX=5, LYRA=1, COVER_GRID=6) land on CPY via the
 // loader clamp; setTheme must also treat any unknown id as CPY.
 for (int legacy : {2, 3, 4, 5, 6}) {
   UITheme theme;
   theme.setTheme(static_cast<CrossPointSettings::UI_THEME>(legacy));
   assert(theme.currentType==CrossPointSettings::CPY && theme.currentMetrics==&CpyMetrics::values);
 }
 { UITheme theme;
   theme.setTheme(static_cast<CrossPointSettings::UI_THEME>(255));  // corrupt byte
   assert(theme.currentType==CrossPointSettings::CPY); }
 { oom=true; SETTINGS.uiTheme=CrossPointSettings::CPY; UITheme theme;
   theme.setTheme(CrossPointSettings::UI_THEME::CPY);
   assert(theme.currentTheme==&theme.fallbackTheme && theme.currentType==CrossPointSettings::CPY);
   assert(theme.currentMetrics==&BaseMetrics::values); oom=false; }
}
'''
for key, value in {'ENUM':enum,
 'FONT_BINDINGS':'\n'.join(re.findall(r'^EpdFont ui(?:10|12)(?:Regular|Bold)Font\(&ubuntu_[^\n]+', ui, re.M)),
 'USESMODERN':uses_modern,
 'METHODS':'\n'.join(method(ui, name) for name in ['void UITheme::setTheme(', 'void UITheme::reload('])}.items():
    program=program.replace('@'+key+'@',value)
with tempfile.TemporaryDirectory(prefix='theme-selection-') as tmp:
    source=Path(tmp)/'check.cpp'; binary=Path(tmp)/'check';source.write_text(program)
    subprocess.run(['c++','-std=c++20',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('Theme registry: CPY-only enum, legacy-id migration to CPY, font binding and OOM fallback passed')
