/* CPYOS flasher UI.
 *
 * esp-web-tools ships an English-only install dialog and renders it into open
 * shadow roots, so the page cannot restyle or translate it with plain CSS.
 * This file translates the dialog at runtime (Chinese browsers, or ?lang=zh)
 * and adds a four-step tracker under the progress circle so a long flash is
 * not just a moving bar. English is the fallback and needs no translation.
 *
 * Keep this file dependency-free: scripts/build_web_flasher.py inlines it into
 * the standalone nightly flashing page.
 */
(function (global) {
  'use strict';

  var ZH = 'zh';
  var EN = 'en';

  function detectLang() {
    try {
      var forced = new URLSearchParams(global.location.search).get('lang');
      if (forced) return forced.toLowerCase().indexOf('zh') === 0 ? ZH : EN;
    } catch (error) { /* no location (tests) */ }
    var tags = [];
    try {
      if (typeof navigator !== 'undefined') {
        tags = navigator.languages && navigator.languages.length
          ? navigator.languages
          : [navigator.language];
      }
    } catch (error) { /* ignore */ }
    for (var i = 0; i < tags.length; i += 1) {
      if (typeof tags[i] === 'string' && tags[i]) {
        return tags[i].toLowerCase().indexOf('zh') === 0 ? ZH : EN;
      }
    }
    return EN;
  }

  var LANG = detectLang();

  // Exact matches are checked first; patterns cover sentences that Lit splits
  // around interpolated values (the manifest name, the percentage, ...).
  var ZH_EXACT = {
    'Connecting': '正在连接设备…',
    'Preparing installation': '正在准备安装…',
    'Preparing installation...': '正在准备安装…',
    'Initializing...': '正在初始化…',
    'Installation prepared': '安装已准备',
    'Erasing': '正在擦除闪存…',
    'Erasing device...': '正在擦除设备…',
    'Device erased': '设备已擦除',
    'Installing': '正在刷写固件',
    'Writing complete': '写入完成',
    'Wrapping up': '正在收尾…',
    'All done!': '全部完成！',
    'Installation complete!': '刷写完成！',
    'Installation complete': '刷写完成',
    'Installation failed': '刷写失败',
    'Error': '出错',
    'Confirm Installation': '确认安装',
    'Erase device': '擦除设备',
    'Erase User Data': '擦除用户数据',
    'Back': '返回',
    'Next': '下一步',
    'Close': '关闭',
    'Install': '安装',
    'Continue': '继续',
    'Logs': '日志',
    'Logs & Console': '日志与控制台',
    'Reset Device': '复位设备',
    'Connected to': '已连接到',
    'Visit Device': '访问设备',
    'Add to Home Assistant': '添加到 Home Assistant',
    'Connect to Wi-Fi': '连接 Wi-Fi',
    'Change Wi-Fi': '更改 Wi-Fi',
    'Fund Development': '资助开发',
    'Your device is running': '设备当前运行',
    'Do you want to': '是否要',
    'install': '安装',
    'update to': '更新到',
    'This will take': '大约需要',
    'a minute': '1 分钟',
    '2 minutes': '2 分钟',
    'Keep this page visible to prevent slow down': '请保持本页可见，以免写入变慢',
    'Do you want to erase the device before installing': '是否在安装',
    'All data on the device will be lost.': '设备上的所有数据都会丢失。',
    'Do you want to reset your device and erase all user data from your device?':
      '是否重置设备并擦除设备上的所有用户数据？',
    'All data on the device will be erased.': '设备上的所有数据都会被擦除。',
    'Disconnected': '设备已断开连接',
    'Failed to connect with the device': '无法连接到设备',
    'Failed to download manifest': '无法下载刷写清单',
    'Error while reading serial port': '读取串口失败',
    'Serial port is not ready. Close any other application using it and try again.':
      '串口未就绪。请关闭占用该串口的其他程序后重试。',
    'Unable to connect': '无法连接',
    'Timeout': '超时',
    'No port selected': '未选择串口',
    "If you didn't select a port because you didn't see your device listed, try the following steps:":
      '如果没有看到设备而没有选择串口，请检查以下几点：',
    'Make sure that the device is connected to this computer (the one that runs the browser that shows this website)':
      '确认设备已连接到这台电脑（运行浏览器查看本页的那一台）。',
    'Most devices have a tiny light when it is powered on. If yours has one, make sure it is on.':
      '大多数设备通电时会亮一个小灯；如果你的设备有，请确认它亮着。',
    'Make sure that the USB cable you use can be used for data and is not a power-only cable.':
      '确认所用的 USB 线支持数据传输，而不是只能充电的线。',
    'If you are using a Linux flavor, make sure that your user is part of the':
      '如果你使用 Linux，请确认当前用户在',
    'group so it has permission to access the device.':
      '用户组里，才能访问设备。',
    'You may need to log out & back in or reboot to activate the new group access.':
      '可能需要重新登录或重启才能生效。',
    'Make sure you have the right drivers installed. Below are the drivers for common chips used in ESP devices:':
      '确认已安装正确的驱动。常见 ESP 芯片的驱动如下：',
    'CP2102 drivers:': 'CP2102 驱动：',
    'CH342, CH343, CH9102 drivers:': 'CH342 / CH343 / CH9102 驱动：',
    'CH340, CH341 drivers:': 'CH340 / CH341 驱动：',
    'Cancel': '取消',
    'Try Again': '重试',
    '.': '。',
    '?': '？'
  };

  var ZH_PATTERNS = [
    [/^Writing progress:\s*(\d+)%$/, '写入进度：$1%'],
    [/^This will take a minute\.?$/, '大约需要 1 分钟。'],
    [/^This will take 2 minutes\.?$/, '大约需要 2 分钟。'],
    [/^Connected to (.+)$/, '已连接到 $1'],
    [/^Update (.+)$/, '更新 $1'],
    [/^Install (.+)$/, '安装 $1'],
    [/^Do you want to (.+)$/, '是否要 $1'],
    [/^Unknown error \((.+)\)$/, '未知错误（$1）']
  ];

  function normalize(text) {
    return (text || '').replace(/\s+/g, ' ').trim();
  }

  function translateText(raw, lang) {
    lang = lang || LANG;
    if (lang !== ZH || !raw) return null;
    var norm = normalize(raw);
    if (!norm) return null;
    var out = Object.prototype.hasOwnProperty.call(ZH_EXACT, norm) ? ZH_EXACT[norm] : undefined;
    if (out === undefined) {
      for (var i = 0; i < ZH_PATTERNS.length; i += 1) {
        if (ZH_PATTERNS[i][0].test(norm)) {
          out = norm.replace(ZH_PATTERNS[i][0], ZH_PATTERNS[i][1]);
          break;
        }
      }
    }
    if (out === undefined || out === norm) return null;
    var lead = /^\s/.test(raw) ? ' ' : '';
    var trail = /\s$/.test(raw) ? ' ' : '';
    return lead + out + trail;
  }

  var STEPS = {
    zh: ['连接设备', '擦除与准备', '写入固件', '完成'],
    en: ['Connect', 'Prepare', 'Write', 'Done']
  };

  var NOTES = {
    zh: {
      connecting: '正在连接设备，请确认 USB 数据线已连好。',
      dashboard: '已连接设备。选择安装方式后开始刷写。',
      confirm: '等待确认后开始写入；刷写期间请不要关闭本页。',
      preparing: '正在准备写入，请保持 USB 连接。',
      erasing: '正在擦除闪存，可能需要十几秒。',
      writing: '正在写入固件{pct}，请保持本页可见并不要拔掉 USB。',
      wrapping: '正在收尾，马上完成…',
      finished: '刷写完成。设备会自行重启，现在可以拔掉 USB 数据线。',
      failed: '刷写中断。请检查 USB 连接后关闭窗口重试。',
      error: '连接或写入出现问题，请关闭窗口后重试。'
    },
    en: {
      connecting: 'Connecting to the device. Check the USB cable.',
      dashboard: 'Device connected. Choose how to install to start.',
      confirm: 'Waiting for confirmation. Keep this page open while it writes.',
      preparing: 'Preparing the write. Keep the USB cable connected.',
      erasing: 'Erasing flash; this can take a few seconds.',
      writing: 'Writing firmware{pct}. Keep this page visible and do not unplug USB.',
      wrapping: 'Finishing up…',
      finished: 'Done. The device restarts on its own; you can unplug USB now.',
      failed: 'The flash was interrupted. Check the cable, close this window and retry.',
      error: 'Something went wrong while connecting or writing. Close this window and retry.'
    }
  };

  function noteFor(state, percent, lang) {
    lang = lang || LANG;
    var table = NOTES[lang] || NOTES.en;
    var text = table[state] || table.error;
    var pct = percent === null || percent === undefined ? '' : ' ' + percent + '%';
    return text.replace('{pct}', pct);
  }

  // The dialog is third-party and its private state is not reachable, so the
  // stage is derived from what it rendered. Both languages are accepted
  // because translation may have run before this call.
  function classify(view, started, stage) {
    stage = stage || 0;
    if (/installation complete|刷写完成|安装完成/i.test(view.message)) {
      return { state: 'finished', stage: 4 };
    }
    if (/install(ation)? failed|刷写失败|安装失败/i.test(view.headline + ' ' + view.message)) {
      return { state: started ? 'failed' : 'error', stage: stage || 1 };
    }
    if (/^(error|出错|错误)$/i.test(view.headline)) {
      return { state: 'error', stage: stage || 1 };
    }
    if (/^(logs|日志)$/i.test(view.headline)) {
      return { state: 'logs', stage: stage || 1 };
    }
    if (view.hasProgress) {
      if (/erasing|擦除/i.test(view.label)) return { state: 'erasing', stage: 2 };
      if (/preparing installation|准备安装/i.test(view.label)) return { state: 'preparing', stage: 2 };
      if (/wrapping up|收尾/i.test(view.label)) return { state: 'wrapping', stage: 3 };
      if (view.percent !== null && view.percent !== undefined ||
          /installing|刷写|写入|this will take|大约需要/i.test(view.label + ' ' + view.headline)) {        return { state: 'writing', stage: 3 };
      }
      return { state: 'connecting', stage: 1 };
    }
    if (view.hasList) return { state: 'dashboard', stage: 2 };
    if (/erase device|擦除设备/i.test(view.headline)) return { state: 'confirm', stage: 2 };
    if (/confirm installation|确认安装/i.test(view.headline)) return { state: 'confirm', stage: 2 };
    if (view.hasMessage) return { state: 'error', stage: stage || 1 };
    return { state: 'unknown', stage: stage || 1 };
  }

  var api = {
    lang: LANG,
    translateText: translateText,
    classify: classify,
    steps: function (lang) { return (STEPS[lang || LANG] || STEPS.en).slice(); },
    note: function (state, percent, lang) { return noteFor(state, percent, lang); }
  };
  global.CPYOSFlasherUI = api;

  if (typeof document === 'undefined') return;

  var STYLE_ID = 'cpyos-flasher-flow-style';
  var FLOW_CLASS = 'cpyos-flasher-flow';
  var CSS = [
    '.' + FLOW_CLASS + ' { margin: 18px 8px 4px; padding-top: 14px;',
    '  border-top: 1px solid #e8e8e8; font: 12px/1.45 -apple-system, "Segoe UI", "PingFang SC", sans-serif;',
    '  color: #3d3d3d; }',
    '.' + FLOW_CLASS + '[hidden] { display: none; }',
    '.' + FLOW_CLASS + ' ol { display: flex; margin: 0; padding: 0; list-style: none; }',
    '.' + FLOW_CLASS + ' li { position: relative; flex: 1; text-align: center; }',
    '.' + FLOW_CLASS + ' li + li::before { content: ""; position: absolute; top: 11px; left: -50%;',
    '  width: 100%; height: 2px; background: #ececec; }',
    '.' + FLOW_CLASS + ' li.done::before, .' + FLOW_CLASS + ' li.active::before { background: #121212; }',
    '.' + FLOW_CLASS + ' .dot { position: relative; z-index: 1; display: inline-grid; place-items: center;',
    '  width: 24px; height: 24px; border-radius: 50%; background: #fff; border: 1.5px solid #cfcfcf;',
    '  color: #7a7a7a; font-size: 11px; font-weight: 600; }',
    '.' + FLOW_CLASS + ' li.done .dot { background: #121212; border-color: #121212; color: #fff; }',
    '.' + FLOW_CLASS + ' li.active .dot { border-color: #121212; color: #121212;',
    '  box-shadow: 0 0 0 4px rgba(18, 18, 18, 0.08); }',
    '.' + FLOW_CLASS + ' li.failed .dot { border-color: #c0392b; color: #c0392b; background: #fff; }',
    '.' + FLOW_CLASS + ' .label { display: block; margin-top: 6px; color: #8a8a8a; }',
    '.' + FLOW_CLASS + ' li.active .label { color: #121212; font-weight: 600; }',
    '.' + FLOW_CLASS + ' li.done .label { color: #4a4a4a; }',
    '.' + FLOW_CLASS + ' li.failed .label { color: #c0392b; }',
    '.' + FLOW_CLASS + ' .note { margin: 10px 0 0; text-align: center; color: #555; }'
  ].join('\n');

  function whenReady(fn) {
    if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', fn, { once: true });
    } else {
      fn();
    }
  }

  function setupDom() {
    var contexts = new Set();

    function schedule(ctx) {
      if (ctx.timer) return;
      // Coalesce bursts; the dialog re-renders several times per state change
      // and its console can emit a line per flash block.
      ctx.timer = setTimeout(function () { refresh(ctx); }, 50);
    }

    function observe(ctx, root) {
      if (!root || ctx.seen.has(root)) return;
      ctx.seen.add(root);
      var observer = new MutationObserver(function () { schedule(ctx); });
      observer.observe(root, { childList: true, subtree: true, characterData: true });
      ctx.observers.push(observer);
    }

    function ensureStyle(ctx, shadowRoot) {
      if (shadowRoot.querySelector('#' + STYLE_ID)) return;
      var style = document.createElement('style');
      style.id = STYLE_ID;
      style.textContent = CSS;
      shadowRoot.appendChild(style);
    }

    function translateTree(ctx, root) {
      var children = root.childNodes;
      for (var i = 0; i < children.length; i += 1) {
        var node = children[i];
        if (node.nodeType === 3) {
          var out = translateText(node.nodeValue);
          if (out !== null) node.nodeValue = out;
          continue;
        }
        if (node.nodeType !== 1) continue;
        if (node.tagName === 'EWT-CONSOLE') continue;
        if (node.classList && node.classList.contains(FLOW_CLASS)) continue;
        if (node.shadowRoot) {
          observe(ctx, node.shadowRoot);
          translateTree(ctx, node.shadowRoot);
        }
        translateTree(ctx, node);
      }
    }

    function textOf(node) {
      if (!node) return '';
      var text = node.shadowRoot ? node.shadowRoot.textContent : node.textContent;
      return normalize(text);
    }

    function view(dialog) {
      var shadow = dialog.shadowRoot;
      if (!shadow) return null;
      var ew = shadow.querySelector('ew-dialog');
      if (!ew) return null;
      var headline = textOf(ew.querySelector('div[slot="headline"]'));
      var progress = ew.querySelector('ewt-page-progress');
      var message = ew.querySelector('ewt-page-message');
      var label = textOf(progress);
      var percentMatch = /(\d{1,3})\s*%/.exec(label);
      return {
        ew: ew,
        headline: headline,
        label: label,
        hasProgress: !!progress,
        hasMessage: !!message,
        hasList: !!ew.querySelector('ew-list'),
        message: textOf(message),
        percent: percentMatch ? Number(percentMatch[1]) : null
      };
    }

    function createFlow() {
      var flow = document.createElement('div');
      flow.className = FLOW_CLASS;
      flow.setAttribute('slot', 'content');
      flow.setAttribute('aria-live', 'polite');
      var list = document.createElement('ol');
      var labels = STEPS[LANG] || STEPS.en;
      for (var i = 0; i < labels.length; i += 1) {
        var item = document.createElement('li');
        var dot = document.createElement('span');
        dot.className = 'dot';
        dot.textContent = String(i + 1);
        var label = document.createElement('span');
        label.className = 'label';
        label.textContent = labels[i];
        item.appendChild(dot);
        item.appendChild(label);
        list.appendChild(item);
      }
      flow.appendChild(list);
      var note = document.createElement('p');
      note.className = 'note';
      flow.appendChild(note);
      return flow;
    }

    function setNote(node, text) {
      if (node.textContent !== text) node.textContent = text;
    }

    function updateFlow(ctx, v, cls) {
      if (!ctx.flow) ctx.flow = createFlow();
      var items = ctx.flow.querySelectorAll('li');
      var note = ctx.flow.querySelector('.note');
      if (cls.state === 'logs' || cls.state === 'unknown') {
        ctx.flow.hidden = true;
        return;
      }
      var done = {};
      var failed = 0;
      var active = cls.stage || 1;
      if (cls.state === 'finished') {
        for (var i = 1; i <= items.length; i += 1) done[i] = true;
        active = 0;
      } else if (cls.state === 'failed') {
        failed = active;
        for (var j = 1; j < active; j += 1) done[j] = true;
      } else if (cls.state === 'error') {
        failed = active;
      } else if (active > 1) {
        for (var k = 1; k < active; k += 1) done[k] = true;
      }
      for (var n = 0; n < items.length; n += 1) {
        var number = n + 1;
        items[n].classList.toggle('done', !!done[number]);
        items[n].classList.toggle('active', number === active);
        items[n].classList.toggle('failed', number === failed);
      }
      var stateForNote = cls.state === 'finished' ? 'finished' : cls.state;
      setNote(note, noteFor(stateForNote, cls.state === 'writing' ? v.percent : null));
      ctx.flow.hidden = false;
      if (ctx.flow.parentNode !== v.ew) v.ew.appendChild(ctx.flow);
    }

    function refresh(ctx) {
      ctx.timer = 0;
      var dialog = ctx.dialog;
      if (!dialog.isConnected) {
        cleanup(ctx);
        return;
      }
      var shadow = dialog.shadowRoot;
      if (!shadow) {
        schedule(ctx);
        return;
      }
      ensureStyle(ctx, shadow);
      observe(ctx, shadow);
      translateTree(ctx, shadow);
      var v = view(dialog);
      if (!v) {
        // The dialog renders asynchronously after `port` is assigned. Retry
        // slowly instead of spinning; cleanup stops once it disconnects.
        ctx.timer = setTimeout(function () { refresh(ctx); }, 150);
        return;
      }
      var cls = classify(v, ctx.started, ctx.stage);
      ctx.stage = Math.max(ctx.stage || 0, cls.stage || 0);
      if (cls.state === 'preparing' || cls.state === 'erasing' || cls.state === 'writing' ||
          cls.state === 'wrapping' || cls.state === 'finished' || cls.state === 'failed') {
        ctx.started = true;
      }
      if (ctx.flowEnabled) updateFlow(ctx, v, cls);
    }

    function cleanup(ctx) {
      if (ctx.timer) clearTimeout(ctx.timer);
      for (var i = 0; i < ctx.observers.length; i += 1) ctx.observers[i].disconnect();
      contexts.delete(ctx);
    }

    function attach(dialog) {
      if (dialog.__cpyosFlasherAttached) return;
      dialog.__cpyosFlasherAttached = true;
      var ctx = {
        dialog: dialog,
        flowEnabled: dialog.tagName === 'EWT-INSTALL-DIALOG',
        observers: [],
        seen: new WeakSet(),
        timer: 0,
        stage: 0,
        started: false,
        flow: null
      };
      contexts.add(ctx);
      if (dialog.shadowRoot) observe(ctx, dialog.shadowRoot);
      schedule(ctx);
    }

    var documentObserver = new MutationObserver(function (records) {
      for (var i = 0; i < records.length; i += 1) {
        var added = records[i].addedNodes;
        for (var j = 0; j < added.length; j += 1) {
          var node = added[j];
          // The install dialog and the "no port selected" dialog are separate
          // web components; both are English-only without this script.
          if (node.nodeType === 1 &&
              (node.tagName === 'EWT-INSTALL-DIALOG' || node.tagName === 'EWT-NO-PORT-PICKED-DIALOG')) {
            attach(node);
          }
        }
      }
      // A dialog removed from the page should stop being observed; refresh()
      // also notices isConnected === false once one of its own mutations
      // arrives, but a closed dialog emits none.
      contexts.forEach(function (ctx) {
        if (!ctx.dialog.isConnected) cleanup(ctx);
      });
    });
    documentObserver.observe(document.body, { childList: true, subtree: true });

    var existing = document.querySelectorAll('ewt-install-dialog, ewt-no-port-picked-dialog');
    for (var i = 0; i < existing.length; i += 1) attach(existing[i]);
  }

  whenReady(setupDom);
})(typeof window !== 'undefined' ? window : globalThis);
