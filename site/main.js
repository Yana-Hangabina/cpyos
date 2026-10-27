(() => {
  'use strict';

  // Read Pico exposes Espressif's native USB-Serial/JTAG device. Filtering the
  // Web Serial chooser by this VID/PID keeps unrelated serial adapters out of
  // the list. esp-web-tools 10.4.0 calls requestPort() without filters, so we
  // add them for the duration of one connection attempt.
  const CPYOS_SERIAL_FILTERS = [{ usbVendorId: 0x303a, usbProductId: 0x1001 }];

  function filterNextSerialRequest() {
    const serial = navigator.serial;
    if (!serial || serial.__cpyosFiltered) return;
    const original = serial.requestPort.bind(serial);
    const filtered = async (options = {}) => {
      try {
        return await original({ ...options, filters: CPYOS_SERIAL_FILTERS });
      } finally {
        if (serial.requestPort === filtered) serial.requestPort = original;
        delete serial.__cpyosFiltered;
      }
    };
    serial.requestPort = filtered;
    serial.__cpyosFiltered = true;
  }

  document.querySelectorAll('esp-web-install-button').forEach((host) => {
    const target = host.querySelector('[slot="activate"]') || host;
    target.addEventListener('click', filterNextSerialRequest, true);
  });

  const root = document.documentElement;
  const themeButton = document.getElementById('theme-toggle');
  const themeOrder = ['auto', 'light', 'dark'];
  const themeLabel = { auto: '自动', light: '浅色', dark: '深色' };

  function currentTheme() {
    const saved = root.dataset.theme;
    return saved === 'light' || saved === 'dark' ? saved : 'auto';
  }

  function applyTheme(theme) {
    if (theme === 'auto') {
      delete root.dataset.theme;
      try { localStorage.removeItem('cpyos-theme'); } catch (error) {}
    } else {
      root.dataset.theme = theme;
      try { localStorage.setItem('cpyos-theme', theme); } catch (error) {}
    }
    if (themeButton) {
      themeButton.title = `主题：${themeLabel[theme]}（点击切换）`;
      themeButton.setAttribute('aria-label', `主题：${themeLabel[theme]}，点击切换`);
    }
  }

  if (themeButton) {
    applyTheme(currentTheme());
    themeButton.addEventListener('click', () => {
      const next = themeOrder[(themeOrder.indexOf(currentTheme()) + 1) % themeOrder.length];
      applyTheme(next);
    });
  }

  function el(id) { return document.getElementById(id); }

  function setLink(id, href) {
    const node = el(id);
    if (node && href) node.href = href;
  }

  function formatDate(value) {
    if (!value) return 'READ PICO';
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return 'READ PICO';
    return `READ PICO · ${date.getUTCFullYear()}.${String(date.getUTCMonth() + 1).padStart(2, '0')}.${String(date.getUTCDate()).padStart(2, '0')}`;
  }

  async function loadRelease() {
    let latest;
    try {
      const response = await fetch('./latest.json', { cache: 'no-cache' });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      latest = await response.json();
    } catch (error) {
      // A checkout without a release still renders the fallback version and
      // points the download links at the releases page.
      return;
    }

    const version = latest.version;
    if (!version) return;

    for (const id of ['release-version', 'hero-version', 'spec-version']) {
      const node = el(id);
      if (node) node.textContent = version;
    }
    const dateNode = el('release-date');
    if (dateNode) dateNode.textContent = formatDate(latest.publishedAt);

    const notes = latest.notes && Array.isArray(latest.notes.zh) ? latest.notes.zh : [];
    const notesNode = el('release-notes');
    if (notesNode && notes.length) {
      notesNode.replaceChildren(...notes.map((note) => {
        const li = document.createElement('li');
        li.textContent = note;
        return li;
      }));
    }

    const base = `./downloads/${version}`;
    const sha = latest.firmware && latest.firmware.sha256 ? latest.firmware.sha256 : '';
    const shaNode = el('firmware-sha');
    if (shaNode) shaNode.textContent = sha || '—';

    const copy = el('copy-sha');
    if (copy && sha) {
      copy.addEventListener('click', async () => {
        try {
          await navigator.clipboard.writeText(sha);
          copy.textContent = '已复制';
          setTimeout(() => { copy.textContent = '复制'; }, 1600);
        } catch (error) {
          copy.textContent = '复制失败';
        }
      });
    }

    const install = el('install-button');
    if (install) install.setAttribute('manifest', `${base}/manifest-install.json`);
    const upgrade = el('upgrade-button');
    if (upgrade) upgrade.setAttribute('manifest', `${base}/manifest-upgrade.json`);

    const releaseUrl = `https://github.com/Yana-Hangabina/cpyos/releases/tag/${encodeURIComponent(version)}`;
    const zipUrl = `https://github.com/Yana-Hangabina/cpyos/releases/download/${encodeURIComponent(version)}/cpyos-readpico-${encodeURIComponent(version)}-full.zip`;
    setLink('download-app', `${base}/readpico-firmware.bin`);
    setLink('download-sums', `${base}/SHA256SUMS`);
    setLink('download-zip', zipUrl);
    setLink('download-release', releaseUrl);
  }

  const reveal = document.querySelectorAll('.reveal');
  if ('IntersectionObserver' in window && !window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
    const observer = new IntersectionObserver((entries) => {
      for (const entry of entries) {
        if (entry.isIntersecting) {
          entry.target.classList.add('on');
          observer.unobserve(entry.target);
        }
      }
    }, { rootMargin: '0px 0px -8% 0px', threshold: 0.08 });
    reveal.forEach((node) => observer.observe(node));
  } else {
    reveal.forEach((node) => node.classList.add('on'));
  }

  loadRelease();
})();
