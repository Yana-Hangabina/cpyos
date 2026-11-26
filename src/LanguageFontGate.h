#pragma once

// Blocking font-download gate: the rule for what the user may do while a language that
// cannot be rendered from app0 is pending.
//
// The product decision (2026-11-26) is: choosing such a language must not be silently
// allowed, because the result is a screen of tofu that looks like a broken device. Instead
// the UI stays on the download page, where the only permitted actions are connecting to a
// network, retrying, or falling back to a language app0 can actually draw. Leaving the gate
// into the system UI is not permitted while the pending language is unrenderable.
//
// This is deliberately a pure decision function rather than UI code, for two reasons that
// both come from this project's history: the screens it governs live in files other sessions
// are editing (SettingsActivity / FontExtrasActivity), and every previous "the gate is
// enforced" claim in this repo turned out to rest on a UI path nobody had exercised. The
// rule is host-tested here; the wiring is then two calls.
//
// Composition: this header reads the coverage table (src/LanguageFontCoverage.h, generated)
// and owns no language knowledge of its own. If a bit is added there, the gate follows.

#include <cstdint>

#include "LanguageFontCoverage.h"

namespace font_gate {

// Where a download attempt stands. `Unknown` is the state before the user has been asked,
// so a page opening can decide whether to show the gate at all.
enum class Download : uint8_t { Unknown, Running, Succeeded, Failed };

enum class Decision : uint8_t {
  // The language renders from app0: no gate, the selection just happens.
  AllowSelection,
  // No pack can certify this script (RTL shaping forms, or codepoints absent from every
  // committed source). Selecting it is refused with an explanation, never a download page.
  RefuseUnsupported,
  // Held on the gate, waiting for the user to finish a download. Network entry is allowed.
  HoldDownload,
  // Held on the gate, but a retry is what the user is asking for.
  AllowRetry,
  // Nothing is pending: leaving is fine.
  AllowExit,
};

// True when switching to this language needs something downloadable that a download can
// actually deliver. `false` does not mean "supported" -- see unsupportedScript().
constexpr bool intercepts(uint8_t language) { return language_font::gateNeeded(language); }

// True when the table says no pack can help, so the gate must refuse rather than offer a
// download. Offering a download here would strand the user on a page that cannot save them.
constexpr bool unsupportedScript(uint8_t language) { return language_font::mustRefuse(language); }

// The escape hatch has to be defined by capability, not by a hard-coded language id:
// "app0 can draw this" is exactly what the coverage table knows. English and Simplified
// Chinese are the two app0-backed UI languages today, but a future tier change must not
// turn the escape hatch into a lie.
constexpr bool rendersFromApp0(uint8_t language) {
  return !intercepts(language) && !unsupportedScript(language);
}

// Does a pending selection still need its packs installed?
//
// `installedMask` is the caller's view of which of the four download kinds are present on
// this device (font_extras::installed ids), and `requiredMask` is normally
// `language_font::maskFor(language) & language_font::kDownloadableMask`. Passing the raw
// mask is fine: bits outside the downloadable set are ignored here on purpose, because a
// missing shaping face is not something "installed" can fix (see kDownloadableMask's
// comment in the coverage header).
constexpr bool packsMissing(uint8_t requiredMask, uint8_t installedMask) {
  return (requiredMask & language_font::kDownloadableMask & ~installedMask) != 0;
}

struct Request {
  uint8_t language;             // the language being switched to
  Download download;            // state of the download for it
  uint8_t requiredMask;         // maskFor(language)
  uint8_t installedMask;        // packs present before this request
  // What the user asked for: another attempt, network settings, or an exit.
  // A plain "enter the language" request is Action::Enter with download == Unknown.
  bool wantsRetry = false;
  bool wantsNetwork = false;
  bool wantsExit = false;
};

// The whole rule. Order matters and each branch is a distinct user-visible outcome:
//
//  1. A language app0 cannot draw and no pack can fix -> refuse, never show a download page.
//  2. Otherwise, if nothing is needed -> the selection is free.
//  3. Otherwise the gate holds, and the question is only which escape is permitted.
constexpr Decision decide(const Request& request) {
  if (unsupportedScript(request.language)) {
    return Decision::RefuseUnsupported;
  }
  if (!packsMissing(request.requiredMask, request.installedMask)) {
    // Nothing is outstanding: the switch is free, and so is leaving.
    return request.wantsExit ? Decision::AllowExit : Decision::AllowSelection;
  }
  // Something required is absent. From here the gate holds: every answer keeps the user
  // on the download page, because the alternative is a screen of tofu they cannot undo.
  if (request.wantsNetwork) {
    // The one outward action that is permitted: connecting is how the user gets here
    // legally. It stays inside the gate -- opening network settings is not an exit.
    return Decision::HoldDownload;
  }
  if (request.wantsRetry || request.download == Download::Failed) {
    // Network settings are permitted too -- but as an action inside the gate, not an exit.
    return Decision::AllowRetry;
  }
  return Decision::HoldDownload;
}

// Whether a fallback language may be adopted while a gated selection is pending. This is
// the only way out besides finishing the download, and it must be checked against the
// fallback's own capability -- not assumed.
constexpr bool mayFallBack(uint8_t fallbackLanguage) { return rendersFromApp0(fallbackLanguage); }

}  // namespace font_gate
