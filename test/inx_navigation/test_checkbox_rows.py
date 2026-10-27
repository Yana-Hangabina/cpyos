#!/usr/bin/env python3
"""Run production boolean rows and theme-specific control draw/hit checks."""
from pathlib import Path
import tempfile
from test_theme_menus import method, run

ROOT = Path(__file__).resolve().parents[2]
base = (ROOT / 'src/components/themes/BaseTheme.cpp').read_text()
settings = (ROOT / 'src/activities/settings/SettingsActivity.cpp').read_text()
start = settings.index('  const auto& settings = *currentSettings;', settings.index('void SettingsActivity::buildScreen'))
end = settings.index('  fui::ListProps props;', start)
helper_start=settings.index('  const auto applyCheckbox',settings.index('void SettingsActivity::buildScreen'))
helper_end=settings.index('  const auto& metrics',helper_start)
program = r'''
#include <cassert>
#include <functional>
#include <string>
#include <vector>
#include "components/lists/list.h"
namespace fui=freeink::ui;
struct CrossPointSettings { static constexpr int INX=5; int uiTheme=0; uint8_t value=0; } SETTINGS;
// The extracted row refresh now consults the theme for its CPY-only trims
// (chevron suffix, icon metrics); pinned off here so the historical-value
// assertions below keep their INX-era meaning.
struct UITheme { static UITheme& getInstance(){static UITheme t;return t;} bool usesCpyLayout(){return false;} };
struct BaseTheme { static void setCheckboxRow(fui::ListItem&,bool); } GUI;
@GUARD@
enum class StrId { STR_STATE_OFF, STR_STATE_ON, OTHER };
enum class SettingType { TOGGLE, ENUM, ACTION };
struct SettingInfo {
 SettingType type=SettingType::TOGGLE;
 uint8_t CrossPointSettings::*valuePtr=&CrossPointSettings::value;
 std::function<uint8_t()> valueGetter;
 std::vector<StrId> labels;
 std::vector<std::string> enumStringValues;
 const auto& enumLabels()const{return labels;}
};
std::vector<SettingInfo> entries(5);
auto* currentSettings=&entries;
std::vector<int16_t> rowSettingIndex_;
std::string rowValues_[5];
fui::ListItem rowItems_[5];
std::string settingValueText(const SettingInfo&){return "historical value";}
void refresh(){ @ROWS@ }
int main(){
 entries[1].type=SettingType::ENUM;
 entries[1].labels={StrId::STR_STATE_OFF,StrId::STR_STATE_ON};
 entries[1].valuePtr=nullptr;entries[1].valueGetter=[](){return SETTINGS.value;};
 entries[2].type=SettingType::ENUM;entries[2].labels={StrId::STR_STATE_OFF,StrId::STR_STATE_ON,StrId::OTHER};
 entries[3].type=SettingType::ENUM;entries[3].labels=entries[1].labels;entries[3].enumStringValues={"Off","On"};
 entries[4].type=SettingType::ACTION;
 // Reuse the same row storage across theme/state changes to catch stale toggles.
 for(int theme:{0,1,2,3,4,6,5,0,5}) for(int checked:{0,1}) {
  SETTINGS.uiTheme=theme;SETTINGS.value=checked;refresh();
  assert(SETTINGS.value==checked && SETTINGS.uiTheme==theme);
  for(int i=0;i<5;++i){
   const bool checkbox=i<2;
   assert(rowItems_[i].toggle==checkbox);
   if(checkbox){assert(rowItems_[i].value==nullptr);assert(rowItems_[i].toggleChecked==bool(checked));}
   else assert(std::string(rowItems_[i].value)=="historical value");
  }
 }
}
'''.replace('@GUARD@',method(base,'BaseTheme::setCheckboxRow')).replace('@ROWS@',settings[helper_start:helper_end]+settings[start:end])
with tempfile.TemporaryDirectory(prefix='checkbox-rows-') as directory:
    run(program,Path(directory),sdk=True)
print('Production boolean rows: all themes, both states, pointer/getter and multi-value selectors pass')

# Compile the real viewport methods, including INX's separate tab-list path.
shared = (ROOT / 'src/activities/UiListActivity.cpp').read_text()
tabs = (ROOT / 'src/activities/UiTabListActivity.cpp').read_text()
keyboard = (ROOT / 'src/activities/settings/KeyboardLayoutsActivity.cpp').read_text()
keyboard_guard = keyboard[keyboard.index('  props.toggleCheckbox = true;'):
                          keyboard.index('  syncListViewport(screen, props);')]
trace = (ROOT / 'test/inx_navigation/InxStyleParity.cpp').read_text().split('#ifdef UPSTREAM_THEME_PARITY')[0]
controls = trace + r'''
#include <cassert>
namespace fui=freeink::ui;
using UiScreen=fui::Screen<24>;
struct CrossPointSettings {static constexpr int INX=5; int uiTheme=0;} SETTINGS;
struct Metrics {int listRowHeight=66;} metrics;
struct UITheme {
 static UITheme& getInstance(){static UITheme t;return t;}
 bool hasMainTabs()const{return SETTINGS.uiTheme==CrossPointSettings::INX;}
 const auto& getMetrics()const{return metrics;}
};
struct UiListActivity {
 fui::ListNav nav;
 struct {bool touch=false;bool hasTouch()const{return touch;}} mappedInput;
 bool usesUpstreamStyle()const{return false;}
 auto& activeNav(){return nav;}
 int listCount()const{return 1;}
 void syncListViewport(UiScreen&,fui::ListProps&,int=0);
};
struct UiTabListActivity:UiListActivity {void syncTabListViewport(UiScreen&,fui::ListProps&);};
@SHARED@
@TABS@
int main(){
 for(int theme:{0})for(bool checked:{false,true})
 for(bool selected:{false,true})for(bool touch:{false,true})for(bool landscape:{false,true})
 for(int path:{0,1,3,4}){
  std::printf("CONTROL %d %d %d %d %d %d\n",theme,checked,selected,touch,landscape,path);
  SETTINGS.uiTheme=theme;
  TraceTarget target;
  fui::DeviceContext device;device.width=landscape?800:480;device.height=landscape?480:800;device.hasTouch=touch;
  fui::InteractionBuffer<24> hits;fui::InputSnapshot input;fui::Frame<24> frame(target,device,input,hits);
  auto tokens=fui::themeTokensForLineHeight(24);
  tokens.listSelectionStyle=fui::SelectionStyle::InvertFill;
  UiScreen screen(frame,tokens);
  UiTabListActivity activity;activity.mappedInput.touch=touch;
  activity.nav.selected=selected?0:-1;
  fui::ListItem item;item.label="Setting";item.toggle=true;item.toggleChecked=checked;
  fui::ListProps props;props.items=&item;props.count=1;props.action=7;props.inputMask=fui::InputTouch;
  if(path==0)activity.syncListViewport(screen,props);
  if(path==1){activity.nav.selected=selected?1:0;activity.syncTabListViewport(screen,props);}
  if(path==3){@KEYBOARD@ activity.syncListViewport(screen,props);}
  if(path==4){props.toggleCheckbox=true;props.toggleWidth=32;props.toggleHeight=30;activity.syncListViewport(screen,props);}
  // Booleans are 56x32 capsules; a caller that asked for a checkbox
  // (KeyboardLayouts' single-choice list) keeps the 28x28 square.
  const bool wantsCheckbox = path==3 || path==4;
  assert(props.toggleCheckbox==wantsCheckbox);
  assert(props.toggleWidth==(wantsCheckbox?28:56));
  assert(props.toggleHeight==(wantsCheckbox?28:32));
  screen.list(props);
  assert(hits.count()==1);
  const auto& hit=hits.data()[0];
  std::printf("hit %d %d %d %d %d %d\n",hit.rect.x,hit.rect.y,hit.rect.width,hit.rect.height,hit.action,hit.value);
  if(touch){
   input.touchX=hit.rect.right()-2;input.touchY=hit.rect.y+hit.rect.height/2;
   input.touchPressed=true;assert(!hits.route(input));
   input.touchPressed=false;input.touchReleased=true;assert(hits.route(input).action==7);
   input.touchReleased=false;assert(!hits.route(input));
  }
 }
}
'''.replace('@SHARED@', method(shared, 'UiListActivity::syncListViewport')).replace(
    '@TABS@', method(tabs, 'UiTabListActivity::syncTabListViewport')).replace('@KEYBOARD@', keyboard_guard)
with tempfile.TemporaryDirectory(prefix='boolean-controls-') as directory:
    output = run(controls, Path(directory), sdk=True)
scenes = output.split('CONTROL ')[1:]
hit_geometry = {}
for scene in scenes:
    name, drawing = scene.split('\n', 1)
    theme, checked, selected, touch, landscape, path = map(int, name.split())
    commands = [line.split() for line in drawing.splitlines()]
    checkbox = path in (3, 4)
    size = 22
    width, height = (size, size) if checkbox else (56, 32)
    outlines = [list(map(int, c[1:])) for c in commands
                if c[0] == 'stroke' and tuple(map(int, c[3:5])) == (width, height)]
    assert len(outlines) == 1, name
    x, y, _, _, _, ink, border, radius, _ = outlines[0]
    assert (border, radius) == ((2, 2) if checkbox else (1, 16)), name
    paper = 4 if selected else 1  # SDK Color::Black=4, Color::White=1
    assert ink == (1 if selected else 4), name
    fills = [list(map(int, c[1:])) for c in commands if c[0] == 'fill']
    if checkbox:
        inset = size // 4
        inner = [f for f in fills if f[:4] == [x+inset, y+inset, size-2*inset, size-2*inset]]
        assert bool(inner) == bool(checked), name
        if checked:
            assert inner[0][5] == ink, name
    hit = next(list(map(int, c[1:])) for c in commands if c[0] == 'hit')
    hx, hy, hw, hh, action, value = hit
    assert hx <= x and x+width <= hx+hw and hy <= y and y+height <= hy+hh, name
    assert (action, value) == (7, 0), name
    key = (theme, selected, touch, landscape, path)
    assert hit_geometry.setdefault(key, hit) == hit, name
print(f'Production boolean controls: {len(scenes)} theme/state/selection/orientation/input traces, '
      'keyboard checkboxes, caller overrides and one-action touch routing pass')

# Exercise the real per-page typography guards as well as their checkbox data.
paths = ['reader/EpubReaderMenuActivity.cpp', 'settings/LanguageSelectActivity.cpp',
         'settings/KOReaderSettingsActivity.cpp', 'settings/OpdsSettingsActivity.cpp']
for path in paths:
    text = (ROOT / 'src/activities' / path).read_text()
    start = text.index('  props.labelText = screen.theme().smallText;', text.index('void ' + Path(path).stem + '::buildScreen'))
    snippet = text[start:text.index('  syncListViewport(screen, props);', start)]
    assert 'SETTINGS.uiTheme' not in snippet, path
    code = r'''
#include <FreeInkApp.h>
#include <cassert>
namespace fui=freeink::ui;
struct Screen {fui::ThemeTokens tokens; const auto& theme()const{return tokens;}} screen;
int main(){screen.tokens.smallText.font=17;
 fui::ListProps props;
 @SNIPPET@
 assert(props.labelText.font==17);
 assert(props.labelText.maxLines==2);
}
'''.replace('@SNIPPET@', snippet)
    with tempfile.TemporaryDirectory(prefix='menu-font-') as directory:
        run(code, Path(directory), sdk=True)
print('Production reader/language/KOReader/OPDS label typography is unconditional and correct')
