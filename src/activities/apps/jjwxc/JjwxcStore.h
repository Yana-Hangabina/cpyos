#pragma once

#include <Epub/JjwxcCommentSidecar.h>
#include <HalStorage.h>
#include <JjwxcCookies.h>
#include <JjwxcProtocol.h>
#include <WeReadStore.h>

#include <cstddef>
#include <cstdint>
#include <string>

// On-SD persistence for the JJWXC app.
//
// Layout:
//   /.crosspoint/jjwxc/credentials            obfuscated token + identifiers
//   /.crosspoint/jjwxc/cookies                obfuscated WAP cookie jar (the session)
//   /.crosspoint/jjwxc/account                obfuscated login name + password
//   /.crosspoint/jjwxc/device                 obfuscated fingerprint; never cleared
//   /.crosspoint/jjwxc/disclaimer             one-byte ToS marker
//   /.crosspoint/jjwxc/<novelId>/novel.bin    JXN1 header + one NovelRecord
//   /.crosspoint/jjwxc/<novelId>/toc.bin      JXT1 header + one TocRecord per chapter
//   /.crosspoint/jjwxc/<novelId>/chapters/<chapterId>.xhtml
//   /.crosspoint/jjwxc/<novelId>/comments/<chapterId>.bin   JJC1 paragraph comment totals
//   /.crosspoint/jjwxc/<novelId>/cover.source.jpg
//   /.crosspoint/jjwxc/<novelId>/cover.thumb.bmp    derived; the detail page draws this
//   /JJWXC/<title>-<novelId>.epub             the single cumulative artefact
//
// The files under `.crosspoint` are the chapter-level cache; the EPUB under
// `/JJWXC` is regenerated from *all* cached chapters on every run, so one novel
// always resolves to exactly one finished file. That is what stops a range
// download from forking into several Recent entries (RecentBooksStore keys on
// the path) and what lets LibraryBuilder read title, author and cover from
// inside the book instead of from the filename. Byte layouts are documented in
// docs/file-formats.md.
//
// Credentials go through obfuscation::obfuscateToBase64, which XORs with the
// ESP32's eFuse MAC before Base64. That is NOT encryption and must never be
// described as such: it stops casual reading of the SD card and binds the blob to
// this specific chip, so a card moved to another device (or a PC) yields nothing
// usable. Anyone holding this device can recover the password, because the device
// itself has to. The password is only stored at all when the user explicitly
// picked "save account and password" at login (SaveScope below).
namespace JjwxcStore {

constexpr const char* kRoot = "/.crosspoint/jjwxc";
constexpr const char* kCredentialsPath = "/.crosspoint/jjwxc/credentials";
constexpr const char* kCookiesPath = "/.crosspoint/jjwxc/cookies";
constexpr const char* kAccountPath = "/.crosspoint/jjwxc/account";
constexpr const char* kDevicePath = "/.crosspoint/jjwxc/device";
constexpr const char* kDisclaimerPath = "/.crosspoint/jjwxc/disclaimer";
// Finished books live outside `.crosspoint`: LibraryBuilder skips every entry
// whose name starts with '.' (lib/LibraryIndex/LibraryBuilder.cpp:196, used at
// :472), so nothing below kRoot can ever reach the shelf.
constexpr const char* kOutputDir = "/JJWXC";
// Pre-EPUB artefacts. Removed once by cleanupLegacyBooks(); nothing reads them
// any more, and the reader progress anchored to those paths is discarded with
// the user's agreement.
constexpr const char* kLegacyBooksDir = "/.crosspoint/jjwxc/books";

// "JXT1" / "JXN1" as they appear on the wire (little-endian uint32).
constexpr uint32_t kTocMagic = 0x3154584A;
constexpr uint32_t kNovelMagic = 0x314E584A;
constexpr uint16_t kIndexVersion = 1;
// Same 12-byte header paradigm as WeRead's indexes (WeReadStore.h:108-116):
// uint32 magic / uint16 version / uint16 recordSize / uint32 recordCount.
constexpr uint16_t kIndexHeaderSize = 12;

enum class ChapterState : uint8_t {
  Missing = 0,      // never fetched, or invalidated by a remoteSize change
  Cached = 1,       // chapters/<chapterId>.xhtml is on the card
  Unpurchased = 2,  // VIP chapter this account has not bought; holds a placeholder
  Locked = 3,       // author-locked chapter; holds a placeholder
};

// True when the record points at a chapter file that belongs in the EPUB: a real
// body (Cached) or an explicit stub explaining why there is none. Missing is the
// only state with no file, and therefore the only one left out of the manifest,
// the nav and the spine alike.
constexpr bool isPackaged(const uint8_t rawState) { return rawState != static_cast<uint8_t>(ChapterState::Missing); }

// True only for a chapter whose body is actually on the card. A stub is packaged
// but not readable, and conflating the two is what made a book of 114 real
// chapters and 72 purchase stubs announce itself as fully cached.
constexpr bool isReadable(const uint8_t rawState) { return rawState == static_cast<uint8_t>(ChapterState::Cached); }

// One entry per chapter of the remote catalogue, in remote order. Fixed size and
// padding-free, so the record is written and read back verbatim.
struct TocRecord {
  uint32_t chapterId = 0;
  char title[96] = {};
  uint32_t remoteSize = 0;  // server-reported size; a change invalidates the cache
  uint8_t state = 0;        // ChapterState
  uint8_t isVip = 0;        // 0 = free, 2 = VIP (ChapterEntry::isVip)
  uint8_t reserved[2] = {};
  uint32_t localBytes = 0;  // size of chapters/<chapterId>.xhtml
  uint32_t savedAt = 0;     // unix seconds of the last successful fetch
};
static_assert(sizeof(TocRecord) == 116, "JXT1 record layout must stay at 116 bytes");

// Per-novel metadata: everything the EPUB's OPF and the "Downloaded" list need
// while offline. Fixed size and padding-free.
//
// The two counts are NOT interchangeable and mixing them up is the bug this
// layout exists to prevent: `cachedChapterCount` is chapters with a real body on
// the card (readable offline), `packagedChapterCount` is everything the EPUB
// contains, which also counts the "[本章未购买]" stubs. Every user-facing "cached
// N" number must use the first one.
struct NovelRecord {
  char novelId[16] = {};
  char title[96] = {};
  char author[48] = {};
  char coverUrl[192] = {};
  uint32_t remoteChapterCount = 0;
  uint32_t cachedChapterCount = 0;    // records whose state == Cached
  uint32_t savedAt = 0;
  uint32_t packagedChapterCount = 0;  // records whose state != Missing
};
static_assert(sizeof(NovelRecord) == 368, "JXN1 record layout must stay at 368 bytes");

bool ensureRoot();
// Creates /.crosspoint/jjwxc/<novelId> and its chapters/ subdirectory.
bool ensureNovelRoot(const char* novelId);

bool isDisclaimerAccepted();
bool acceptDisclaimer();

bool loadCredentials(jjwxc::Credentials& out);
bool saveCredentials(const jjwxc::Credentials& cred);
bool clearCredentials();

// The WAP session. This, not `credentials`, is what a WAP login produces: the sid is
// 133 characters and Credentials::token is 48, and widening that field would grow
// every copy of the struct for a value only one of the two protocols uses. The jar
// already has a serialised form and already carries the readerId inside the sid, so
// persisting the jar is both smaller and harder to get out of sync.
//
// Logout clears this file and leaves `device` alone -- the same split that fixed the
// "every login demands a verification code" bug: the session is disposable, the
// device's identity is not.
//
// `nowUnix` is the clock the expiry check uses. Pass 0 when the device has not synced:
// the jar then treats nothing as expired, so a cold boot before NTP is not a logout.
bool loadCookies(jjwxc::CookieJar& jar, uint64_t nowUnix);
bool saveCookies(const jjwxc::CookieJar& jar);
bool clearCookies();
// The one question the UI actually asks: is there a session to use.
bool hasSession(const jjwxc::CookieJar& jar, uint64_t nowUnix);

// What the user allowed the app to keep after a login. Chosen on the login page
// and persisted with the account itself, so the next login can offer it again as
// the default instead of asking from scratch.
enum class SaveScope : uint8_t {
  Nothing = 0,  // this session only; nothing reaches the card
  AccountOnly = 1,
  AccountAndPassword = 2,
};

// loginName is what JJWXC's own API calls it: an email, a phone number or a
// username. password is empty unless the scope asked for it, and loadAccount()
// refuses to hand one back when the stored scope did not.
struct SavedAccount {
  std::string loginName;
  std::string password;
  SaveScope scope = SaveScope::Nothing;

  bool valid() const { return !loginName.empty(); }
  bool hasPassword() const { return !password.empty(); }
};

// Kept apart from credentials on purpose: that file is the *session* and logout
// clears it, while this one is the *login shortcut* and has to survive a logout,
// or "sign in with the last account" would never work a second time.
bool loadAccount(SavedAccount& out);
// scope Nothing removes the file rather than writing an empty one.
bool saveAccount(const SavedAccount& acc);
bool clearAccount();

// The device fingerprint JJWXC's login endpoint is sent, plus the readerId the
// server handed back. This is a THIRD file, and the split is not cosmetic: it used
// to live inside `credentials`, so logging out -- which removes that file -- threw
// the fingerprint away with the token. The next login then presented a fresh one,
// the server saw a brand-new device, and demanded an emailed verification code
// every single time. Nothing clears this file; it is the one thing about this
// device that must be stable across sessions.
struct DeviceIdentity {
  char identifiers[64] = {};
  char readerId[16] = {};

  bool hasIdentifiers() const { return identifiers[0] != '\0'; }
};

bool loadDevice(DeviceIdentity& out);
bool saveDevice(const DeviceIdentity& dev);

std::string novelDirectory(const char* novelId);
std::string tocPath(const char* novelId);
std::string novelInfoPath(const char* novelId);
std::string chapterPath(const char* novelId, uint32_t chapterId);

// Paragraph-comment totals per chapter, a device-local sidecar the reader reads
// while offline. Format and validation live in Epub/JjwxcCommentSidecar.h; this
// module only owns the path, the SD write and the atomic replace.
//
// One file per chapter rather than one per novel: a cache run rejects chapters
// one at a time, and rewriting a whole-novel table per chapter would be O(n^2)
// SD traffic on a 4000-chapter book. `zero counters` are the caller's business
// to drop -- the reader treats a missing paragraph as "no bubble", so storing
// an entry with total == 0 buys nothing and costs six bytes.
std::string commentCountsDir(const char* novelId);
std::string commentCountsPath(const char* novelId, uint32_t chapterId);
// Writes the file atomically (`.part` then rename). count > kMaxEntries is
// rejected rather than truncated: a silently shortened table would hide
// bubbles, and the caller can always save a smaller, page-scoped subset.
bool saveCommentCounts(const char* novelId, uint32_t chapterId, const JjwxcCommentSidecar::Count* counts,
                       size_t count);
// Reads at most `maxOut` entries and always reports the file's entry count, so
// "damaged file" (false) stays distinguishable from "more rows than asked for".
// A missing or rejected file is false with outCount == 0.
bool loadCommentCounts(const char* novelId, uint32_t chapterId, JjwxcCommentSidecar::Count* out, size_t maxOut,
                       size_t& outCount);
// Comment bodies for one paragraph (the list shown by the reader's bubble
// panel). The first successful fetch is written here; later opens read it with
// the radio off, and the panel's Refresh action rewrites it.
std::string commentBodiesPath(const char* novelId, uint32_t chapterId, uint32_t paragraphId);
bool saveCommentBodies(const char* novelId, uint32_t chapterId, uint32_t paragraphId,
                       const JjwxcCommentSidecar::Bodies::Item* items, size_t count, uint32_t total);
// A missing or rejected file is false with outCount == 0 (and total == 0).
bool loadCommentBodies(const char* novelId, uint32_t chapterId, uint32_t paragraphId,
                       JjwxcCommentSidecar::Bodies::Item* out, size_t maxOut, size_t& outCount, uint32_t& total);
bool removeCommentBodies(const char* novelId, uint32_t chapterId, uint32_t paragraphId);
std::string coverSourcePath(const char* novelId);
// The device-wrapped AES-128 content key for the finished EPUB at `epubPath`
// (bookkey::write). Reuses an existing key when it was wrapped by this device,
// otherwise generates and writes a fresh one. MUST run on the UI task: the
// device-secret path touches NVS, which cannot be executed from a PSRAM-stack
// job. The packager receives the raw bytes from the caller.
bool ensureBookContentKey(const std::string& epubPath, uint8_t (&key)[16]);

// Device-bound key for the on-card chapter cache. The salt lives at
// /.crosspoint/jjwxc/cachekey; key+nonceSeed are derived from the eFuse MAC and
// that salt (JjwxcBookCrypto). Safe to call from the cache job: it touches SD
// and eFuse only, never NVS.
bool ensureCacheCrypto(uint8_t (&key)[16], uint8_t (&nonceSeed)[8]);
// Replaces a plaintext chapters/<id>.xhtml with a JJX1 blob. Already-sealed
// files are left untouched. Best effort at the call sites: a failed seal leaves
// the chapter readable (the finished EPUB is protected independently).
bool sealChapterFile(const std::string& path);
// The detail page's drawable cover: cover.source.jpg run through the same
// JPEG/PNG -> BMP converter WeRead uses for its shelf covers
// (WeReadClient.cpp:3427-3430), at the size the page lays out. It is a derived
// cache like Epub's thumb_<height>.bmp: the EPUB keeps packaging from
// cover.source.jpg, and deleting this file only costs one regeneration.
std::string coverThumbPath(const char* novelId);
// Transient names, all siblings of the artefact each one becomes.
std::string contentPartPath(const char* novelId);
std::string navPartPath(const char* novelId);
std::string centralPartPath(const char* novelId);
std::string bookPartPath(const char* novelId);
// The one finished artefact for a novel. Derived from the *persisted* title, so a
// server-side rename cannot silently fork the library entry; saveNovel() removes
// the stale file when the derived name does change.
std::string finalBookPath(const NovelRecord& rec);

bool loadNovel(const char* novelId, NovelRecord& out);
// Writes novel.bin atomically. When the derived EPUB path changed because the
// title changed, the previous EPUB is removed so one novel never owns two files.
bool saveNovel(const NovelRecord& rec);

// Sequential and random access to toc.bin, with header validation.
// `openForUpdate()` keeps the handle writable so a download patches one record in
// place instead of rewriting the whole index per chapter (O(n^2) SD traffic on a
// 4000-chapter novel otherwise).
class TocReader {
 public:
  bool open(const char* novelId);
  bool openForUpdate(const char* novelId);
  bool isOpen() const { return file_.isOpen(); }
  uint32_t count() const { return count_; }
  // Not const: HalFile::seek()/read() are mutating, exactly as in WeRead's
  // readTocRecord(HalFile&, ...) (WeReadStore.cpp:575-577).
  bool read(uint32_t index, TocRecord& out);
  // Sequential cursor, for the streaming merge in loadChapters().
  bool next(TocRecord& out);
  void rewind();
  bool update(uint32_t index, const TocRecord& rec);
  void close();

 private:
  bool openPath(const std::string& path, bool writable);

  HalFile file_;
  uint32_t count_ = 0;
  uint32_t cursor_ = 0;
};

// Reads novel.bin for every <novelId> directory below kRoot. Entries arrive in
// directory order; the caller caps and sorts. Returns false on a directory-open
// failure; a novel with a damaged novel.bin is skipped rather than failing the
// scan.
using NovelSink = bool (*)(void* ctx, const NovelRecord& rec, size_t index);
bool listNovels(NovelSink sink, void* ctx, size_t maxEntries);

// What one catalogue holds.
//
// `firstPending`/`lastPending` are toc ordinals and span every chapter that is
// not readable yet -- Missing, Unpurchased and Locked alike. The old Missing-only
// range is what made a book whose tail was 72 purchase stubs report "all chapters
// cached", because a whole-book cache had turned every Missing into Unpurchased.
struct TocSummary {
  uint32_t count = 0;
  uint32_t readable = 0;     // state == Cached
  uint32_t packaged = 0;     // state != Missing, i.e. everything in the EPUB
  uint32_t unpurchased = 0;  // state == Unpurchased
  uint32_t locked = 0;       // state == Locked
  uint32_t firstPending = 0;
  uint32_t lastPending = 0;
  bool hasPending = false;
};

// Walks toc.bin once and reports its summary plus the highest ordinal that would
// go into the EPUB. The download job uses the count to size the packaging
// progress bar, and the highest ordinal to decide whether a run only appended --
// which is what makes carrying the reader's progress across the cache
// invalidation safe. Returns false when there is no readable index.
bool readTocSummary(const char* novelId, TocSummary& out, uint32_t& outHighestPackaged);

// Streams a freshly fetched remote catalogue into toc.bin while carrying over the
// cache state of the previous one.
//
// Both lists are in ascending chapterId order (JjwxcApi verifies the remote list
// is monotonic), so the merge is a single forward walk with one record of
// lookahead: a 4000-chapter novel needs no in-memory copy of its old index, which
// at 116 bytes per record would be 464 KB. The new index goes to `toc.bin.part`
// through WeReadStore::IndexWriter and only replaces toc.bin on finish(), so a
// cancelled or failed catalogue fetch leaves the previous one intact.
class TocMerger {
 public:
  bool begin(const char* novelId);
  // Merges one remote entry and appends it. `out` receives the record written, so
  // the caller can mirror the state into its in-memory list.
  bool add(const jjwxc::ChapterEntry& entry, TocRecord& out);
  bool finish(TocSummary& out);
  void abort();

 private:
  WeReadStore::IndexWriter writer_;
  TocReader previous_;
  TocRecord previousRecord_{};
  bool previousValid_ = false;
  std::string novelId_;
  TocSummary summary_{};
  bool active_ = false;
};

// The EPUB spine map: one entry per spine item, in spine order. Written at
// packaging time so a reader position (spineIndex) can be translated back into a
// remote chapterId for cross-device progress sync -- the reader itself has no
// idea what a JJWXC chapter is.
//
// Format: magic "JXS1" / uint16 version / uint16 entrySize / uint32 count,
// followed by count x (uint32 chapterId, uint32 tocOrdinal), little-endian.
struct SpineEntry {
  uint32_t chapterId = 0;
  uint32_t ordinal = 0;
};

std::string spineMapPath(const char* novelId);
// Walks toc.bin and rewrites the map from what is actually packaged. Best effort:
// packaging must not fail because the map could not be written.
bool writeSpineMap(const char* novelId);
bool readSpineEntry(const char* novelId, uint32_t spineIndex, SpineEntry& out);
bool findSpineForOrdinal(const char* novelId, uint32_t ordinal, uint32_t& spineIndex);
bool findOrdinalForChapter(const char* novelId, uint32_t chapterId, uint32_t& ordinal);
// Last reader position whose chapterId/offset was pushed to the cloud. Kept as
// the raw 10-byte progress.bin image, so "nothing changed" is a byte compare.
std::string syncedProgressPath(const char* novelId);

// Deletes one novel completely: the EPUB, the per-chapter files, toc/novel
// metadata, the spine map and the synced-progress marker. The reader's cache
// directory is keyed by the EPUB path and is removed by the caller (it lives in
// BookCacheUtils, not here). A missing directory is success.
bool removeNovel(const char* novelId);

// Removes everything an interrupted run left in one novel directory: the
// `.part`/`.bak` siblings of toc.bin, novel.bin, the EPUB and each chapter.
// Called before a job writes anything, so debris cannot accumulate.
bool reclaimNovelTempFiles(const char* novelId);

// One-shot removal of the pre-EPUB TXT products: every `*.txt`, `*.meta` and
// `*.part` under kLegacyBooksDir plus the `txt_<hash>` reader caches built for
// them, then the directory itself. Idempotent (a missing directory is success)
// and never fatal: callers log and carry on.
bool cleanupLegacyBooks();

}  // namespace JjwxcStore
