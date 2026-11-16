// Host regression for the byte-mode QR version table.
//
// QrUtils used to pick versions with four rough thresholds; payloads in the
// gaps (79..114 bytes, ...) were encoded into a symbol scanners cannot read,
// and qrcode_initText() still reports success. The table in src/util/QrVersion.h
// is the ECC-L byte capacity, so pin every boundary here.

#include <cstdio>

#include "QrVersion.h"

namespace {
int g_checks = 0;
int g_failures = 0;

void check(const bool ok, const int len, const int expected) {
    ++g_checks;
    const int actual = QrUtils::byteModeVersionFor(static_cast<size_t>(len));
    if (!ok || actual != expected) {
        ++g_failures;
        std::printf("FAIL: len=%d expected v%d got v%d\n", len, expected, actual);
    }
}
}  // namespace

int main() {
    // The cases that motivated the fix: a 91-byte JJWXC login URL must be v5,
    // not the old "<=114 -> v4" (v4 holds only 78 bytes at ECC-L).
    check(true, 0, 1);
    check(true, 1, 1);
    check(true, 17, 1);
    check(true, 18, 2);
    check(true, 32, 2);
    check(true, 33, 3);
    check(true, 53, 3);
    check(true, 54, 4);
    check(true, 78, 4);
    check(true, 79, 5);
    check(true, 91, 5);
    check(true, 106, 5);
    check(true, 107, 6);
    check(true, 114, 6);
    check(true, 134, 6);
    check(true, 135, 7);
    check(true, 271, 10);
    check(true, 272, 11);
    check(true, 2953, 40);
    check(true, 2954, 40);  // clamped; QrUtils truncates the payload first
    check(true, 5000, 40);

    // Every boundary: capacity[v] stays in v, one more byte moves to v+1.
    const int capacities[40] = {
        17,   32,   53,   78,   106,  134,  154,  192,  230,  271,  321,  367,  425,  458,
        520,  586,  644,  718,  792,  858,  929,  1003, 1091, 1171, 1273, 1367, 1465, 1528,
        1628, 1732, 1840, 1952, 2068, 2188, 2303, 2431, 2563, 2699, 2809, 2953,
    };
    for (int v = 1; v <= 40; ++v) {
        const int len = capacities[v - 1];
        check(true, len, v);
        if (v < 40) check(true, len + 1, v + 1);
    }

    // Monotonic and never under-capacity for payloads the caller can pass.
    int previous = 0;
    for (int len = 1; len <= 2953; ++len) {
        const int v = QrUtils::byteModeVersionFor(static_cast<size_t>(len));
        ++g_checks;
        if (v < previous || capacities[v - 1] < len) {
            ++g_failures;
            std::printf("FAIL: len=%d v=%d previous=%d\n", len, v, previous);
            break;
        }
        previous = v;
    }

    if (g_failures == 0) {
        std::printf("qr version: %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("qr version: %d/%d checks failed\n", g_failures, g_checks);
    return 1;
}
