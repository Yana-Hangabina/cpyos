# CPYOS project site

Static GitHub Pages site for 脆皮鸭 CPYOS. It has no build step: `index.html`,
`styles.css` and `main.js` are served as-is. The release workflow adds
`latest.json`, `ota/readpico.json` and `downloads/<version>/` from the release
bundle before uploading `_site/`.

The interface section uses CSS-drawn layout mockups. The site ships **no device
screenshots** and contains no book titles, Wi-Fi SSIDs or account data.

## Preview

```bash
python3 -m http.server 8765
# open http://localhost:8765/
```

Without a `latest.json`, the page falls back to version 0.1.0 and points the
download links at the Releases page. To preview with a release, run:

```bash
python3 scripts/build_cpyos_release.py --version 0.1.0
python3 scripts/build_cpyos_site.py --release-dir dist/cpyos/0.1.0 --output _site
python3 -m http.server 8765 --directory _site
```

## Theme

Light is the default; `prefers-color-scheme: dark` switches automatically. The
button in the header cycles auto → light → dark and stores the choice in
`localStorage` (`cpyos-theme`). The `data-theme` attribute is applied before the
first paint by the inline script in `<head>`.

## Flashing

The install and upgrade buttons are `esp-web-install-button` elements from
esp-web-tools 10.4.0 (pinned with SRI). Their manifests are set from
`latest.json` at runtime. Browsers must support Web Serial and the page must be
served over https or localhost.

esp-web-tools 10.4.0 calls `navigator.serial.requestPort()` without filters.
For the duration of one connection attempt, `main.js` wraps that call with:

```js
[{ usbVendorId: 0x303a, usbProductId: 0x1001 }]
```

That is the Read Pico's native USB-Serial/JTAG device, so the browser chooser no
longer lists unrelated serial adapters.
