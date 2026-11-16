// Host regression for the Metalio Plus AI/BOOT key timing rules.
// The production wrapper polls from the UI loop (~10 ms); these cases advance
// the clock in the same steps, because a classifier fed only at gesture edges
// cannot know how long a contact was held.

#include <cstdio>

#include "plus_ai_key_sampler.h"

using plus_ai_key::Event;
using plus_ai_key::Sampler;

namespace {
int g_checks = 0;
int g_failures = 0;

void check(const bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// Feed `level` for `ms` in 10 ms steps; return the last non-None event.
Event hold(Sampler& s, const bool level, uint32_t& now, const uint32_t ms, Event* first = nullptr) {
    Event last = Event::None;
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += 10) {
        now += 10;
        const Event e = s.update(level, now);
        if (e != Event::None) {
            if (first != nullptr && *first == Event::None) *first = e;
            last = e;
        }
    }
    return last;
}
}  // namespace

int main() {
    // A key held at boot (the one that powered the unit on) must not become a
    // gesture, and its release must not produce a Short either.
    {
        Sampler s;
        uint32_t now = 0;
        s.reset(true, now, false);
        check(hold(s, true, now, 100) == Event::None, "boot-held press is ignored");
        check(hold(s, false, now, 100) == Event::None, "boot release does not click");
        check(hold(s, true, now, 100) == Event::None, "first real press starts");
        check(hold(s, false, now, 100) == Event::Short, "first real press is a Short");
    }

    // A contact shorter than the 25 ms debounce is not a press.
    {
        Sampler s;
        uint32_t now = 0;
        s.reset(false, now, true);
        check(hold(s, true, now, 10) == Event::None, "10 ms bounce");
        check(hold(s, false, now, 100) == Event::None, "debounced bounce is ignored");
        check(hold(s, true, now, 100) == Event::None, "next press starts");
        check(hold(s, false, now, 100) == Event::Short, "next press is a Short");
    }

    // Short below the 500 ms split.
    {
        Sampler s;
        uint32_t now = 0;
        s.reset(false, now, true);
        (void)hold(s, true, now, 200);
        check(hold(s, false, now, 100) == Event::Short, "200 ms press is short");
    }

    // Long fires exactly once at the 500 ms boundary; its release is swallowed.
    {
        Sampler s;
        uint32_t now = 0;
        s.reset(false, now, true);
        Event first = Event::None;
        (void)hold(s, true, now, 700, &first);
        check(first == Event::Long, "500 ms press produces Long");
        int longs = 0;
        for (int i = 0; i < 20; ++i) {
            now += 10;
            if (s.update(true, now) == Event::Long) ++longs;
        }
        check(longs == 0, "Long is not repeated while held");
        check(hold(s, false, now, 100) == Event::None, "release after Long is not Short");
    }

    // 25 ms is the first accepted press duration.
    {
        Sampler s;
        uint32_t now = 0;
        s.reset(false, now, true);
        (void)hold(s, true, now, 40);
        check(hold(s, false, now, 60) == Event::Short, "30-40 ms edge is a Short");
    }

    // A 300 ms hold followed by a release is still a Short, not a Long.
    {
        Sampler s;
        uint32_t now = 0;
        s.reset(false, now, true);
        (void)hold(s, true, now, 300);
        check(hold(s, false, now, 100) == Event::Short, "300 ms release is Short");
    }

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
