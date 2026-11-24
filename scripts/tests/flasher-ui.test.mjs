// Host-side checks for site/flasher-ui.js. Run with:
//   node scripts/tests/flasher-ui.test.mjs
// The DOM half needs a browser; these cover the string table and the stage
// classifier both language paths rely on.
import '../../site/flasher-ui.js';

const ui = globalThis.CPYOSFlasherUI;
let failures = 0;

function eq(actual, expected, what) {
  const ok = JSON.stringify(actual) === JSON.stringify(expected);
  if (!ok) {
    failures += 1;
    console.error(`FAIL ${what}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }
}

function check(cond, what) {
  if (!cond) {
    failures += 1;
    console.error(`FAIL ${what}`);
  }
}

// --- translation -----------------------------------------------------------

for (const [en, zh] of [
  ['Connecting', '正在连接设备…'],
  ['Preparing installation', '正在准备安装…'],
  ['Erasing', '正在擦除闪存…'],
  ['Wrapping up', '正在收尾…'],
  ['Installing', '正在刷写固件'],
  ['Installation complete!', '刷写完成！'],
  ['Confirm Installation', '确认安装'],
  ['Erase device', '擦除设备'],
  ['Installation failed', '刷写失败'],
  ['Back', '返回'],
  ['Next', '下一步'],
  ['Install', '安装'],
  ['Logs & Console', '日志与控制台'],
  ['Reset Device', '复位设备'],
  ['Disconnected', '设备已断开连接'],
  ['Your device is running', '设备当前运行'],
  ['This will take', '大约需要'],
  ['2 minutes', '2 分钟'],
  ['Keep this page visible to prevent slow down', '请保持本页可见，以免写入变慢'],
  ['Do you want to erase the device before installing', '是否在安装'],
  ['All data on the device will be lost.', '设备上的所有数据都会丢失。'],
  ['No port selected', '未选择串口'],
  ['Cancel', '取消'],
  ['Try Again', '重试'],
  ['CP2102 drivers:', 'CP2102 驱动：'],
  ['Make sure that the USB cable you use can be used for data and is not a power-only cable.',
    '确认所用的 USB 线支持数据传输，而不是只能充电的线。']
]) {
  eq(ui.translateText(en, 'zh'), zh, `translate ${JSON.stringify(en)}`);
}

// Fragments keep the whitespace that separates interpolated values.
eq(ui.translateText('Connected to ', 'zh'), '已连接到 ', 'fragment keeps trailing space');
eq(ui.translateText('Connected to Read Pico', 'zh'), '已连接到 Read Pico', 'connected pattern');
eq(ui.translateText('Update CrossMux Read Pico', 'zh'), '更新 CrossMux Read Pico', 'update pattern');
eq(ui.translateText('Install CrossMux Read Pico', 'zh'), '安装 CrossMux Read Pico', 'install pattern');
eq(ui.translateText('Writing progress: 42%', 'zh'), '写入进度：42%', 'writing progress pattern');

// No-ops: English mode, already-Chinese text and values that carry no copy.
eq(ui.translateText('Connecting', 'en'), null, 'english mode is identity');
eq(ui.translateText('正在连接设备…', 'zh'), null, 'translation is idempotent');
eq(ui.translateText('42%', 'zh'), null, 'percent passes through');
eq(ui.translateText('', 'zh'), null, 'empty text passes through');

// --- stage classifier ------------------------------------------------------

const view = (over = {}) => Object.assign({
  headline: '', label: '', message: '', hasProgress: false, hasMessage: false,
  hasList: false, percent: null
}, over);

eq(ui.classify(view({ hasProgress: true, label: '正在连接设备…' }), false, 0),
  { state: 'connecting', stage: 1 }, 'connecting zh');
eq(ui.classify(view({ hasProgress: true, label: 'Connecting' }), false, 0),
  { state: 'connecting', stage: 1 }, 'connecting en');
eq(ui.classify(view({ headline: 'CrossMux Read Pico', hasList: true }), false, 1),
  { state: 'dashboard', stage: 2 }, 'dashboard');
eq(ui.classify(view({ headline: 'Confirm Installation' }), false, 1),
  { state: 'confirm', stage: 2 }, 'confirm install');
eq(ui.classify(view({ headline: 'Erase device' }), false, 1),
  { state: 'confirm', stage: 2 }, 'ask erase');
eq(ui.classify(view({ hasProgress: true, label: '正在准备安装…' }), true, 2),
  { state: 'preparing', stage: 2 }, 'preparing');
eq(ui.classify(view({ hasProgress: true, label: '正在擦除闪存…' }), true, 2),
  { state: 'erasing', stage: 2 }, 'erasing');
eq(ui.classify(view({ hasProgress: true, label: '正在刷写固件 42%', percent: 42 }), true, 3),
  { state: 'writing', stage: 3 }, 'writing zh');
eq(ui.classify(view({ hasProgress: true, label: 'This will take 2 minutes.' }), true, 3),
  { state: 'writing', stage: 3 }, 'writing en by its take-a-minute line');
eq(ui.classify(view({ hasProgress: true, headline: 'Installing', label: 'This will take 2 minutes.' }), true, 3),
  { state: 'writing', stage: 3 }, 'writing en by headline');
eq(ui.classify(view({ hasProgress: true, label: '正在收尾…' }), true, 3),
  { state: 'wrapping', stage: 3 }, 'wrapping');
eq(ui.classify(view({ message: '刷写完成！' }), true, 3),
  { state: 'finished', stage: 4 }, 'finished zh');
eq(ui.classify(view({ message: 'Installation complete!' }), true, 3),
  { state: 'finished', stage: 4 }, 'finished en');
eq(ui.classify(view({ headline: '刷写失败' }), true, 3),
  { state: 'failed', stage: 3 }, 'failed after writing');
eq(ui.classify(view({ headline: 'Installation failed' }), false, 1),
  { state: 'error', stage: 1 }, 'failure before install is an error');
eq(ui.classify(view({ headline: 'Error', message: 'Disconnected' }), false, 1),
  { state: 'error', stage: 1 }, 'disconnected before install');
eq(ui.classify(view({ headline: 'Logs' }), true, 3),
  { state: 'logs', stage: 3 }, 'logs panel');
eq(ui.classify(view({ headline: '日志' }), true, 3),
  { state: 'logs', stage: 3 }, 'logs panel zh');

// --- notes and labels -------------------------------------------------------

eq(ui.steps('zh'), ['连接设备', '擦除与准备', '写入固件', '完成'], 'zh steps');
eq(ui.steps('en'), ['Connect', 'Prepare', 'Write', 'Done'], 'en steps');
check(ui.note('writing', 42, 'zh').includes('42%'), 'writing note carries the percentage');
check(ui.note('writing', 42, 'en').includes('42%'), 'writing note carries the percentage (en)');
check(ui.note('finished', null, 'zh').includes('拔掉'), 'finished note tells the user to unplug');

if (failures) {
  console.error(`${failures} failure(s)`);
  process.exit(1);
}
console.log('flasher-ui: all checks passed');
