# USB-less power and wake pass (operator steps + automatic readback)

USB power hides two things this board must prove without a cable: a real
light-sleep wake and the battery discharge curve. The firmware now writes both
to the card so the results survive a cable replug (a CDC open resets the SoC,
so a console-only log would be lost).

## What the firmware records

`/.crosspoint/power.log` (append-only, one line per event):

```
boot reset=<esp_reset_reason> cause=<wakeup cause> ms=<uptime>
wake reason=<0 timer|1 power|2 failed> ms=... mv=... ma=... soc=... chg=... usb=0
sample ms=... mv=... ma=... soc=... chg=... usb=0 wifi=0|1
```

- `wake` is one line per `HalPowerManager::lightSleepFor()` attempt that left
  the CPU (including a refused park, `reason=2`).
- `battlog <minutes>` starts one `sample` per minute; `battlog 0` stops it.
  Sampling lives on the UI loop, so a sleeping device naturally writes nothing
  until it wakes. The remaining minutes are kept in `/.crosspoint/battlog` and
  resumed after a reboot or a cable replug, so a power cycle in the middle of a
  cable-out run no longer loses the battery curve (verified 2026-11-16:
  `battlog 2` -> reboot -> `battlog: resumed 2 min`).
- `boot` delimits the session: replugging USB resets the chip and produces a
  new line.

`boot.log` (the ESP log mirror) still holds the detailed driver/park lines.

## Confirmed result (2026-11-16)

The second cable-out run (48.7 minutes on battery) recorded
`previous-wake reason=power stage=14`: light sleep parked and restored cleanly
and the power key woke it, with no `panic` in the window. The badge render was
then captured over USB with `CMD:SLEEPBADGE 1`
(`artifacts/plus_light_sleep_badge_20261116.png`): the 浅睡 box draws at the
bottom-right of the standby face. The earlier run had shown the same frame
after a power-key wake; it had no samples because that run rebooted before the
in-RAM `battlog` deadline was started.

The first clean cable-out run produced:

```
usb=0 ms=94455
sample ms=117854 mv=4355 ma=-75 soc=100 chg=0 usb=0 wifi=0
previous-wake reason=1 stage=14
```

`reason=1` is the power key, `stage=14` means the park/sleep/restore returned
normally. No `panic stage` appeared in this run; the `panic stage=14` seen
during `slptest` exists only when USB is attached and the CDC is parked, which
the product path never does.

The clock disappearing on the light face is expected (`lightSleepStyle=0`,
`LIGHT_SLEEP_DUCK_STATIC`, `showClock=false`). The **浅睡 badge** is a setting
(`sleepStateBadge`, default ON); this unit had it persisted as OFF, so no badge
was drawn. `CMD:badge 1` sets and saves it; a normal fresh settings file starts
with it ON. `CMD:SLEEPBADGE 1` paints the badge on the standby face without
parking the CPU (screenshot it over USB), `0` clears it.

## Before unplugging

With USB attached:

```
batt                 # note mV / mA / SOC and charging=1
battlog 240          # four hours of one-minute samples
```

Make sure the sleep timeout is **not** `Never` if you want the timeout path
(Settings -> System -> sleep). For the power-key path, leave a book open or stay
on Home.

## Steps

1. Unplug USB. The console disappears on purpose (`dev` note: no CDC).
2. Leave the unit idle. With the saved timeout it enters Standby, then light
   sleep after ~35 s (the Standby overlay keeps the panel image). When the park
   really happened, the **浅睡 badge** appears at the bottom-right of that face
   (`sleepStateBadge` defaults ON; `slpdiag` shows `badge=1`).
3. 1–2 minutes later, short-press the power key. The clock page must redraw and
   the device must stay awake; a second short press returns to the previous
   page. Do **not** long-press (3 s powers the PMU off).
4. Optional reader path: open a book, press power (Standby), wait, press power
   again; the reader must return with the same page.
5. Optional discharge: leave it asleep for 30–60 minutes. The BQ27220 `ma`
   column and the `soc` slope give the average current; the first sample after
   wake is the post-sleep reading.
6. Optional AI key (no cable needed): on Home, short-press the AI key — the
   voice page opens; Back must return to Home (the key pushes the page on top
   of whatever was showing: Home, a book, or Settings). Long-press it — the
   page opens and starts the turn once the channel is up, if `voice.json` has a
   server URL. Without Wi-Fi/4G the page now shows 未找到网络 and stays alive;
   it must never reboot (`voiceprobe` with the radio off prints
   `no network; connect Wi-Fi or 4G first`).
7. Optional USB MSC: this is a separate path. From the transfer page choose
   USB drive on the Mac, copy a file, eject the volume, and confirm the unit
   reboots to Home. If the Mac eject leaves the unit silent, unplug/replug USB
   (there is no reset button).

## Replug and automatic readback

Plug USB back in, then run (one port open, no manual typing):

```
~/.espressif/python_env/idf6.1_py3.12_env/bin/python exp/plus_session.py \
    /tmp/plus_power.log "c:cat /.crosspoint/power.log 20000" "w:2" \
    "c:batt" "w:2" "c:cat /boot.log 4000" --boot-wait 20
```

Then summarise the saved file (the parser ignores the `CAT_START`/`CAT_END`
frame lines and is unit-tested by the host gate):

```
python3 ports/metalio_plus/scripts/analyze_power_log.py /tmp/plus_power.log \
    --last-session --strict --require-battery
```

Replugging appends the final `boot` line, so `--last-session` selects exactly
the cable-out window; use `--since <bytes>` instead when several boots landed
inside the window. It prints boots by reset reason, every wake attempt, USB
transitions, panics, and the battery curve (mean mA, mAh, mV/SOC deltas);
`--strict` exits 1 on a panic or a refused park; `--require-battery` needs at
least one sample and rejects any `usb=1` sample (a short cable-out run still
passes, it just has no current curve). The checks are:

- at least one `wake reason=power` or `wake reason=timer` after the `boot` line,
  and that the `sample` timestamps resume after it;
- `reason=2` lines mean the park was refused: the following `boot`/serial log
  carries `light sleep refused at <code> (err=...)`, which names the step
  (SD park, TPS, expander, INT line, rail cut, ...);
- the `mv`/`ma`/`soc` series is monotonic enough to compute mA·h over the
  interval; `usb=0` must hold for every sample in the session;
- `wake.log`-style lines are not expected: this port persists to `power.log`.

If the unit does not wake on a short press, note the LED/panel state before
replugging; the next `boot` line's `reset` value tells a watchdog/panic reset
from a PMU power-off.

## Known limits

- The S31 build has no DFS: CPU stays at 320 MHz, so the measured current is the
  rail + panel + radio, not a downclocked figure.
- Light sleep cuts the shared P5/P14 rails; any child activity that still owns
  an I2C/SPI/SD transaction refuses the park (`reason=2`) rather than half
  parking the board.
- A BLE page-turner connection is not a wake source; it is dropped when the
  rails are cut and must reconnect after wake.
