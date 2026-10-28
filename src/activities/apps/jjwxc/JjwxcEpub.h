#pragma once

#include <HalStorage.h>
#include <JjwxcBookCrypto.h>
#include <WeReadStore.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "JjwxcStore.h"

// Turns the JJWXC chapter cache into the one cumulative EPUB per novel.
//
// Reused from lib/WeReadWebApi rather than duplicated (all of it is public API
// and already linked into every image that builds this app):
//   WeReadStore::StoreOnlyZipWriter   store-only ZIP, central directory parked on
//                                     SD, so RAM cost is the caller's 4 KB buffer
//   WeReadStore::IndexWriter          12-byte-header index + .part + atomic replace
//   WeReadStore::atomicReplace        rename-with-backup used for the finished EPUB
//   WeReadStore::looksLikeZip         EPUB/ZIP sanity check before the replace
//   WeReadXhtmlCodec::writeLiteral    one SD transaction per literal run
//
// NOT reused, and why:
//   WeReadClient's writePackageFiles()/packageBook() are file-static and coupled
//   to WeReadStore::BookRecord, the WRI2/WIP1 image-work index and WeRead's Error
//   enum; the JJWXC equivalents below are ~a third of the size because a JJWXC
//   novel has no images and no footnotes.
//   WeReadXhtmlCodec::sanitizeChapter() emits one <p> with <br/> per line and
//   interleaves a <!--source-offset--> comment per text run for WeRead's progress
//   mapping. JJWXC has no progress mapping, and those markers would inflate every
//   chapter several-fold, so ChapterWriter below does the minimal job instead:
//   XML-escape plus paragraph splitting.
namespace JjwxcEpub {

enum class Result : uint8_t {
  Ok = 0,
  // Not named `Storage`: that identifier is a macro expanding to
  // HalStorage::getInstance(), so `Storage,` here would not compile. The same
  // trap is documented for FailKind in JjwxcActivity.h.
  StorageWrite,  // an SD read/write failed
  Integrity,     // the assembled ZIP did not validate
  Empty,         // no chapter on the card to package
  Cancelled,
};

using CancelCheck = bool (*)(void* ctx);
using ProgressCallback = void (*)(void* ctx);

// Escapes `src` into `dst` for XML text content, stopping before it could emit a
// partial entity; the result is always NUL-terminated and the return value is its
// length. Exposed so the packaging passes and the chapter writer share one rule.
//
// Used instead of WeReadXhtmlCodec::writeXmlText(), which emits one
// HalFile::write() per byte: over a 4000-chapter nav that is hundreds of
// thousands of storage-mutex round trips for the titles alone.
size_t xmlEscapeInto(char* dst, size_t dstSize, const char* src);

// Streams a decoded chapter body straight into `chapters/<chapterId>.xhtml`.
//
// One writer per chapter, opened and closed inside that chapter's iteration: the
// file never spans two fetches, so a mis-routed response body cannot end up in
// the wrong chapter file (the failure mode abort() exists for).
class ChapterWriter {
 public:
  // Writes encrypted to `finalPath + ".part"`; finish() patches the header and
  // atomically replaces. The partial file is ciphertext from the first byte, so
  // a crash cannot leave a readable chapter behind.
  bool begin(const std::string& finalPath, const char* title);
  // Feed decoded UTF-8 as it arrives from jjwxc::fetchChapter().
  bool feed(const char* data, size_t len);
  // Closes the document, replaces the target and reports its plaintext size.
  bool finish(uint32_t& outBytes);
  // Discards the partial file. Safe to call when not active.
  void abort();
  bool active() const { return active_; }

 private:
  bool put(char c);
  bool putLiteral(const char* text);
  bool putEscaped(char c);
  bool flushOut();
  bool closeParagraph();

  HalFile file_;
  std::string finalPath_;
  std::string partPath_;
  // Batches the escaped output: a byte-at-a-time HalFile::write() would take the
  // storage mutex and hit the card once per character.
  static constexpr size_t kOutBytes = 512;
  char out_[kOutBytes] = {};
  size_t outLen_ = 0;
  bool paragraphOpen_ = false;
  bool active_ = false;
  // Encryption state. The header is written with zero length/CRC at begin and
  // patched in place at finish, after the ciphertext has been streamed.
  jjwxc::BookCrypto::CtrStream stream_ = {};
  uint8_t nonce_[jjwxc::BookCrypto::kNonceLen] = {};
  uint32_t plainBytes_ = 0;
  uint32_t crc_ = 0xFFFFFFFFu;
};

// A chapter this account cannot read gets an explicit stub instead of a body, so
// the EPUB says why a chapter is empty rather than silently showing the previous
// chapter's text.
bool writePlaceholder(const std::string& finalPath, const char* title, const char* note);

// Regenerates nav.xhtml + content.opf from toc.bin and assembles every cached
// chapter into `finalBookPath(novel)`. Every chapter and the cover are encrypted
// as AES-128-CBC entries (the reader's protected-EPUB path); `contentKey` must be
// the raw 16-byte key whose device-wrapped form is already at `keyPath` (written
// by ensureBookContentKey on the UI task). `buffer` is the ZIP writer's copy
// buffer (>= 1 KB; 4 KB matches WeRead's kIoBufferSize).
//
// Chapters whose state is Missing, or whose file has gone away, are left out of
// the manifest, the nav and the spine alike -- a gap in the middle of a novel
// therefore produces a shorter book, never a hole pointing at a missing entry.
Result packageBook(const JjwxcStore::NovelRecord& novel, uint8_t* buffer, size_t bufferSize,
                   const uint8_t (&contentKey)[16], const std::string& keyPath, CancelCheck isCancelled,
                   void* cancelCtx, ProgressCallback onProgress, void* progressCtx);

}  // namespace JjwxcEpub
