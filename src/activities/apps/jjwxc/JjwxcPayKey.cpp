#include "JjwxcPayKey.h"

#include <DeviceSecret.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_random.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/hash.h>

#include <cstring>
#include <memory>
#include <string>

namespace jjwxcpay {
namespace {

constexpr const char* kTag = "JJPAY";
constexpr const char* kPath = "/.crosspoint/jjwxc/paykey";
constexpr const char* kDir = "/.crosspoint/jjwxc";

// magic(4) version(1) | iv(12) | wrapped password(6) | tag(16). The 5-byte
// header is the GCM additional data, so a version bump cannot be confused
// with a valid old file.
constexpr uint8_t MAGIC[4] = {'F', 'I', 'P', 'K'};
constexpr uint8_t VERSION = 1;
constexpr size_t HEADER_LEN = 5;
constexpr size_t IV_LEN = 12;
constexpr size_t CODE_LEN = 6;
constexpr size_t TAG_LEN = 16;
constexpr size_t FILE_LEN = HEADER_LEN + IV_LEN + CODE_LEN + TAG_LEN;

struct AesGcm {
  std::unique_ptr<Aes> aes = makeUniqueNoThrow<Aes>();
  bool ready = aes && wc_AesInit(aes.get(), nullptr, INVALID_DEVID) == 0;
  ~AesGcm() {
    if (ready) wc_AesFree(aes.get());
  }
};

bool wrapKey(uint8_t (&out)[32]) {
  uint8_t secret[32];
  if (!deviceSecret(secret)) return false;
  static constexpr char LABEL[] = "jjwxc-pay:";
  uint8_t input[sizeof(LABEL) - 1 + sizeof(secret)];
  memcpy(input, LABEL, sizeof(LABEL) - 1);
  memcpy(input + sizeof(LABEL) - 1, secret, sizeof(secret));
  return wc_Sha256Hash(input, sizeof(input), out) == 0;
}

bool validCode(const char* code) {
  if (code == nullptr) return false;
  for (int i = 0; i < static_cast<int>(CODE_LEN); ++i) {
    if (code[i] < '0' || code[i] > '9') return false;
  }
  return code[CODE_LEN] == '\0';
}

void wipe(void* p, size_t n) {
  volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
  while (n-- > 0) *v++ = 0;
}

bool removeFile() {
  if (!Storage.exists(kPath)) return true;
  return Storage.remove(kPath);
}

}  // namespace

bool available() { return Storage.exists(kPath); }

bool load(char (&out)[7]) {
  out[0] = '\0';
  uint8_t file[FILE_LEN];
  size_t got = 0;
  {
    HalFile f;
    if (!Storage.openFileForRead(kTag, kPath, f)) return false;
    const int n = f.read(file, sizeof(file));
    if (n > 0) got = static_cast<size_t>(n);
  }
  if (got != FILE_LEN || memcmp(file, MAGIC, sizeof(MAGIC)) != 0 || file[4] != VERSION) {
    wipe(file, sizeof(file));
    removeFile();
    return false;
  }
  uint8_t kek[32];
  if (!wrapKey(kek)) {
    wipe(file, sizeof(file));
    return false;
  }
  const uint8_t* iv = file + HEADER_LEN;
  const uint8_t* wrapped = iv + IV_LEN;
  const uint8_t* tag = wrapped + CODE_LEN;
  uint8_t code[CODE_LEN] = {};
  AesGcm gcm;
  const bool opened =
      gcm.ready && wc_AesGcmSetKey(gcm.aes.get(), kek, sizeof(kek)) == 0 &&
      wc_AesGcmDecrypt(gcm.aes.get(), code, wrapped, CODE_LEN, iv, IV_LEN, tag, TAG_LEN, file, HEADER_LEN) == 0;
  wipe(kek, sizeof(kek));
  wipe(file, sizeof(file));
  if (!opened) {
    LOG_ERR(kTag, "saved payment password is not from this device; removing it");
    wipe(code, sizeof(code));
    removeFile();
    return false;
  }
  for (size_t i = 0; i < CODE_LEN; ++i) {
    out[i] = static_cast<char>(code[i]);
  }
  out[CODE_LEN] = '\0';
  wipe(code, sizeof(code));
  if (!validCode(out)) {
    wipe(out, CODE_LEN);
    out[0] = '\0';
    removeFile();
    return false;
  }
  return true;
}

bool save(const char* code) {
  if (!validCode(code)) return false;
  if (!Storage.ensureDirectoryExists(kDir)) return false;
  uint8_t kek[32];
  if (!wrapKey(kek)) return false;

  uint8_t file[FILE_LEN];
  memcpy(file, MAGIC, sizeof(MAGIC));
  file[4] = VERSION;
  uint8_t* iv = file + HEADER_LEN;
  esp_fill_random(iv, IV_LEN);
  uint8_t* wrapped = iv + IV_LEN;
  uint8_t* tag = wrapped + CODE_LEN;

  AesGcm gcm;
  const bool sealed =
      gcm.ready && wc_AesGcmSetKey(gcm.aes.get(), kek, sizeof(kek)) == 0 &&
      wc_AesGcmEncrypt(gcm.aes.get(), wrapped, reinterpret_cast<const uint8_t*>(code), CODE_LEN, iv, IV_LEN, tag,
                       TAG_LEN, file, HEADER_LEN) == 0;
  wipe(kek, sizeof(kek));
  if (!sealed) {
    wipe(file, sizeof(file));
    LOG_ERR(kTag, "could not wrap the payment password");
    return false;
  }
  const std::string tmp = std::string(kPath) + ".tmp";
  {
    HalFile f;
    if (!Storage.openFileForWrite(kTag, tmp, f) || f.write(file, FILE_LEN) != FILE_LEN) {
      wipe(file, sizeof(file));
      Storage.remove(tmp.c_str());
      LOG_ERR(kTag, "could not write the payment password");
      return false;
    }
  }
  wipe(file, sizeof(file));
  if (!Storage.replaceFile(tmp.c_str(), kPath)) {
    Storage.remove(tmp.c_str());
    LOG_ERR(kTag, "could not commit the payment password");
    return false;
  }
  LOG_INF(kTag, "payment password remembered (device-wrapped)");
  return true;
}

bool clear() {
  const bool ok = removeFile();
  if (!ok) LOG_ERR(kTag, "could not remove the saved payment password");
  return ok;
}

}  // namespace jjwxcpay
