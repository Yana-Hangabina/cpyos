#pragma once

// Which surface owns the input this frame, and what a given input does there.
//
// Scope: the four states this round introduced or touched.  The engine-refusal
// card (engineProblem_ != 0, CROSSPOINT_PDF_XIP only) is handled before this
// table because its inputs are different in kind -- a tap downloads the engine
// rather than dismissing anything -- and it predates this round.  If it ever
// needs the same treatment, add it here instead of another inline block.
//
// The reader now has four states a user can be looking at -- a document that
// failed to load, the one-shot Wi-Fi notice, the failed-page card, and a normal
// page -- and the precedence between them was four if-blocks in loop().  Two
// real bugs came out of that shape: the tap that dismissed the notice also
// turned a page, and a skipped page re-showed the card.  Both are ordering
// faults, so the ordering is a table now, executable in a host test.

namespace pdf_reader_ui {

enum class Input { None, Back, Confirm, Prev, Next, TapRetry, TapSkip, TapOther, HintTimeout };
enum class Action {
  Base,        // nothing of ours: let ReaderActivity::loop() run
  Swallow,     // this surface owns the frame; eat the input
  DismissHint, // close the notice, eat the input
  RetryPage,   // clear the failure card and re-render the same page
  SkipForward, // leave the card and move on
  SkipBack,
  LeaveReader, // finish()/back out of the reader
};

struct State {
  bool loadFailed = false;
  bool hintPending = false;
  bool renderFailed = false;
  bool escalated = false;  // three consecutive failures: "this file cannot be shown"
};

inline Action decide(const State s, const Input in) {
  // 1. The document never opened: any single input leaves, nothing else happens.
  if (s.loadFailed) {
    return in == Input::None ? Action::Swallow : Action::LeaveReader;
  }
  // 2. The one-shot notice is a modal: it eats the frame either way, and the
  //    tap that closes it must not also turn a page.
  if (s.hintPending) {
    if (in == Input::None) return Action::Swallow;
    return Action::DismissHint;
  }
  // 3. The failed-page card.
  if (s.renderFailed) {
    switch (in) {
      case Input::Back: return Action::LeaveReader;
      case Input::Confirm: return Action::RetryPage;
      case Input::Prev: return Action::SkipBack;
      case Input::Next: return Action::SkipForward;
      case Input::TapRetry: return Action::RetryPage;
      case Input::TapSkip: return s.escalated ? Action::LeaveReader : Action::SkipForward;
      // A tap that missed both pills does nothing.  Treating it as a retry
      // made a stray tap on a failing page pay the full 75 s budget again.
      case Input::TapOther: return Action::Swallow;
      default: return Action::Swallow;
    }
  }
  // 4. A normal page.
  return Action::Base;
}

// The skip pill and the page keys both end in pageTurn().  The card may only go
// away when that turn actually moved: with nowhere to go (a one-page file, the
// last page) dropping the card re-renders the page that just failed, which the
// user reads as "skip retried it and broke again".  Kept here, not inline, so
// the rule has a test rather than a comment.
enum class SkipResult { StayOnCard, LeaveCard };

inline SkipResult afterSkip(const bool moved) {
  return moved ? SkipResult::LeaveCard : SkipResult::StayOnCard;
}

}  // namespace pdf_reader_ui
