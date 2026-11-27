// Host regression for the PDF engine remedy mapping (src/pdf/PdfModuleRuntime.h).
//
// The engine is bound to a firmware build by appTag = sha256(firmware.elf), so an app-only
// update orphans an installed engine BY DESIGN and `Incompatible` becomes the ordinary state a
// reader meets after upgrading.  Which refusals a download can fix therefore decides whether
// the reader is offered 更新引擎 or told to restart:
//   * Incompatible missing from the set  -> every upgraded device is told "engine not for this
//     firmware" with no way out from where they stand;
//   * NoPsram/MapFailed inside the set -> a memory-state failure sends the user into a
//     1.9 MB download that cannot help, and the real remedy (power cycle) is never named.
// The stored encoding is pinned too: the reader keeps Refusal+1 in a byte, and an off-by-one
// there shows the card for the wrong reason (and offers the wrong remedy).
#include <pdf/PdfModuleRuntime.h>

#include <cstdio>
#include <initializer_list>

namespace {

int failures = 0;
int checks = 0;

void expect(const bool condition, const char* what) {
  ++checks;
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
}

using pdf_module::Refusal;

void testDownloadIsTheRemedyForAMissingOrStaleEngine() {
  expect(pdf_module::refusalIsDownloadable(Refusal::Incompatible), "an engine built for another firmware is re-downloadable");
  expect(pdf_module::refusalIsDownloadable(Refusal::NoPartition), "an absent pdfmod partition is fixable by installing");
  expect(pdf_module::refusalIsDownloadable(Refusal::BadHeader), "a corrupt/absent image is fixable by installing");
}

void testMemoryStateIsNotASoldAsADownload() {
  expect(!pdf_module::refusalIsDownloadable(Refusal::MapFailed), "a failed MMU reservation is not fixed by a download");
  expect(!pdf_module::refusalIsDownloadable(Refusal::NoPsram), "a missing PSRAM heap needs a power cycle, not a fetch");
  expect(!pdf_module::refusalIsDownloadable(Refusal::None), "'no problem' asks for no remedy at all");
}

void testCardEncodingRoundTrips() {
  expect(pdf_module::decodeRefusal(0) == Refusal::None, "a zero card slot means no problem");
  for (const Refusal refusal : {Refusal::NoPartition, Refusal::BadHeader, Refusal::Incompatible, Refusal::MapFailed,
                                Refusal::NoPsram}) {
    const uint8_t stored = pdf_module::encodeRefusal(refusal);
    expect(stored != 0, "an encoded refusal must never collide with the 'no problem' zero");
    expect(pdf_module::decodeRefusal(stored) == refusal, "the card slot round-trips");
  }
  // The card decides its remedy from the decoded value, so the two must agree: pinning this
  // here catches a mapping that only holds for the enum constant and not for what the reader
  // actually stores.
  expect(pdf_module::refusalIsDownloadable(pdf_module::decodeRefusal(pdf_module::encodeRefusal(Refusal::Incompatible))),
         "a stored Incompatible still offers the download");
  expect(!pdf_module::refusalIsDownloadable(pdf_module::decodeRefusal(pdf_module::encodeRefusal(Refusal::NoPsram))),
         "a stored NoPsram still refuses to offer a download");
}

}  // namespace

int main() {
  testDownloadIsTheRemedyForAMissingOrStaleEngine();
  testMemoryStateIsNotASoldAsADownload();
  testCardEncodingRoundTrips();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
