#include "PluginManager.h"

#if defined(CROSSPOINT_PLUGIN_RUNTIME)

#include <ArduinoJson.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "HalStorage.h"
#include "PluginHash.h"
#include "cpyos_plugin_api.h"

namespace native_plugins {
namespace {

constexpr uint8_t kElfMagic[4] = {0x7F, 'E', 'L', 'F'};
constexpr uint16_t kMachineRiscV = 243;
constexpr uint16_t kMachineXtensa = 94;

bool isSimpleFileName(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return false;
  if (name == "." || name == "..") return false;
  return true;
}

}  // namespace

bool validateElfFile(const std::string& path, size_t& sizeOut, std::string& error) {
  HalFile file = Storage.open(path.c_str(), O_RDONLY);
  if (!file.isOpen()) {
    error = "cannot open plugin.elf";
    return false;
  }
  const size_t size = file.fileSize();
  if (size < 52 || size > kElfCap) {
    error = size == 0 ? "plugin.elf is empty" : "plugin.elf is too large";
    return false;
  }
  uint8_t header[20] = {};
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    error = "short ELF header";
    return false;
  }
  if (std::memcmp(header, kElfMagic, sizeof(kElfMagic)) != 0) {
    error = "not an ELF file";
    return false;
  }
  if (header[4] != 1) {  // ELFCLASS32
    error = "ELF is not 32-bit";
    return false;
  }
  if (header[5] != 1) {  // ELFDATA2LSB
    error = "ELF is not little-endian";
    return false;
  }
  const uint16_t type = static_cast<uint16_t>(header[16] | (header[17] << 8));
  if (type != 2 && type != 3) {  // ET_EXEC or ET_DYN
    error = "ELF is not executable";
    return false;
  }
  const uint16_t machine = static_cast<uint16_t>(header[18] | (header[19] << 8));
#if defined(__riscv)
  constexpr uint16_t expected = kMachineRiscV;
#elif defined(__XTENSA__)
  constexpr uint16_t expected = kMachineXtensa;
#else
#error "native plugin runtime has no architecture mapping"
#endif
  if (machine != expected) {
    error = "plugin.elf was built for another architecture";
    return false;
  }
  sizeOut = size;
  return true;
}

const char* expectedArch() {
#if defined(__riscv)
  return "riscv32";
#elif defined(__XTENSA__)
  return "xtensa";
#else
  return "unknown";
#endif
}

bool isSafePluginId(const std::string& id) {
  if (id.empty() || id.size() > 32) return false;
  if (id[0] == '.') return false;
  for (const char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '_' || c == '.';
    if (!ok) return false;
  }
  return true;
}

bool loadPlugin(const std::string& dirName, PluginInfo& out) {
  out = PluginInfo{};
  out.dirName = dirName;
  out.id = dirName;

  if (!isSafePluginId(dirName)) {
    out.error = "unsafe plugin directory name";
    return false;
  }

  const std::string dir = std::string(kPluginRoot) + "/" + dirName;
  const std::string manifestPath = dir + "/" + kManifestName;
  if (!Storage.exists(manifestPath.c_str())) {
    out.error = "manifest.json is missing";
    return false;
  }

  std::string json;
  if (!Storage.readFileToString("PLUG", manifestPath, kManifestCap, json)) {
    out.error = "cannot read manifest.json";
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json);
  if (err) {
    out.error = "manifest.json is not valid JSON";
    return false;
  }

  const char* id = doc["id"] | "";
  const char* name = doc["name"] | "";
  const char* version = doc["version"] | "";
  const char* arch = doc["arch"] | "";
  const char* entry = doc["entry"] | "app_main";
  const char* elf = doc["elf"] | kDefaultElfName;
  const int api = doc["api"] | 0;

  if (*id != '\0' && dirName != id) {
    out.error = "manifest id does not match its directory";
    return false;
  }
  if (api != static_cast<int>(CPYOS_PLUGIN_API_VERSION)) {
    out.error = "manifest api version is not supported";
    return false;
  }
  if (std::strcmp(arch, expectedArch()) != 0) {
    out.error = std::string("manifest arch must be ") + expectedArch();
    return false;
  }
  if (std::strcmp(entry, "app_main") != 0) {
    out.error = "only the app_main entry is supported";
    return false;
  }
  if (!isSimpleFileName(elf) || std::strlen(elf) < 4 || std::strcmp(elf + std::strlen(elf) - 4, ".elf") != 0) {
    out.error = "manifest elf must name a .elf file";
    return false;
  }

  const char* description = doc["description"] | "";
  const char* author = doc["author"] | "";

  out.name = *name != '\0' ? name : dirName;
  out.version = *version != '\0' ? version : "-";
  out.description = description;
  out.author = author;
  out.elfName = elf;
  out.elfPath = dir + "/" + out.elfName;
  out.crashed = Storage.exists((dir + "/" + kCrashMarkerName).c_str());

  size_t elfSize = 0;
  if (!validateElfFile(out.elfPath, elfSize, out.error)) {
    return false;
  }
  const char* expectedSha = doc["sha256"] | "";
  if (*expectedSha != '\0') {
    const std::string actual = sha256FileHex(out.elfPath.c_str());
    if (actual.empty() || actual != expectedSha) {
      out.error = "sha256 mismatch";
      return false;
    }
  }
  out.elfSize = elfSize;
  out.error.clear();
  return true;
}

std::vector<PluginInfo> scan() {
  std::vector<std::string> dirs;
  HalFile root = Storage.open(kPluginRoot, O_RDONLY);
  if (root.isOpen() && root.isDirectory()) {
    root.rewindDirectory();
    for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
      if (!entry.isDirectory()) {
        entry.close();
        continue;
      }
      char name[64] = {};
      entry.getName(name, sizeof(name));
      entry.close();
      if (name[0] != '\0' && name[0] != '.' && isSafePluginId(name) && dirs.size() < 64) {
        dirs.emplace_back(name);
      }
    }
    root.close();
  } else {
    LOG_INF("PLUG", "plugin root missing: %s", kPluginRoot);
  }

  std::vector<PluginInfo> plugins;
  plugins.reserve(dirs.size());
  for (const auto& dirName : dirs) {
    const std::string manifestPath = std::string(kPluginRoot) + "/" + dirName + "/" + kManifestName;
    if (!Storage.exists(manifestPath.c_str())) continue;
    PluginInfo info;
    loadPlugin(dirName, info);
    if (!info.error.empty()) {
      LOG_INF("PLUG", "package %s rejected: %s", dirName.c_str(), info.error.c_str());
    }
    plugins.push_back(std::move(info));
  }
  std::sort(plugins.begin(), plugins.end(), [](const PluginInfo& a, const PluginInfo& b) {
    const std::string& an = a.name.empty() ? a.dirName : a.name;
    const std::string& bn = b.name.empty() ? b.dirName : b.name;
    return an < bn;
  });
  return plugins;
}

}  // namespace native_plugins

#endif  // CROSSPOINT_PLUGIN_RUNTIME
