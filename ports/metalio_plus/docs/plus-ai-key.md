# Physical AI key and the AI-voice memory budget

Status: implemented and host/build-verified (2026-11-16); the physical key pass
and a real xiaozhi server pass still need a device with a fingertip on GPIO61.

## What the key is

The vendor board file calls GPIO61 `BOOT_BUTTON_GPIO`, but kiikoread's
`metalio_hw` documents the same pin as the **official AI/BOOT input**: active
low, 25 ms debounce, 500 ms short/long split, and "release after a hold never
produces a tap". The vendor/kiikoread UI uses it as a configurable action key
(refresh / bookmark / frontlight / home); CPYOS binds it to its own AI-voice
page because that is the feature the key is named after.

The key is not in `HalGPIO`'s button mask, so it cannot collide with page
turns or the power key. `main/plus_ai_key.cpp` samples it from the UI loop:

```
short press  -> open Tools -> AI Voice
long press   -> open AI Voice and start listening once the channel is up
```

On the AI-voice page the same key is the hands-free talk toggle: a press while
idle starts a turn, a press while listening stops it. A press during the
connect window cancels the queued turn but leaves the channel coming up.

### Pages that ignore the key

`Boot`, `Sleep`, `Standby`, `WifiSelection`, `KeyboardEntry`, `OtaUpdate`,
`UsbDrive`, `PlusTransfer`, `PlusOta`, `PlusCamera`, `PowerOff`, and any moment
with an activity switch already queued. Without this list a press while typing
a Wi-Fi password would replace the keyboard with the voice page and lose the
modal state.

### Why long-press auto-listens instead of hold-to-talk

The xiaozhi channel needs a TCP/TLS/WebSocket handshake before audio can flow.
On this link the first connect is 2–10 s. Holding a physical key for that long
to say one sentence is poor UX and, on an e-ink device, easy to misread as a
stuck key. The long press therefore opens the page, shows `连接中`, and starts
capturing as soon as `Phase::Connected` arrives; the capsule and subheader say
`聆听中` while the turn is live. The second press ends it.

## Memory budget (why the OOM message should not come back)

The 2026-11-15 build reported `内存不足` when the page was opened with Wi-Fi
resident. The cause was the voice worker asking for a **16 KiB contiguous
internal** stack while TLS, the websocket client, libopus and the Wi-Fi driver
were already in that pool (observed internal free ~35–50 KB, largest block
~27 KB).

| consumer | where | size | note |
|---|---|---:|---|
| voice worker task | **PSRAM** | 16K words = **64 KiB** stack | started/removed with the caps variants; only sockets + I2S calls run there, never internal flash |
| opus encoder + decoder | internal heap | runtime, small | libopus state, freed when the worker exits |
| audio slots | **PSRAM** | 8 × 1,026 B ≈ 8 KiB | queue between capture and the sender |
| microphone frame | internal `.bss` | 3,840 B | `g_micStereo` (stereo 60 ms @16 kHz) |
| TLS buffers | **PSRAM** | varies | `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` |
| websocket/lwIP | internal | a few KB | one client |

The worker is refused before it starts when the largest internal block is below
**16 KiB**; the page then shows `内存不足，请稍后重试` (`STR_BT_LOW_MEMORY`, whose
wording is now generic) instead of aborting inside a TLS allocation. This is the
same failure-closed rule as `PsramAllocator`: a state with no correct silent
answer must not be silent.

`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` stays **off** (the S31 Wi-Fi driver OOMs
when its buffers go to PSRAM), so Wi-Fi still owns internal RAM; the point of
the change is that voice no longer competes for a second stack.

## Bluetooth audio, 4G and the page-turner

The same budget reasoning applies to the other two radios:

- **BT audio** (BH1098) worker also uses a 16 KiB **PSRAM** stack (see
  `plus_bt_audio_activity.cpp`); it talks UART/I2S only.
- **BLE page-turner** uses NimBLE with kiikoread's single-central profile:
  `MAX_CCCDS=2`, `ATT_PREFERRED_MTU=23`, `MSYS_1/2=6`, `ACL_BUF_COUNT=6`,
  `ATT_MAX_PREP_ENTRIES=4`, adv-report flow control 50, and
  `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`. The defaults (8 CCCDs, 256-byte MTU,
  24 MSYS/ACL buffers) cost several KB of internal RAM for no benefit on this
  device. Bond keys still go to NVS (`NIMBLE_NVS_PERSIST`), so a reboot keeps
  the pairing.
- **4G** (`PlusCellular`) keeps its small `cell-recover` task (8 KiB internal,
  only after a rail cycle) and owns no full-time stack; TLS is still mbedTLS
  with external memory.

No setting makes Wi-Fi + 4G + BT audio + AI voice run at once *comfortably*:
the SoC has one radio front end, and `plus_audio`/cellular share the P14 rail.
The supported combinations are: Wi-Fi or 4G for network, plus BLE as the input
device; BT **audio** parks the modem for its rail reset (vendor sequence) and
re-initialises it afterwards. That is a hardware/rail policy, not an OOM.

## Verification

Bench (no finger needed):

```
aikey short        # opens PlusVoice
aikey long         # opens PlusVoice with auto-listen queued
```

Expected log lines:

```
VOICEUI: enter: url=1 token=1 version=1 auto=0|1
plusVoice: worker up: stack in PSRAM, internal largest=<bytes>
VOICEUI: phase 0 -> 1 (err 0)          # Connecting
VOICEUI: phase 1 -> 2 (err 0)          # Connected; auto-listen starts here
VOICEUI: phase 2 -> 0 (err 0)          # Back/stop
plusVoice: worker exited; stack free=<bytes>
```

With Wi-Fi connected and `btaudio` open the BT page must log
`BTAUD job ... started` and `inquiry started`, never
`worker task creation failed`.

Measured on 2026-11-16 with the saved AP associated (`internal largest=27,648`):

```
voiceprobe: start=1 phase=1 err=0 largest=27648
[VOICE] worker up: stack in PSRAM, internal largest=27648
[VOICE] worker exited; stack free=13752        # ~52 KiB used of 64 KiB
voiceprobe: stopped phase=3                    # refused local port, expected
```

`voiceprobe` starts the real worker against `ws://127.0.0.1:9/` and stops it;
it needs no xiaozhi server. The important part is that the 64 KiB stack no
longer comes out of the 27,648-byte internal block; the measured high-water is
why it must not be shrunk to the advertised 16 KiB.

★ **It needs an active link.** Releasing the radio (`NetworkStartup::releaseRadio`)
tears lwIP down, so the first socket call afterwards aborts inside
`tcpip_send_msg_wait_sem` (`assert: Invalid mbox`) and reboots the device. This
was reproduced live on 2026-11-16 by running `voiceprobe` with Wi-Fi off. The
worker now checks `WiFi.status()` **before** the audio rail and the WebSocket
task and ends in `Failed` with `err=1005` (`kErrorNoNetwork`); the page maps
that to `STR_NO_NETWORKS` instead of a generic connection failure, and
`voiceprobe` refuses with `voiceprobe: no network; connect Wi-Fi or 4G first`.
`WiFi.status()` already reports the 4G link when the network type is cellular,
so the same guard covers both transports.

Physical pass:

1. Short-press the AI key on Home -> AI Voice opens, idle.
2. Long-press it -> page opens, status goes `连接中` -> `聆听中`; speak; press
   again to stop and wait for the reply.
3. On a modal page (Wi-Fi password keyboard), the key does nothing.
4. On the AI page, the same key toggles the turn; Back leaves and releases a
   radio this page brought up. **Back returns to the page the key was pressed
   on** (Home, reader, settings, ...); only the tools-drawer entry replaces My,
   so its Back still lands on the app grid. When the page underneath is the
   reader, `onExit` reloads the SD reader face (`sdFontSystem.ensureLoaded`):
   `beginConnect()` hands render memory back before the TLS session and a pop
   does not re-run the reader's `onEnter`, so without this the book would come
   back in the 8 pt fallback.
