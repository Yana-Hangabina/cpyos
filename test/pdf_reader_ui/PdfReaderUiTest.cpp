// The four surfaces and who owns the input, as a table.  Each case below is a
// bug or a requirement, not a hypothetical: see docs/engineering/
// pdf-reader-hardening-status.md.
#include "PdfReaderUi.h"

#include <cstdio>
#include <initializer_list>

using namespace pdf_reader_ui;

static int checks = 0, failures = 0;
#define CHECK(cond, ...)                                                     \
  do {                                                                       \
    ++checks;                                                                 \
    if (!(cond)) { ++failures; std::printf("FAIL line %d: ", __LINE__);        \
                   std::printf(__VA_ARGS__); std::printf("\n"); }             \
  } while (0)

int main() {
  const State normal{};
  const State loadFailed{true, false, false, false};
  const State hint{false, true, false, false};
  const State failed{false, false, true, false};
  const State failedHard{false, false, true, true};

  // Normal page: our code has no opinion.
  CHECK(decide(normal, Input::Next) == Action::Base, "a normal page was swallowed");
  CHECK(decide(normal, Input::TapOther) == Action::Base, "a normal tap was swallowed");

  // Load failure: any input leaves; an empty frame changes nothing.
  CHECK(decide(loadFailed, Input::None) == Action::Swallow, "the load card gave the frame away");
  for (const Input in : {Input::Back, Input::Confirm, Input::Next, Input::TapOther}) {
    CHECK(decide(loadFailed, in) == Action::LeaveReader, "an input on the load card did not leave");
  }

  // The notice is a modal: even a page-turn key only closes it (the bug fixed
  // this round was the tap doing both).
  CHECK(decide(hint, Input::TapOther) == Action::DismissHint, "a tap did not close the notice");
  CHECK(decide(hint, Input::Next) == Action::DismissHint, "a page key fell through the notice");
  CHECK(decide(hint, Input::HintTimeout) == Action::DismissHint, "the notice never timed out");
  CHECK(decide(hint, Input::None) == Action::Swallow, "the notice let input through");

  // The failed-page card.
  CHECK(decide(failed, Input::Confirm) == Action::RetryPage, "Confirm did not retry");
  CHECK(decide(failed, Input::TapRetry) == Action::RetryPage, "the retry pill did not retry");
  // A missed tap must not re-run the page: that is a second 75 s budget on a
  // page that just failed.
  CHECK(decide(failed, Input::TapOther) == Action::Swallow, "a tap between pills re-ran the page");
  CHECK(decide(failed, Input::TapSkip) == Action::SkipForward, "the skip pill did not skip");
  CHECK(decide(failed, Input::Next) == Action::SkipForward, "a page key did not skip");
  CHECK(decide(failed, Input::Prev) == Action::SkipBack, "backward skip went the wrong way");
  CHECK(decide(failed, Input::Back) == Action::LeaveReader, "Back did not leave the card");
  CHECK(decide(failed, Input::None) == Action::Swallow, "the card let input through");

  // Escalated: three failures mean "this file cannot be shown here", so the
  // second action leaves instead of turning a page.
  CHECK(decide(failedHard, Input::TapSkip) == Action::LeaveReader, "the escalated card skipped instead of leaving");
  CHECK(decide(failedHard, Input::Confirm) == Action::RetryPage, "the escalated card could not retry");
  CHECK(decide(failedHard, Input::Next) == Action::SkipForward,
        "a page key stopped working on the escalated card");

  // Precedence: a pending notice outranks a failed card (the notice is painted
  // on top), and a load failure outranks both.
  CHECK(decide(State{false, true, true, false}, Input::Confirm) == Action::DismissHint,
        "the notice did not outrank the failure card");
  CHECK(decide(State{true, true, true, true}, Input::Confirm) == Action::LeaveReader,
        "a load failure did not outrank everything");

  // Skipping off the card is conditional on the turn having moved.
  CHECK(pdf_reader_ui::afterSkip(true) == pdf_reader_ui::SkipResult::LeaveCard,
        "a moved page kept the card");
  CHECK(pdf_reader_ui::afterSkip(false) == pdf_reader_ui::SkipResult::StayOnCard,
        "a skip with nowhere to go dropped the card (it would retry the page)");

  std::printf("pdf reader ui: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
