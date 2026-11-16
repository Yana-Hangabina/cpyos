// Host regression for the browser plugin job table (lease, claim echo,
// recycling). Compiles the same header the firmware uses.
//
//   bash ports/metalio_plus/test/run_plugin_jobs_test.sh

#include "plugin_job_table.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(cond)) {                                                        \
      ++g_failures;                                                       \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond);                 \
    }                                                                     \
  } while (0)

static void fill(plugin_jobs::Job* job, const char* plugin, const char* action, const char* args) {
  std::snprintf(job->plugin, sizeof(job->plugin), "%s", plugin);
  std::snprintf(job->action, sizeof(job->action), "%s", action);
  std::snprintf(job->args, sizeof(job->args), "%s", args != nullptr ? args : "");
}

int main() {
  using namespace plugin_jobs;

  {
    Table<6> t;
    Job* a = t.alloc(10);
    Job* b = t.alloc(11);
    CHECK(a != nullptr && b != nullptr);
    CHECK(a->id == 1 && a->state == kPending && a->updatedAt == 10);
    CHECK(b->id == 2);
    fill(a, "alpha", "run", "{\"x\":1}");

    CHECK(t.claim("beta", 20, 600000) == nullptr);
    Job* c = t.claim("alpha", 21, 600000);
    CHECK(c == a && c->state == kRunning && c->claim == 1);
    CHECK(t.claim("alpha", 22, 600000) == nullptr);  // already running

    CHECK(t.complete(1, 999, true, "{}", 23) == Outcome::StaleClaim);
    CHECK(a->state == kRunning);
    CHECK(t.complete(1, 1, true, "{\"ok\":1}", 24) == Outcome::Ok);
    CHECK(a->state == kDone && std::strcmp(a->result, "{\"ok\":1}") == 0);
    CHECK(t.complete(1, 1, false, nullptr, 25) == Outcome::AlreadySettled);
    CHECK(t.complete(999, 1, true, nullptr, 25) == Outcome::NotFound);
  }

  {
    // Lease expiry: a late completion with the old claim must not win.
    Table<6> t;
    Job* a = t.alloc(0);
    fill(a, "alpha", "run", "{}");
    CHECK(t.claim("alpha", 10, 1000) != nullptr);   // updatedAt = 10
    CHECK(t.claim("alpha", 1009, 1000) == nullptr); // 999 ms: still inside
    const uint32_t firstClaim = a->claim;
    Job* again = t.claim("alpha", 1011, 1000);      // 1001 ms: expired
    CHECK(again == a);
    CHECK(a->claim != firstClaim);
    const uint32_t secondClaim = a->claim;
    CHECK(t.complete(a->id, firstClaim, true, "{}", 1012) == Outcome::StaleClaim);
    CHECK(t.complete(a->id, secondClaim, false, "{\"err\":1}", 1013) == Outcome::Ok);
    CHECK(a->state == kError);
  }

  {
    // Queue full at 6 pending; a finished slot is recycled oldest-first.
    Table<6> t;
    for (int i = 0; i < 6; ++i) {
      Job* j = t.alloc(static_cast<uint32_t>(i));
      CHECK(j != nullptr);
      fill(j, "alpha", "run", "{}");
    }
    CHECK(t.alloc(10) == nullptr);

    CHECK(t.claim("alpha", 20, 600000) != nullptr);  // id 1 -> running
    CHECK(t.complete(1, t.find(1)->claim, true, "{}", 30) == Outcome::Ok);
    // id 2 is still pending, so the oldest *finished* is id 1 even though the
    // full condition is relieved by that same slot; alloc must take id 1.
    Job* recycled = t.alloc(40);
    CHECK(recycled != nullptr && recycled->id == 7 && recycled->state == kPending);
  }

  {
    // Result bounds: 191 bytes fit, 192 use the marker.
    Table<6> t;
    Job* a = t.alloc(0);
    fill(a, "alpha", "run", "{}");
    t.claim("alpha", 1, 600000);
    const std::string fits(191, 'x');
    CHECK(t.complete(a->id, a->claim, true, fits.c_str(), 2) == Outcome::Ok);
    CHECK(std::strlen(a->result) == 191);

    Job* b = t.alloc(3);
    fill(b, "alpha", "run", "{}");
    t.claim("alpha", 4, 600000);
    const std::string tooLong(192, 'x');
    CHECK(t.complete(b->id, b->claim, true, tooLong.c_str(), 5) == Outcome::Ok);
    CHECK(std::strcmp(b->result, "{\"error\":\"result too large\"}") == 0);
  }

  {
    // 32-bit millisecond wrap: an expired lease is detected with unsigned math.
    Table<6> t;
    Job* a = t.alloc(0xFFFFFF00u);
    fill(a, "alpha", "run", "{}");
    CHECK(t.claim("alpha", 0xFFFFFF50u, 1000) != nullptr);  // 80 ms in
    Job* again = t.claim("alpha", 0x00000400u, 1000);       // wrapped: 1200 ms
    CHECK(again == a);
  }

  {
    Table<6> t;
    t.alloc(1);
    t.clear();
    CHECK(t.find(1) == nullptr);
    Job* a = t.alloc(2);
    CHECK(a != nullptr && a->id == 1);  // ids restart after clear
  }

  std::printf("%s: %d checks, %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_checks,
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
