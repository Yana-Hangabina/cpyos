// Host test for src/LanguageFontGate.h: the blocking font-download rule.
//
// The important assertion is the exhaustive one (invariant()), not the examples: the gate
// exists to make one promise -- you cannot get to a screen that will render as tofu -- and
// that promise is about *every* language in the table, not about the two or three someone
// thought to write down.

#include <cstdio>
#include <string>

#include "LanguageFontGate.h"

namespace {

int failures = 0;
int checks = 0;

void expect(bool ok, const std::string& what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

font_gate::Request requestFor(uint8_t language, uint8_t installed, font_gate::Download download,
                              bool retry = false, bool network = false, bool exit = false) {
  font_gate::Request r{};
  r.language = language;
  r.download = download;
  r.requiredMask = language_font::maskFor(language);
  r.installedMask = installed;
  r.wantsRetry = retry;
  r.wantsNetwork = network;
  r.wantsExit = exit;
  return r;
}

// Everything this language still needs that a download can deliver.
constexpr uint8_t requiredDownloadable(uint8_t language) {
  return language_font::maskFor(language) & language_font::kDownloadableMask;
}

void testExamples() {
  // Arabic: needs a runtime shaping face, so a download page would be a false promise.
  const auto arabic = requestFor(language_font::AR, 0, font_gate::Download::Unknown);
  expect(font_gate::decide(arabic) == font_gate::Decision::RefuseUnsupported,
         "Arabic must be refused, not gated on a download");

  // Nothing in today's table has a downloadable gap (see the note below), so the hold
  // logic is driven by a *synthetic* requirement mask: a language that would need the World
  // pack. This tests the rule instead of testing which languages the generator happened to
  // emit today, and it keeps working when the tiers change.
  font_gate::Request gated = requestFor(language_font::VI, 0, font_gate::Download::Unknown);
  gated.requiredMask = language_font::kNeedWorldPack;
  expect(font_gate::decide(gated) == font_gate::Decision::HoldDownload,
         "a language with missing packs must hold the user on the gate");
  gated.wantsExit = true;
  expect(font_gate::decide(gated) == font_gate::Decision::HoldDownload,
         "Back while a gated language is pending must not reach the system UI");
  gated.wantsExit = false;
  gated.wantsNetwork = true;
  expect(font_gate::decide(gated) == font_gate::Decision::HoldDownload,
         "opening network settings stays inside the gate");
  gated.wantsNetwork = false;
  gated.download = font_gate::Download::Failed;
  expect(font_gate::decide(gated) == font_gate::Decision::AllowRetry,
         "a failed download offers retry");
  // With every required pack present, the switch and the exit are both free.
  font_gate::Request done = requestFor(language_font::VI, language_font::kNeedWorldPack,
                                       font_gate::Download::Succeeded);
  expect(font_gate::decide(done) == font_gate::Decision::AllowSelection,
         "installed packs must let the selection through");
  done.wantsExit = true;
  expect(font_gate::decide(done) == font_gate::Decision::AllowExit,
         "nothing pending means leaving is allowed");

  // English and Simplified Chinese are app0-backed: the gate must never appear.
  expect(font_gate::decide(requestFor(language_font::EN, 0, font_gate::Download::Unknown)) ==
             font_gate::Decision::AllowSelection,
         "English must not be gated");
  expect(font_gate::rendersFromApp0(language_font::ZH_CN), "Simplified Chinese renders from app0");
}

// The promise, stated for all 35 languages and both install states.
void testInvariant() {
  for (uint8_t lang = 0; lang < language_font::kCount; ++lang) {
    const uint8_t need = requiredDownloadable(lang);

    // (a) If a decision lets the user in or out, nothing downloadable is missing and the
    //     script is not one the table says no pack can fix.
    const auto freeEntry = font_gate::decide(requestFor(lang, need, font_gate::Download::Unknown));
    const bool entered = freeEntry == font_gate::Decision::AllowSelection;
    expect(!entered || (need == 0 || !font_gate::unsupportedScript(lang)),
           "an allowed selection must not have missing packs");

    // (b) With nothing installed and a real gap, no request shape may release the gate.
    if (need != 0 && !font_gate::unsupportedScript(lang)) {
      const font_gate::Decision flags[4] = {
          font_gate::decide(requestFor(lang, 0, font_gate::Download::Unknown)),
          font_gate::decide(requestFor(lang, 0, font_gate::Download::Running)),
          font_gate::decide(requestFor(lang, 0, font_gate::Download::Unknown, false, false, true)),
          font_gate::decide(requestFor(lang, 0, font_gate::Download::Failed, false, true, false)),
      };
      for (const auto decision : flags) {
        expect(decision == font_gate::Decision::HoldDownload ||
                   decision == font_gate::Decision::AllowRetry,
               "a pending language must never yield AllowSelection/AllowExit");
      }
    }

    // (c) Unfixable scripts are refused rather than offered a download that cannot help.
    if (font_gate::unsupportedScript(lang)) {
      expect(font_gate::decide(requestFor(lang, 0, font_gate::Download::Unknown)) ==
                 font_gate::Decision::RefuseUnsupported,
             "an unfixable script must be refused");
      expect(!font_gate::rendersFromApp0(lang), "an unfixable script is not an escape hatch");
    }
  }
}

// The escape hatch must be defined by capability, so it cannot drift into naming a language
// that the tiers have stopped covering.
void testFallbackIsCapabilityBased() {
  int fallbacks = 0;
  for (uint8_t lang = 0; lang < language_font::kCount; ++lang) {
    if (font_gate::mayFallBack(lang)) {
      ++fallbacks;
      expect(requiredDownloadable(lang) == 0, "a fallback language must need no pack");
    }
  }
  expect(fallbacks >= 2, "the gate needs at least English and Chinese as exits");
  // Today the generator finds no *downloadable* UI gap for any language: app0's UI subset
  // plus the 438-codepoint extension covers every built-in translation, and the only
  // refused scripts are Arabic/Persian (runtime shaping forms). So the hold path below is
  // latent by construction -- if this ever stops being true, the gate suddenly has live
  // users and the wiring (SettingsActivity / FontExtrasActivity) must land with it.
  int gatedLanguages = 0;
  for (uint8_t lang = 0; lang < language_font::kCount; ++lang) {
    if (font_gate::intercepts(lang)) {
      ++gatedLanguages;
    }
  }
  std::printf("languages the table currently gates: %d\n", gatedLanguages);
  std::printf("fallback-capable languages: %d\n", fallbacks);
}

}  // namespace

int main() {
  testExamples();
  testInvariant();
  testFallbackIsCapabilityBased();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
