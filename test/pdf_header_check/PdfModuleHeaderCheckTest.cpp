// Host regression for the pre-erase engine-image check (src/pdf/PdfModuleHeaderCheck.h).
//
// The rule this pins is a sequencing one: whatever can be caught before the partition is
// erased must be caught there, because a half-valid image used to pass inspection, wipe the
// only working engine, and then fail the loader's CRC check -- on a board with no BOOT or
// RESET button.  So every geometry field of the header is refused with a distinct reason.
//
// Two sources of truth about "what a valid image looks like" are checked against each other:
// hand-written vectors, and the REAL build/pdfmod.bin produced by
// scripts/build_pdf_module.py (passed as argv[1]; skipped, not silently passed, when absent).
// A predicate that only accepts my own idea of the format would be worthless.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "PdfModuleApi.h"
#include "PdfModuleHeaderCheck.h"

static int failures = 0;
static int checks = 0;

static void check(const bool ok, const std::string& what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("  FAIL: %s\n", what.c_str());
  }
}

using pdf_header::Status;

// The layout scripts/build_pdf_module.py emits: a 64 KiB header sector, then .text aligned
// up to the next 64 KiB boundary, then .rodata, then .data -- file ends at data+dataSize.
static constexpr uint64_t kPartBytes = 0x1E0000;  // the pdfmod partition in the XIP table
static PdfModuleHeader valid() {
  PdfModuleHeader h{};
  h.magic = kPdfModuleMagic;
  h.version = kPdfModuleVersion;
  h.abi = kPdfModuleAbiVersion;
  h.hostApi = kPdfModuleHostApi;
  h.layoutKey = kPdfModuleLayoutKey;
  h.engineRev = 42;
  for (int i = 0; i < 32; ++i) h.buildId[i] = static_cast<uint8_t>(i);
  h.headerBytes = kPdfModuleHeaderBytes;
  h.codeOffset = kPdfModuleHeaderBytes;
  h.entryOffset = 0x32b8;
  h.codeSize = 1133372;
  h.rodataOffset = kPdfModuleHeaderBytes + 0x120000;  // align_up(1133372, 64K)
  h.rodataSize = 580216;
  h.dataOffset = h.rodataOffset + 0x90000;  // align_up(580216, 64K)
  h.dataSize = 28296;
  h.bssSize = 25348;
  return h;
}
static constexpr uint64_t kValidFileBytes = 0x1C6E88;  // the size the vectors above describe

static Status of(const PdfModuleHeader& h, const uint64_t fileBytes = kValidFileBytes,
                 const uint64_t partBytes = kPartBytes) {
  return pdf_header::check(h, fileBytes, partBytes);
}

static void testAcceptsAWellFormedImage() {
  check(of(valid()) == Status::Ok, "the shape the builder emits is accepted");
  // Zero partition size means "unknown": the file checks must still run.
  check(of(valid(), kValidFileBytes, 0) == Status::Ok, "unknown partition size is not a refusal");
  check(std::strcmp(pdf_header::reason(Status::Ok), "ok") == 0, "ok has a reason string");
}

static void testRefusesEachBrokenField() {
  {
    auto h = valid();
    h.magic ^= 1u;
    check(of(h) == Status::BadMagic, "foreign magic refused");
  }
  {
    auto h = valid();
    h.version = 7;
    check(of(h) == Status::BadVersion, "unknown format version refused");
  }
  {
    // The whole point of the ABI field: an engine built for a different ops layout.
    auto h = valid();
    h.abi = kPdfModuleAbiVersion - 1;
    check(of(h) == Status::AbiMismatch, "an engine one ABI behind is refused");
    h.abi = kPdfModuleAbiVersion + 1;
    check(of(h) == Status::AbiMismatch, "and one ahead");
  }
  {
    PdfModuleHeader h = valid();
    h.hostApi = (kPdfModuleHostApiMajor + 1u) << 16;
    check(of(h) == Status::HostApiMismatch, "a newer host API major is incompatible");
  }
  {
    PdfModuleHeader h = valid();
    h.layoutKey ^= 1u;
    check(of(h) == Status::LayoutMismatch, "a different memory layout is incompatible");
    check(pdf_header::isCompatibilityStatus(Status::LayoutMismatch), "layout is fixable by an engine update");
  }
  {
    auto h = valid();
    h.codeOffset = 0;
    check(of(h) == Status::CodeOffsetWrong, "code must start after the header sector");
  }
  {
    auto h = valid();
    h.codeSize = 0;
    check(of(h) == Status::CodeEmpty, "an empty code section is refused");
  }
  {
    auto h = valid();
    h.rodataOffset = h.codeOffset;  // on top of the code
    check(of(h) == Status::SectionsOverlap, "rodata cannot overlap code");
    h = valid();
    h.dataOffset = h.rodataOffset;
    check(of(h) == Status::SectionsOverlap, "data cannot overlap rodata");
    h = valid();
    h.rodataOffset = h.codeOffset + h.codeSize - 1;  // one byte short of aligned-after
    check(of(h) == Status::SectionsOverlap, "sections must not start inside the previous one");
  }
  {
    auto h = valid();
    h.entryOffset = h.codeSize;  // one past the end
    check(of(h) == Status::EntryOutsideCode, "entry must live inside the code section");
    h.entryOffset = h.codeSize + 100000;
    check(of(h) == Status::EntryOutsideCode, "and not far past it either");
  }
  {
    auto h = valid();
    h.bssSize = 0x8000000u;  // 128 MB of zero-fill in an 8 MB PSRAM part
    check(of(h) == Status::BssTooLarge, "absurd .bss is a corrupt header, not a memory plan");
  }
}

static void testRefusesTruncation() {
  // THE regression this file exists for: header intact, payload missing. Before the check
  // moved in front of the erase, this passed inspection and destroyed a working engine.
  check(of(valid(), kValidFileBytes - 1) == Status::FileTooSmall, "one byte short is truncated");
  check(of(valid(), 0x20000) == Status::FileTooSmall, "header + code only is still truncated");
  {
    auto h = valid();
    h.dataSize = 0x400000;  // claims more than the file, and more than the partition
    check(of(h) == Status::FileTooSmall, "an oversized data section is refused");
    check(of(h, kValidFileBytes, kPartBytes) == Status::FileTooSmall,
          "file is checked before partition, so the reason stays truthful");
  }
  {
    auto h = valid();
    const uint64_t fits = h.dataOffset + h.dataSize;
    check(of(h, fits + 4096, fits - 1) == Status::PartitionTooSmall,
          "an image that fits the file but not the partition is refused");
  }
}

static void testReasonStringsCoverEveryStatus() {
  // A refusal with no message is a refusal the user cannot act on; the UI prints these.
  const Status all[] = {Status::Ok, Status::BadMagic, Status::BadVersion, Status::AbiMismatch,
                        Status::HostApiMismatch, Status::LayoutMismatch,
                        Status::CodeOffsetWrong, Status::CodeEmpty, Status::SectionsOverlap,
                        Status::FileTooSmall, Status::PartitionTooSmall, Status::EntryOutsideCode,
                        Status::BssTooLarge};
  for (const Status s : all) {
    const char* text = pdf_header::reason(s);
    check(text != nullptr && text[0] != '\0',
          std::string("reason exists for status ") + std::to_string(static_cast<unsigned>(s)));
  }
}

// The real artifact, so "valid" means valid to the builder and not only to this file.
static void testRealImage(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::printf("  SKIP: %s not found; the real-image check did not run\n", path);
    return;
  }
  in.seekg(0, std::ios::end);
  const uint64_t bytes = static_cast<uint64_t>(in.tellg());
  in.seekg(0);
  PdfModuleHeader h{};
  in.read(reinterpret_cast<char*>(&h), sizeof(h));
  check(static_cast<bool>(in), "read the header of the built image");
  const Status status = pdf_header::check(h, bytes, kPartBytes);
  check(status == Status::Ok,
        std::string("build/pdfmod.bin passes its own pre-erase check (") + pdf_header::reason(status) + ")");
  std::printf("  real image: %llu bytes, abi=%u code=%u rodata=%u data=%u bss=%u -> %s\n",
              static_cast<unsigned long long>(bytes), h.abi, h.codeSize, h.rodataSize, h.dataSize,
              h.bssSize, pdf_header::reason(status));
  // And the same image, truncated by one byte, must be refused: the check is not vacuous on
  // real data just because it happens to be well-formed.
  check(pdf_header::check(h, bytes - 1, kPartBytes) == Status::FileTooSmall,
        "the real image truncated by one byte is refused");
}

int main(int argc, char** argv) {
  testAcceptsAWellFormedImage();
  testRefusesEachBrokenField();
  testRefusesTruncation();
  testReasonStringsCoverEveryStatus();
  testRealImage(argc > 1 ? argv[1] : "build/pdfmod.bin");
  std::printf("%s: %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
  return failures == 0 ? 0 : 1;
}
