#pragma once

#if defined(CROSSPOINT_PLUGIN_RUNTIME)

#include <cstddef>
#include <string>
#include <vector>

// Installed native plugins live under a fixed SD directory:
//   /.crosspoint/plugins/<id>/manifest.json
//   /.crosspoint/plugins/<id>/plugin.elf
// The manager only reads and validates; installation stays an out-of-band copy
// (file transfer, USB drive, or the serial putfile bench command).
namespace native_plugins {

struct PluginInfo {
  std::string dirName;
  std::string id;
  std::string name;
  std::string version;
  std::string description;
  std::string author;
  std::string elfName;
  std::string elfPath;
  size_t elfSize = 0;
  bool crashed = false;  // a previous run left its .running marker
  std::string error;     // non-empty when the package is not loadable
};

constexpr const char* kPluginRoot = "/.crosspoint/plugins";
constexpr const char* kManifestName = "manifest.json";
constexpr const char* kDefaultElfName = "plugin.elf";
constexpr const char* kCrashMarkerName = ".running";
constexpr size_t kManifestCap = 4096;
constexpr size_t kElfCap = 2u * 1024u * 1024u;

// Structural ELF validation shared by the scanner and the catalog installer.
bool validateElfFile(const std::string& path, size_t& sizeOut, std::string& error);

// Expected architecture string in manifest.json for this firmware.
const char* expectedArch();

// Scan every subdirectory of kPluginRoot. Invalid packages are returned too,
// with `error` set, so the UI can explain why a folder was rejected.
std::vector<PluginInfo> scan();

// Validate one directory name (without path). Kept public for host tests.
bool isSafePluginId(const std::string& id);

// Read + validate a package. `dirName` is a directory under kPluginRoot.
bool loadPlugin(const std::string& dirName, PluginInfo& out);

}  // namespace native_plugins

#endif  // CROSSPOINT_PLUGIN_RUNTIME
