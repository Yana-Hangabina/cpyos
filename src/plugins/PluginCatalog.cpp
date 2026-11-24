#include "PluginCatalog.h"

#if defined(CROSSPOINT_PLUGIN_RUNTIME)

#include <ArduinoJson.h>

#include <cstdlib>
#include <cstring>

#include "HalStorage.h"
#include "Logging.h"
#include "PluginHash.h"
#include "PluginManager.h"
#include "cpyos_plugin_api.h"
#include "network/HttpDownloader.h"

namespace native_plugins {
namespace {
std::string g_catalogOverride;


bool safeEntryId(const std::string& id) { return isSafePluginId(id); }

bool validDigest(const std::string& digest) {
  uint8_t bytes[32];
  return hexToSha256(digest.c_str(), bytes);
}

bool parseUnsigned(const std::string& text, size_t& out) {
  if (text.empty()) return false;
  size_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<size_t>(c - '0');
    if (value > kElfCap) return false;
  }
  out = value;
  return true;
}

bool installFile(const std::string& temp, const std::string& target) {
  if (Storage.replaceFile(temp.c_str(), target.c_str())) return true;
  // Some FAT wrappers refuse replace-over-existing; remove then rename is the
  // same atomic-ish sequence the rest of the tree uses for sidecars.
  Storage.remove(target.c_str());
  return Storage.rename(temp.c_str(), target.c_str());
}

bool writeTextFile(const char* path, const char* data, size_t length) {
  HalFile file;
  if (!Storage.openFileForWrite("PLUG", path, file)) return false;
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(data), length);
  file.close();
  return written == length;
}

bool writeManifest(const std::string& dir, const CatalogEntry& entry) {
  JsonDocument doc;
  doc["id"] = entry.id;
  doc["name"] = entry.name;
  doc["version"] = entry.version;
  doc["api"] = entry.api;
  doc["arch"] = entry.arch;
  doc["entry"] = "app_main";
  doc["elf"] = kDefaultElfName;
  doc["description"] = entry.description;
  doc["sha256"] = entry.sha256;
  std::string json;
  serializeJson(doc, json);
  const std::string temp = dir + "/manifest.part";
  if (!writeTextFile(temp.c_str(), json.data(), json.size())) return false;
  return installFile(temp, dir + "/" + kManifestName);
}

}  // namespace

const char* catalogUrl() {
  if (!g_catalogOverride.empty()) return g_catalogOverride.c_str();
#if defined(CPYOS_PLUGIN_CATALOG_URL)
  return CPYOS_PLUGIN_CATALOG_URL;
#else
  return "https://yana-hangabina.github.io/cpyos/plugins/manifest-v1.txt";
#endif
}

void setCatalogUrlOverride(const char* url) {
  g_catalogOverride = url != nullptr ? url : "";
}

bool parseCatalog(const char* text, const size_t length, std::vector<CatalogEntry>& out, std::string& error) {
  out.clear();
  error.clear();
  if (text == nullptr || length == 0 || length > kCatalogCap) {
    error = "catalog is empty or too large";
    return false;
  }
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < length) {
    size_t end = start;
    while (end < length && text[end] != '\n') ++end;
    std::string line(text + start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    start = end + 1;
  }
  if (lines.empty() || lines[0] != kCatalogHeader) {
    error = "catalog header is not PEXT1";
    return false;
  }
  for (size_t i = 1; i < lines.size(); ++i) {
    if (lines[i].empty()) continue;
    std::vector<std::string> fields;
    size_t fieldStart = 0;
    while (fieldStart <= lines[i].size()) {
      const size_t tab = lines[i].find('\t', fieldStart);
      if (tab == std::string::npos) {
        fields.push_back(lines[i].substr(fieldStart));
        break;
      }
      fields.push_back(lines[i].substr(fieldStart, tab - fieldStart));
      fieldStart = tab + 1;
    }
    if (fields.size() != 9) {
      error = "catalog row does not have 9 fields";
      return false;
    }
    CatalogEntry entry;
    entry.id = fields[0];
    entry.name = fields[1];
    entry.version = fields[2];
    entry.arch = fields[4];
    entry.sha256 = fields[6];
    entry.url = fields[7];
    entry.description = fields[8];
    if (!safeEntryId(entry.id) || entry.name.empty() || entry.version.empty() ||
        fields[3].empty() || fields[3].find_first_not_of("0123456789") != std::string::npos ||
        (entry.arch != "riscv32" && entry.arch != "xtensa") || !parseUnsigned(fields[5], entry.size) ||
        entry.size == 0 || !validDigest(entry.sha256) || entry.url.empty() ||
        (entry.url.compare(0, 8, "https://") != 0 && entry.url.compare(0, 7, "http://") != 0 &&
         entry.url.compare(0, 7, "file://") != 0 && entry.url[0] != '/')) {
      error = "catalog row is invalid";
      return false;
    }
    entry.api = static_cast<uint32_t>(std::strtoul(fields[3].c_str(), nullptr, 10));
    if (entry.api != CPYOS_PLUGIN_API_VERSION) {
      error = "catalog api version is not supported";
      return false;
    }
    if (out.size() >= kMaxCatalogEntries) {
      error = "catalog has too many entries";
      return false;
    }
    out.push_back(std::move(entry));
  }
  return true;
}

bool loadLocalCatalog(std::vector<CatalogEntry>& out, std::string& error) {
  out.clear();
  error.clear();
  std::string text;
  if (!Storage.exists(kLocalCatalogPath)) return false;
  if (!Storage.readFileToString("PLUG", kLocalCatalogPath, kCatalogCap + 1, text)) return false;
  return parseCatalog(text.data(), text.size(), out, error);
}

bool loadCachedCatalog(std::vector<CatalogEntry>& out, std::string& error) {
  out.clear();
  error.clear();
  std::string text;
  if (!Storage.exists(kCatalogCachePath)) return false;
  if (!Storage.readFileToString("PLUG", kCatalogCachePath, kCatalogCap + 1, text)) return false;
  return parseCatalog(text.data(), text.size(), out, error);
}

bool saveCatalogCache(const char* text, const size_t length) {
  const std::string temp = std::string(kCatalogCachePath) + ".part";
  if (!writeTextFile(temp.c_str(), text, length)) return false;
  return Storage.replaceFile(temp.c_str(), kCatalogCachePath);
}

bool installCatalogEntry(const CatalogEntry& entry, const CatalogProgress& progress, const bool* cancelFlag,
                         std::string& error) {
  error.clear();
  if (!isSafePluginId(entry.id) || entry.url.empty() || entry.sha256.size() != 64) {
    error = "invalid catalog entry";
    return false;
  }
  const std::string dir = std::string(kPluginRoot) + "/" + entry.id;
  if (!Storage.ensureDirectoryExists(kPluginRoot) || !Storage.ensureDirectoryExists(dir.c_str())) {
    error = "cannot create plugin directory";
    return false;
  }
  const std::string temp = dir + "/plugin.part";
  Storage.remove(temp.c_str());
  const std::string localSource = entry.url.rfind("file://", 0) == 0 ? entry.url.substr(7) : entry.url;
  if (!localSource.empty() && localSource[0] == '/') {
    HalFile source = Storage.open(localSource.c_str(), O_RDONLY);
    HalFile target;
    if (!source.isOpen() || !Storage.openFileForWrite("PLUG", temp.c_str(), target)) {
      error = "cannot open local package";
      return false;
    }
    uint8_t buffer[512];
    size_t copied = 0;
    bool ok = true;
    for (;;) {
      if (cancelFlag != nullptr && *cancelFlag) {
        ok = false;
        error = "download cancelled";
        break;
      }
      const int read = source.read(buffer, sizeof(buffer));
      if (read < 0) {
        ok = false;
        break;
      }
      if (read == 0) break;
      if (target.write(buffer, static_cast<size_t>(read)) != static_cast<size_t>(read)) {
        ok = false;
        break;
      }
      copied += static_cast<size_t>(read);
      if (progress) progress(copied, entry.size);
    }
    source.close();
    target.close();
    if (!ok) {
      Storage.remove(temp.c_str());
      error = "cannot copy local package";
      return false;
    }
  } else {
    const auto result = HttpDownloader::downloadToFile(entry.url, temp, progress, cancelFlag, "", "", {}, false, 20000);
    if (result != HttpDownloader::OK) {
      Storage.remove(temp.c_str());
      error = result == HttpDownloader::ABORTED ? "download cancelled" : "download failed";
      return false;
    }
  }
  size_t actualSize = 0;
  {
    HalFile file = Storage.open(temp.c_str(), O_RDONLY);
    if (!file.isOpen()) {
      Storage.remove(temp.c_str());
      error = "cannot read downloaded package";
      return false;
    }
    actualSize = file.fileSize();
    file.close();
  }
  if (actualSize != entry.size) {
    Storage.remove(temp.c_str());
    error = "downloaded size mismatch";
    return false;
  }
  if (sha256FileHex(temp.c_str()) != entry.sha256) {
    Storage.remove(temp.c_str());
    error = "downloaded sha256 mismatch";
    return false;
  }
  size_t validatedSize = 0;
  if (!validateElfFile(temp.c_str(), validatedSize, error)) {
    Storage.remove(temp.c_str());
    return false;
  }
  if (!installFile(temp, dir + "/" + kDefaultElfName)) {
    Storage.remove(temp.c_str());
    error = "cannot install plugin.elf";
    return false;
  }
  if (!writeManifest(dir, entry)) {
    error = "cannot write manifest.json";
    return false;
  }
  PluginInfo info;
  if (!loadPlugin(entry.id, info)) {
    error = info.error.empty() ? "installed package failed validation" : info.error;
    Storage.removeDir(dir.c_str());
    return false;
  }
  LOG_INF("PLUG", "installed %s %s (%u bytes)", entry.id.c_str(), entry.version.c_str(),
          static_cast<unsigned>(actualSize));
  return true;
}

}  // namespace native_plugins

#endif  // CROSSPOINT_PLUGIN_RUNTIME
