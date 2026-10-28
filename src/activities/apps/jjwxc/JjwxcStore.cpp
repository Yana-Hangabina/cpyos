#include "JjwxcStore.h"

#include <BookKey.h>
#include <DeviceSecret.h>
#include <HalSystem.h>
#include <JjwxcBookCrypto.h>
#include <Logging.h>
#include <Memory.h>
#include <ObfuscationUtils.h>
#include <Txt.h>
#include <WeReadStore.h>
#include <esp_random.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "util/StringUtils.h"

namespace {

constexpr const char* kTag = "JJWXC";
// token(41) + identifiers(56) + nickname + readerId, with separators and slack.
constexpr size_t kMaxCredentialsPlain = 512;
constexpr size_t kMaxCredentialsStored = 2048;
// Bounds on the login form itself: the keyboard caps both fields at 64, so these
// are what a saved account can hold. Base64 of an XORed blob is ~4/3 the input.
constexpr size_t kMaxLoginName = 64;
constexpr size_t kMaxPassword = 64;
constexpr size_t kMaxAccountPlain = 192;
constexpr size_t kMaxAccountStored = 512;
// The WAP jar: sid (133) + JJSESS (387) + five small companions serialises to ~1 KB,
// so 1536 is headroom rather than a guess at the maximum. Base64 of the XORed blob is
// ~4/3 of that. Deliberately NOT sized for the jar's theoretical worst case (8 cookies
// x 512-char values = 5 KB), because the transient is internal heap on a device whose
// largest free block has measured 16 KB after Wi-Fi+TLS.
constexpr size_t kMaxCookiesPlain = 1536;
constexpr size_t kMaxCookiesStored = 2560;
// Longest legacy books/ entry this app ever wrote: "preview-<15 id>-<10 digits>.txt".
constexpr size_t kLegacyNameBytes = 64;

bool writeSmallFile(const char* path, const std::string& content) {
  HalFile file;
  if (!Storage.openFileForWrite(kTag, path, file)) {
    LOG_ERR(kTag, "openFileForWrite failed: %s", path);
    return false;
  }
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(content.data()), content.size());
  file.flush();
  // No explicit close(): `file` is a local, DESTRUCTOR_CLOSES_FILE=1 closes it on
  // scope exit (AGENTS.md rule #5). Every caller renames or replaces only after
  // this returns, so the handle is already gone by then.
  if (written != content.size()) {
    LOG_ERR(kTag, "short write %s: %u/%u", path, (unsigned)written, (unsigned)content.size());
    return false;
  }
  return true;
}

bool readSmallFile(const char* path, std::string& out, size_t cap) {
  return Storage.readFileToString(kTag, path, cap, out);
}

// Splits "a\nb\nc" into fields; missing trailing fields stay empty.
void splitLines(const std::string& s, std::vector<std::string>& out, size_t maxFields) {
  out.clear();
  size_t pos = 0;
  while (out.size() < maxFields) {
    const size_t nl = s.find('\n', pos);
    if (nl == std::string::npos) {
      out.push_back(s.substr(pos));
      break;
    }
    out.push_back(s.substr(pos, nl - pos));
    pos = nl + 1;
  }
}

// Little-endian field extraction out of a raw byte buffer. memcpy rather than a
// cast: the buffer is only byte-aligned and a wider load through a reinterpret_cast
// faults on the C3's RISC-V core (AGENTS.md rule #6).
uint16_t readLe16(const uint8_t* p) {
  uint16_t v = 0;
  memcpy(&v, p, sizeof(v));
  return v;
}

uint32_t readLe32(const uint8_t* p) {
  uint32_t v = 0;
  memcpy(&v, p, sizeof(v));
  return v;
}

bool removeIfExists(const char* path) {
  if (!Storage.exists(path)) return true;
  if (Storage.remove(path)) return true;
  LOG_ERR(kTag, "removal failed: %s", path);
  return false;
}

// Removes every file directly inside `dir` and then the directory itself. The
// comment cache is a flat directory, so one level is all the walk needs; adding
// recursion here would only add a code path nothing uses.
bool removeFlatDirectory(const std::string& dir) {
  if (!Storage.exists(dir.c_str())) return true;
  bool ok = true;
  HalFile handle = Storage.open(dir.c_str());
  if (!handle.isOpen()) {
    LOG_ERR(kTag, "cannot open directory %s", dir.c_str());
    return false;
  }
  char name[96] = {};
  while (true) {
    HalFile entry = handle.openNextFile();
    if (!entry.isOpen()) break;
    name[0] = '\0';
    entry.getName(name, sizeof(name));
    entry.close();
    if (name[0] == '\0') continue;
    const std::string path = dir + "/" + name;
    if (!Storage.remove(path.c_str())) {
      LOG_ERR(kTag, "cannot remove %s", path.c_str());
      ok = false;
    }
  }
  handle.close();
  if (!Storage.removeDir(dir.c_str())) {
    LOG_ERR(kTag, "cannot remove %s", dir.c_str());
    ok = false;
  }
  return ok;
}

// Drops a file plus the `txt_<hash(path)>` reader cache built for it. Txt owns
// that path scheme, so ask it rather than recomputing the hash here; removing
// only the .txt would orphan the cache forever.
bool removeTxtAndCache(const std::string& path) {
  const Txt legacy(path, "/.crosspoint");
  if (!legacy.clearCache()) LOG_ERR(kTag, "legacy reader cache cleanup failed: %s", path.c_str());
  return Storage.remove(path.c_str());
}

}  // namespace

namespace JjwxcStore {

bool ensureRoot() {
  return Storage.ensureDirectoryExists("/.crosspoint") && Storage.ensureDirectoryExists(kRoot) &&
         Storage.ensureDirectoryExists(kOutputDir);
}

bool ensureNovelRoot(const char* novelId) {
  if (!novelId || !novelId[0]) return false;
  if (!ensureRoot()) return false;
  const std::string dir = novelDirectory(novelId);
  if (!Storage.ensureDirectoryExists(dir.c_str())) {
    LOG_ERR(kTag, "cannot create %s", dir.c_str());
    return false;
  }
  const std::string chapters = dir + "/chapters";
  if (!Storage.ensureDirectoryExists(chapters.c_str())) {
    LOG_ERR(kTag, "cannot create %s", chapters.c_str());
    return false;
  }
  return true;
}

bool isDisclaimerAccepted() {
  HalFile file;
  return Storage.openFileForRead(kTag, kDisclaimerPath, file) && file.fileSize() > 0;
}

bool acceptDisclaimer() {
  if (!Storage.ensureDirectoryExists("/.crosspoint") || !Storage.ensureDirectoryExists(kRoot)) return false;
  return writeSmallFile(kDisclaimerPath, "1\n");
}

bool loadCredentials(jjwxc::Credentials& out) {
  out = jjwxc::Credentials{};
  std::string stored;
  if (!readSmallFile(kCredentialsPath, stored, kMaxCredentialsStored)) return false;
  if (stored.empty()) return false;

  bool ok = false;
  bool tooLong = false;
  const std::string plain = obfuscation::deobfuscateFromBase64(stored.c_str(), kMaxCredentialsPlain, &ok, &tooLong);
  if (!ok || tooLong || plain.empty()) {
    // Almost always means the card was written by a different device: the XOR
    // key is the eFuse MAC, so the blob does not survive a move.
    LOG_ERR(kTag, "credentials did not deobfuscate (ok=%d tooLong=%d)", (int)ok, (int)tooLong);
    return false;
  }

  std::vector<std::string> fields;
  splitLines(plain, fields, 4);
  if (fields.empty() || fields[0].empty()) return false;
  snprintf(out.token, sizeof(out.token), "%s", fields[0].c_str());
  if (fields.size() > 1) snprintf(out.identifiers, sizeof(out.identifiers), "%s", fields[1].c_str());
  if (fields.size() > 2) snprintf(out.nickName, sizeof(out.nickName), "%s", fields[2].c_str());
  if (fields.size() > 3) snprintf(out.readerId, sizeof(out.readerId), "%s", fields[3].c_str());
  return out.valid();
}

bool saveCredentials(const jjwxc::Credentials& cred) {
  if (!Storage.ensureDirectoryExists("/.crosspoint") || !Storage.ensureDirectoryExists(kRoot)) return false;
  std::string plain;
  plain.reserve(192);
  plain += cred.token;
  plain += '\n';
  plain += cred.identifiers;
  plain += '\n';
  plain += cred.nickName;
  plain += '\n';
  plain += cred.readerId;
  plain += '\n';

  const String stored = obfuscation::obfuscateToBase64(plain);
  if (stored.length() == 0) {
    LOG_ERR(kTag, "obfuscateToBase64 produced nothing");
    return false;
  }
  return writeSmallFile(kCredentialsPath, std::string(stored.c_str()));
}

bool clearCredentials() { return removeIfExists(kCredentialsPath); }

// ---------------------------------------------------------------------------
// WAP cookie jar (the session; see the comment in JjwxcStore.h for why this is
// not folded into `credentials`)
// ---------------------------------------------------------------------------

bool loadCookies(jjwxc::CookieJar& jar, const uint64_t nowUnix) {
  jar.clear();
  std::string stored;
  if (!readSmallFile(kCookiesPath, stored, kMaxCookiesStored)) return false;
  if (stored.empty()) return false;

  bool ok = false;
  bool tooLong = false;
  const std::string plain = obfuscation::deobfuscateFromBase64(stored.c_str(), kMaxCookiesPlain, &ok, &tooLong);
  if (!ok || tooLong || plain.empty()) {
    // Same story as the other two blobs: the XOR key is the eFuse MAC, so a card
    // written by another device decodes to garbage here.
    LOG_ERR(kTag, "cookies did not deobfuscate (ok=%d tooLong=%d)", (int)ok, (int)tooLong);
    return false;
  }
  if (!jar.deserialize(plain.c_str(), plain.size())) {
    LOG_ERR(kTag, "cookies deobfuscated but did not parse (%u bytes)", (unsigned)plain.size());
    return false;
  }
  // A jar that parsed but holds no live sid is not a session, and reporting it as one
  // would send the user into a request that 302s to the login page.
  if (!hasSession(jar, nowUnix)) {
    LOG_INF(kTag, "cookies restored but no live sid (%u entries)", (unsigned)jar.count(nowUnix));
    return false;
  }
  return true;
}

bool saveCookies(const jjwxc::CookieJar& jar) {
  if (!Storage.ensureDirectoryExists("/.crosspoint") || !Storage.ensureDirectoryExists(kRoot)) return false;

  // Sized for the real set, not the theoretical one: sid (133) plus JJSESS (387) plus
  // five small companions serialises to about 1 KB, so this is ~1.5x headroom.
  //
  // PSRAM, not the stack. This runs on the 12 KB job task while a TLS session may
  // still be open, and the read path already holds two std::strings on the internal
  // heap (readSmallFile's and deobfuscateFromBase64's) at a point where the largest
  // free internal block has measured as low as 16 KB.
  const auto buf = memory::makePsramByteBufferNoThrow(kMaxCookiesPlain);
  if (!buf) {
    LOG_ERR(kTag, "no PSRAM for the cookie blob (%u bytes)", (unsigned)kMaxCookiesPlain);
    return false;
  }
  char* plain = reinterpret_cast<char*>(buf.get());
  const size_t n = jar.serialize(plain, kMaxCookiesPlain);
  if (n == 0) {
    LOG_ERR(kTag, "cookie jar serialised to nothing");
    return false;
  }
  // serialize() stops at the first cookie that does not fit, so a full buffer means
  // something was dropped. Say so: a silently partial jar restores without its sid
  // and reads as an expired session.
  if (n + 1 >= kMaxCookiesPlain) {
    LOG_ERR(kTag, "cookie jar did not fit %u bytes; the persisted session may be partial", (unsigned)kMaxCookiesPlain);
  }

  const String stored = obfuscation::obfuscateToBase64(std::string(plain, n));
  if (stored.length() == 0) {
    LOG_ERR(kTag, "obfuscateToBase64 produced nothing");
    return false;
  }
  return writeSmallFile(kCookiesPath, std::string(stored.c_str()));
}

bool clearCookies() { return removeIfExists(kCookiesPath); }

bool hasSession(const jjwxc::CookieJar& jar, const uint64_t nowUnix) { return jar.has("sid", nowUnix); }

// ---------------------------------------------------------------------------
// Saved login (see the SaveScope comment in JjwxcStore.h: obfuscation, not
// encryption -- the device has to be able to read its own password back)
// ---------------------------------------------------------------------------

bool loadAccount(SavedAccount& out) {
  out = SavedAccount{};
  std::string stored;
  if (!readSmallFile(kAccountPath, stored, kMaxAccountStored)) return false;
  if (stored.empty()) return false;

  bool ok = false;
  bool tooLong = false;
  const std::string plain = obfuscation::deobfuscateFromBase64(stored.c_str(), kMaxAccountPlain, &ok, &tooLong);
  if (!ok || tooLong || plain.empty()) {
    // Same story as credentials: the XOR key is the eFuse MAC, so a card written
    // by another device decodes to garbage here.
    LOG_ERR(kTag, "saved account did not deobfuscate (ok=%d tooLong=%d)", (int)ok, (int)tooLong);
    return false;
  }

  // scope, login name, password -- the password last, so anything odd in the two
  // fields ahead of it cannot shift it into the wrong slot.
  std::vector<std::string> fields;
  splitLines(plain, fields, 3);
  if (fields.empty()) return false;
  const long scope = strtol(fields[0].c_str(), nullptr, 10);
  if (scope != static_cast<long>(SaveScope::AccountOnly) && scope != static_cast<long>(SaveScope::AccountAndPassword)) {
    // Includes SaveScope::Nothing, which saveAccount() writes as "no file at all".
    return false;
  }
  out.scope = static_cast<SaveScope>(scope);
  if (fields.size() > 1) out.loginName = fields[1].substr(0, kMaxLoginName);
  if (fields.size() > 2 && out.scope == SaveScope::AccountAndPassword) {
    out.password = fields[2].substr(0, kMaxPassword);
  }
  return out.valid();
}

bool saveAccount(const SavedAccount& acc) {
  if (!acc.valid()) return clearAccount();
  if (acc.scope == SaveScope::Nothing) return clearAccount();
  if (!Storage.ensureDirectoryExists("/.crosspoint") || !Storage.ensureDirectoryExists(kRoot)) return false;

  const bool withPassword = acc.scope == SaveScope::AccountAndPassword;
  std::string plain;
  plain.reserve(kMaxLoginName + kMaxPassword + 8);
  plain += std::to_string(static_cast<int>(acc.scope));
  plain += '\n';
  plain += acc.loginName.substr(0, kMaxLoginName);
  plain += '\n';
  // An AccountOnly save must not leave a previously stored password behind, so the
  // field is written empty rather than carried over.
  if (withPassword) plain += acc.password.substr(0, kMaxPassword);
  plain += '\n';

  const String stored = obfuscation::obfuscateToBase64(plain);
  if (stored.length() == 0) {
    LOG_ERR(kTag, "obfuscateToBase64 produced nothing");
    return false;
  }
  return writeSmallFile(kAccountPath, std::string(stored.c_str()));
}

bool clearAccount() { return removeIfExists(kAccountPath); }

// ---------------------------------------------------------------------------
// Device fingerprint
// ---------------------------------------------------------------------------

bool loadDevice(DeviceIdentity& out) {
  out = DeviceIdentity{};
  std::string stored;
  if (!readSmallFile(kDevicePath, stored, kMaxAccountStored)) return false;
  if (stored.empty()) return false;

  bool ok = false;
  bool tooLong = false;
  const std::string plain = obfuscation::deobfuscateFromBase64(stored.c_str(), kMaxAccountPlain, &ok, &tooLong);
  if (!ok || tooLong || plain.empty()) {
    LOG_ERR(kTag, "device identity did not deobfuscate (ok=%d tooLong=%d)", (int)ok, (int)tooLong);
    return false;
  }
  std::vector<std::string> fields;
  splitLines(plain, fields, 2);
  if (fields.empty() || fields[0].empty()) return false;
  snprintf(out.identifiers, sizeof(out.identifiers), "%s", fields[0].c_str());
  if (fields.size() > 1) snprintf(out.readerId, sizeof(out.readerId), "%s", fields[1].c_str());
  return out.hasIdentifiers();
}

bool saveDevice(const DeviceIdentity& dev) {
  if (!dev.hasIdentifiers()) return false;
  if (!Storage.ensureDirectoryExists("/.crosspoint") || !Storage.ensureDirectoryExists(kRoot)) return false;
  std::string plain;
  plain.reserve(96);
  plain += dev.identifiers;
  plain += '\n';
  plain += dev.readerId;
  plain += '\n';

  const String stored = obfuscation::obfuscateToBase64(plain);
  if (stored.length() == 0) {
    LOG_ERR(kTag, "obfuscateToBase64 produced nothing");
    return false;
  }
  return writeSmallFile(kDevicePath, std::string(stored.c_str()));
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

std::string novelDirectory(const char* novelId) {
  // Sanitised like WeRead's bookDirectory (WeReadStore.cpp:597-599): the id comes
  // off the network and becomes a directory name.
  return std::string(kRoot) + "/" + StringUtils::sanitizeFilename(novelId ? novelId : "", 15);
}

std::string tocPath(const char* novelId) { return novelDirectory(novelId) + "/toc.bin"; }
std::string novelInfoPath(const char* novelId) { return novelDirectory(novelId) + "/novel.bin"; }
std::string coverSourcePath(const char* novelId) { return novelDirectory(novelId) + "/cover.source.jpg"; }
std::string coverThumbPath(const char* novelId) { return novelDirectory(novelId) + "/cover.thumb.bmp"; }
std::string contentPartPath(const char* novelId) { return novelDirectory(novelId) + "/content.part"; }
std::string navPartPath(const char* novelId) { return novelDirectory(novelId) + "/nav.part"; }
std::string centralPartPath(const char* novelId) { return novelDirectory(novelId) + "/central.part"; }
std::string bookPartPath(const char* novelId) { return novelDirectory(novelId) + "/book.epub.part"; }

std::string chapterPath(const char* novelId, const uint32_t chapterId) {
  char name[40];
  snprintf(name, sizeof(name), "/chapters/%lu.xhtml", static_cast<unsigned long>(chapterId));
  return novelDirectory(novelId) + name;
}

std::string commentCountsDir(const char* novelId) { return novelDirectory(novelId) + "/comments"; }

// One byte, root level: it is a global caching preference, not per novel.
std::string commentBodiesPrefPath() { return std::string(kRoot) + "/commentbodies"; }

std::string commentCountsPath(const char* novelId, const uint32_t chapterId) {
  char name[40];
  snprintf(name, sizeof(name), "/comments/%lu.bin", static_cast<unsigned long>(chapterId));
  return novelDirectory(novelId) + name;
}

bool saveCommentCounts(const char* novelId, const uint32_t chapterId,
                       const JjwxcCommentSidecar::Count* counts, const size_t count) {
  if (novelId == nullptr || novelId[0] == '\0' || chapterId == 0) return false;
  if ((counts == nullptr && count != 0) || count > JjwxcCommentSidecar::kMaxEntries) {
    LOG_ERR(kTag, "comment sidecar rejected: %u entries", (unsigned)count);
    return false;
  }
  if (!ensureNovelRoot(novelId)) return false;
  const std::string dir = commentCountsDir(novelId);
  if (!Storage.ensureDirectoryExists(dir.c_str())) {
    LOG_ERR(kTag, "cannot create %s", dir.c_str());
    return false;
  }

  const size_t bytes = JjwxcCommentSidecar::kHeaderSize + count * JjwxcCommentSidecar::kEntrySize;
  // Heap, not stack: save is called from the download job as well as the UI,
  // and neither task has 3 KB of stack to spare for a rare, tiny write.
  auto image = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!image) {
    LOG_ERR(kTag, "OOM: comment sidecar image (%u bytes)", (unsigned)bytes);
    return false;
  }
  if (JjwxcCommentSidecar::encode(counts, count, image.get(), bytes) != bytes) {
    LOG_ERR(kTag, "comment sidecar encode failed");
    return false;
  }

  const std::string finalPath = commentCountsPath(novelId, chapterId);
  const std::string partPath = finalPath + ".part";
  {
    HalFile file;
    if (!Storage.openFileForWrite(kTag, partPath.c_str(), file)) return false;
    const size_t written = file.write(image.get(), bytes);
    file.flush();
    // No explicit close: the scope destructor closes it (rule #5), before the
    // rename below can touch the path.
    if (written != bytes) {
      LOG_ERR(kTag, "short write %s: %u/%u", partPath.c_str(), (unsigned)written, (unsigned)bytes);
      Storage.remove(partPath.c_str());
      return false;
    }
  }
  if (!WeReadStore::atomicReplace(partPath, finalPath)) {
    Storage.remove(partPath.c_str());
    LOG_ERR(kTag, "atomicReplace failed: %s", finalPath.c_str());
    return false;
  }
  LOG_DBG(kTag, "comment sidecar: chapter=%lu %u entries", (unsigned long)chapterId, (unsigned)count);
  return true;
}

bool loadCommentCounts(const char* novelId, const uint32_t chapterId, JjwxcCommentSidecar::Count* out,
                       const size_t maxOut, size_t& outCount) {
  outCount = 0;
  if (novelId == nullptr || novelId[0] == '\0' || chapterId == 0) return false;
  if (out == nullptr && maxOut != 0) return false;

  const std::string path = commentCountsPath(novelId, chapterId);
  // Ask first: until the app writes these files, every chapter would otherwise
  // go through a failed open and (depending on the SD layer) an error line.
  if (!Storage.exists(path.c_str())) return false;

  HalFile file;
  if (!Storage.openFileForRead(kTag, path, file)) return false;
  const uint64_t size = file.fileSize64();
  const uint64_t cap =
      JjwxcCommentSidecar::kHeaderSize + JjwxcCommentSidecar::kMaxEntries * JjwxcCommentSidecar::kEntrySize;
  if (size < JjwxcCommentSidecar::kHeaderSize || size > cap) {
    LOG_ERR(kTag, "comment sidecar size out of range: %lu", (unsigned long)size);
    return false;
  }
  auto image = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(size));
  if (!image) {
    LOG_ERR(kTag, "OOM: comment sidecar read (%u bytes)", (unsigned)size);
    return false;
  }
  if (file.read(image.get(), static_cast<size_t>(size)) != static_cast<int>(size)) {
    LOG_ERR(kTag, "comment sidecar read failed");
    return false;
  }
  size_t count = 0;
  if (!JjwxcCommentSidecar::decode(image.get(), static_cast<size_t>(size), out, maxOut, count)) {
    // A rejected table means no bubbles, never wrong ones; say so once instead
    // of leaving the reader silently bubble-less.
    LOG_ERR(kTag, "comment sidecar rejected: chapter=%lu", (unsigned long)chapterId);
    return false;
  }
  outCount = count;
  return true;
}

std::string commentBodiesPath(const char* novelId, const uint32_t chapterId, const uint32_t paragraphId) {
  char name[64];
  snprintf(name, sizeof(name), "/comments/%lu-%lu.bin", static_cast<unsigned long>(chapterId),
           static_cast<unsigned long>(paragraphId));
  return novelDirectory(novelId) + name;
}

bool saveCommentBodies(const char* novelId, const uint32_t chapterId, const uint32_t paragraphId,
                       const JjwxcCommentSidecar::Bodies::Item* items, const size_t count, const uint32_t total) {
  if (novelId == nullptr || novelId[0] == '\0' || chapterId == 0 || paragraphId == 0) return false;
  const size_t bytes = JjwxcCommentSidecar::Bodies::encodedSize(items, count);
  if (bytes == 0) {
    LOG_ERR(kTag, "comment body cache rejected: %u entries", (unsigned)count);
    return false;
  }
  if (!ensureNovelRoot(novelId)) return false;
  const std::string dir = commentCountsDir(novelId);
  if (!Storage.ensureDirectoryExists(dir.c_str())) {
    LOG_ERR(kTag, "cannot create %s", dir.c_str());
    return false;
  }

  // The image can reach ~123 KB; this runs on the PSRAM-stacked job/worker, so
  // do not ask the internal heap for it.
  auto image = memory::makePsramByteBufferNoThrow(bytes);
  if (!image) {
    LOG_ERR(kTag, "OOM: comment body cache (%u bytes)", (unsigned)bytes);
    return false;
  }
  if (JjwxcCommentSidecar::Bodies::encode(items, count, total, image.get(), bytes) != bytes) {
    LOG_ERR(kTag, "comment body cache encode failed");
    return false;
  }

  const std::string finalPath = commentBodiesPath(novelId, chapterId, paragraphId);
  const std::string partPath = finalPath + ".part";
  {
    HalFile file;
    if (!Storage.openFileForWrite(kTag, partPath.c_str(), file)) return false;
    const size_t written = file.write(image.get(), bytes);
    file.flush();
    if (written != bytes) {
      LOG_ERR(kTag, "short write %s: %u/%u", partPath.c_str(), (unsigned)written, (unsigned)bytes);
      Storage.remove(partPath.c_str());
      return false;
    }
  }
  if (!WeReadStore::atomicReplace(partPath, finalPath)) {
    Storage.remove(partPath.c_str());
    LOG_ERR(kTag, "atomicReplace failed: %s", finalPath.c_str());
    return false;
  }
  LOG_DBG(kTag, "comment body cache: chapter=%lu paragraph=%lu %u entries", (unsigned long)chapterId,
          (unsigned long)paragraphId, (unsigned)count);
  return true;
}

bool loadCommentBodies(const char* novelId, const uint32_t chapterId, const uint32_t paragraphId,
                       JjwxcCommentSidecar::Bodies::Item* out, const size_t maxOut, size_t& outCount,
                       uint32_t& total) {
  outCount = 0;
  total = 0;
  if (novelId == nullptr || novelId[0] == '\0' || chapterId == 0 || paragraphId == 0) return false;
  if (out == nullptr && maxOut != 0) return false;

  const std::string path = commentBodiesPath(novelId, chapterId, paragraphId);
  if (!Storage.exists(path.c_str())) return false;

  HalFile file;
  if (!Storage.openFileForRead(kTag, path, file)) return false;
  const uint64_t size = file.fileSize64();
  const uint64_t cap = JjwxcCommentSidecar::Bodies::kMaxEncodedBytes;
  if (size < JjwxcCommentSidecar::Bodies::kHeaderSize || size > cap) {
    LOG_ERR(kTag, "comment body cache size out of range: %lu", (unsigned long)size);
    return false;
  }
  auto image = memory::makePsramByteBufferNoThrow(static_cast<size_t>(size));
  if (!image) {
    LOG_ERR(kTag, "OOM: comment body cache read (%u bytes)", static_cast<unsigned>(size));
    return false;
  }
  if (file.read(image.get(), static_cast<size_t>(size)) != static_cast<int>(size)) {
    LOG_ERR(kTag, "comment body cache read failed");
    return false;
  }
  size_t count = 0;
  if (!JjwxcCommentSidecar::Bodies::decode(image.get(), static_cast<size_t>(size), out, maxOut, count, total)) {
    LOG_ERR(kTag, "comment body cache rejected: chapter=%lu paragraph=%lu", (unsigned long)chapterId,
            (unsigned long)paragraphId);
    return false;
  }
  outCount = count;
  return true;
}

bool removeCommentBodies(const char* novelId, const uint32_t chapterId, const uint32_t paragraphId) {
  if (novelId == nullptr || novelId[0] == '\0' || chapterId == 0 || paragraphId == 0) return false;
  const std::string path = commentBodiesPath(novelId, chapterId, paragraphId);
  return removeIfExists(path.c_str());
}

std::string finalBookPath(const NovelRecord& rec) {
  const std::string title = StringUtils::sanitizeFilename(rec.title, 80);
  const std::string id = StringUtils::sanitizeFilename(rec.novelId, 15);
  return std::string(kOutputDir) + "/" + title + "-" + id + ".epub";
}

bool loadCacheCommentBodiesPref(bool& enabled) {
  enabled = false;  // opt-in: the whole-book cost is hours, not minutes
  std::string stored;
  const std::string path = commentBodiesPrefPath();
  if (!readSmallFile(path.c_str(), stored, 8)) return false;
  enabled = !stored.empty() && stored[0] == '1';
  return true;
}

bool saveCacheCommentBodiesPref(const bool enabled) {
  if (!Storage.ensureDirectoryExists("/.crosspoint") || !Storage.ensureDirectoryExists(kRoot)) return false;
  const std::string path = commentBodiesPrefPath();
  return writeSmallFile(path.c_str(), enabled ? "1\n" : "0\n");
}

bool ensureBookContentKey(const std::string& epubPath, uint8_t (&key)[16]) {
  int64_t expiresAt = 0;
  if (bookkey::read(epubPath, key, &expiresAt)) {
    // A key wrapped by another device decrypts to garbage; bookkey::read already
    // rejects it (GCM tag), so reaching here means this device owns the key.
    return true;
  }
  esp_fill_random(key, sizeof(key));
  if (!bookkey::write(epubPath, key, /*expiresAt=*/0)) {
    LOG_ERR(kTag, "cannot wrap content key for %s", epubPath.c_str());
    return false;
  }
  LOG_INF(kTag, "new content key for %s", epubPath.c_str());
  return true;
}

bool readCacheSalt(uint8_t (&salt)[jjwxc::BookCrypto::kSaltLen]) {
  const std::string saltPath = std::string(kRoot) + "/cachekey";
  HalFile file;
  if (!Storage.openFileForRead(kTag, saltPath, file)) return false;
  return file.read(salt, sizeof(salt)) == static_cast<int>(sizeof(salt));
}

bool loadOrCreateCacheSalt(uint8_t (&salt)[jjwxc::BookCrypto::kSaltLen]) {
  if (readCacheSalt(salt)) return true;
  esp_fill_random(salt, sizeof(salt));
  const std::string saltPath = std::string(kRoot) + "/cachekey";
  const std::string tmp = saltPath + ".part";
  {
    HalFile file;
    if (!Storage.openFileForWrite(kTag, tmp, file)) return false;
    const size_t written = file.write(salt, sizeof(salt));
    file.flush();
    if (written != sizeof(salt)) {
      Storage.remove(tmp.c_str());
      return false;
    }
  }
  if (!WeReadStore::atomicReplace(tmp, saltPath)) {
    Storage.remove(tmp.c_str());
    LOG_ERR(kTag, "cannot persist cache salt");
    return false;
  }
  return true;
}

bool ensureCacheCrypto(uint8_t (&key)[jjwxc::BookCrypto::kKeyLen]) {
  // Derived once per session: the salt is on SD and the KDF is an HMAC, so
  // re-deriving per chapter would add a file open to every read for no gain.
  static std::mutex lock;
  static bool loaded = false;
  static uint8_t cached[jjwxc::BookCrypto::kKeyLen] = {};
  std::lock_guard<std::mutex> guard(lock);
  if (loaded) {
    memcpy(key, cached, sizeof(cached));
    return true;
  }
  if (!ensureRoot()) return false;
  uint8_t salt[jjwxc::BookCrypto::kSaltLen] = {};
  if (!loadOrCreateCacheSalt(salt)) return false;
  HalSystem::DeviceId mac{};
  if (!HalSystem::getDeviceId(mac)) return false;
  uint8_t macBytes[jjwxc::BookCrypto::kMacLen] = {};
  memcpy(macBytes, mac.data(), sizeof(macBytes));
  uint8_t secret[jjwxc::BookCrypto::kSecretLen] = {};
  if (!deviceSecret(secret)) {
    // Fail closed. Falling back to the v1 MAC+salt key here would silently
    // re-open the copy-the-card path this version exists to close.
    LOG_ERR(kTag, "cache key unavailable: device secret missing");
    return false;
  }
  if (!jjwxc::BookCrypto::deriveBookKey(secret, macBytes, salt, cached)) return false;
  loaded = true;
  memcpy(key, cached, sizeof(cached));
  return true;
}

bool ensureLegacyCacheCrypto(uint8_t (&key)[jjwxc::BookCrypto::kKeyLen]) {
  if (!ensureRoot()) return false;
  uint8_t salt[jjwxc::BookCrypto::kSaltLen] = {};
  // Do not create a salt here: a v1 blob encrypted under a different salt is
  // already unrecoverable, and creating a fresh one would silently invalidate
  // every other legacy chapter.
  if (!readCacheSalt(salt)) return false;
  HalSystem::DeviceId mac{};
  if (!HalSystem::getDeviceId(mac)) return false;
  uint8_t macBytes[jjwxc::BookCrypto::kMacLen] = {};
  memcpy(macBytes, mac.data(), sizeof(macBytes));
  return jjwxc::BookCrypto::deriveLegacyBookKey(macBytes, salt, key);
}

bool sealPlainBufferToFile(const std::string& path, const uint8_t* plain, size_t len) {
  if (plain == nullptr && len != 0) return false;
  if (len > 8u * 1024u * 1024u) return false;
  uint8_t key[jjwxc::BookCrypto::kKeyLen] = {};
  if (!ensureCacheCrypto(key)) return false;
  uint8_t nonce[jjwxc::BookCrypto::kNonceLen] = {};
  esp_fill_random(nonce, sizeof(nonce));

  const size_t cap = len + jjwxc::BookCrypto::kHeaderLen;
  auto blob = memory::makePsramByteBufferNoThrow(cap);
  if (!blob) return false;
  size_t blobLen = 0;
  if (!jjwxc::BookCrypto::encryptBookBlob(key, nonce, plain, len, blob.get(), cap, blobLen)) {
    LOG_ERR(kTag, "cache encryption failed for %s", path.c_str());
    return false;
  }
  const std::string tmp = path + ".enc.part";
  {
    HalFile file;
    if (!Storage.openFileForWrite(kTag, tmp, file)) return false;
    const size_t written = file.write(blob.get(), blobLen);
    file.flush();
    if (written != blobLen) {
      Storage.remove(tmp.c_str());
      return false;
    }
  }
  if (!WeReadStore::atomicReplace(tmp, path)) {
    Storage.remove(tmp.c_str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// novel.bin
// ---------------------------------------------------------------------------

namespace {

// One record behind the standard 12-byte header, written through WeRead's
// IndexWriter so the `.part` + atomic replace path is shared rather than
// reimplemented (WeReadStore.h:139-155).
bool writeSingleRecordIndex(const std::string& finalPath, const uint32_t magic, const void* record,
                            const uint16_t recordSize) {
  WeReadStore::IndexWriter writer;
  if (!writer.begin(finalPath, magic, recordSize)) {
    LOG_ERR(kTag, "IndexWriter.begin failed: %s", finalPath.c_str());
    return false;
  }
  if (!writer.append(record) || !writer.finish()) {
    writer.abort();
    LOG_ERR(kTag, "IndexWriter write failed: %s", finalPath.c_str());
    return false;
  }
  return true;
}

// Validates the header and reads the single record back out.
bool readSingleRecordIndex(const std::string& path, const uint32_t magic, const uint16_t recordSize, void* record) {
  HalFile file;
  if (!Storage.openFileForRead(kTag, path, file)) return false;
  uint8_t header[kIndexHeaderSize];
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) return false;
  if (readLe32(header) != magic || readLe16(header + 4) != kIndexVersion ||
      readLe16(header + 6) != recordSize || readLe32(header + 8) != 1) {
    LOG_ERR(kTag, "bad index header: %s", path.c_str());
    return false;
  }
  return file.read(record, recordSize) == static_cast<int>(recordSize);
}

}  // namespace

bool loadNovel(const char* novelId, NovelRecord& out) {
  out = NovelRecord{};
  if (!readSingleRecordIndex(novelInfoPath(novelId), kNovelMagic, sizeof(NovelRecord), &out)) return false;
  // Records written before the readable/packaged split carried a single count in
  // cachedChapterCount that meant "packaged". The slot now holding
  // packagedChapterCount was `reserved` and always zero, so a zero there next to a
  // non-zero cached count identifies one. Both numbers are optimistic then (stubs
  // counted as readable); the next saveNovel() -- which every cache run and every
  // catalogue refresh does -- replaces them with the real split.
  if (out.packagedChapterCount == 0 && out.cachedChapterCount > 0) {
    out.packagedChapterCount = out.cachedChapterCount;
  }
  return true;
}

bool saveNovel(const NovelRecord& rec) {
  if (!ensureNovelRoot(rec.novelId)) return false;

  // A rename on the server changes finalBookPath(). Retire the previous artefact
  // first so one novel cannot own two EPUBs, which is exactly the "several books
  // for one work" bug this layout exists to prevent.
  NovelRecord previous;
  std::string previousPath;
  if (readSingleRecordIndex(novelInfoPath(rec.novelId), kNovelMagic, sizeof(NovelRecord), &previous)) {
    previousPath = finalBookPath(previous);
  }

  if (!writeSingleRecordIndex(novelInfoPath(rec.novelId), kNovelMagic, &rec, sizeof(NovelRecord))) return false;

  const std::string current = finalBookPath(rec);
  // Only retire the old artefact once the new one is actually there: saveNovel()
  // also runs on the failure path, where deleting the previous EPUB would leave
  // the novel with nothing readable at all.
  if (!previousPath.empty() && previousPath != current && Storage.exists(current.c_str())) {
    LOG_INF(kTag, "novel renamed; retiring %s", previousPath.c_str());
    removeIfExists(previousPath.c_str());
  }
  return true;
}

// ---------------------------------------------------------------------------
// toc.bin
// ---------------------------------------------------------------------------

bool TocReader::openPath(const std::string& path, const bool writable) {
  close();
  count_ = 0;
  cursor_ = 0;
  file_ = writable ? Storage.open(path.c_str(), O_RDWR) : Storage.open(path.c_str());
  if (!file_.isOpen()) return false;

  uint8_t header[kIndexHeaderSize];
  if (file_.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    close();
    return false;
  }
  if (readLe32(header) != kTocMagic || readLe16(header + 4) != kIndexVersion ||
      readLe16(header + 6) != sizeof(TocRecord)) {
    // A version bump or a truncated write: treat as "no catalogue yet" so the
    // caller rebuilds it, never as a hard error.
    LOG_ERR(kTag, "unusable toc header: %s", path.c_str());
    close();
    return false;
  }
  count_ = readLe32(header + 8);
  const uint64_t expected = static_cast<uint64_t>(kIndexHeaderSize) + static_cast<uint64_t>(count_) * sizeof(TocRecord);
  if (file_.fileSize64() < expected) {
    LOG_ERR(kTag, "truncated toc: %s (%llu < %llu)", path.c_str(), static_cast<unsigned long long>(file_.fileSize64()),
            static_cast<unsigned long long>(expected));
    close();
    return false;
  }
  return true;
}

bool TocReader::open(const char* novelId) { return openPath(tocPath(novelId), false); }
bool TocReader::openForUpdate(const char* novelId) { return openPath(tocPath(novelId), true); }

bool TocReader::read(const uint32_t index, TocRecord& out) {
  if (!file_.isOpen() || index >= count_) return false;
  const uint64_t offset = static_cast<uint64_t>(kIndexHeaderSize) + static_cast<uint64_t>(index) * sizeof(TocRecord);
  if (offset > SIZE_MAX || !file_.seek(static_cast<size_t>(offset))) return false;
  out = TocRecord{};
  return file_.read(&out, sizeof(out)) == static_cast<int>(sizeof(out));
}

bool TocReader::next(TocRecord& out) {
  if (cursor_ >= count_) return false;
  if (!read(cursor_, out)) return false;
  ++cursor_;
  return true;
}

void TocReader::rewind() { cursor_ = 0; }

bool TocReader::update(const uint32_t index, const TocRecord& rec) {
  if (!file_.isOpen() || index >= count_) return false;
  const uint64_t offset = static_cast<uint64_t>(kIndexHeaderSize) + static_cast<uint64_t>(index) * sizeof(TocRecord);
  if (offset > SIZE_MAX || !file_.seek(static_cast<size_t>(offset))) return false;
  return file_.write(&rec, sizeof(rec)) == sizeof(rec);
}

void TocReader::close() {
  // Explicit close: this is a member handle, one of the three cases AGENTS.md
  // rule #5 lists. It is also what lets saveNovel()/packaging reuse the name.
  if (file_.isOpen()) {
    file_.flush();
    file_.close();
  }
  count_ = 0;
  cursor_ = 0;
}

bool readTocSummary(const char* novelId, TocSummary& out, uint32_t& outHighestPackaged) {
  out = TocSummary{};
  outHighestPackaged = 0;
  TocReader toc;
  if (!toc.open(novelId)) return false;
  TocRecord rec;
  for (uint32_t i = 0; i < toc.count(); ++i) {
    if (!toc.read(i, rec)) {
      toc.close();
      return false;
    }
    if (isPackaged(rec.state)) {
      ++out.packaged;
      outHighestPackaged = i;
    }
    if (isReadable(rec.state)) {
      ++out.readable;
    } else {
      // Not readable yet, whatever the reason: this is the range the "uncached
      // part" preset and the retry entry both have to cover.
      if (!out.hasPending) out.firstPending = i;
      out.lastPending = i;
      out.hasPending = true;
      if (rec.state == static_cast<uint8_t>(ChapterState::Unpurchased)) {
        ++out.unpurchased;
      } else if (rec.state == static_cast<uint8_t>(ChapterState::Locked)) {
        ++out.locked;
      } else if (rec.state == static_cast<uint8_t>(ChapterState::AppOnly)) {
        ++out.appOnly;
      }
    }
    ++out.count;
  }
  toc.close();
  return true;
}

// ---------------------------------------------------------------------------
// toc.bin merge
// ---------------------------------------------------------------------------

bool TocMerger::begin(const char* novelId) {
  abort();
  if (!novelId || !novelId[0]) return false;
  novelId_ = novelId;
  if (!ensureNovelRoot(novelId)) return false;
  if (!writer_.begin(tocPath(novelId), kTocMagic, sizeof(TocRecord))) {
    LOG_ERR(kTag, "cannot start toc.bin for %s", novelId);
    return false;
  }
  active_ = true;
  // A first-ever catalogue has no previous index; that is normal, not an error.
  previousValid_ = previous_.open(novelId) && previous_.next(previousRecord_);
  return true;
}

bool TocMerger::add(const jjwxc::ChapterEntry& entry, TocRecord& out) {
  if (!active_) return false;
  out = TocRecord{};
  out.chapterId = entry.chapterId;
  snprintf(out.title, sizeof(out.title), "%s", entry.name);
  out.remoteSize = entry.size;
  out.isVip = entry.isVip;
  out.state = static_cast<uint8_t>(ChapterState::Missing);

  // Both sides ascend by chapterId, so one forward walk lines them up. A chapter
  // the server dropped simply never matches and falls out of the new index.
  while (previousValid_ && previousRecord_.chapterId < entry.chapterId) {
    previousValid_ = previous_.next(previousRecord_);
  }
  if (previousValid_ && previousRecord_.chapterId == entry.chapterId) {
    switch (static_cast<ChapterState>(previousRecord_.state)) {
      case ChapterState::Cached:
        // A changed remote size means the server now serves different bytes than
        // the ones on the card, so the cache is stale: drop back to Missing and
        // let the next download refetch it. This is the per-chapter invalidation
        // that replaces WeRead's whole-book `refreshingBook` flag.
        if (previousRecord_.remoteSize == entry.size &&
            Storage.exists(chapterPath(novelId_.c_str(), entry.chapterId).c_str())) {
          out.state = static_cast<uint8_t>(ChapterState::Cached);
          out.localBytes = previousRecord_.localBytes;
          out.savedAt = previousRecord_.savedAt;
        }
        break;
      case ChapterState::Unpurchased:
        // Remembering this is what stops a whole-book cache from asking the server
        // about every unpurchased VIP chapter again on every run. A chapter that
        // has since become free is worth one more attempt.
        if (entry.isVip != 0) {
          out.state = previousRecord_.state;
          out.localBytes = previousRecord_.localBytes;
          out.savedAt = previousRecord_.savedAt;
        }
        break;
      case ChapterState::Locked:
      case ChapterState::AppOnly:
        out.state = previousRecord_.state;
        out.localBytes = previousRecord_.localBytes;
        out.savedAt = previousRecord_.savedAt;
        break;
      case ChapterState::Missing:
        break;
    }
  }
  // Author-locked chapters cannot be fetched at all, whatever the local state.
  if (entry.isLock) out.state = static_cast<uint8_t>(ChapterState::Locked);

  if (isPackaged(out.state)) ++summary_.packaged;
  if (isReadable(out.state)) {
    ++summary_.readable;
  } else {
    // summary_.count is still the ordinal this record is about to take.
    if (!summary_.hasPending) summary_.firstPending = summary_.count;
    summary_.lastPending = summary_.count;
    summary_.hasPending = true;
    if (out.state == static_cast<uint8_t>(ChapterState::Unpurchased)) {
      ++summary_.unpurchased;
    } else if (out.state == static_cast<uint8_t>(ChapterState::Locked)) {
      ++summary_.locked;
    } else if (out.state == static_cast<uint8_t>(ChapterState::AppOnly)) {
      ++summary_.appOnly;
    }
  }
  ++summary_.count;
  if (!writer_.append(&out)) {
    LOG_ERR(kTag, "toc append failed at record %lu", static_cast<unsigned long>(summary_.count - 1));
    active_ = false;
    return false;
  }
  return true;
}

bool TocMerger::finish(TocSummary& out) {
  out = TocSummary{};
  if (!active_) return false;
  active_ = false;
  previous_.close();
  if (!writer_.finish()) {
    writer_.abort();
    LOG_ERR(kTag, "toc finish failed for %s", novelId_.c_str());
    return false;
  }
  out = summary_;
  summary_ = TocSummary{};
  return true;
}

void TocMerger::abort() {
  active_ = false;
  previousValid_ = false;
  previousRecord_ = TocRecord{};
  previous_.close();
  writer_.abort();
  summary_ = TocSummary{};
}

// ---------------------------------------------------------------------------
// Scan and cleanup
// ---------------------------------------------------------------------------
bool listNovels(const NovelSink sink, void* ctx, const size_t maxEntries) {
  if (!sink) return false;
  if (!Storage.exists(kRoot)) return true;  // nothing cached yet is not an error
  HalFile dir = Storage.open(kRoot);
  if (!dir.isOpen()) {
    LOG_ERR(kTag, "cannot open %s", kRoot);
    return false;
  }

  size_t emitted = 0;
  char name[32] = {};
  while (emitted < maxEntries) {
    HalFile entry = dir.openNextFile();
    if (!entry.isOpen()) break;
    name[0] = '\0';
    entry.getName(name, sizeof(name));
    const bool isDir = entry.isDirectory();
    // Local, so no explicit close() (rule #5).
    entry.close();
    if (name[0] == '\0' || !isDir || name[0] == '.') continue;

    NovelRecord rec;
    if (!loadNovel(name, rec)) continue;
    if (!sink(ctx, rec, emitted)) break;
    ++emitted;
  }
  dir.close();
  return true;
}

bool removeNovel(const char* novelId) {
  if (novelId == nullptr || novelId[0] == '\0') return false;
  NovelRecord rec;
  if (loadNovel(novelId, rec)) {
    const std::string book = finalBookPath(rec);
    if (!book.empty()) Storage.remove(book.c_str());
  }
  const std::string dir = novelDirectory(novelId);
  if (!Storage.exists(dir.c_str())) return true;

  const char* rootFiles[] = {"toc.bin", "novel.bin", "spine.bin", "synced_progress", "cover.source.jpg",
                             "cover.thumb.bmp", "toc.bin.part", "toc.bin.bak", "novel.bin.part", "novel.bin.bak",
                             "content.part", "nav.part", "central.part", "book.epub.part", "book.epub.bak",
                             "cover.source.jpg.part"};
  for (const char* name : rootFiles) {
    const std::string path = dir + "/" + name;
    if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
      LOG_ERR(kTag, "cannot remove %s", path.c_str());
    }
  }
  const std::string chapters = dir + "/chapters";
  removeFlatDirectory(chapters);
  // Counts and bodies share this directory; leaving it behind made removeDir()
  // fail, and a later re-download inherited stale paragraph bubbles.
  removeFlatDirectory(commentCountsDir(novelId));
  if (!Storage.removeDir(dir.c_str())) {
    LOG_ERR(kTag, "cannot remove %s", dir.c_str());
    return false;
  }
  return true;
}

bool reclaimNovelTempFiles(const char* novelId) {
  const std::string dir = novelDirectory(novelId);
  if (!Storage.exists(dir.c_str())) return true;

  bool clean = true;
  static constexpr const char* kRootParts[] = {"toc.bin.part", "toc.bin.bak",     "novel.bin.part", "novel.bin.bak",
                                               "content.part", "nav.part",        "central.part",   "book.epub.part",
                                               "book.epub.bak", "cover.source.jpg.part"};
  for (const char* part : kRootParts) {
    if (!removeIfExists((dir + "/" + part).c_str())) clean = false;
  }

  const std::string chapters = dir + "/chapters";
  if (!Storage.exists(chapters.c_str())) return clean;
  HalFile dirHandle = Storage.open(chapters.c_str());
  if (!dirHandle.isOpen()) {
    LOG_ERR(kTag, "cannot open %s for cleanup", chapters.c_str());
    return false;
  }
  char name[kLegacyNameBytes] = {};
  while (true) {
    HalFile entry = dirHandle.openNextFile();
    if (!entry.isOpen()) break;
    name[0] = '\0';
    entry.getName(name, sizeof(name));
    entry.close();
    const size_t len = strlen(name);
    const bool isPart = len > 5 && strcmp(name + len - 5, ".part") == 0;
    const bool isBak = len > 4 && strcmp(name + len - 4, ".bak") == 0;
    if (!isPart && !isBak) continue;
    if (!removeIfExists((chapters + "/" + name).c_str())) clean = false;
  }
  dirHandle.close();
  return clean;
}

bool cleanupLegacyBooks() {
  if (!Storage.exists(kLegacyBooksDir)) return true;  // already gone: idempotent
  LOG_INF(kTag, "removing legacy TXT products under %s", kLegacyBooksDir);

  HalFile dir = Storage.open(kLegacyBooksDir);
  if (!dir.isOpen()) {
    LOG_ERR(kTag, "cannot open %s for legacy cleanup", kLegacyBooksDir);
    return false;
  }
  char name[kLegacyNameBytes] = {};
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry.isOpen()) break;
    name[0] = '\0';
    entry.getName(name, sizeof(name));
    const bool isDir = entry.isDirectory();
    entry.close();
    if (name[0] == '\0' || isDir) continue;

    const size_t len = strlen(name);
    const bool isTxt = len > 4 && strcmp(name + len - 4, ".txt") == 0;
    const bool isMeta = len > 5 && strcmp(name + len - 5, ".meta") == 0;
    const bool isPart = len > 5 && strcmp(name + len - 5, ".part") == 0;
    if (!isTxt && !isMeta && !isPart) continue;

    const std::string path = std::string(kLegacyBooksDir) + "/" + name;
    // Only real books and previews had a reader cache; `.meta`/`.part` never did,
    // and hashing a path that never existed would just miss.
    const bool ok = isTxt ? removeTxtAndCache(path) : Storage.remove(path.c_str());
    if (!ok) LOG_ERR(kTag, "legacy removal failed: %s", path.c_str());
  }
  dir.close();

  if (!Storage.removeDir(kLegacyBooksDir)) {
    // Leftover files keep the directory non-empty. Not fatal: nothing reads it.
    LOG_ERR(kTag, "cannot remove %s", kLegacyBooksDir);
    return false;
  }
  return true;
}

namespace {
constexpr uint32_t kSpineMagic = 0x3153584A;  // "JXS1"
constexpr uint16_t kSpineVersion = 1;
constexpr uint16_t kSpineEntrySize = 8;

void putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void putU32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}
uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

std::string spineMapPath(const char* novelId) {
  return std::string(kRoot) + "/" + novelId + "/spine.bin";
}

std::string syncedProgressPath(const char* novelId) {
  return std::string(kRoot) + "/" + novelId + "/synced_progress";
}

bool writeSpineMap(const char* novelId) {
  if (novelId == nullptr || novelId[0] == '\0') return false;
  TocReader toc;
  if (!toc.open(novelId)) return false;

  // Every toc record is a spine item now: the EPUB includes a placeholder page
  // for uncached/unpurchased/locked chapters, so spine N is toc ordinal N. A map
  // that listed only packaged chapters shifted every index after the first gap and
  // made reader progress and "read this chapter" land on the wrong chapter.
  uint32_t count = toc.count();
  if (count == 0) {
    toc.close();
    return false;
  }

  HalFile file;
  const std::string path = spineMapPath(novelId);
  if (!Storage.openFileForWrite(kTag, path, file)) {
    toc.close();
    return false;
  }
  uint8_t header[12];
  putU32(header, kSpineMagic);
  putU16(header + 4, kSpineVersion);
  putU16(header + 6, kSpineEntrySize);
  putU32(header + 8, count);
  bool ok = file.write(header, sizeof(header)) == sizeof(header);

  uint32_t written = 0;
  for (uint32_t i = 0; ok && i < toc.count(); ++i) {
    TocRecord rec;
    if (!toc.read(i, rec)) {
      ok = false;
      break;
    }
    uint8_t entry[8];
    putU32(entry, rec.chapterId);
    putU32(entry + 4, i);
    if (file.write(entry, sizeof(entry)) != sizeof(entry)) {
      ok = false;
      break;
    }
    ++written;
  }
  file.flush();
  file.close();
  toc.close();
  if (!ok || written != count) {
    Storage.remove(path.c_str());
    return false;
  }
  LOG_INF(kTag, "spine map: %u entries for %s", (unsigned)count, novelId);
  return true;
}

namespace {
// Opens the map and validates the header. The caller then reads entries
// sequentially; the map is at most a few thousand 8-byte records.
bool openSpineMap(const char* novelId, HalFile& file, uint32_t& count) {
  if (!Storage.openFileForRead(kTag, spineMapPath(novelId), file)) return false;
  uint8_t header[12];
  if (file.read(header, sizeof(header)) != sizeof(header)) {
    file.close();
    return false;
  }
  if (getU32(header) != kSpineMagic || getU16(header + 4) != kSpineVersion ||
      getU16(header + 6) != kSpineEntrySize) {
    file.close();
    return false;
  }
  count = getU32(header + 8);
  return true;
}
}  // namespace

bool readSpineEntry(const char* novelId, uint32_t spineIndex, SpineEntry& out) {
  HalFile file;
  uint32_t count = 0;
  if (!openSpineMap(novelId, file, count)) return false;
  if (spineIndex >= count) {
    file.close();
    return false;
  }
  uint8_t entry[8];
  for (uint32_t i = 0; i <= spineIndex; ++i) {
    if (file.read(entry, sizeof(entry)) != sizeof(entry)) {
      file.close();
      return false;
    }
  }
  file.close();
  out.chapterId = getU32(entry);
  out.ordinal = getU32(entry + 4);
  return true;
}

bool findSpineForOrdinal(const char* novelId, uint32_t ordinal, uint32_t& spineIndex) {
  HalFile file;
  uint32_t count = 0;
  if (!openSpineMap(novelId, file, count)) return false;
  uint8_t entry[8];
  for (uint32_t i = 0; i < count; ++i) {
    if (file.read(entry, sizeof(entry)) != sizeof(entry)) break;
    if (getU32(entry + 4) == ordinal) {
      spineIndex = i;
      file.close();
      return true;
    }
  }
  file.close();
  return false;
}

bool findOrdinalForChapter(const char* novelId, uint32_t chapterId, uint32_t& ordinal) {
  HalFile file;
  uint32_t count = 0;
  if (!openSpineMap(novelId, file, count)) return false;
  uint8_t entry[8];
  for (uint32_t i = 0; i < count; ++i) {
    if (file.read(entry, sizeof(entry)) != sizeof(entry)) break;
    if (getU32(entry) == chapterId) {
      ordinal = getU32(entry + 4);
      file.close();
      return true;
    }
  }
  file.close();
  return false;
}

}  // namespace JjwxcStore
