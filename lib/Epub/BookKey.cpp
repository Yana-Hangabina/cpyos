#include "BookKey.h"

#include <DeviceSecret.h>
#include <HalStorage.h>
#include <HalSystem.h>  // getDeviceId: the hardware identity the v2 wrap uses
#include <Logging.h>
#include <Memory.h>
#include <esp_random.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/hash.h>

#include <cstring>
#include <memory>

namespace bookkey {

namespace {

// magic(4) version(1) expiresAt(8, LE) | iv(12) | wrapped key(16) | tag(16).
// The 13-byte header is the GCM additional data.
constexpr uint8_t MAGIC[4] = {'F', 'I', 'B', 'K'};
// v2 wraps with a key derived from the eFuse MAC: no flashing tool can change it,
// so an "erase device" install (routine here when partitions move) no longer
// orphans every book. v1 used the NVS-stored secret and is read + migrated.
constexpr uint8_t VERSION_HARDWARE = 2;
constexpr uint8_t VERSION_SECRET = 1;
constexpr size_t HEADER_LEN = 13;
constexpr size_t IV_LEN = 12;
constexpr size_t TAG_LEN = 16;
constexpr size_t FILE_LEN = HEADER_LEN + IV_LEN + KEY_LEN + TAG_LEN;

std::string keyPath(const std::string& bookPath) { return bookPath + ".key"; }

// wolfSSL's Aes carries the GCM tables (KBs): heap, not the task stack.
struct AesGcm {
  std::unique_ptr<Aes> aes = makeUniqueNoThrow<Aes>();
  bool ready = aes && wc_AesInit(aes.get(), nullptr, INVALID_DEVID) == 0;
  ~AesGcm() {
    if (ready) wc_AesFree(aes.get());
  }
};

// sha256("book-key:hw1:" || MAC): the identity no flashing tool can change, so
// this key survives a full erase. The label is domain-separated from every other
// use of the same identity and must never be rotated except with a format change
// -- doing so would orphan the whole library in one line.
bool hardwareWrapKey(uint8_t (&out)[32]) {
  HalSystem::DeviceId id{};
  if (!HalSystem::getDeviceId(id)) return false;
  static constexpr char LABEL[] = "book-key:hw1:";
  uint8_t input[sizeof(LABEL) - 1 + sizeof(id)];
  memcpy(input, LABEL, sizeof(LABEL) - 1);
  memcpy(input + sizeof(LABEL) - 1, id.data(), sizeof(id));
  return wc_Sha256Hash(input, sizeof(input), out) == 0;
}

// The pre-v2 derivation, kept only to read (and then migrate) existing books.
// Distinct label from every plugin device ID, which hashes the secret first.
bool secretWrapKey(uint8_t (&out)[32]) {
  uint8_t secret[32];
  if (!deviceSecret(secret)) return false;
  static constexpr char LABEL[] = "book-key:";
  uint8_t input[sizeof(LABEL) - 1 + sizeof(secret)];
  memcpy(input, LABEL, sizeof(LABEL) - 1);
  memcpy(input + sizeof(LABEL) - 1, secret, sizeof(secret));
  return wc_Sha256Hash(input, sizeof(input), out) == 0;
}

}  // namespace

bool write(const std::string& bookPath, const uint8_t (&key)[KEY_LEN], const int64_t expiresAt) {
  uint8_t kek[32];
  uint8_t version = VERSION_HARDWARE;
  if (!hardwareWrapKey(kek)) {
    // A build without a hardware id keeps the old scheme. Say it out loud: those
    // books are the ones a full erase still orphans, and a silent fallback would
    // make that look like a corrupted file later.
    LOG_ERR("BKEY", "no hardware id; wrapping with the NVS secret (a full erase will orphan this key)");
    if (!secretWrapKey(kek)) return false;
    version = VERSION_SECRET;
  }

  uint8_t file[FILE_LEN];
  memcpy(file, MAGIC, sizeof(MAGIC));
  file[4] = version;
  const uint64_t expires = static_cast<uint64_t>(expiresAt);
  for (int i = 0; i < 8; i++) file[5 + i] = static_cast<uint8_t>(expires >> (8 * i));
  uint8_t* iv = file + HEADER_LEN;
  esp_fill_random(iv, IV_LEN);
  uint8_t* wrapped = iv + IV_LEN;
  uint8_t* tag = wrapped + KEY_LEN;

  AesGcm gcm;
  const bool sealed =
      gcm.ready && wc_AesGcmSetKey(gcm.aes.get(), kek, sizeof(kek)) == 0 &&
      wc_AesGcmEncrypt(gcm.aes.get(), wrapped, key, KEY_LEN, iv, IV_LEN, tag, TAG_LEN, file, HEADER_LEN) == 0;
  if (!sealed) {
    LOG_ERR("BKEY", "Wrap failed");
    return false;
  }
  // Stage and swap: a torn write must not replace a working key with garbage.
  const std::string path = keyPath(bookPath);
  const std::string tmp = path + ".tmp";
  {
    HalFile f;
    if (!Storage.openFileForWrite("BKEY", tmp, f) || f.write(file, FILE_LEN) != FILE_LEN) {
      LOG_ERR("BKEY", "Write failed: %s", tmp.c_str());
      return false;
    }
  }
  return Storage.replaceFile(tmp.c_str(), path.c_str());
}

ReadStatus readStatus(const std::string& bookPath, uint8_t (&key)[KEY_LEN], int64_t* expiresAt) {
  uint8_t file[FILE_LEN];
  size_t got = 0;
  {
    HalFile f;
    if (!Storage.openFileForRead("BKEY", keyPath(bookPath), f)) return ReadStatus::Missing;
    const int n = f.read(file, sizeof(file));
    if (n <= 0) return ReadStatus::Unreadable;
    got = static_cast<size_t>(n);
  }
  if (got != FILE_LEN || memcmp(file, MAGIC, sizeof(MAGIC)) != 0 ||
      (file[4] != VERSION_HARDWARE && file[4] != VERSION_SECRET)) {
    return ReadStatus::Unreadable;
  }

  const uint8_t version = file[4];
  uint8_t kek[32];
  // A version whose key cannot even be derived (no hardware id, or the secret
  // refused on this task) is not a mismatch: reporting ForeignDevice for it would
  // send the user to re-cache a book that is probably fine.
  const bool derived = version == VERSION_HARDWARE ? hardwareWrapKey(kek) : secretWrapKey(kek);
  if (!derived) return ReadStatus::Unreadable;

  const uint8_t* iv = file + HEADER_LEN;
  const uint8_t* wrapped = iv + IV_LEN;
  const uint8_t* tag = wrapped + KEY_LEN;
  AesGcm gcm;
  const bool opened =
      gcm.ready && wc_AesGcmSetKey(gcm.aes.get(), kek, sizeof(kek)) == 0 &&
      wc_AesGcmDecrypt(gcm.aes.get(), key, wrapped, KEY_LEN, iv, IV_LEN, tag, TAG_LEN, file, HEADER_LEN) == 0;
  if (!opened) {
    // Well-formed, and this device's identity does not open it: a key written by
    // another device, by a previous secret generation, or a tampered file. Only
    // this case can be repaired by re-caching the book.
    LOG_ERR("BKEY", "wrapped by another identity or tampered: %s", bookPath.c_str());
    return ReadStatus::ForeignDevice;
  }
  uint64_t expires = 0;
  for (int i = 0; i < 8; i++) expires |= static_cast<uint64_t>(file[5 + i]) << (8 * i);
  *expiresAt = static_cast<int64_t>(expires);

  if (version == VERSION_SECRET) {
    // Migrate on the first successful read, so the next erase costs the user
    // nothing. Best effort: a failed rewrite leaves a working v1 key in place.
    if (write(bookPath, key, *expiresAt)) {
      LOG_INF("BKEY", "migrated a v1 key to the hardware wrap: %s", bookPath.c_str());
    } else {
      LOG_ERR("BKEY", "could not migrate a v1 key (retried on the next open): %s", bookPath.c_str());
    }
  }
  return ReadStatus::Ok;
}

bool read(const std::string& bookPath, uint8_t (&key)[KEY_LEN], int64_t* expiresAt) {
  return readStatus(bookPath, key, expiresAt) == ReadStatus::Ok;
}

}  // namespace bookkey
