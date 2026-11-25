# Plus flasher vendor files

- `esptool-js-0.7.0.js` — unmodified esptool-js 0.7.0 browser bundle (Apache-2.0;
  see `ESPTOOL-LICENSE`). It is the first release with an ESP32-S31 chip class.
- `esp32s31-stub.json` — Espressif esptool 5.5.0 S31 RAM stub, MIT
  (`ESP-FLASHER-STUB-MIT`). SHA-256 is pinned in `plus-flasher.js`.
- `spark-md5-3.0.2.js` — SparkMD5 3.0.2 for device-side MD5 verification
  (MIT; see `SPARK-MD5-LICENSE`).
- `s31-transport.js` — CPYOS Web Serial transport for the S31 ROM/stub,
  written for this project.

The Plus installer uses these files directly because esp-web-tools does not
ship an ESP32-S31 chip module. Read Pico keeps the esp-web-tools path.
