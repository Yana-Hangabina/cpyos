#pragma once

#include <cstdint>

// Fault codes: one vocabulary for the failures that can end in a crash, a reboot,
// or a hang.
//
// Why codes exist (2026-11-27, after the 0.8.2 field reports): a photograph of the
// crash screen used to be the whole payload a user could send, and it identified
// the *symptom* ("abort() was called at PC 0x423569f3") but nothing about the
// minutes before it. The refusal paths added by the stack-guard and callback work
// each log a different English sentence on serial -- which a field reporter cannot
// read -- and every one of them means "a crash was averted right here". A short,
// stable, language-neutral code fixes that:
//
//   * it costs no i18n bytes (the embedded EN blob had 55 B of headroom when this
//     was written, so new English prose was not an option -- see AGENTS 67.12),
//   * it survives a release, so a code in a screenshot from 0.8.3 still resolves
//     after 0.9.0, which is why the values are fixed by domain range and why a tag
//     is never reused for a different meaning,
//   * it is countable, so the screen can say `STK-21 x3` instead of three lines.
//
// Recording policy, per the 2026-11-27 decision: only failures that can lead to a
// crash, reboot or hang are recorded and shown. Self-healing degradations (a face
// falling back to streaming, a missing glyph, hyphenation turning off) keep their
// reserved range 0xC0..0xFF and are *not* raised yet: surfacing every degradation
// as a fault would train users to ignore the fault list. Adding a code there is a
// deliberate later step, not a cleanup.
//
// This header is the only place a code is defined. `lib/Diagnostics/FaultLog.h`
// records them, and scripts/tests/test_fault_codes.py checks that every declared
// code is raised somewhere and that nothing raises an undeclared one.
namespace fault {

enum class Code : uint16_t {
  // --- 0x01..0x1F: how the device died -------------------------------------------------
  PanicCacheStack = 0x01,    // esp_task_stack_is_sane_cache_disabled: flash op on a PSRAM stack
  PanicAssert = 0x02,        // some other assert()/configASSERT(), expression + file:line kept
  PanicBadFunctionCall = 0x03,  // empty std::function under -fno-exceptions -> terminate
  PanicBadAlloc = 0x04,      // operator new failed
  PanicLengthError = 0x05,   // libstdc++ container grew past its bound
  PanicAbortGeneric = 0x06,  // abort() with no classifiable reason
  PanicOtherException = 0x07,  // any other uncaught C++ type (its name goes to the report)
  PanelPushHang = 0x08,      // the scan watchdog fired: a panel push never returned -> restart
  WatchdogReset = 0x09,      // this boot followed a task watchdog reset
  BrownoutReset = 0x0A,      // this boot followed a brown-out reset
  PanicStackOverflow = 0x0B,  // "Stack canary watchpoint triggered": a frame outgrew its stack

  // --- 0x20..0x41: internal-flash refusals (a crash averted, at a cost) ------------------
  CoreFontProbeRefused = 0x20,     // SystemCoreFont::probe()
  CoreFontEvalRefused = 0x21,      // SystemCoreFont::ready()/status()
  CoreFontResidentRefused = 0x22,  // SystemCoreFont::installResident()
  CoreFontReadRefused = 0x23,      // SystemCoreFont carrier read
  FontPackStreamRefused = 0x24,    // SystemFontPack::readSource(), a FreeType table read
  FontPackLoadRefused = 0x25,      // SystemFontPack::begin() partition validation
  EmojiPackReadRefused = 0x26,     // EmojiFont::loadPack()
  LanguagePackReadRefused = 0x27,  // i18n_pack::init(): the whole UI falls back to English
  LanguagePackLoadRefused = 0x28,  // i18n_pack::loadLanguage(): one language falls back
  WebPackReadRefused = 0x29,       // web asset pack: /files and /settings show the recovery page
  HyphenReadRefused = 0x2A,        // HyphenationStore partition trie
  HyphenProbeRefused = 0x2B,       // HyphenationStore container probe
  HyphenLoadRefused = 0x2C,        // HyphenationStore::load()
  PinyinDbRefused = 0x2D,          // PinyinDb::loadBundled(): pinyin input degraded
  DeviceSecretRefused = 0x2E,      // deviceSecret() on a PSRAM stack (encrypted data unreadable)
  FontCachePreloadRefused = 0x2F,  // SdCardFontCache::preload() refused: fonts load from SD
  OtaPackWriteRefused = 0x30,      // OtaUpdater data-pack write
  OtaInstallRefused = 0x31,        // OtaUpdater::installUpdate()
  OtaBootSwitchRefused = 0x32,     // otadata write refused (the new slot would never boot)
  SlotReadRefused = 0x33,          // HalOtaSlot image read (font flash cache)
  SlotEraseRefused = 0x34,         // HalOtaSlot erase
  SlotWriteRefused = 0x35,         // HalOtaSlot write
  SlotRollbackRefused = 0x36,      // HalOtaSlot::confirmRunningImage(): rollback stays armed
  FlasherWriteRefused = 0x37,      // firmware_flash::flashFromSdPath()
  FlasherChipIdRefused = 0x38,     // runningPartitionChipId(): image chip check is unknown
  RadioDeepReleaseRefused = 0x39,  // radio teardown after a flash write refused
  RadioStorageSwitchRefused = 0x3A,  // WiFi storage restore refused
  PdfAssetCarrierRefused = 0x3B,   // PDF asset carrier read
  PdfEngineStartRefused = 0x3C,    // pdf_engine::startUpdate()
  PdfRuntimeMapRefused = 0x3D,     // PdfModuleRuntime image map
  PdfRuntimeSdRefused = 0x3E,      // PdfModuleRuntimeSd image read
  PdfReaderCacheRefused = 0x3F,    // PdfReaderActivity engine-partition reads
  PdfProbeRefused = 0x40,          // bench paging probe only
  XtcNoCallback = 0x41,            // XtcParser::loadPageStreaming() with no sink (NO_PAGE_CALLBACK)

  // --- 0x42..0x5F: a callback that would have aborted the process ------------------------
  // The other half of the 0.8.2 report: PC 0x423569f3 was std::terminate, reached
  // because libstdc++ still throws bad_function_call under -fno-exceptions. Those
  // calls are now routed through callbacks::call(), which skips and reports; a skip
  // is an averted abort at a specific site, so it is recorded. `detail` carries the
  // site's index in the callback report table, whose names crash_report.txt prints.
  CallbackEmpty = 0x42,  // an empty std::function was skipped instead of called
                        // (detail = its index in the callback report table)

  // --- 0x60..0x7F: a safety net or worker that does not exist ---------------------------
  RenderStackFallback = 0x60,  // PSRAM render stack refused -> internal RAM (TLS can starve)
  PluginStackFallback = 0x61,  // PSRAM plugin stack refused -> internal RAM
  ScanWatchdogAbsent = 0x62,   // the panel scan watchdog task could not be created
  StuckDetectorAbsent = 0x63,  // the loop-heartbeat forensics task could not be created
  WorkerCreateFailed = 0x64,   // a named worker task could not start (which one: detail)
  WorkerStackFallback = 0x65,  // a worker's PSRAM stack was refused; it runs on an internal RAM stack
  CallbackTableFull = 0x66,    // more distinct empty-callback sites than the fixed table holds

  // --- 0x80..0x9F: display path --------------------------------------------------------
  PresentBarrierTimeout = 0x80,  // a settings/SD write had to wait past the scan barrier
  AreaCleanRefused = 0x81,       // local clean refused -> full-screen clean (a visible flash)
  ExpanderNotUp = 0x82,          // FCA9555 down: the panel rails cannot be sequenced at all
  UnalignedBand = 0x83,          // a column band was not 4-px aligned (drives wrong pixels)
  // 0x84..0x9F are unused on purpose. A "no panel driver" code was drafted
  // and removed: this tree binds the driver at compile time, so there is no
  // runtime branch that could raise it, and a declared-but-unreachable code
  // is exactly what scripts/tests/test_fault_codes.py fails the build for.

  // --- 0xA0..0xBF: state that silently went wrong ---------------------------------------
  PersistHeapFail = 0xA0,     // settings/statistics JSON could not be allocated: not saved
  StateVersionUnknown = 0xA1,  // a persisted blob was written by a newer/older format
  AtomicReplaceDiscarded = 0xA2,  // a caller ignored the result of an atomic replace

  // --- 0xC0..0xFF: reserved for the self-healing tier (deliberately not raised) ---------
  ReservedSelfHealingFirst = 0xC0,
  ReservedSelfHealingLast = 0xFF,
};

// One table, three projections (value, tag, developer meaning). The static_asserts
// below make the table itself the checked artifact: a duplicated value, a duplicated
// tag, or a gap that leaves two tags pointing at the same meaning fails to compile,
// so the vocabulary cannot drift the way two parallel lists would.
struct Entry {
  Code code;
  const char* tag;   // what the crash screen prints: 6 characters plus a terminator
  const char* what;  // what crash_report.txt prints (developer sentence, ASCII)
};

inline constexpr Entry kTable[] = {
    {Code::PanicCacheStack, "PNC-01", "flash operation on a PSRAM-stack task (cache disabled)"},
    {Code::PanicAssert, "PNC-02", "assert failed (expression and file:line in the report)"},
    {Code::PanicBadFunctionCall, "PNC-03", "empty std::function called under -fno-exceptions"},
    {Code::PanicBadAlloc, "PNC-04", "operator new failed"},
    {Code::PanicLengthError, "PNC-05", "std::length_error"},
    {Code::PanicAbortGeneric, "PNC-06", "abort() with no classifiable reason"},
    {Code::PanicOtherException, "PNC-07", "uncaught C++ exception of another type"},
    {Code::PanelPushHang, "PNC-08", "panel scan watchdog fired; device restarted"},
    {Code::WatchdogReset, "PNC-09", "this boot follows a task watchdog reset"},
    {Code::BrownoutReset, "PNC-10", "this boot follows a brown-out reset"},
    {Code::PanicStackOverflow, "PNC-11", "stack canary watchpoint: a frame outgrew its task stack"},

    {Code::CoreFontProbeRefused, "STK-01", "core font carrier probe refused on a PSRAM stack"},
    {Code::CoreFontEvalRefused, "STK-02", "core font evaluation refused; verdict not cached"},
    {Code::CoreFontResidentRefused, "STK-03", "core font resident install refused"},
    {Code::CoreFontReadRefused, "STK-04", "core font carrier read refused"},
    {Code::FontPackStreamRefused, "STK-05", "system font pack table read refused (glyph source)"},
    {Code::FontPackLoadRefused, "STK-06", "system font pack load refused on a PSRAM stack"},
    {Code::EmojiPackReadRefused, "STK-07", "emoji pack read refused"},
    {Code::LanguagePackReadRefused, "STK-08", "language pack read refused; UI falls back to English"},
    {Code::LanguagePackLoadRefused, "STK-09", "one packed language refused; that language falls back"},
    {Code::WebPackReadRefused, "STK-10", "web asset pack refused; pages fall back to the recovery page"},
    {Code::HyphenReadRefused, "STK-11", "hyphenation trie read refused"},
    {Code::HyphenProbeRefused, "STK-12", "hyphenation container probe refused"},
    {Code::HyphenLoadRefused, "STK-13", "hyphenation load refused"},
    {Code::PinyinDbRefused, "STK-14", "pinyin database read refused"},
    {Code::DeviceSecretRefused, "STK-15", "device secret read refused; protected data unreadable"},
    {Code::FontCachePreloadRefused, "STK-16", "SD font cache preload refused"},
    {Code::OtaPackWriteRefused, "STK-17", "OTA data pack write refused"},
    {Code::OtaInstallRefused, "STK-18", "OTA install refused; slot untouched"},
    {Code::OtaBootSwitchRefused, "STK-19", "otadata switch refused"},
    {Code::SlotReadRefused, "STK-20", "OTA slot image read refused"},
    {Code::SlotEraseRefused, "STK-21", "OTA slot erase refused"},
    {Code::SlotWriteRefused, "STK-22", "OTA slot write refused"},
    {Code::SlotRollbackRefused, "STK-23", "rollback confirm refused; image stays pending"},
    {Code::FlasherWriteRefused, "STK-24", "SD firmware flash refused"},
    {Code::FlasherChipIdRefused, "STK-25", "running partition chip id unknown (read refused)"},
    {Code::RadioDeepReleaseRefused, "STK-26", "radio deep release after flash write refused"},
    {Code::RadioStorageSwitchRefused, "STK-27", "Wi-Fi storage mode switch refused"},
    {Code::PdfAssetCarrierRefused, "STK-28", "PDF asset carrier read refused"},
    {Code::PdfEngineStartRefused, "STK-29", "PDF engine update start refused"},
    {Code::PdfRuntimeMapRefused, "STK-30", "PDF engine runtime map refused"},
    {Code::PdfRuntimeSdRefused, "STK-31", "PDF engine runtime SD read refused"},
    {Code::PdfReaderCacheRefused, "STK-32", "PDF reader engine cache access refused"},
    {Code::PdfProbeRefused, "STK-33", "PDF paging probe refused (bench only)"},
    {Code::XtcNoCallback, "STK-34", "XTC page streaming had no destination callback"},

    {Code::CallbackEmpty, "CBK-01", "empty std::function skipped (would have aborted)"},

    {Code::RenderStackFallback, "TSK-01", "PSRAM render stack refused; fell back to internal RAM"},
    {Code::PluginStackFallback, "TSK-02", "PSRAM plugin stack refused; fell back to internal RAM"},
    {Code::ScanWatchdogAbsent, "TSK-03", "panel scan watchdog task was not created"},
    {Code::StuckDetectorAbsent, "TSK-04", "stuck-forensics task was not created"},
    {Code::WorkerCreateFailed, "TSK-05", "a background worker task could not start"},
    {Code::WorkerStackFallback, "TSK-06", "worker PSRAM stack refused; it runs on an internal RAM stack"},
    {Code::CallbackTableFull, "TSK-07", "more empty-callback sites than the report table holds"},

    {Code::PresentBarrierTimeout, "PNL-01", "present barrier timed out before an SD write"},
    {Code::AreaCleanRefused, "PNL-02", "local clean refused; a full-screen clean ran instead"},
    {Code::ExpanderNotUp, "PNL-03", "IO expander not responding; panel rails cannot be sequenced"},
    {Code::UnalignedBand, "PNL-04", "column band rectangle was not 4-px aligned"},

    {Code::PersistHeapFail, "STR-01", "persistable store could not allocate; state not saved"},
    {Code::StateVersionUnknown, "STR-02", "persisted state version unknown; defaults used"},
    {Code::AtomicReplaceDiscarded, "STR-03", "an atomic replace failure was ignored by its caller"},
};

inline constexpr uint16_t kTableSize = static_cast<uint16_t>(sizeof(kTable) / sizeof(kTable[0]));

// Tags are what a screenshot contains, so they must be unique: two meanings sharing
// a tag would make a report ambiguous in exactly the way this table exists to avoid.
constexpr bool kTableIsSane = [] {
  for (uint16_t i = 0; i < kTableSize; ++i) {
    for (uint16_t j = 0; j < kTableSize; ++j) {
      if (i == j) continue;
      if (static_cast<uint16_t>(kTable[i].code) == static_cast<uint16_t>(kTable[j].code)) return false;
      // Comparing the six visible characters and the terminator is a complete string
      // comparison for this fixed shape, and stays constexpr without a runtime
      // strcmp.
      const char* a = kTable[i].tag;
      const char* b = kTable[j].tag;
      bool sameTag = true;
      for (uint8_t k = 0; k < 7; ++k) {
        if (a[k] != b[k]) {
          sameTag = false;
          break;
        }
        if (a[k] == '\0') break;
      }
      if (sameTag) return false;
    }
  }
  return true;
}();
static_assert(kTableIsSane, "fault::kTable must not repeat a code value or a tag");

// "" for a code with no table entry -- which the gate treats as a bug, never as a
// silent "unknown": an empty tag on the crash screen would look like a new fault
// type instead of a missing table row.
inline const char* tagOf(const Code code) {
  const uint16_t wanted = static_cast<uint16_t>(code);
  for (uint16_t i = 0; i < kTableSize; ++i) {
    if (static_cast<uint16_t>(kTable[i].code) == wanted) return kTable[i].tag;
  }
  return "";
}

inline const char* whatOf(const Code code) {
  const uint16_t wanted = static_cast<uint16_t>(code);
  for (uint16_t i = 0; i < kTableSize; ++i) {
    if (static_cast<uint16_t>(kTable[i].code) == wanted) return kTable[i].what;
  }
  return "unlisted fault code";
}

}  // namespace fault
