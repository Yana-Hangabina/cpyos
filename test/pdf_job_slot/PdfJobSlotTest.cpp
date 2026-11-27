// Pins the ownership rule that lets an expired render be a failed page rather
// than a heap corruption or a reboot loop.
#include "PdfJobSlot.h"

#include <cstdio>

using namespace pdf_job_slot;

static int checks = 0, failures = 0;
#define CHECK(cond, ...)                                                     \
  do {                                                                       \
    ++checks;                                                                 \
    if (!(cond)) { ++failures; std::printf("FAIL line %d: ", __LINE__);        \
                   std::printf(__VA_ARGS__); std::printf("\n"); }             \
  } while (0)

int main() {
  CHECK(submit(State{false, false}) == Verdict::Reuse, "an idle slot refused a job");
  CHECK(submit(State{false, true}) == Verdict::Reuse, "a free slot with a stale done refused a job");
  CHECK(submit(State{true, false}) == Verdict::Refuse, "a running job's slot was reused");
  CHECK(submit(State{true, true}) == Verdict::Reuse, "a finished job's slot was never returned");
  CHECK(slotReturnedByWait(true), "a completed wait did not return the slot");
  CHECK(!slotReturnedByWait(false), "an expired wait returned a slot the worker still owns");
  {
    State s{};
    CHECK(submit(s) == Verdict::Reuse, "first submit refused");
    s.inFlight = true;
    s.done = false;
    if (slotReturnedByWait(false)) s.inFlight = false;
    CHECK(s.inFlight, "an expired wait cleared ownership");
    CHECK(submit(s) == Verdict::Refuse, "retry during a running job was accepted");
    s.done = true;
    CHECK(submit(s) == Verdict::Reuse, "the slot never came back after the worker finished");
  }
  {
    State s{true, true};
    CHECK(submit(s) == Verdict::Reuse, "finished slot not reusable");
    s.inFlight = true;
    s.done = false;
    CHECK(submit(s) == Verdict::Refuse, "a reused slot did not start in flight");
  }
  // 7. Deleting the worker is refused while any job is still in flight,
  //    including the abandoned (timed-out) one that the user can walk away from.
  CHECK(mayStopWorker(State{false, false}), "deleting an idle worker was refused");
  CHECK(!mayStopWorker(State{true, false}), "deleted the worker under a running job");
  CHECK(mayStopWorker(State{true, true}), "a finished job still blocked the delete");
  {
    State abandoned{true, false};  // timeout expired, worker still inside MuPDF
    CHECK(!mayStopWorker(abandoned), "an abandoned job let the worker be deleted mid-page");
    abandoned.done = true;
    CHECK(mayStopWorker(abandoned), "the worker was never deletable again after it finished");
  }

  std::printf("pdf job slot: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
