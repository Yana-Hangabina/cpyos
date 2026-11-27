#include "CrashActivity.h"

#include <FaultLog.h>
#include <GfxRenderer.h>
#include <HalSystem.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>

#include "BootTimings.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Greedy wrap into fixed lines, measured through the renderer so the result
// matches what drawText() will do. Allocation-free on purpose: this runs on the
// crash screen, where the heap is the thing that just failed.
uint8_t wrapFixed(GfxRenderer& renderer, int fontId, const char* text, int maxWidth, char* out,
                  uint8_t maxLines) {
  uint8_t count = 0;
  if (text == nullptr || maxWidth <= 0) return 0;
  while (*text != '\0' && count < maxLines) {
    char line[CrashActivity::kLineChars];
    size_t used = 0;
    while (*text != '\0') {
      if (*text == '\n') {
        ++text;
        break;
      }
      unsigned char lead = static_cast<unsigned char>(*text);
      size_t sequence = 1;
      if ((lead & 0xE0) == 0xC0) {
        sequence = 2;
      } else if ((lead & 0xF0) == 0xE0) {
        sequence = 3;
      } else if ((lead & 0xF8) == 0xF0) {
        sequence = 4;
      }
      if (used + sequence >= CrashActivity::kLineChars) break;
      if (*text == ' ' && used == 0) {  // no leading space on a continuation line
        ++text;
        continue;
      }
      memcpy(line + used, text, sequence);
      const size_t candidate = used + sequence;
      line[candidate] = '\0';
      if (renderer.getTextWidth(fontId, line) > maxWidth && used > 0) {
        line[used] = '\0';
        break;  // this codepoint starts the next line
      }
      used = candidate;
      text += sequence;
    }
    line[used] = '\0';
    memcpy(out + static_cast<size_t>(count) * CrashActivity::kLineChars, line, used + 1);
    ++count;
    while (*text == ' ') ++text;
  }
  return count;
}

}  // namespace

void CrashActivity::onEnter() {
  Activity::onEnter();

  char reason[kLineChars * 4];
  reason[0] = '\0';
  if (HalSystem::getPanicMessage(reason, sizeof(reason)) == 0) {
    const char* fallback = tr(STR_CRASH_NO_REASON);
    snprintf(reason, sizeof(reason), "%s", fallback != nullptr ? fallback : "");
  }

  // docs/07 D2: when the boot that panicked never reached its first frame, the
  // record it left behind names the stage it died in - the one fact the panic
  // reason and the stack dump cannot give.
  char trail[48];
  trail[0] = '\0';
  const int trailLength = boottimings::incompletePrevious(trail, sizeof(trail));

  buildContextLines();

  // Wrapping needs the renderer metrics, which live on this thread.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth() - 2 * metrics.contentSidePadding;
  descriptionCount_ = wrapFixed(renderer, UI_10_FONT_ID, tr(STR_CRASH_DESCRIPTION), width, &description_[0][0], 4);
  reasonCount_ = wrapFixed(renderer, UI_10_FONT_ID, reason, width, &reason_[0][0], 5);
  if (trailLength > 0) {
    char message[64];
    snprintf(message, sizeof(message), "%s %s", tr(STR_CRASH_BOOT_STAGE), trail);
    trailCount_ = wrapFixed(renderer, UI_10_FONT_ID, message, width, &trail_[0][0], 3);
  }

  requestUpdateAndWait();
}

// Everything here is what a photo can carry back to us. A field report of 0.8.2
// was two screens reading only "assert failed: spi_flash_disable_interrupts..."
// and "abort() was called at PC 0x423569f3" -- the second of which is
// __terminate and therefore names no exception at all. The lines below name the
// throwing task, the screen and lifecycle step that were live, the exception
// type, decodable return addresses, and the memory the crash happened in.
//
// 0.8.4 added the two numbers that report lacked: the size of the allocation
// that failed (`new=...`) and the heap when it did (`heap=...`), both written by
// the new_handler / operator-new wrapper. Without them `PNC-04` is a class, not
// a site.
void CrashActivity::buildContextLines() {
  const HalSystem::CrashContext ctx = HalSystem::getCrashContext();
  char buffer[kLineChars];
  contextCount_ = 0;

  snprintf(buffer, sizeof(buffer), "ver=%s", CROSSPOINT_VERSION);
  memcpy(context_[contextCount_++], buffer, sizeof(buffer));

  // Fault codes lead the block, right after the version: they are the part a
  // reporter can read aloud, the part that survives a blurry photo, and the part
  // that says what was already going wrong *before* this death. Deliberately not
  // passed through tr() -- the codes are language-neutral by design, and the
  // embedded English blob has no room for sentences about them (AGENTS 67.12).
  {
    char codes[64];
    const uint8_t shown = fault::formatLine(codes, sizeof(codes) - 1, fault::kCapacity, nullptr);
    if (shown > 0) {
      fault::Slot all[fault::kCapacity];
      const uint8_t distinct = fault::copy(all, fault::kCapacity);
      snprintf(buffer, sizeof(buffer), distinct > shown ? "fault=%s +%u" : "fault=%s", codes,
               static_cast<unsigned>(distinct - shown));
      memcpy(context_[contextCount_++], buffer, sizeof(buffer));
    }
    if (const char* where = fault::assertLocation(); where[0] != '\0') {
      snprintf(buffer, sizeof(buffer), "at=%s", where);
      memcpy(context_[contextCount_++], buffer, sizeof(buffer));
    }
    // The allocation that failed and the heap at that moment. Only present when
    // the death came through operator new, which is what PNC-04 means.
    const uint32_t failedBytes = fault::detailOf(fault::Code::PanicBadAlloc);
    const uint32_t heapPacked = fault::detailOf(fault::Code::HeapAtAllocFailure);
    if (failedBytes != 0 || heapPacked != 0) {
      snprintf(buffer, sizeof(buffer), "new=%uB heap=%uK/%uK", static_cast<unsigned>(failedBytes),
               static_cast<unsigned>(heapPacked & 0xFFFFu), static_cast<unsigned>(heapPacked >> 16));
      memcpy(context_[contextCount_++], buffer, sizeof(buffer));
    }
  }

  if (ctx.hasPanicRecord && ctx.task[0] != '\0') {
    snprintf(buffer, sizeof(buffer), "task=%s core=%d", ctx.task, ctx.core);
    memcpy(context_[contextCount_++], buffer, sizeof(buffer));
  }
  if (ctx.activity[0] != '\0') {
    snprintf(buffer, sizeof(buffer), "screen=%s stage=%s", ctx.activity, ctx.stage);
    memcpy(context_[contextCount_++], buffer, sizeof(buffer));
  }
  if (ctx.exceptionName[0] != '\0') {
    // Mangled libstdc++ name, deliberately not demangled: __cxa_demangle needs a
    // heap allocation, and this screen exists to report failures of the heap.
    // "St17bad_function_call" / "St11length_error" / "operator new failed" are
    // readable enough to act on and unambiguous for us.
    snprintf(buffer, sizeof(buffer), "throw=%s", ctx.exceptionName);
    memcpy(context_[contextCount_++], buffer, sizeof(buffer));
  }
  if (ctx.hasPanicRecord && (ctx.pc != 0 || ctx.lr != 0 || ctx.sp != 0)) {
    snprintf(buffer, sizeof(buffer), "pc=%08X lr=%08X sp=%08X", static_cast<unsigned>(ctx.pc),
             static_cast<unsigned>(ctx.lr), static_cast<unsigned>(ctx.sp));
    memcpy(context_[contextCount_++], buffer, sizeof(buffer));
  }
  // Repeat detection belongs on the screen, not only the serial log: the user
  // photographs whichever line they can see, and "same crash again" is the one
  // question that changes how the report is triaged.
  {
    const HalSystem::CrashHistory history = HalSystem::getCrashHistory();
    if (history.count > 0 && contextCount_ < kMaxLines) {
      snprintf(buffer, sizeof(buffer), "crash#%u %s", static_cast<unsigned>(history.streak),
               history.repeats() ? "(same as last)" : "(new address)");
      memcpy(context_[contextCount_++], buffer, sizeof(buffer));
    }
  }
  for (uint8_t start = 0; start < ctx.frameCount && contextCount_ < kMaxLines; start += 4) {
    int used = snprintf(buffer, sizeof(buffer), "ret=");
    for (uint8_t i = start; i < ctx.frameCount && i < start + 4; ++i) {
      snprintf(buffer + used, sizeof(buffer) - static_cast<size_t>(used), " %08X",
               static_cast<unsigned>(ctx.frames[i]));
      used += 9;
    }
    memcpy(context_[contextCount_++], buffer, sizeof(buffer));
  }
  if ((ctx.heapFree != 0 || ctx.psramFree != 0) && contextCount_ < kMaxLines) {
    snprintf(buffer, sizeof(buffer), "mem=%u/%u psram=%u up=%us", static_cast<unsigned>(ctx.heapFree),
             static_cast<unsigned>(ctx.heapLargest), static_cast<unsigned>(ctx.psramFree),
             static_cast<unsigned>(ctx.uptimeMs / 1000));
    memcpy(context_[contextCount_++], buffer, sizeof(buffer));
  }
}

void CrashActivity::loop() {
  int x = 0;
  int y = 0;
  const bool back = mappedInput.wasPressed(MappedInputManager::Button::Back);
  const bool forward = mappedInput.wasScreenTapped(x, y) || mappedInput.wasPressed(MappedInputManager::Button::Confirm);
  if (!back && !forward) return;
  // Back stays the way out on every page -- it is the gesture a reporter already
  // knows and the one the footer names. A tap or Confirm pages forward while the
  // "+N" marker says more exists, and exits once nothing is left to show.
  if (!back && contextMore) {
    ++contextPage;
    requestUpdate();
    return;
  }
  finish();
}

void CrashActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto x = metrics.contentSidePadding;
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  // Crash report is a dead end, not a pushed screen: no back button.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CRASH_TITLE), nullptr,
                 false);

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  for (uint8_t i = 0; i < descriptionCount_; ++i) {
    renderer.drawText(UI_10_FONT_ID, x, y, description_[i]);
    y += lineHeight;
  }

  y += metrics.verticalSpacing * 2;
  renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_CRASH_REASON));
  y += lineHeight + metrics.verticalSpacing;

  for (uint8_t i = 0; i < reasonCount_; ++i) {
    renderer.drawText(UI_10_FONT_ID, x, y, reason_[i]);
    y += lineHeight;
  }

  // Crash context: what the panic could not say about itself. Drawn in the
  // small font so a long mangled exception name and eight return addresses fit
  // on one screen; the values are only useful to us, but their *presence* is
  // what tells a reporter that the photo is enough.
  if (contextCount_ > 0) {
    y += metrics.verticalSpacing;
    const int smallLineHeight = renderer.getLineHeight(SMALL_FONT_ID) + 2;
    const int capacity = (renderer.getScreenHeight() - 40 - y) / smallLineHeight;
    contextMore = false;
    if (capacity > 0) {
      // Paginated, not cut off. `contextPage` is written here and read in loop():
      // the render task and the UI task can disagree about it for one repaint,
      // which costs a repeated page at worst -- the value is never used to decide
      // anything about memory.
      const size_t perPage = static_cast<size_t>(capacity);
      const size_t pages = (static_cast<size_t>(contextCount_) + perPage - 1) / perPage;
      if (static_cast<size_t>(contextPage) >= pages) contextPage = 0;
      size_t index = static_cast<size_t>(contextPage) * perPage;
      for (; index < contextCount_ && index < static_cast<size_t>(contextPage) * perPage + perPage; ++index) {
        renderer.drawText(SMALL_FONT_ID, x, y, context_[index]);
        y += smallLineHeight;
      }
      if (index < contextCount_) {
        char more[8];
        // "+N" rather than a sentence: it must be readable on any language's
        // device and it must not be the reason this render allocates.
        snprintf(more, sizeof(more), "+%u", static_cast<unsigned>(contextCount_ - index));
        renderer.drawText(SMALL_FONT_ID, x, y, more);
        contextMore = true;
      }
    }
  }

  if (trailCount_ > 0) {
    y += metrics.verticalSpacing;
    for (uint8_t i = 0; i < trailCount_; ++i) {
      renderer.drawText(UI_10_FONT_ID, x, y, trail_[i], true, EpdFontFamily::BOLD);
      y += lineHeight;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}