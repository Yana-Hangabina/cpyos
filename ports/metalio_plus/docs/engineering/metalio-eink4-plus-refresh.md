# Metalio E-INK4 Plus — refresh / waveform analysis (2026-11-11)

Reference for this document:

* `kiikoread` rc110 (`boards/metalio47/src/main/display.c`, ~1439 lines) — the
  most complete driving policy for this exact panel family.
* The vendor application shipped in `repos/metalio-e-ink4-plus` (formerly
  `.scratch/refs/metalio-vendor`, now a compatibility symlink; author of the
  waveform suite we build against).
* Our port: `ports/metalio_plus/components/cpyos_ui/HalDisplay.cpp`.

## 1. What each side does

| Aspect | kiikoread rc110 | Our port before 2026-11-11 | Our port now |
|---|---|---|---|
| Waveform profiles | FULL GC (48 phases), normal GL, DU (20), trimmed GC/GL (36/37), gray8 GL (30), fast DU (14), short DU (8), wipe/reveal | `MY_WAVEFORM`: DU 20, GC16 48, GL16 48 | same tables; GC16/GL16 only |
| Mode selection | per scene: monochrome/UI → **DU**, normal/photo → **GL16**, periodic/manual → **GC16 full** | FULL → GC16, everything else → GL16, every push a **full-screen** scan | same baseline (area/DU/equalize experiments reverted, see §2) |
| 16-level (AA text, images) | GL16 gray for normal frames, periodic GC16 clean | **GC16 on every gray commit** (flash every page) | first → `from_white` GC16, explicit clean → GC16, ordinary → full-screen GL16 |
| Partial updates | `epd_hl_update_area` + dirty masks; `hl_update_area(force_full)` for the white-floor scenes | `displayWindow` degraded to a full-frame push; no per-frame crop | `displayWindow` = rect blit + `epd_hl_update_area` (GL16, data-masked); push path keeps the baseline full-screen differential |
| Why full-pixel | "GL16 must be full-pixel (the extra white frame depends on that)" — unchanged pixels still need the LUT or the gray floor stays | unchanged pixels were skipped | noted for future work; the baseline path stays differential |
| Ghost management | cleanup debt per region, white-phase substitution, switch-erase waveforms; GC16 every 24 page transitions (60 s floor) | shared periodic FULL only | unchanged: shared periodic FULL cadence |

## 2. Frontlight slider: experiments and current decision

`FrontlightPanelActivity::render()` calls `displayBuffer(FAST_REFRESH, TransientUi)` for
**every drag frame**, then one `Normal` frame on release. Five variants were driven on
the glass (logs show the real boxes, e.g. `area transient 75,44 92x382 ... 320ms`):

| Variant | Result on the glass |
|---|---|
| Baseline: differential full-screen GL16 (vendor `epd_hl_update_screen`) | current state; residue accumulates, bounded by the periodic FULL cadence |
| Area-full GL16 of the changed box (`epd_hl_update_area_full`) | knob dots gone, but the filled bar drifts in tone (GL16 black != GC16 black) and each 48-phase step visibly shimmers |
| + burst-union GC16 settle on the first Normal frame | one extra flash at release, tone still drifts while dragging |
| Fast DU (vendor 14-frame profile) + differential area | knob dots and bar trails come back **during** the drag: this panel's DU white->black segment traps charge (the reference notes the same: "continuous DU dirties the white") |
| Area-scoped equalize-black + GL16 (`epd_hl_update_area_equalize_black`) | still leaves residue and is slower/flashier (48 phases per step) |

**Decision (2026-11-12, per the user): revert to the baseline policy** — `FULL` is
a whole-panel non-differential GC16, every other push is the vendor differential
GL16, `displayWindow` is a rect blit + differential area GL16. The two added
primitives (`epd_hl_update_area_full`, `epd_hl_update_area_equalize_black`) are
kept in the vendored highlevel.c as reference, **unused**.

A clean slider needs a design change rather than another waveform guess: either the
reference's derived FOLLOW waveform (needs an optical bench), or drawing the sheet
so a drag does not re-drive large solid-black areas. Deferred.

## 3. Measured on hardware (2026-11-11)

* Reader page turn, 16-level text, before: **GC16 every page** (full black/white
  flash), ~870 ms.
* After the gray change: **GL16**, ~868 ms on the first measurement — the
  waveform phase count is the same (48), so the win is the *absence of the
  flash*, not time.
* Framebuffer conversion, measured indirectly: the per-pixel loop is ~300 ms of
  the 868 ms. The byte-wise version replaces it with row copies; the expected
  page time is ~560 ms once verified on the unit.
* DU step (20 phases) ≈ 0.42× the phases of a GL16 step; the frontlight drag
  should drop from ~800 ms to ~350 ms per step before the blit saving.

## 4. Open items (in order)

1. **Slider ghost on glass** — the policy above (full-pixel area drive + burst-union
   GC16 settle) is the reference's answer; it needs one finger test on the glass to
   confirm. If a trail remains, the next levers are, in order: (a) trimmed GL/GC
   profiles (36/37 phases, `my_waveform_fast()` already built), (b) the reference's
   scene waveforms, (c) the DU+settle split for *dynamic* frames (DU is measured at
   ~0.42× the phases, but a generic DU table ghosted this panel before).
2. **Enable band diff** (`CROSSPOINT_PORT_NO_BAND_DIFF`) once `displayWindow` is
   proven. It is what makes keystrokes and overlays push only a band.
   (2026-11-12: transient frames already use the 20-phase DU table — keystroke
   push 735 -> 399 ms; see BRINGUP. Band diff would layer on top of that.)
3. **Trimmed profiles**: `my_waveform_fast()` (36/37 phases) is built into
   `MY_WAVEFORM` and unused; adopting it cuts ~25 % of panel time on UI frames.
4. **Scene waveforms** (bw / gray / exit) as in the reference, if the two-mode
   policy is not enough for photo pages.
5. **Gate-lock safety**: `epd_hl_update_area_full()` still runs the full 693-line
   scan; the area only masks data. Keep an eye out for horizontal striping after
   many area updates and, if it appears, disable the area path with a flag rather
   than tuning waveforms.

## 5. Provenance

Refresh policy and the forced area drive are derived from **kiikoread rc110**
(`boards/metalio47/src/main/display.c`, Apache-2.0) and from the vendored epdiy's
`highlevel.c` as extended by kiikoread (LGPL-3.0-or-later). See
`ports/metalio_plus/NOTICE-kiiKOREAD.md`. No file marked
`LicenseRef-Kiikoread-Personal-Study-1.0` was copied; `scripts/tests/test_license_scope.py`
enforces that.
