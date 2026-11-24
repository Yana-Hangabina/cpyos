#pragma once

#if defined(CROSSPOINT_PLUGIN_RUNTIME)

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Remote/local catalog for native plugin packages.
//
// The device first looks for a card catalog at
// /.crosspoint/plugins/catalog.txt; if it is absent it fetches the URL from
// CPYOS_PLUGIN_CATALOG_URL (or the default GitHub Pages path). This keeps
// offline installation/test possible and makes the network path optional.
namespace native_plugins {

struct CatalogEntry {
  std::string id;
  std::string name;
  std::string version;
  uint32_t api = 0;
  std::string arch;
  size_t size = 0;
  std::string sha256;
  std::string url;
  std::string description;
};

constexpr const char* kCatalogHeader = "PEXT1";
constexpr const char* kLocalCatalogPath = "/.crosspoint/plugins/catalog.txt";
constexpr const char* kCatalogCachePath = "/.crosspoint/plugins/catalog.cache";
constexpr size_t kCatalogCap = 16384;
constexpr size_t kMaxCatalogEntries = 16;

const char* catalogUrl();
void setCatalogUrlOverride(const char* url);
bool parseCatalog(const char* text, size_t length, std::vector<CatalogEntry>& out, std::string& error);
bool loadLocalCatalog(std::vector<CatalogEntry>& out, std::string& error);
bool loadCachedCatalog(std::vector<CatalogEntry>& out, std::string& error);
bool saveCatalogCache(const char* text, size_t length);

// Download one package into /.crosspoint/plugins/<id>/ and write its local
// manifest. The existing package is only replaced after size + SHA-256 pass.
using CatalogProgress = std::function<void(size_t downloaded, size_t total)>;
bool installCatalogEntry(const CatalogEntry& entry, const CatalogProgress& progress, const bool* cancelFlag,
                         std::string& error);

}  // namespace native_plugins

#endif  // CROSSPOINT_PLUGIN_RUNTIME
