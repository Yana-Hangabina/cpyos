/* CPYOS web installer for Metalio E-INK4 Plus (ESP32-S31).
 *
 * The Plus is not an esp-web-tools target (no stable S31 chip module), so this
 * installer talks to esptool-js 0.7.0 directly. It writes the partition table,
 * both OTA app slots and every data pack from the release bundle, then verifies
 * each range with the flash MD5 command. Read Pico keeps its esp-web-tools
 * button.
 */
import SparkMD5 from './vendor/plus-flasher/spark-md5-3.0.2.js';
import { S31Transport } from './vendor/plus-flasher/s31-transport.js';

const S31_STUB_SHA256 = 'bccb338d09714d35283048936056adc2ba4d2172a694ad1f79a08ed04cc242ad';
const NVS = { address: 0x9000, size: 0x5000 };

const $ = (id) => document.getElementById(id);
const statusNode = $('plus-status');
const logNode = $('plus-log');
const progressNode = $('plus-progress');
const connectNode = $('plus-connect');
const eraseNode = $('plus-erase');
const manifestNode = $('plus-installer');

let manifestPromise = null;
let manifestBase = '';
let busy = false;

function say(text) {
  if (statusNode) statusNode.textContent = text;
}

function appendLog(text) {
  if (!logNode) return;
  logNode.textContent = `${logNode.textContent}${text}\n`.slice(-24000);
  logNode.scrollTop = logNode.scrollHeight;
}

function setProgress(value, label) {
  if (progressNode) {
    progressNode.hidden = false;
    progressNode.value = Math.max(0, Math.min(100, value));
  }
  if (label) say(`${label}（${Math.round(value)}%）`);
}

function setBusy(value) {
  busy = value;
  if (connectNode) connectNode.disabled = value;
  if (eraseNode) eraseNode.disabled = value;
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes)) return '';
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function manifestUrl() {
  const raw = manifestNode?.dataset.manifest || './downloads/0.8.0/flash-plus.json';
  return new URL(raw, document.baseURI).toString();
}

async function loadManifest() {
  if (!manifestPromise) {
    manifestPromise = (async () => {
      const response = await fetch(manifestUrl(), { cache: 'no-store' });
      if (!response.ok) throw new Error(`找不到 E-INK4 Plus 安装清单（HTTP ${response.status}）`);
      const config = await response.json();
      manifestBase = new URL('.', manifestUrl()).toString();
      if (!Array.isArray(config.parts) || !config.parts.length) {
        throw new Error('E-INK4 Plus 安装清单没有固件分段');
      }
      return config;
    })().catch((error) => {
      manifestPromise = null;
      throw error;
    });
  }
  return manifestPromise;
}

async function sha256Hex(bytes) {
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  return Array.from(new Uint8Array(digest), (b) => b.toString(16).padStart(2, '0')).join('');
}

async function fetchPart(path, index, count) {
  const url = new URL(path, manifestBase || document.baseURI).toString();
  const response = await fetch(url, { cache: 'no-store' });
  if (!response.ok) throw new Error(`下载 ${path} 失败（HTTP ${response.status}）`);
  const total = Number(response.headers.get('content-length')) || 0;
  let received = 0;
  let lastPaint = 0;
  const reader = response.body?.getReader();
  const chunks = [];
  if (!reader) {
    const bytes = new Uint8Array(await response.arrayBuffer());
    return bytes;
  }
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    chunks.push(value);
    received += value.length;
    const now = performance.now();
    if (now - lastPaint > 120 || received === total) {
      lastPaint = now;
      const share = total ? received / total : 0;
      const overall = ((index + share) / count) * 35;
      setProgress(overall, total
        ? `正在下载安装包 ${index + 1}/${count} · ${formatBytes(received)} / ${formatBytes(total)}`
        : `正在下载安装包 ${index + 1}/${count} · ${formatBytes(received)}`);
    }
  }
  const bytes = new Uint8Array(received);
  let offset = 0;
  for (const chunk of chunks) {
    bytes.set(chunk, offset);
    offset += chunk.length;
  }
  return bytes;
}

async function loadParts(config) {
  const unique = [];
  const seen = new Set();
  for (const part of config.parts) {
    if (!seen.has(part.path)) {
      seen.add(part.path);
      unique.push(part);
    }
  }
  const files = new Map();
  for (let i = 0; i < unique.length; i += 1) {
    const part = unique[i];
    const bytes = await fetchPart(part.path, i, unique.length);
    if (part.size && bytes.length !== part.size) {
      throw new Error(`${part.path} 大小不符：清单 ${part.size} B，下载 ${bytes.length} B`);
    }
    if (part.sha256) {
      const actual = await sha256Hex(bytes);
      if (actual !== part.sha256.toLowerCase()) {
        throw new Error(`${part.path} 校验失败（SHA-256 不匹配）`);
      }
    }
    files.set(part.path, bytes);
    appendLog(`已校验 ${part.path} · ${formatBytes(bytes.length)}`);
  }
  return files;
}

async function loadStub() {
  const response = await fetch(new URL('./vendor/plus-flasher/esp32s31-stub.json', import.meta.url), {
    cache: 'no-store',
  });
  if (!response.ok) throw new Error(`S31 RAM stub 下载失败（HTTP ${response.status}）`);
  const bytes = new Uint8Array(await response.arrayBuffer());
  if ((await sha256Hex(bytes)) !== S31_STUB_SHA256) {
    throw new Error('S31 RAM stub 校验失败');
  }
  const stub = JSON.parse(new TextDecoder().decode(bytes));
  if (stub.text_start !== 788533248 || stub.data_start !== 788647352 || stub.entry !== 788538252) {
    throw new Error('S31 RAM stub 地址不符合预期');
  }
  const decode = (text) => Uint8Array.from(atob(text), (c) => c.charCodeAt(0));
  stub.text = decode(stub.text);
  stub.data = decode(stub.data);
  return stub;
}

async function prepareS31Download(loader) {
  if (loader.chip?.CHIP_NAME !== 'ESP32-S31' || loader.chip.IMAGE_CHIP_ID !== 32) {
    throw new Error('不是 ESP32-S31 下载口');
  }
  if (loader.secureDownloadMode) throw new Error('设备处于安全下载模式，无法写入');
  if (loader.IS_STUB) return;
  await loader.writeReg(0x20801018, 0x50d83aa1);
  await loader.writeReg(0x20801000, 0);
  await loader.writeReg(0x20801018, 0);
  await loader.writeReg(0x20801020, 0x50d83aa1);
  const swd = await loader.readReg(0x2080101c);
  await loader.writeReg(0x2080101c, (swd | 0x40000) >>> 0);
  await loader.writeReg(0x20801020, 0);
}

async function runS31Stub(loader, stub) {
  if (loader.IS_STUB) return;
  const block = loader.ESP_RAM_BLOCK;
  for (const [address, bytes] of [[stub.text_start, stub.text], [stub.data_start, stub.data]]) {
    if (!bytes.length) continue;
    await loader.memBegin(bytes.length, Math.ceil(bytes.length / block), block, address);
    for (let at = 0, sequence = 0; at < bytes.length; at += block, sequence += 1) {
      await loader.memBlock(bytes.subarray(at, at + block), sequence);
    }
  }
  await loader.memFinish(stub.entry);
  const hello = await loader.transport.read(5000);
  if (!hello || hello.length !== 4 || hello[0] !== 79 || hello[1] !== 72 || hello[2] !== 65 || hello[3] !== 73) {
    throw new Error('S31 RAM stub 启动握手失败');
  }
  loader.IS_STUB = true;
  await loader.applyUsbFlashWriteSize();
  appendLog('S31 官方 RAM stub 已运行');
}

async function prepareS31Flash(loader, size) {
  if (!loader.IS_STUB) throw new Error('需要 S31 下载程序');
  if (size !== 0x1000000) throw new Error(`需要 16 MB Flash，检测到 ${formatBytes(size)}`);
  const request = new Uint8Array(24);
  const view = new DataView(request.buffer);
  [0, size, 0x10000, 0x1000, 0x100, 0xffff].forEach((value, index) => view.setUint32(index * 4, value, true));
  await loader.checkCommand('set SPI params', 0x0b, request, undefined, undefined, 5000);
  appendLog('Flash 参数：16 MB / 64 KiB 块 / 4 KiB 扇区');
}

async function verifyRange(loader, address, data) {
  const actual = await loader.flashMd5sum(address, data.length);
  const expected = SparkMD5.ArrayBuffer.hash(data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength));
  if (String(actual).toLowerCase() !== expected) {
    throw new Error(`写入校验失败：0x${address.toString(16)}`);
  }
  appendLog(`设备端 MD5 校验通过 0x${address.toString(16)} · ${formatBytes(data.length)}`);
}

async function resetS31(loader) {
  await loader.writeReg(0x20801018, 0x50d83aa1);
  await loader.writeReg(0x20801004, 2000);
  await loader.writeReg(0x20801000, 0xd0000102);
  await loader.writeReg(0x20801018, 0);
}

async function flash() {
  if (busy) return;
  if (!navigator.serial) {
    say('当前浏览器不支持 Web Serial，请使用桌面版 Chrome 或 Edge。');
    return;
  }
  setBusy(true);
  if (logNode) logNode.textContent = '';
  setProgress(0, '等待选择设备');
  let port;
  let loader;
  let connected = false;
  let completed = false;
  try {
    // Must run inside the click handler. Both CPYOS boards expose the same
    // Espressif USB-JTAG VID/PID.
    port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a, usbProductId: 0x1001 }] });
    const config = await loadManifest();
    const files = await loadParts(config);
    const stub = await loadStub();
    const { ESPLoader } = await import('./vendor/plus-flasher/esptool-js-0.7.0.js');

    const transport = new S31Transport(port);
    loader = new ESPLoader({
      transport,
      baudrate: 115200,
      romBaudrate: 115200,
      serialOptions: { bufferSize: 65536 },
      terminal: {
        clean() {},
        write: (text) => appendLog(String(text).trimEnd()),
        writeLine: (text) => appendLog(String(text).trimEnd()),
      },
    });

    const originalRunStub = loader.runStub.bind(loader);
    loader.runStub = async () => {
      await prepareS31Download(loader);
      await runS31Stub(loader, stub);
      return loader.chip;
    };
    say('正在连接 ESP32-S31（最长 30 秒）…');
    await loader.main('usb_reset');
    loader.runStub = originalRunStub;
    if (loader.chip?.CHIP_NAME !== 'ESP32-S31' || loader.chip.IMAGE_CHIP_ID !== 32) {
      throw new Error(`检测到 ${loader.chip?.CHIP_NAME || '未知芯片'}，不是 Metalio E-INK4 Plus`);
    }
    connected = true;
    appendLog(`已连接 ${loader.chip.CHIP_NAME} · MAC ${await loader.chip.readMac(loader)}`);
    const sizeLabel = await loader.detectFlashSize();
    const flashSize = loader.flashSizeBytes(sizeLabel);
    await prepareS31Flash(loader, flashSize);

    const parts = config.parts
      .map((part) => ({ address: part.offset, data: files.get(part.path) }))
      .sort((a, b) => a.address - b.address);
    setProgress(40, `正在写入 ${config.version || ''}，请保持 USB 连接`);
    await loader.writeFlash({
      fileArray: parts,
      flashSize: 'keep',
      flashMode: 'keep',
      flashFreq: 'keep',
      eraseAll: false,
      compress: true,
      reportProgress: (index, written, total) => {
        setProgress(40 + ((index + written / total) / parts.length) * 45);
      },
    });

    for (let i = 0; i < parts.length; i += 1) {
      setProgress(85 + (i / parts.length) * 10, `正在校验 ${i + 1}/${parts.length}`);
      await verifyRange(loader, parts[i].address, parts[i].data);
    }

    if (eraseNode?.checked) {
      setProgress(97, '正在清除设备设置');
      const blank = new Uint8Array(NVS.size).fill(0xff);
      await loader.writeFlash({
        fileArray: [{ address: NVS.address, data: blank }],
        flashSize: 'keep',
        flashMode: 'keep',
        flashFreq: 'keep',
        eraseAll: false,
        compress: true,
      });
      appendLog('已清除 NVS 设置（SD 卡文件保留）');
    }

    setProgress(100, '刷写完成，正在重启');
    try {
      await resetS31(loader);
    } catch (error) {
      appendLog(`重启命令失败：${error.message}`);
    }
    completed = true;
    say('刷写完成。设备会自动重启，现在可以拔掉 USB。');
  } catch (error) {
    const message = error?.name === 'NotFoundError'
      ? '已取消选择，没有写入任何内容。'
      : error?.message || String(error);
    appendLog(`错误：${message}`);
    say(`刷写未完成：${message}`);
  } finally {
    setBusy(false);
    if (connected && !completed && loader?.IS_STUB) {
      // Never leave a failed attempt parked in the S31 ROM/stub: the screen
      // keeps the last frame and taps stop working until a power cycle.
      try { await resetS31(loader); } catch (error) { appendLog(`失败后复位失败：${error.message}`); }
    }
    if (loader?.transport) {
      try { await loader.transport.disconnect(); } catch (error) { appendLog(`串口关闭：${error.message}`); }
    } else if (port?.readable) {
      try { await port.close(); } catch (error) { /* already closed */ }
    }
  }
}

function selectDevice(device) {
  document.querySelectorAll('[data-device-panel]').forEach((panel) => {
    panel.hidden = panel.dataset.devicePanel !== device;
  });
  document.querySelectorAll('.device-card').forEach((card) => {
    const input = card.querySelector('input[type="radio"]');
    card.classList.toggle('selected', input?.value === device);
  });
  const title = $('installer-title');
  if (title) title.textContent = device === 'plus' ? 'E-INK4 Plus 完整安装' : 'Read Pico 完整安装';
  const desc = $('installer-desc');
  if (desc) {
    desc.textContent = device === 'plus'
      ? '网页会写入分区表、两个应用槽和六个数据包（含阅读字库），适合首次安装或恢复。CloudZao 市场只写一个应用槽，不能替代这里的完整安装。'
      : '网页完整安装会重建 Read Pico 的分区表并写入 PDF 引擎与数据包，适合首次安装或恢复。0.7.0 及更早版本请使用这里，不能走 OTA。';
  }
  try { localStorage.setItem('cpyos-flash-device', device); } catch (error) { /* ignore */ }
  if (device === 'pico') {
    if (connectNode) connectNode.disabled = !navigator.serial;
    const version = $('release-version')?.textContent || '0.8.0';
    const app = $('download-app');
    if (app) app.href = new URL(`./downloads/${version}/readpico-firmware.bin`, document.baseURI).toString();
    const zip = $('download-zip');
    if (zip) {
      zip.hidden = false;
      zip.textContent = '完整安装包';
    }
    return;
  }
  if (device === 'plus') {
    const zip = $('download-zip');
    if (zip) zip.hidden = true;
    loadManifest()
      .then((config) => {
        if (connectNode) connectNode.disabled = !navigator.serial;
        const app = config.parts.find((part) => /cpyos-plus.*\.bin$/i.test(part.path) && !/partition-table/i.test(part.path));
        if (app) {
          const link = $('download-app');
          if (link) link.href = new URL(app.path, manifestBase || manifestUrl()).toString();
        }
      })
      .catch((error) => {
        say(`此版本没有可用的 Plus 网页安装包：${error.message}`);
        if (connectNode) connectNode.disabled = true;
      });
  }
}

function init() {
  if (!manifestNode || !connectNode) return;
  connectNode.addEventListener('click', flash);
  document.querySelectorAll('input[name="flash-device"]').forEach((input) => {
    input.addEventListener('change', () => selectDevice(input.value));
  });
  let saved = 'pico';
  try { saved = localStorage.getItem('cpyos-flash-device') || 'pico'; } catch (error) { /* ignore */ }
  const radio = document.querySelector(`input[name="flash-device"][value="${saved}"]`);
  if (radio) radio.checked = true;
  selectDevice(radio ? saved : 'pico');
  if (!navigator.serial) {
    say('当前浏览器不支持 Web Serial，请使用桌面版 Chrome 或 Edge。');
    connectNode.disabled = true;
  }
}

init();
window.CPYOSPlusInstaller = { flash, selectDevice, loadManifest, loadParts, loadStub };
document.documentElement.dataset.plusInstaller = 'ready';
