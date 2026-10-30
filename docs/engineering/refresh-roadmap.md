# Refresh / power roadmap (future work that needs instruments or photo gates)

Status 2026-10-30: the software-level levers are exhausted. Measured results and the
next steps that require a scope or a repeatable photo A/B:

| Item | Status | Result |
|---|---|---|
| Band / windowed pushes | shipped | fewer driven pixels, no flash; **scan time unchanged** |
| Local clean (rect GC16) | default on | residue equals a whole-screen clean (photos) |
| DU for fast page turns | shipped (fast turn only) | scan 360 -> 150 ms/page (GL16 37 phases -> DU 2) |
| Line-period setter (vendor FAST profile) | tried, ineffective | `epd_lcd_set_line_timing` does not change the frame cadence here (283 ms/page at 16 us and 11 us) |
| Baseline writeback (copy ~99 ms) | tried | overlaps the scan; word-wise copy and pointer swap both gave no net win; reverted |
| Idle rail off | shipped | rails drop after 8 s without a push |
| CPU 240 -> 160 MHz | analysed | scans need 240; 160 buys 17 lines of margin for <0.01 mAh per scan |

Next (needs a scope / photo gate, highest value first):

1. **Scan generation frame pacing.** The frame period is fixed at `epd_lcd_init` from the
   board config; reaching the vendor FAST line profile (~7 us/line) requires changing
   initialisation/scan generation and a per-profile photo comparison (black level,
   ghosting). Risk: every waveform phase changes.
2. **DU phase table.** DU still uses 2 phases of the 36-phase table (150 ms). A trimmed
   DU table would shorten the scan further; black level and residue need photos.
3. **Text waveform with per-pixel optical-state compensation.** The only route to
   removing the periodic clean entirely; needs a measurement method.
4. **Pixel clock.** The vendor FAST profile also raises pclk; this port runs 18 MHz.
   Re-solving blanking/CKV risks latching (see the area-scan history).

Entry conditions: a scope or a repeatable photo A/B; one variable per change; a
compile-time or runtime fallback with a known baseline photo.
