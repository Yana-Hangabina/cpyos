#include "JjwxcEpub.h"

#include <I18n.h>
#include <JjwxcBookCrypto.h>
#include <Logging.h>
#include <Memory.h>
#include <WeReadProtocol.h>
#include <WeReadXhtmlCodec.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kTag = "JJWXC";

// The three fixed EPUB entries. Identical in spirit to WeRead's constants
// (WeReadClient.cpp:1749-1760); styles.css is byte-for-byte the same rule.
constexpr char kMimetype[] = "application/epub+zip";
constexpr char kContainer[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
    "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
    "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
constexpr char kStylesheet[] = "p{text-indent:2em}";
constexpr char kXhtmlOpen[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                              "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>";
constexpr char kNavOpen[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                            "<html xmlns=\"http://www.w3.org/1999/xhtml\" "
                            "xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>";
constexpr char kOpfOpen[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                            "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
                            "unique-identifier=\"book-id\"><metadata "
                            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"book-id\">";

// A JJWXC novel is Chinese prose; the TXT pipeline used to hardcode "en" into
// book.bin, which is one of the things this packaging fixes.
constexpr char kLanguage[] = "zh-CN";

// Longest composed nav line: prefix + an escaped 95-byte title + suffix. Only
// the five XML metacharacters expand (1 -> 5 bytes), so 512 leaves room for the
// theoretical all-ampersand title without ever truncating a real one.
constexpr size_t kLineBytes = 512;

// ---------------------------------------------------------------------------
// Protected-EPUB entry writer
//
// The reader's ContentProtection path decrypts an entry as:
//   16-byte IV || AES-128-CBC(raw deflate stream || PKCS#7)
// and inflates it with windowBits -15. The vendored miniz is built without a
// deflate API (MinizConfig.h), so the raw stream is made of stored (BTYPE=00)
// blocks: 5 header bytes per 65535-byte chunk, no compressor required. The
// format was verified end-to-end against the real ProtectedBook host harness
// (cpyos/src/protected_epub/), not inferred.
// ---------------------------------------------------------------------------
constexpr size_t kDeflateBlock = 0xFFFF;

size_t storedDeflateSize(const size_t n) {
  const size_t blocks = n == 0 ? 1 : (n + kDeflateBlock - 1) / kDeflateBlock;
  return n + blocks * 5;
}

size_t protectedEntrySize(const size_t n) {
  const size_t deflated = storedDeflateSize(n);
  return 16 + (deflated / 16 + 1) * 16;  // IV + ciphertext of the padded stream
}

size_t writeStoredDeflate(const uint8_t* src, const size_t n, uint8_t* out) {
  if (n == 0) {
    out[0] = 0x01;  // BFINAL=1, BTYPE=00
    out[1] = 0;
    out[2] = 0;
    out[3] = 0xFF;
    out[4] = 0xFF;
    return 5;
  }
  size_t pos = 0;
  size_t o = 0;
  while (pos < n) {
    const size_t chunk = std::min(n - pos, kDeflateBlock);
    const uint16_t len = static_cast<uint16_t>(chunk);
    const uint16_t nlen = static_cast<uint16_t>(~len);
    out[o++] = (pos + chunk >= n) ? 0x01 : 0x00;
    out[o++] = static_cast<uint8_t>(len & 0xFF);
    out[o++] = static_cast<uint8_t>(len >> 8);
    out[o++] = static_cast<uint8_t>(nlen & 0xFF);
    out[o++] = static_cast<uint8_t>(nlen >> 8);
    memcpy(out + o, src + pos, chunk);
    o += chunk;
    pos += chunk;
  }
  return o;
}

uint32_t plainCrc(const uint8_t* data, const size_t n) {
  return WeReadProtocol::crc32Update(0xFFFFFFFF, data, n) ^ 0xFFFFFFFF;
}

bool buildProtectedEntry(const uint8_t* plain, const size_t n, const uint8_t (&key)[16], uint8_t* out,
                         const size_t outCap, size_t& storedLen) {
  const size_t need = protectedEntrySize(n);
  storedLen = 0;
  if (out == nullptr || outCap < need) return false;
  esp_fill_random(out, 16);
  uint8_t iv[16];
  memcpy(iv, out, 16);
  const size_t deflated = writeStoredDeflate(plain, n, out + 16);
  const size_t cipherLen = need - 16;
  const size_t pad = cipherLen - deflated;
  memset(out + 16 + deflated, static_cast<int>(pad), pad);

  // The key schedule is expanded once here instead of once per 16-byte block;
  // a 4000-chapter book otherwise pays hundreds of thousands of expansions.
  if (!jjwxc::BookCrypto::aes128CbcEncrypt(key, iv, out + 16, cipherLen, out + 16)) return false;
  storedLen = need;
  return true;
}

bool readFileToPsram(const std::string& path, memory::ByteBuffer& buffer, size_t& len) {
  len = 0;
  HalFile file;
  if (!Storage.openFileForRead(kTag, path, file)) return false;
  const uint64_t size = file.fileSize64();
  // A chapter or cover above 8 MB is not a book this app produced.
  if (size > 8u * 1024u * 1024u) return false;
  buffer = memory::makePsramByteBufferUninitializedNoThrow(static_cast<size_t>(size));
  if (!buffer && size != 0) return false;
  if (size != 0 && file.read(buffer.get(), static_cast<size_t>(size)) != static_cast<int>(size)) return false;
  len = static_cast<size_t>(size);
  return true;
}

bool readChapterToPsram(const std::string& path, memory::ByteBuffer& buffer, size_t& len) {
  if (!readFileToPsram(path, buffer, len)) return false;
  const uint8_t version = jjwxc::BookCrypto::blobVersion(buffer.get(), len);
  if (version == 0) {
    // A JJX1 magic with an unknown version is not plaintext; refuse rather than
    // hand ciphertext to the packager.
    if (len >= 4 && memcmp(buffer.get(), "JJX1", 4) == 0) return false;
    // First protected repackage after the cache-encryption upgrade: seal the
    // plaintext source now so later card copies do not expose it. Best effort;
    // the buffer already holds what the package needs.
    if (!JjwxcStore::sealPlainBufferToFile(path, buffer.get(), len)) {
      LOG_ERR(kTag, "chapter cache not sealed: %s", path.c_str());
    }
    return true;
  }
  uint8_t key[jjwxc::BookCrypto::kKeyLen] = {};
  if (version == jjwxc::BookCrypto::kVersionLegacy) {
    if (!JjwxcStore::ensureLegacyCacheCrypto(key)) return false;
  } else {
    if (!JjwxcStore::ensureCacheCrypto(key)) return false;
  }
  size_t plainLen = 0;
  if (!jjwxc::BookCrypto::decryptBookBlob(key, buffer.get(), len, buffer.get(), len, plainLen)) {
    LOG_ERR(kTag, "cache decrypt failed: %s", path.c_str());
    return false;
  }
  len = plainLen;
  if (version == jjwxc::BookCrypto::kVersionLegacy) {
    // Migrate on first read: re-encrypt under the v2 (device-secret) key so the
    // public-MAC path stops being useful for this file.
    if (!JjwxcStore::sealPlainBufferToFile(path, buffer.get(), len)) {
      LOG_ERR(kTag, "legacy chapter not re-sealed: %s", path.c_str());
    }
  }
  return true;
}

bool copySmallFile(const std::string& source, const std::string& destination) {
  HalFile in;
  if (!Storage.openFileForRead(kTag, source, in)) return false;
  HalFile out;
  if (!Storage.openFileForWrite(kTag, destination, out)) return false;
  uint8_t buffer[64];
  while (true) {
    const int got = in.read(buffer, sizeof(buffer));
    if (got < 0) return false;
    if (got == 0) break;
    if (out.write(buffer, static_cast<size_t>(got)) != static_cast<size_t>(got)) return false;
  }
  out.flush();
  return true;
}

// The manifest is plaintext and small per entry, but a 4000-chapter novel makes
// it ~1.2 MB. Stream it to a temp file rather than materializing it whole.
bool writeEncryptionManifest(const std::string& path, const uint32_t count, const bool hasCover,
                             const bool coverPng) {
  HalFile file;
  if (!Storage.openFileForWrite(kTag, path, file)) return false;
  const char header[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<encryption xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" "
      "xmlns:enc=\"http://www.w3.org/2001/04/xmlenc#\">\n";
  if (file.write(header, sizeof(header) - 1) != sizeof(header) - 1) return false;
  char line[320];
  for (uint32_t i = 0; i < count; ++i) {
    const int n = snprintf(line, sizeof(line),
                           "<enc:EncryptedData><enc:EncryptionMethod "
                           "Algorithm=\"http://www.w3.org/2001/04/xmlenc#aes128-cbc\"/>"
                           "<enc:CipherData><enc:CipherReference URI=\"OEBPS/ch%06lu.xhtml\"/>"
                           "</enc:CipherData></enc:EncryptedData>\n",
                           static_cast<unsigned long>(i));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line) ||
        file.write(line, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
      return false;
    }
  }
  if (hasCover) {
    const int n = snprintf(line, sizeof(line),
                           "<enc:EncryptedData><enc:EncryptionMethod "
                           "Algorithm=\"http://www.w3.org/2001/04/xmlenc#aes128-cbc\"/>"
                           "<enc:CipherData><enc:CipherReference URI=\"OEBPS/cover.%s\"/>"
                           "</enc:CipherData></enc:EncryptedData>\n",
                           coverPng ? "png" : "jpg");
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line) ||
        file.write(line, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
      return false;
    }
  }
  const char footer[] = "</encryption>\n";
  if (file.write(footer, sizeof(footer) - 1) != sizeof(footer) - 1) return false;
  file.flush();
  return true;
}

}  // namespace

namespace JjwxcEpub {

size_t xmlEscapeInto(char* dst, const size_t dstSize, const char* src) {
  if (!dst || dstSize == 0) return 0;
  dst[0] = '\0';
  if (!src) return 0;
  size_t n = 0;
  for (const auto* p = reinterpret_cast<const unsigned char*>(src); *p; ++p) {
    char single[2] = {static_cast<char>(*p), '\0'};
    const char* escaped = single;
    switch (*p) {
      case '&': escaped = "&amp;"; break;
      case '<': escaped = "&lt;"; break;
      case '>': escaped = "&gt;"; break;
      case '"': escaped = "&quot;"; break;
      case '\'': escaped = "&apos;"; break;
      default: break;
    }
    const size_t len = strlen(escaped);
    if (n + len + 1 > dstSize) break;
    memcpy(dst + n, escaped, len);
    n += len;
  }
  dst[n] = '\0';
  return n;
}

namespace {

bool literal(HalFile& file, const char* text) { return WeReadXhtmlCodec::writeLiteral(file, text); }

// The cover entry is named after what the bytes actually are: Epub picks its
// decoder from the href's extension (lib/Epub/Epub.cpp:62), so a PNG saved under
// a .jpg name would fail to become a thumbnail.
bool coverIsPng(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead(kTag, path, file)) return false;
  uint8_t magic[8] = {};
  return file.read(magic, sizeof(magic)) == static_cast<int>(sizeof(magic)) &&
         memcmp(magic, "\x89PNG\r\n\x1a\n", sizeof(magic)) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// ChapterWriter
// ---------------------------------------------------------------------------

bool ChapterWriter::flushOut() {
  if (outLen_ == 0) return true;
  if (plainBytes_ > 0xFFFFFFFFu - static_cast<uint32_t>(outLen_)) {
    LOG_ERR(kTag, "chapter too large: %s", partPath_.c_str());
    return false;
  }
  const size_t wanted = outLen_;
  crc_ = jjwxc::BookCrypto::crc32Update(crc_, reinterpret_cast<const uint8_t*>(out_), outLen_);
  plainBytes_ += static_cast<uint32_t>(outLen_);
  jjwxc::BookCrypto::CtrStream::xorBytes(stream_, reinterpret_cast<const uint8_t*>(out_),
                                         reinterpret_cast<uint8_t*>(out_), outLen_);
  const size_t written = file_.write(reinterpret_cast<const uint8_t*>(out_), wanted);
  outLen_ = 0;
  if (written == wanted) return true;
  LOG_ERR(kTag, "short chapter write (%u/%u): %s", (unsigned)written, (unsigned)wanted, partPath_.c_str());
  return false;
}

bool ChapterWriter::put(const char c) {
  if (outLen_ >= kOutBytes && !flushOut()) return false;
  out_[outLen_++] = c;
  return true;
}

bool ChapterWriter::putLiteral(const char* text) {
  if (!text) return true;
  const size_t len = strlen(text);
  size_t done = 0;
  while (done < len) {
    if (outLen_ >= kOutBytes && !flushOut()) return false;
    const size_t take = std::min(len - done, kOutBytes - outLen_);
    memcpy(out_ + outLen_, text + done, take);
    outLen_ += take;
    done += take;
  }
  return true;
}

bool ChapterWriter::putEscaped(const char c) {
  switch (c) {
    case '&': return putLiteral("&amp;");
    case '<': return putLiteral("&lt;");
    case '>': return putLiteral("&gt;");
    default: return put(c);
  }
}

bool ChapterWriter::closeParagraph() {
  if (!paragraphOpen_) return true;
  paragraphOpen_ = false;
  return putLiteral("</p>");
}

bool ChapterWriter::begin(const std::string& finalPath, const char* title) {
  abort();
  finalPath_ = finalPath;
  partPath_ = finalPath + ".part";
  uint8_t key[jjwxc::BookCrypto::kKeyLen] = {};
  if (!JjwxcStore::ensureCacheCrypto(key)) {
    // Fail closed: without the device secret we can neither seal nor read the
    // cache. Falling back to plaintext would silently re-open the copy path.
    LOG_ERR(kTag, "cannot derive chapter cache key: %s", finalPath.c_str());
    return false;
  }
  esp_fill_random(nonce_, sizeof(nonce_));
  jjwxc::BookCrypto::CtrStream::init(stream_, key, nonce_);
  plainBytes_ = 0;
  crc_ = 0xFFFFFFFFu;

  if (Storage.exists(partPath_.c_str())) Storage.remove(partPath_.c_str());
  if (!Storage.openFileForWrite(kTag, partPath_, file_)) {
    LOG_ERR(kTag, "cannot open %s", partPath_.c_str());
    return false;
  }
  uint8_t header[jjwxc::BookCrypto::kHeaderLen];
  jjwxc::BookCrypto::writeHeader(header, nonce_, 0, 0);
  if (file_.write(header, sizeof(header)) != sizeof(header)) {
    LOG_ERR(kTag, "cannot write chapter header: %s", partPath_.c_str());
    abort();
    return false;
  }
  active_ = true;
  outLen_ = 0;
  paragraphOpen_ = false;

  if (!putLiteral(kXhtmlOpen)) {
    abort();
    return false;
  }
  // The chapter name is the document title *and* the visible heading: a plain
  // text body carries no heading of its own, and <h1> is one of the reader's
  // recognised header tags (ChapterHtmlSlimParser.cpp:50).
  for (const char* p = title; p && *p; ++p) {
    if (!putEscaped(*p)) {
      abort();
      return false;
    }
  }
  if (!putLiteral("</title></head><body><h1>")) {
    abort();
    return false;
  }
  for (const char* p = title; p && *p; ++p) {
    if (!putEscaped(*p)) {
      abort();
      return false;
    }
  }
  if (!putLiteral("</h1>")) {
    abort();
    return false;
  }
  return true;
}

bool ChapterWriter::feed(const char* data, const size_t len) {
  if (!active_) return false;
  for (size_t i = 0; i < len; ++i) {
    const char c = data[i];
    if (c == '\r') continue;
    if (c == '\n') {
      // Any run of newlines ends one paragraph and starts the next, so both the
      // "one paragraph per line" and the "blank line between paragraphs" shapes
      // the API has been seen to return come out as separate <p> elements.
      if (!closeParagraph()) return false;
      continue;
    }
    // Control characters other than tab are not legal XML 1.0 and would make the
    // whole document unparseable.
    if (static_cast<unsigned char>(c) < 0x20 && c != '\t') continue;
    if (!paragraphOpen_) {
      if (!putLiteral("<p>")) return false;
      paragraphOpen_ = true;
    }
    if (!putEscaped(c)) return false;
  }
  return true;
}

bool ChapterWriter::finish(uint32_t& outBytes) {
  outBytes = 0;
  if (!active_) return false;
  const bool ok = closeParagraph() && putLiteral("</body></html>") && flushOut();
  uint8_t header[jjwxc::BookCrypto::kHeaderLen];
  jjwxc::BookCrypto::writeHeader(header, nonce_, plainBytes_, crc_ ^ 0xFFFFFFFFu);
  // The header went out with a zero length/CRC before the body; patch it now
  // that the plaintext size and CRC are known. The header is not part of the
  // keystream, so rewriting it does not touch the ciphertext.
  const bool patched = ok && file_.seek64(0) && file_.write(header, sizeof(header)) == sizeof(header);
  file_.flush();
  // Explicit close: the handle is a member and atomicReplace() renames over the
  // destination (AGENTS.md rule #5).
  file_.close();
  active_ = false;
  if (!ok || !patched) {
    LOG_ERR(kTag, "chapter write failed: %s", finalPath_.c_str());
    Storage.remove(partPath_.c_str());
    return false;
  }
  if (!WeReadStore::atomicReplace(partPath_, finalPath_)) {
    LOG_ERR(kTag, "atomicReplace failed: %s", finalPath_.c_str());
    Storage.remove(partPath_.c_str());
    return false;
  }
  outBytes = plainBytes_;
  return true;
}

void ChapterWriter::abort() {
  if (file_.isOpen()) file_.close();
  active_ = false;
  paragraphOpen_ = false;
  outLen_ = 0;
  if (!partPath_.empty() && Storage.exists(partPath_.c_str())) Storage.remove(partPath_.c_str());
}

bool writePlaceholder(const std::string& finalPath, const char* title, const char* note) {
  // Deliberately goes through the same writer as a real chapter: the stub is
  // built from this chapter's own title plus a fixed localised note, so there is
  // no path by which another chapter's body can end up in this file.
  ChapterWriter writer;
  if (!writer.begin(finalPath, title)) return false;
  const size_t noteLen = note ? strlen(note) : 0;
  if (!writer.feed(note ? note : "", noteLen)) {
    writer.abort();
    return false;
  }
  uint32_t bytes = 0;
  if (!writer.finish(bytes)) return false;
  return true;
}

// ---------------------------------------------------------------------------
// Packaging
// ---------------------------------------------------------------------------

namespace {

// Everything but Missing has a file on the card worth putting in the book.
// Every catalogue chapter belongs in the EPUB now. A chapter without a body gets
// a generated placeholder page instead of being omitted: the reader's TOC is then
// the full catalogue, with "not cached / unpurchased / locked" visible before the
// user tries to read it. `hasBody` below is what still decides file vs placeholder.
bool hasBody(const char* novelId, const JjwxcStore::TocRecord& rec) {
  return JjwxcStore::isPackaged(rec.state) &&
         Storage.exists(JjwxcStore::chapterPath(novelId, rec.chapterId).c_str());
}

}  // namespace

Result packageBook(const JjwxcStore::NovelRecord& novel, uint8_t* buffer, const size_t bufferSize,
                   const uint8_t (&contentKey)[16], const std::string& keyPath, const CancelCheck isCancelled,
                   void* cancelCtx, const ProgressCallback onProgress, void* progressCtx) {
  if (!buffer || bufferSize < 1024 || novel.novelId[0] == '\0') return Result::StorageWrite;

  JjwxcStore::TocReader toc;
  if (!toc.open(novel.novelId)) return Result::StorageWrite;
  const uint32_t count = toc.count();
  if (count == 0) return Result::Empty;

  const std::string navPath = JjwxcStore::navPartPath(novel.novelId);
  const std::string opfPath = JjwxcStore::contentPartPath(novel.novelId);
  const std::string partPath = JjwxcStore::bookPartPath(novel.novelId);
  const std::string centralPath = JjwxcStore::centralPartPath(novel.novelId);
  const std::string manifestPath = JjwxcStore::novelDirectory(novel.novelId) + "/encryption.part";
  const std::string finalPath = JjwxcStore::finalBookPath(novel);
  const std::string coverPath = JjwxcStore::coverSourcePath(novel.novelId);
  const bool hasCover = Storage.exists(coverPath.c_str());
  const bool coverPng = hasCover && coverIsPng(coverPath);

  // The key was wrapped on the UI task. The path is derived from the persisted
  // title, which can change between the UI snapshot and this job; keep the key
  // next to the file this run actually writes.
  const std::string expectedKeyPath = finalPath + ".key";
  if (keyPath.empty()) return Result::StorageWrite;
  if (keyPath != expectedKeyPath && !copySmallFile(keyPath, expectedKeyPath)) {
    LOG_ERR(kTag, "cannot place content key for %s", finalPath.c_str());
    return Result::StorageWrite;
  }

  // A failed pass leaves both part files behind otherwise, and the next run's
  // reclaim would only catch them on entry.
  const auto dropParts = [&navPath, &opfPath, &manifestPath] {
    if (Storage.exists(navPath.c_str())) Storage.remove(navPath.c_str());
    if (Storage.exists(opfPath.c_str())) Storage.remove(opfPath.c_str());
    if (Storage.exists(manifestPath.c_str())) Storage.remove(manifestPath.c_str());
  };

  if (Storage.exists(navPath.c_str())) Storage.remove(navPath.c_str());
  if (Storage.exists(opfPath.c_str())) Storage.remove(opfPath.c_str());
  if (Storage.exists(manifestPath.c_str())) Storage.remove(manifestPath.c_str());
  HalFile nav;
  HalFile opf;
  if (!Storage.openFileForWrite(kTag, navPath, nav) || !Storage.openFileForWrite(kTag, opfPath, opf)) {
    LOG_ERR(kTag, "cannot open nav/opf parts for %s", novel.novelId);
    // One of the two may already be open; close both before unlinking (rule #5).
    if (nav.isOpen()) nav.close();
    if (opf.isOpen()) opf.close();
    dropParts();
    return Result::StorageWrite;
  }

  char title[512];
  xmlEscapeInto(title, sizeof(title), novel.title);
  char author[128];
  xmlEscapeInto(author, sizeof(author), novel.author);

  char line[kLineBytes];
  Result result = Result::Ok;
  uint32_t included = 0;

  // ---- pass 1: nav entries + OPF manifest ---------------------------------
  const bool navOk = literal(nav, kNavOpen) && literal(nav, title) &&
                     literal(nav, "</title></head><body><nav epub:type=\"toc\"><ol>");
  const bool opfOk = literal(opf, kOpfOpen) && literal(opf, "jjwxc-") && literal(opf, novel.novelId) &&
                     literal(opf, "</dc:identifier><dc:title>") && literal(opf, title) && literal(opf, "</dc:title>");
  if (!navOk || !opfOk) result = Result::StorageWrite;
  if (result == Result::Ok && author[0] != '\0') {
    if (!literal(opf, "<dc:creator>") || !literal(opf, author) || !literal(opf, "</dc:creator>")) {
      result = Result::StorageWrite;
    }
  }
  if (result == Result::Ok) {
    int n = snprintf(line, sizeof(line), "<dc:language>%s</dc:language></metadata><manifest>"
                                         "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" "
                                         "properties=\"nav\"/>"
                                         "<item id=\"style\" href=\"styles.css\" media-type=\"text/css\"/>",
                     kLanguage);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line) || !literal(opf, line)) result = Result::StorageWrite;
  }
  if (result == Result::Ok && hasCover) {
    // The href must carry the same extension as the ZIP entry: Epub picks the
    // cover decoder from that extension (lib/Epub/Epub.cpp:62), while the
    // media-type attribute is what ContentOpfParser matches on.
    const int n = snprintf(line, sizeof(line),
                           "<item id=\"cover-image\" href=\"cover.%s\" media-type=\"image/%s\" "
                           "properties=\"cover-image\"/>",
                           coverPng ? "png" : "jpg", coverPng ? "png" : "jpeg");
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line) || !literal(opf, line)) result = Result::StorageWrite;
  }

  for (uint32_t i = 0; result == Result::Ok && i < count; ++i) {
    if (isCancelled && isCancelled(cancelCtx)) {
      result = Result::Cancelled;
      break;
    }
    JjwxcStore::TocRecord rec;
    if (!toc.read(i, rec)) {
      result = Result::StorageWrite;
      break;
    }
    const std::string source = JjwxcStore::chapterPath(novel.novelId, rec.chapterId);
    const bool real = hasBody(novel.novelId, rec);

    char entry[32];
    int n = snprintf(entry, sizeof(entry), "ch%06lu.xhtml", static_cast<unsigned long>(i));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(entry)) {
      result = Result::StorageWrite;
      break;
    }
    n = snprintf(line, sizeof(line), "<li><a href=\"%s\">", entry);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line)) {
      result = Result::StorageWrite;
      break;
    }
    size_t used = static_cast<size_t>(n);
    used += xmlEscapeInto(line + used, sizeof(line) - used, rec.title);
    // The reader's Contents panel shows cached/unpurchased state with its own
    // tail icons now; do not append "（已缓存）" / "（未缓存）" / … to the nav
    // title. The placeholder page still explains a missing body after opening.
    (void)real;
    const int tail = snprintf(line + used, sizeof(line) - used, "</a></li>");
    if (tail <= 0 || used + static_cast<size_t>(tail) >= sizeof(line) || !literal(nav, line)) {
      result = Result::StorageWrite;
      break;
    }
    n = snprintf(line, sizeof(line), "<item id=\"ch%06lu\" href=\"%s\" media-type=\"application/xhtml+xml\"/>",
                 static_cast<unsigned long>(i), entry);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line) || !literal(opf, line)) {
      result = Result::StorageWrite;
      break;
    }
    ++included;
  }

  if (result == Result::Ok && included == 0) {
    // Nothing on the card: an empty EPUB would be a book that opens to no pages.
    result = Result::Empty;
  }
  if (result == Result::Ok && (!literal(nav, "</ol></nav></body></html>") || !literal(opf, "</manifest><spine>"))) {
    result = Result::StorageWrite;
  }

  // ---- pass 2: spine ------------------------------------------------------
  for (uint32_t i = 0; result == Result::Ok && i < count; ++i) {
    if (isCancelled && isCancelled(cancelCtx)) {
      result = Result::Cancelled;
      break;
    }
    JjwxcStore::TocRecord rec;
    if (!toc.read(i, rec)) {
      result = Result::StorageWrite;
      break;
    }
    const int n = snprintf(line, sizeof(line), "<itemref idref=\"ch%06lu\"/>", static_cast<unsigned long>(i));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line) || !literal(opf, line)) {
      result = Result::StorageWrite;
      break;
    }
  }
  if (result == Result::Ok && !literal(opf, "</spine></package>")) result = Result::StorageWrite;

  nav.flush();
  opf.flush();
  // Explicit close before the ZIP pass reopens both files for reading (rule #5's
  // "before reopen" case): SdFat would otherwise hand the reader a handle whose
  // cached size predates everything just written.
  nav.close();
  opf.close();
  toc.close();
  if (result != Result::Ok) {
    dropParts();
    return result;
  }

  // The manifest is plaintext (ProtectedBook probes it before it has the key).
  // Every spine entry is encrypted, including placeholders, so the list is the
  // whole catalogue rather than only the readable chapters.
  if (!writeEncryptionManifest(manifestPath, count, hasCover, coverPng)) {
    dropParts();
    return Result::StorageWrite;
  }

  // ---- pass 3: assemble ---------------------------------------------------
  if (!Storage.ensureDirectoryExists(JjwxcStore::kOutputDir)) return Result::StorageWrite;
  WeReadStore::StoreOnlyZipWriter zip;
  if (!zip.begin(partPath, centralPath, buffer, bufferSize)) {
    dropParts();
    return Result::StorageWrite;
  }
  const bool fixedOk =
      zip.addBuffer("mimetype", reinterpret_cast<const uint8_t*>(kMimetype), strlen(kMimetype)) &&
      zip.addBuffer("META-INF/container.xml", reinterpret_cast<const uint8_t*>(kContainer), strlen(kContainer)) &&
      zip.addFile("META-INF/encryption.xml", manifestPath, nullptr, nullptr) &&
      zip.addFile("OEBPS/content.opf", opfPath, nullptr, nullptr) &&
      zip.addFile("OEBPS/nav.xhtml", navPath, nullptr, nullptr) &&
      zip.addBuffer("OEBPS/styles.css", reinterpret_cast<const uint8_t*>(kStylesheet), strlen(kStylesheet));
  if (!fixedOk) {
    zip.abort();
    dropParts();
    return Result::StorageWrite;
  }

  if (hasCover) {
    memory::ByteBuffer cover;
    size_t coverLen = 0;
    if (!readFileToPsram(coverPath, cover, coverLen)) {
      zip.abort();
      dropParts();
      return Result::StorageWrite;
    }
    const size_t need = protectedEntrySize(coverLen);
    auto stored = memory::makePsramByteBufferNoThrow(need);
    size_t storedLen = 0;
    if (!stored || !buildProtectedEntry(cover.get(), coverLen, contentKey, stored.get(), need, storedLen) ||
        !zip.addStoredEntry(coverPng ? "OEBPS/cover.png" : "OEBPS/cover.jpg", stored.get(), storedLen, coverLen,
                            plainCrc(cover.get(), coverLen))) {
      zip.abort();
      dropParts();
      return Result::StorageWrite;
    }
  }

  JjwxcStore::TocReader spine;
  if (!spine.open(novel.novelId)) {
    zip.abort();
    dropParts();
    return Result::StorageWrite;
  }
  Result packResult = Result::Ok;
  for (uint32_t i = 0; packResult == Result::Ok && i < spine.count(); ++i) {
    if (isCancelled && isCancelled(cancelCtx)) {
      packResult = Result::Cancelled;
      break;
    }
    JjwxcStore::TocRecord rec;
    if (!spine.read(i, rec)) {
      packResult = Result::StorageWrite;
      break;
    }
    const std::string source = JjwxcStore::chapterPath(novel.novelId, rec.chapterId);
    const bool real = hasBody(novel.novelId, rec);
    char entryName[48];
    const int n = snprintf(entryName, sizeof(entryName), "OEBPS/ch%06lu.xhtml", static_cast<unsigned long>(i));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(entryName)) {
      packResult = Result::StorageWrite;
      break;
    }

    const uint8_t* plain = nullptr;
    size_t plainLen = 0;
    char placeholder[768];
    memory::ByteBuffer chapter;
    if (real) {
      if (!readChapterToPsram(source, chapter, plainLen)) {
        packResult = Result::StorageWrite;
        break;
      }
      plain = chapter.get();
    } else {
      int len = snprintf(placeholder, sizeof(placeholder),
                         "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                         "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>%s</title></head>"
                         "<body><p>%s</p></body></html>",
                         tr(STR_JJWXC_NAV_MISSING), tr(STR_JJWXC_STUB_MISSING));
      if (len <= 0 || static_cast<size_t>(len) >= sizeof(placeholder)) {
        packResult = Result::StorageWrite;
        break;
      }
      plain = reinterpret_cast<const uint8_t*>(placeholder);
      plainLen = static_cast<size_t>(len);
    }

    const size_t need = protectedEntrySize(plainLen);
    auto stored = memory::makePsramByteBufferNoThrow(need);
    size_t storedLen = 0;
    if (!stored || !buildProtectedEntry(plain, plainLen, contentKey, stored.get(), need, storedLen) ||
        !zip.addStoredEntry(entryName, stored.get(), storedLen, static_cast<uint32_t>(plainLen),
                            plainCrc(plain, plainLen))) {
      packResult = Result::StorageWrite;
      break;
    }
    if (onProgress) onProgress(progressCtx);
  }
  spine.close();

  if (packResult != Result::Ok) {
    zip.abort();
    dropParts();
    return packResult;
  }
  // finish() writes the central directory back out of central.part and removes it.
  if (!zip.finish(nullptr, nullptr)) {
    dropParts();
    return Result::Integrity;
  }
  if (!WeReadStore::looksLikeZip(partPath)) {
    LOG_ERR(kTag, "packaged EPUB failed validation: %s", partPath.c_str());
    Storage.remove(partPath.c_str());
    dropParts();
    return Result::Integrity;
  }
  dropParts();
  if (!WeReadStore::atomicReplace(partPath, finalPath)) {
    LOG_ERR(kTag, "atomicReplace failed: %s", finalPath.c_str());
    Storage.remove(partPath.c_str());
    return Result::StorageWrite;
  }
  LOG_INF(kTag, "packaged %lu chapters into %s", static_cast<unsigned long>(included), finalPath.c_str());
  // Best effort: the map only powers cross-device progress jumps. Failing to
  // write it must not turn a finished book into a failed one.
  if (!JjwxcStore::writeSpineMap(novel.novelId)) {
    LOG_ERR(kTag, "spine map not written for %s", novel.novelId);
  }
  return Result::Ok;
}

}  // namespace JjwxcEpub
