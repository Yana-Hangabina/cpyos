#include <cstdio>

#include "PdfInk.h"

// The guard this pins: a text-only pass that drew nothing must not count as a
// rendered page.  The numbers are the rule's own boundary, written literally so
// that changing kMinInk has to be a deliberate act with a visible edit here.
static int checks = 0;
static int failures = 0;

#define CHECK(cond, what)                        \
  do {                                           \
    ++checks;                                    \
    if (!(cond)) {                               \
      ++failures;                                \
      std::printf("FAIL: %s\n", (what));         \
    }                                            \
  } while (0)

int main() {
  // Flat pages report exactly zero ink (the histogram says one tone).
  CHECK(pdf_ink::blank(0.0f), "a perfectly blank page passed the guard");

  // A scan with images skipped lands here: a few stray speckles at most.
  CHECK(pdf_ink::blank(1e-6f), "speckle-level ink passed the guard");
  CHECK(pdf_ink::blank(4.9e-4f), "ink just under the floor passed the guard");

  // The floor itself is not blank: the comparison is strict.
  CHECK(!pdf_ink::blank(5e-4f), "the floor was misread as blank");
  CHECK(!pdf_ink::blank(5e-3f), "0.5% ink (one short line) was called blank");
  CHECK(!pdf_ink::blank(0.05f), "5% ink (a normal text page) was called blank");
  CHECK(!pdf_ink::blank(0.5f), "half the page inked was called blank");

  // The ratio itself, from a synthetic histogram.  These are the numbers the
  // device reports in 'rendered blank (ink N.NNN%)', so they must be the share
  // of samples below the paper/ink midpoint -- not a guess.
  size_t flatWhite[256] = {0};
  flatWhite[255] = 1000;
  CHECK(pdf_ink::inkFromHistogram(flatWhite, 255, 255, 1000) == 0.0f, "a flat white page reported ink");

  size_t flatGrey[256] = {0};
  flatGrey[180] = 1000;
  CHECK(pdf_ink::inkFromHistogram(flatGrey, 180, 186, 1000) == 0.0f,
        "a one-tone page with a narrow clip band reported ink");

  size_t halfInk[256] = {0};
  halfInk[0] = 500;
  halfInk[255] = 500;
  const float half = pdf_ink::inkFromHistogram(halfInk, 0, 255, 1000);
  CHECK(half > 0.49f && half < 0.51f, "half-black/half-white did not report about 0.5");

  size_t sparse[256] = {0};
  sparse[10] = 10;
  sparse[255] = 990;
  const float sparseInk = pdf_ink::inkFromHistogram(sparse, 0, 255, 1000);
  CHECK(sparseInk > 0.009f && sparseInk < 0.011f, "1% ink did not report about 0.01");
  CHECK(!pdf_ink::blank(sparseInk), "a page with 1% ink was called blank");

  // A sample exactly at the midpoint counts as ink (the '<=' in the rule).
  size_t atMid[256] = {0};
  atMid[127] = 200;
  atMid[255] = 800;
  const float midInk = pdf_ink::inkFromHistogram(atMid, 0, 255, 1000);
  CHECK(midInk > 0.19f && midInk < 0.21f, "the midpoint sample was not counted as ink");

  CHECK(pdf_ink::inkFromHistogram(flatWhite, 255, 255, 0) == 0.0f, "zero samples divided by zero");

  // Monotonic around the boundary: no ink ratio may change its verdict twice.
  bool seenInk = false;
  for (float r = 0.0f; r <= 0.01f; r += 1e-5f) {
    const bool b = pdf_ink::blank(r);
    if (!b) seenInk = true;
    if (seenInk && b) {
      CHECK(false, "blank() flipped back to blank as ink grew");
      break;
    }
  }

  std::printf("pdf ink: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
