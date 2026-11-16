# Speaker, microphone and voice on the Metalio E-INK4 Plus

Status: **speaker and microphone verified on hardware** (2026-11-14). Bluetooth
audio, the voice memo and the xiaozhi push-to-talk AI client are implemented
(2026-11-15) but still need a headset/server pass; the WeChat mini-program needs
the vendor cloud (see the last section). The BT and AI-voice workers now run on
PSRAM stacks with a fail-closed internal-RAM guard (2026-11-16) — see
`docs/plus-ai-key.md` for the budget and the physical AI-key binding.

## Hardware

The ESP32-S31 does not drive the speaker or microphone directly. A **BH1098G**
Bluetooth audio SoC owns both I2S clocks and the analog path:

| Signal | Pin / owner |
|---|---|
| I2S BCLK / WS / DOUT / DIN | GPIO42 / GPIO43 / GPIO44 / GPIO45 (ESP **slave**) |
| Audio control UART1 | TX GPIO39, RX GPIO38, 115200 |
| Speaker PA enable | TCA9555 **P0.4** (NS4150B `CTRL`, active high) |
| Shared 4G/BT/PA rail | TCA9555 **P1.4** (active high) |

Audio only flows after the ESP puts the BH1098G into **mode 1** over UART1
(`AT+RX=2`, 700 ms, then `AT+MODE=1` and wait for `SET MODE 1`). The vendor
power-up sequence also pulls P1.4 low for 1 s with a 500 ms motor pulse to
discharge the shared rail.

## Where the code comes from

The audio stack is ported from the official Metalio firmware source
(`https://github.com/CloudZao/Metalio-E-INK4-Plus`, `main`, reference checkout
`2b2c36ae`, checked out at `repos/metalio-e-ink4-plus`; the old
`.scratch/refs/metalio-vendor` path is a compatibility symlink):

- `main/boards/metalio-e-ink4-plus/periph/metalio_audio.{h,cc}` — BT power reset,
  UART observer, `AT+RX=2` / `AT+MODE=1`, retries, 700 ms settle.
- `main/boards/common/audio/bt_audio_codec.{h,cc}` — I2S **slave**, 16 kHz,
  32-bit stereo slots, `sample * (volume/100)^2 * 65536`, mic `>>12`.
- `main/boards/metalio-e-ink4-plus/config.h` — pins and sample rates.

Licence: MIT, Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd. and
Project Contributors (see the upstream `LICENSE`). The command sequence, retry
count, settle times and I2S slot format are kept identical; only the plumbing
(no `SimpleUart` observer, no `AudioCodec` base class) is CPYOS-specific.

## What is implemented here

| File | Role |
|---|---|
| `components/cpyos_metalio_bsp/plus_audio.{h,c}` | Mode-1 handshake (background, 8 KiB task), I2S channel init, PA control, 16 kHz mono playback, stereo mic capture |
| `components/cpyos_ui/compat/HalAudioOutput.cpp` | CPYOS `HalAudioOutput` backend over `plus_audio` |
| `lib/SoundFeedback` | shared UI cues; `METALIO_PLUS_CALIBRATION` added |
| `components/cpyos_ui/assets/sounds/*.wav` | the same `select.wav` / `tap.wav` Read Pico embeds |

The handshake is **lazy**: nothing runs unless sound feedback is enabled or a
consumer asks for audio. With feedback on it is warmed at boot in the background,
because the ~2.5 s handshake must never block a key press; the first cue that
arrives before the mode is ready is dropped.

Serial bench commands:

```
audio                 # state (0 not ready / 1 initialising / 2 ready / 3 failed)
audio tone [ms]       # 440 Hz test tone, PA on for the duration
audio mic [frames]    # capture and print the RMS (a silent mic reports 0)
```

Verified 2026-11-14:

```
audio: state=2 ready=1 rate=16000
PLUS-AUD: BH1098 mode 1 ready (16000 Hz, I2S slave)
audio: tone 400 ms, 6400 frames written
audio: mic frames=400 rms=95
```

The first attempt crashed `plus-console` (12 KiB stack) while the I2S driver
initialised. I2S init now runs on the audio worker (8 KiB) and the console stack
is 20 KiB; playback from the UI task was never affected.

## Side volume keys (TCA9555 P1.0 / P1.1)

Same rule as the official UI (`display/lv_adapter_epdiy.cc`, `OnVolUpClick` /
`OnVolUpLong`): **inside the reader the keys turn pages** (they keep the old
BTN_UP / BTN_DOWN mapping); everywhere else a click changes the speaker volume
by 10 % and a long press (600 ms) jumps to 100 % / 0 %. Each change is saved in
`SETTINGS.audioVolume` and shown as a short speaker + 10-segment overlay
(no new i18n key: the English blob is at its 15-bit offset limit).

`HalGPIO::setSideKeysNavigate(bool)` switches the mapping per frame; while it is
off the keys surface as `wasVolumeUp/DownPressed/Released` and
`isVolumeUp/DownHeld` instead of logical Up/Down. Bench: `volume [0-100]`.

## Limits

- 16 kHz mono only; the BH1098G provides that clock in mode 1. Other sample
  rates are rejected by `HalAudioOutput` rather than resampled.
- **4G survives an audio start.** The BT reset cuts P14, which also powers the
  modem, so `plus_audio` calls the weak `plus_audio_prepare_rail_cycle()` hook
  first (the cellular component stops the modem) and
  `plus_audio_rail_cycle_done()` right after the rail comes back up (the modem
  is re-initialised in the background, vendor `Nt26Board` order). Audio is no
  longer refused while 4G is active; an in-flight 4G transfer is interrupted by
  the reset exactly as it is on the vendor firmware.
- If a future power path cuts P1.4 (sleep/off), call `plus_audio_shutdown()`;
  the next use re-runs the reset + handshake.
- Enabling sound feedback makes the first boot run the vendor BT reset (shared
  rail, 500 ms motor pulse).

## AI voice and WeChat mini-program

Both are built into the official firmware on top of this audio path:

- `main/audio/audio_service.{h,cc}` — mic → AFE → Opus → send queue, and
  server → Opus → playback queue. Depends on `78/esp-opus-encoder`,
  `espressif/esp-sr` (wake word) and the vendor audio codec.
- `main/protocols/websocket_protocol.cc` — xiaozhi WebSocket v1/v2/v3 framing.
- `main/chat_cloud.h` — provider choice (Baiwen / Xiaozhi / Yunxin). The
  WebSocket URL and token come from NVS namespaces (`xz_websocket`,
  `yx_websocket`), i.e. **from the vendor cloud during activation**;
  `cloudzao_endpoints.c` is blank in the open-source tree.
- WeChat mini-program = `ink-screen` endpoints (`library/sync`,
  `push-resources`) plus a WSS channel, also behind the vendor cloud account.

Consequence: the audio **device** side is done. The AI client now speaks the
official xiaozhi protocol with a user-supplied server URL/token; the WeChat
mini-program still needs the vendor cloud. Wake word (esp-sr) is not usable
here: the vendor stub (`model_path.h`) and `CONFIG_WAKE_WORD_DISABLED=y`
confirm the S31 build has no AFE; the client is push-to-talk only.

### What exists now (2026-11-15)

- **Bluetooth audio page** (Settings → Controls → BT audio): the vendor mode
  2/3 handshake, `AT+INQUIRING` / `AT+BT:<addr><name>` / `AT+CONNECT=<addr>`,
  `AT+BTSCO=0` disconnect; mode 1 is restored on exit. No cloud.
- **Voice memo** (tools tile): mic → mono 16 kHz WAV under `/recordings`, and
  playback through the same PA path. No codec, no cloud.
- **AI voice client is implemented (2026-11-15), untested without a server.**
  The official wire format is ported literally: `Authorization: Bearer`,
  `Protocol-Version`, `Device-Id`/`Client-Id` headers, the vendor client
  hello (with `features.mcp=false`), server-hello parsing, and v1/v2/v3 Opus
  framing (`type 0`, other binary types and fragmented frames dropped).
  Push-to-talk only: the capsule (or Confirm) sends `listen start
  {mode:"manual"}`, 60 ms / 960-sample frames from the mic are Opus-encoded at
  16 kHz mono, and server audio is decoded to the BH1098's fixed 16 kHz. No
  wake word, AEC, AGC, MCP or IoT surface.
  - Dependencies (managed, not vendored): `78/esp-opus-encoder ~2.4.1` (pulls
    `78/esp-opus 1.0.5`), `espressif/esp_websocket_client ^1.8.0`,
    `espressif/cjson ^1.7.19`. `78/esp-websocket` is **yanked and targets
    IDF ^5.3**, so it cannot be used on 6.1; the 60-line wrapper's behaviour is
    implemented directly on `esp_websocket_client`.
  - Config: `/.crosspoint/voice.json` `{url, token, version}`; the token is
    obfuscated with the eFuse MAC and never logged. A self-hosted xiaozhi
    server URL/token is enough; no vendor activation.
  - TLS: CA bundle first, then an explicit retry without verification (logged),
    matching the port's `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`.
  - Cost: image 6,199,600 B (about 485 KB spare); peak internal RAM ~70–100 KB,
    so the page frees render memory before connecting.
  - Still needs: a real xiaozhi server and a headset/mic test; libopus
    fixed-point is the flash/RAM unknown; wire format is vendor-source exact,
    not live-traffic verified.
- **WeChat mini-program is not portable** without the vendor cloud: every
  endpoint/WSS host and the device token are blank/cloud-issued.
