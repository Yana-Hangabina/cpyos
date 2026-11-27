#include "CrashActivity.h"

#include <FaultLog.h>
#include <GfxRenderer.h>
#include <HalSystem.h>
#include <I18n.h>

#include <cstdio>

#include "BootTimings.h"
#include "components/UITheme.h"
#include "fontIds.h"

void CrashActivity::onEnter() {
  Activity::onEnter();

  panicMessage = HalSystem::getPanicInfo(false);
  if (panicMessage.empty()) {
    panicMessage = tr(STR_CRASH_NO_REASON);
  }

  // docs/07 D2: when the boot that panicked never reached its first frame, the
  // record it left behind names the stage it died in - the one fact the panic
  // reason and the stack dump cannot give.
  char trail[48];
  if (boottimings::incompletePrevious(trail, sizeof(trail)) > 0) {
    bootTrailMessage = std::string(tr(STR_CRASH_BOOT_STAGE)) + " " + trail;
  }

  buildContextLines();

  requestUpdateAndWait();
}

// Everything here is what a photo can carry back to us. A field report of 0.8.2
// was two screens reading only "assert failed: spi_flash_disable_interrupts..."
// and "abort() was called at PC 0x423569f3" -- the second of which is
// __terminate and therefore names no exception at all. The lines below name the
// throwing task, the screen and lifecycle step that were live, the exception
// type, decodable return addresses, and the memory the crash happened in.
void CrashActivity::buildContextLines() {
  const HalSystem::CrashContext ctx = HalSystem::getCrashContext();
  char buffer[96];

  snprintf(buffer, sizeof(buffer), "ver=%s", CROSSPOINT_VERSION);
  contextLines.emplace_back(buffer);

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
      contextLines.emplace_back(buffer);
    }
    if (const char* where = fault::assertLocation(); where[0] != '\0') {
      snprintf(buffer, sizeof(buffer), "at=%s", where);
      contextLines.emplace_back(buffer);
    }
  }

  if (ctx.hasPanicRecord && ctx.task[0] != '\0') {
    snprintf(buffer, sizeof(buffer), "task=%s core=%d", ctx.task, ctx.core);
    contextLines.emplace_back(buffer);
  }
  if (ctx.activity[0] != '\0') {
    snprintf(buffer, sizeof(buffer), "screen=%s stage=%s", ctx.activity, ctx.stage);
    contextLines.emplace_back(buffer);
  }
  if (ctx.exceptionName[0] != '\0') {
    // Mangled libstdc++ name, deliberately not demangled: __cxa_demangle needs a
    // heap allocation, and this screen exists to report failures of the heap.
    // "St17bad_function_call" / "St11length_error" / "operator new failed" are
    // readable enough to act on and unambiguous for us.
    snprintf(buffer, sizeof(buffer), "throw=%s", ctx.exceptionName);
    contextLines.emplace_back(buffer);
  }
  if (ctx.hasPanicRecord && (ctx.pc != 0 || ctx.lr != 0 || ctx.sp != 0)) {
    snprintf(buffer, sizeof(buffer), "pc=%08X lr=%08X sp=%08X", static_cast<unsigned>(ctx.pc),
             static_cast<unsigned>(ctx.lr), static_cast<unsigned>(ctx.sp));
    contextLines.emplace_back(buffer);
  }
  // Repeat detection belongs on the screen, not only in the serial log: the
  // user photographs whichever line they can see, and "same crash again" is the
  // one question that changes how the report is triaged.
  {
    const HalSystem::CrashHistory history = HalSystem::getCrashHistory();
    if (history.count > 0) {
      snprintf(buffer, sizeof(buffer), "crash#%u %s", static_cast<unsigned>(history.streak),
               history.repeats() ? "(same as last)" : "(new address)");
      contextLines.emplace_back(buffer);
    }
  }
  for (uint8_t start = 0; start < ctx.frameCount; start += 4) {
    int used = snprintf(buffer, sizeof(buffer), "ret=");
    for (uint8_t i = start; i < ctx.frameCount && i < start + 4; ++i) {
      snprintf(buffer + used, sizeof(buffer) - static_cast<size_t>(used), " %08X", static_cast<unsigned>(ctx.frames[i]));
      used += 9;
    }
    contextLines.emplace_back(buffer);
  }
  if (ctx.heapFree != 0 || ctx.psramFree != 0) {
    snprintf(buffer, sizeof(buffer), "mem=%u/%u psram=%u up=%us", static_cast<unsigned>(ctx.heapFree),
             static_cast<unsigned>(ctx.heapLargest), static_cast<unsigned>(ctx.psramFree),
             static_cast<unsigned>(ctx.uptimeMs / 1000));
    contextLines.emplace_back(buffer);
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
  const auto contentWidth = pageWidth - 2 * metrics.contentSidePadding;
  const auto x = metrics.contentSidePadding;
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  // Crash report is a dead end, not a pushed screen: no back button.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CRASH_TITLE), nullptr,
                 false);

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  auto descLines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_CRASH_DESCRIPTION), contentWidth, 10);
  for (const auto& line : descLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  y += metrics.verticalSpacing * 2;
  renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_CRASH_REASON));
  y += lineHeight + metrics.verticalSpacing;

  auto panicLines = renderer.wrappedText(UI_10_FONT_ID, panicMessage.c_str(), contentWidth, 5);
  for (const auto& line : panicLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  // Crash context: what the panic could not say about itself. Drawn in the
  // small font so a long mangled exception name and eight return addresses fit
  // on one screen; the values are only useful to us, but their *presence* is
  // what tells a reporter that the photo is enough.
  if (!contextLines.empty()) {
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
      const size_t pages = (contextLines.size() + perPage - 1) / perPage;
      if (static_cast<size_t>(contextPage) >= pages) contextPage = 0;
      const size_t first = static_cast<size_t>(contextPage) * perPage;
      size_t index = first;
      for (; index < contextLines.size() && index < first + perPage; ++index) {
        renderer.drawText(SMALL_FONT_ID, x, y, contextLines[index].c_str());
        y += smallLineHeight;
      }
      if (index < contextLines.size()) {
        char more[16];
        // "+N" rather than a sentence: it must be readable on any language's
        // device and it must not be the reason this render allocates.
        snprintf(more, sizeof(more), "+%u", static_cast<unsigned>(contextLines.size() - index));
        renderer.drawText(SMALL_FONT_ID, x, y, more);
        contextMore = true;
      }
    }
  }

  if (!bootTrailMessage.empty()) {
    y += metrics.verticalSpacing;
    auto trailLines = renderer.wrappedText(UI_10_FONT_ID, bootTrailMessage.c_str(), contentWidth, 3);
    for (const auto& line : trailLines) {
      renderer.drawText(UI_10_FONT_ID, x, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += lineHeight;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
