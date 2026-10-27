#include "PersistableStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>
#include <limits>

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  const std::string targetPath = path ? path : "";
  if (targetPath.empty()) {
    LOG_ERR("PERSIST", "Missing JSON path for write");
    return false;
  }

  Storage.mkdir("/.crosspoint");
  const std::string tempPath = targetPath + ".tmp";
  if (Storage.exists(tempPath.c_str())) {
    Storage.remove(tempPath.c_str());
  }

  size_t written = 0;
  {
    HalFile file;
    if (!Storage.openFileForWrite("PERSIST", tempPath.c_str(), file)) {
      LOG_ERR("PERSIST", "Failed to open %s", tempPath.c_str());
      return false;
    }
    written = serializeJson(doc, file);
    file.flush();
  }
  if (written == 0 || written != measureJson(doc)) {
    Storage.remove(tempPath.c_str());
    LOG_ERR("PERSIST", "Failed to serialize %s", targetPath.c_str());
    return false;
  }

  // Never remove-then-rename: a power loss between the two loses the whole file.
  // Rename the old file aside (atomic), move the temp into place, then drop the
  // backup. At every instant either the old or the new file is complete, and
  // readDocFromFile falls back to the backup if the primary is missing.
  const std::string backupPath = targetPath + ".bak";
  if (Storage.exists(targetPath.c_str())) {
    Storage.remove(backupPath.c_str());
    if (!Storage.rename(targetPath.c_str(), backupPath.c_str())) {
      Storage.remove(tempPath.c_str());
      LOG_ERR("PERSIST", "Failed to back up %s", targetPath.c_str());
      return false;
    }
  }
  if (!Storage.rename(tempPath.c_str(), targetPath.c_str())) {
    // Put the previous file back so the next boot still has settings.
    Storage.rename(backupPath.c_str(), targetPath.c_str());
    Storage.remove(tempPath.c_str());
    LOG_ERR("PERSIST", "Failed to rename temp file for %s", targetPath.c_str());
    return false;
  }
  Storage.remove(backupPath.c_str());
  return true;
}

bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
  const char* readPath = path;
  std::string backupPath;
  if (!Storage.exists(path)) {
    // Missing primary usually means the write was interrupted between the two
    // renames; the backup still holds the previous complete document.
    backupPath = std::string(path) + ".bak";
    if (!Storage.exists(backupPath.c_str())) {
      return false;  // Expected on first boot — not an error.
    }
    LOG_ERR("PERSIST", "Primary %s missing; reading backup", path);
    readPath = backupPath.c_str();
  }
  String json = Storage.readFile(readPath);
  if (json.isEmpty()) {
    LOG_ERR("PERSIST", "Failed to read %s (empty)", readPath);
    return false;
  }
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("PERSIST", "JSON parse error in %s: %s", readPath, error.c_str());
    return false;
  }
  return true;
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave) {
  bool valid = false;
  return extractPassword(doc, needsResave, std::numeric_limits<size_t>::max(), valid);
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave, const size_t maxLength,
                                                  bool& valid) {
  valid = true;
  bool ok = false;
  bool tooLong = false;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", maxLength, &ok, &tooLong);
  if (tooLong) {
    valid = false;
    return "";
  }
  if (!ok) {
    // Deobfuscation failed — fall back to legacy plaintext password.
    const char* legacyPassword = doc["password"] | "";
    const size_t legacyLength = strlen(legacyPassword);
    if (legacyLength > maxLength) {
      valid = false;
      return "";
    }
    pass.assign(legacyPassword, legacyLength);
    if (!pass.empty()) needsResave = true;
  }
  // A successfully decoded empty string is a legitimate value; preserve as-is.
  return pass;
}
