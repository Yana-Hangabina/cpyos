#include "FontExtras.h"

#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "FontInstaller.h"
#include "NetworkStartup.h"
#include "SdCardFontRegistry.h"
#include "network/CrossMuxEndpoints.h"
#include "network/DownloadProbe.h"
#include "network/DownloadRoutes.h"
#include "network/HttpDownloader.h"
#include "util/FilePublish.h"

namespace font_extras {
namespace {

#ifdef CPYOS_FONT_EXTRAS_MANIFEST_URL
constexpr char kDefaultManifestUrl[] = CPYOS_FONT_EXTRAS_MANIFEST_URL;
#else
constexpr char kDefaultManifestUrl[] = "";
#endif
#ifdef CPYOS_FONT_EXTRAS_MANIFEST_RAW_URL
constexpr char kRawManifestUrl[] = CPYOS_FONT_EXTRAS_MANIFEST_RAW_URL;
#else
constexpr char kRawManifestUrl[] = "";
#endif

std::string g_manifestOverride;
// Mirror (or empty for direct GitHub) that served the last manifest or pack.
// Pack downloads try it first; a successful pack can update it for the next one.
std::string g_preferredMirror;
// Language-neutral diagnostics for the last failure: path, byte counts, the
// publish outcome label. Never a translated string and never a credential -- the
// UI appends it to the reason so a field report can name the file and the step
// instead of the single "无法写入存储卡" that 0.8.2 offered.
std::string g_lastDetail;
constexpr size_t kFileBufferBytes = 4096;
constexpr int kManifestAttemptTimeoutMs = 20000;
constexpr int kPackAttemptTimeoutMs = 30000;
// A pack this small is not worth three TLS handshakes of route measuring; the
// shortlist order is used as-is.
constexpr uint64_t kProbeMinBytes = 512 * 1024;
constexpr int kProbeTimeoutMs = 6000;
constexpr size_t kProbeSampleBytes = 32768;

// lwIP asserts if SNTP's pending DNS callback runs without the TCPIP lock when
// the browser/network code clears the DNS cache (same class of crash as the
// browser's udp_new_ip_type abort). Font network work never needs the clock.
// The depth counter keeps nesting honest: the activity holds the suspension for
// the whole Wi-Fi session while individual requests pause on top of it.
class TimeSyncPause {
 public:
  TimeSyncPause() { NetworkStartup::suspendTimeSync(); }
  ~TimeSyncPause() { NetworkStartup::resumeTimeSync(); }
  TimeSyncPause(const TimeSyncPause&) = delete;
  TimeSyncPause& operator=(const TimeSyncPause&) = delete;
};

bool requireWifi(std::string& error) {
  if (WiFi.status() == WL_CONNECTED) return true;
  error = "Wi-Fi 未连接";
  return false;
}

bool readTextFile(const std::string& path, std::string& out, const size_t cap) {
  return Storage.readFileToString("FEXT", path, cap, out);
}

bool writeTextFile(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite("FEXT", tmp, file)) {
    g_lastDetail = "cannot create " + tmp;
    return false;
  }
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  file.close();
  if (written != text.size()) {
    Storage.remove(tmp.c_str());
    g_lastDetail = "short write to " + tmp;
    return false;
  }
  // The state file is replaced, not created, on every install -- and a FAT rename
  // over an existing entry is the one this card has been seen to refuse.
  const auto outcome = file_publish::replace("FEXT", tmp, path);
  if (!file_publish::succeeded(outcome)) {
    g_lastDetail = std::string(file_publish::name(outcome)) + ": " + path;
    Storage.remove(tmp.c_str());
    return false;
  }
  return true;
}

// SdFat's mkdir() returns false when the directory already exists, so a second
// call (installLocalFile then saveInstalled) would fail. ensureDirectoryExists()
// is idempotent.
bool ensureStateDir() { return Storage.ensureDirectoryExists(kStateDir); }

bool sha256File(const char* path, uint8_t out[32]) {
  HalFile file;
  if (!Storage.openFileForRead("FEXT", path, file)) return false;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  uint8_t buffer[kFileBufferBytes];
  bool ok = true;
  for (;;) {
    const int read = file.read(buffer, sizeof(buffer));
    if (read < 0) {
      ok = false;
      break;
    }
    if (read == 0) break;
    mbedtls_sha256_update(&ctx, buffer, static_cast<size_t>(read));
  }
  if (ok) mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
  return ok;
}

bool sfntMagicValid(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("FEXT", path, file)) return false;
  uint8_t magic[4] = {};
  const int read = file.read(magic, sizeof(magic));
  file.close();
  if (read != 4) return false;
  const uint32_t tag = (static_cast<uint32_t>(magic[0]) << 24) | (static_cast<uint32_t>(magic[1]) << 16) |
                       (static_cast<uint32_t>(magic[2]) << 8) | magic[3];
  return tag == 0x00010000u || tag == 0x4F54544Fu /*OTTO*/ || tag == 0x74746366u /*ttcf*/ ||
         tag == 0x74727565u /*true*/ || tag == 0x74797031u /*typ1*/;
}

bool fileSizeMatches(const char* path, const uint64_t size) {
  HalFile file;
  if (!Storage.openFileForRead("FEXT", path, file)) return false;
  const size_t actualSize = file.size();
  file.close();
  return actualSize == size;
}

const char* const* mirrorList(size_t& count) {
  count = CrossMuxEndpoints::CPYOS_DOWNLOAD_MIRROR_COUNT;
  return CrossMuxEndpoints::CPYOS_DOWNLOAD_MIRRORS;
}

bool parseInstalledLine(const std::string& line, InstalledPack& pack) {
  // <id>\t<kind>\t<family>\t<file>\t<size>\t<sha256>
  std::vector<std::string> fields;
  size_t start = 0;
  for (size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == '\t') {
      fields.push_back(line.substr(start, i - start));
      start = i + 1;
    }
  }
  if (fields.size() != 6) return false;
  if (fields[0].empty() || fields[0].size() > kMaxIdBytes || !isValidId(fields[0].c_str())) return false;
  if (!parseKind(fields[1].c_str(), pack.kind)) return false;
  if (fields[2].size() > kMaxFamilyBytes || fields[2].empty()) return false;
  if (pack.kind == Kind::Fallback && !isValidFamily(fields[2].c_str())) return false;
  if (pack.kind == Kind::Emoji && fields[2] != "-") return false;
  if (fields[3].empty() || fields[3].size() > kMaxFileBytes || !isValidFileName(fields[3].c_str())) return false;
  char* end = nullptr;
  const unsigned long long size = std::strtoull(fields[4].c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || size < 512 || size > kMaxPackBytes) return false;
  if (!parseSha256Hex(fields[5].c_str(), pack.sha256)) return false;
  std::memcpy(pack.id, fields[0].data(), fields[0].size());
  pack.id[fields[0].size()] = '\0';
  std::memcpy(pack.family, fields[2].data(), fields[2].size());
  pack.family[fields[2].size()] = '\0';
  std::memcpy(pack.file, fields[3].data(), fields[3].size());
  pack.file[fields[3].size()] = '\0';
  pack.size = static_cast<uint64_t>(size);
  return true;
}

bool parseInstalled(const std::string& text, std::vector<InstalledPack>& out) {
  out.clear();
  bool header = false;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    if (!header) {
      if (line != "FEXTI1") return false;
      header = true;
      continue;
    }
    InstalledPack pack;
    if (!parseInstalledLine(line, pack)) return false;
    out.push_back(pack);
  }
  return header;
}

bool saveInstalled(const std::vector<InstalledPack>& packs) {
  std::string text = "FEXTI1\n";
  for (const auto& pack : packs) {
    char line[256];
    char sha[65];
    for (size_t i = 0; i < 32; ++i) std::snprintf(sha + i * 2, 3, "%02x", pack.sha256[i]);
    std::snprintf(line, sizeof(line), "%s\t%s\t%s\t%s\t%llu\t%s\n", pack.id, kindName(pack.kind), pack.family,
                  pack.file, static_cast<unsigned long long>(pack.size), sha);
    text += line;
  }
  return ensureStateDir() && writeTextFile(kStateFile, text);
}

bool recordInstalled(const Pack& pack) {
  std::vector<InstalledPack> installed;
  loadInstalled(installed);
  installed.erase(std::remove_if(installed.begin(), installed.end(),
                                 [&](const InstalledPack& entry) { return std::strcmp(entry.id, pack.id) == 0; }),
                  installed.end());
  InstalledPack entry;
  std::strncpy(entry.id, pack.id, sizeof(entry.id) - 1);
  std::strncpy(entry.family, pack.family, sizeof(entry.family) - 1);
  std::strncpy(entry.file, pack.file, sizeof(entry.file) - 1);
  entry.kind = pack.kind;
  entry.size = pack.size;
  std::memcpy(entry.sha256, pack.sha256, sizeof(entry.sha256));
  installed.push_back(entry);
  return saveInstalled(installed);
}

std::string familyDir(const std::string& family) {
  const char* root = SdCardFontRegistry::findFamilyRoot(family.c_str());
  if (root == nullptr) root = SdCardFontRegistry::defaultWriteRoot();
  return std::string(root) + "/" + family;
}

}  // namespace

const char* manifestUrl() {
  if (!g_manifestOverride.empty()) return g_manifestOverride.c_str();
  return kDefaultManifestUrl;
}

const std::string& lastDetail() { return g_lastDetail; }

void setManifestUrlOverride(const char* url) {
  g_manifestOverride = (url != nullptr) ? url : "";
  g_preferredMirror.clear();
}

namespace {
Failure classifyHttp(const HttpDownloader::DownloadError result, const bool timedOut) {
  switch (result) {
    case HttpDownloader::NO_WIFI:
      return Failure::NoWifi;
    case HttpDownloader::UNAUTHORIZED:
      return Failure::HttpStatus;
    case HttpDownloader::FILE_ERROR:
      return Failure::SdCard;
    case HttpDownloader::ABORTED:
      return Failure::Network;  // the caller reports a cancel separately
    case HttpDownloader::OK:
      return Failure::None;
    case HttpDownloader::HTTP_ERROR:
    default:
      return timedOut ? Failure::Timeout : Failure::Network;
  }
}
}  // namespace

bool fetchManifest(std::vector<Pack>& packs, bool& fromCache, std::string& error,
                   const ProgressCallback& progress, const bool* cancelFlag, Failure* failure) {
  packs.clear();
  fromCache = false;
  error.clear();
  g_lastDetail.clear();
  if (failure != nullptr) *failure = Failure::None;
  const char* url = manifestUrl();
  if (url == nullptr || *url == '\0') {
    error = "font extras manifest URL is not configured";
    if (failure != nullptr) *failure = Failure::Manifest;
    return false;
  }
  ensureStateDir();

  size_t mirrorCount = 0;
  const char* const* mirrors = mirrorList(mirrorCount);
  std::vector<download_routes::Candidate> candidates;
  if (!g_manifestOverride.empty()) {
    candidates.push_back({"override", url, kManifestAttemptTimeoutMs});
  } else {
    candidates = download_routes::buildManifestShortlist(url, kRawManifestUrl, mirrors, mirrorCount,
                                                         kManifestAttemptTimeoutMs, 4);
  }

  const bool wifiReady = WiFi.status() == WL_CONNECTED;
  std::string networkText;
  bool networkOk = false;
  Failure networkFailure = wifiReady ? Failure::Network : Failure::NoWifi;
  if (!wifiReady) {
    // Never start TLS/DNS on a dead radio: lwIP asserts udp_new_ip_type when
    // SNTP's DNS callback runs without the TCPIP lock. The cache below still
    // gives the UI a usable list.
    LOG_INF("FEXT", "Wi-Fi is not connected; using the cached manifest");
  } else {
    TimeSyncPause timeSyncPause;
    for (const auto& candidate : candidates) {
      if (cancelFlag != nullptr && *cancelFlag) {
        error = "已取消";
        return false;
      }
      networkText.clear();
      LOG_INF("FEXT", "manifest attempt [%s]: %s", candidate.label.c_str(), candidate.url.c_str());
      const uint32_t attemptStarted = millis();
      // Stream into RAM instead of opening an SD file: the manifest is tiny and
      // this is the same fetchUrl path OTA uses on this low-internal-RAM board.
      HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
      const bool ok = HttpDownloader::fetchUrl(
          candidate.url,
          [&networkText, progress, cancelFlag](const uint8_t* data, const size_t len) {
            if (cancelFlag != nullptr && *cancelFlag) return false;
            if (networkText.size() + len > kMaxManifestBytes) return false;
            networkText.append(reinterpret_cast<const char*>(data), len);
            if (progress) progress(networkText.size(), 0);
            return true;
          },
          "", "", {}, candidate.timeoutMs, cancelFlag, &result);
      const bool timedOut = millis() - attemptStarted >= static_cast<uint32_t>(candidate.timeoutMs) - 500;
      networkFailure = classifyHttp(result, timedOut);
      if (cancelFlag != nullptr && *cancelFlag) {
        error = "已取消";
        return false;
      }
      if (ok && !networkText.empty()) {
        g_preferredMirror = download_routes::mirrorPrefixForUrl(candidate.url, mirrors, mirrorCount);
        LOG_INF("FEXT", "manifest served by %s", candidate.label.c_str());
        networkOk = true;
        break;
      }
    }
  }

  std::string text;
  if (networkOk) {
    text = std::move(networkText);
    char parseError[96] = {};
    size_t count = 0;
    std::vector<Pack> parsed(kMaxPacks);
    if (!parseManifest(text.data(), text.size(), parsed.data(), kMaxPacks, count, parseError, sizeof(parseError))) {
      error = parseError;
      LOG_ERR("FEXT", "manifest rejected: %s", parseError);
      if (failure != nullptr) *failure = Failure::Manifest;
      return false;
    }
    packs.assign(parsed.begin(), parsed.begin() + static_cast<ptrdiff_t>(count));
    // Keep the last good manifest for offline listing/uninstall.
    if (writeTextFile(kManifestCache, text)) {
      LOG_INF("FEXT", "manifest cached: %zu packs", count);
    }
    return true;
  }

  if (!readTextFile(kManifestCache, text, kMaxManifestBytes + 1)) {
    error = "无法获取字体扩展清单";
    if (failure != nullptr) *failure = networkFailure;
    return false;
  }
  char parseError[96] = {};
  size_t count = 0;
  std::vector<Pack> parsed(kMaxPacks);
  if (!parseManifest(text.data(), text.size(), parsed.data(), kMaxPacks, count, parseError, sizeof(parseError))) {
    error = parseError;
    if (failure != nullptr) *failure = Failure::Manifest;
    return false;
  }
  packs.assign(parsed.begin(), parsed.begin() + static_cast<ptrdiff_t>(count));
  fromCache = true;
  // The list is usable, but the caller still learns that the network failed.
  if (failure != nullptr) *failure = networkFailure;
  LOG_INF("FEXT", "using cached manifest: %zu packs", count);
  return true;
}

bool loadCachedManifest(std::vector<Pack>& packs, std::string& error) {
  packs.clear();
  error.clear();
  std::string text;
  if (!readTextFile(kManifestCache, text, kMaxManifestBytes + 1)) {
    error = "没有已缓存的清单";
    return false;
  }
  char parseError[96] = {};
  size_t count = 0;
  std::vector<Pack> parsed(kMaxPacks);
  if (!parseManifest(text.data(), text.size(), parsed.data(), kMaxPacks, count, parseError, sizeof(parseError))) {
    error = parseError;
    return false;
  }
  packs.assign(parsed.begin(), parsed.begin() + static_cast<ptrdiff_t>(count));
  return true;
}

bool loadInstalled(std::vector<InstalledPack>& out) {
  out.clear();
  std::string text;
  if (!Storage.exists(kStateFile)) return true;
  if (!readTextFile(kStateFile, text, 8 * 1024)) {
    LOG_ERR("FEXT", "cannot read %s", kStateFile);
    return false;
  }
  if (!parseInstalled(text, out)) {
    // A power loss during saveInstalled can leave a truncated file. Treat it as
    // empty instead of blocking every install/read for the rest of the boot.
    LOG_ERR("FEXT", "installed state is corrupt; continuing with an empty set");
    out.clear();
  }
  return true;
}

std::string packPath(const Pack& pack) {
  if (pack.kind == Kind::Emoji) return kEmojiPath;
  char path[256] = {};
  FontInstaller::buildFontPath(pack.family, pack.file, path, sizeof(path));
  return path;
}

bool isInstalled(const Pack& pack) {
  std::vector<InstalledPack> installed;
  if (!loadInstalled(installed)) return false;
  for (const auto& entry : installed) {
    if (std::strcmp(entry.id, pack.id) != 0) continue;
    if (entry.size != pack.size || std::memcmp(entry.sha256, pack.sha256, sizeof(pack.sha256)) != 0) return false;
    const std::string path = packPath(pack);
    return fileSizeMatches(path.c_str(), pack.size);
  }
  return false;
}

bool installedFallbackFamilies(std::vector<std::string>& out) {
  out.clear();
  std::vector<InstalledPack> installed;
  if (!loadInstalled(installed)) return false;
  for (const auto& entry : installed) {
    if (entry.kind != Kind::Fallback) continue;
    if (out.size() >= kMaxSupplementFamilies) break;
    const Pack pack = [&] {
      Pack p;
      std::strncpy(p.id, entry.id, sizeof(p.id) - 1);
      std::strncpy(p.family, entry.family, sizeof(p.family) - 1);
      std::strncpy(p.file, entry.file, sizeof(p.file) - 1);
      p.kind = entry.kind;
      p.size = entry.size;
      std::memcpy(p.sha256, entry.sha256, sizeof(p.sha256));
      return p;
    }();
    if (fileSizeMatches(packPath(pack).c_str(), entry.size)) out.emplace_back(entry.family);
  }
  return true;
}

bool emojiInstalled() {
  std::vector<InstalledPack> installed;
  if (!loadInstalled(installed)) return false;
  for (const auto& entry : installed) {
    if (entry.kind != Kind::Emoji) continue;
    return fileSizeMatches(kEmojiPath, entry.size);
  }
  return false;
}

// Remove the scratch names a previous attempt could have left behind. Two names,
// because 0.8.2 staged a pack download twice -- this module wrote `<name>.part`
// and then handed that path to a downloader that stages at `<dest>.part` itself --
// so a stale `<name>.part.part` on the card makes every later attempt fail at the
// same step, and FAT does not always let it be deleted.
void clearScratch(const std::string& temp) {
  if (Storage.exists(temp.c_str()) && !Storage.remove(temp.c_str())) {
    LOG_ERR("FEXT", "stale scratch file could not be removed: %s", temp.c_str());
  }
  const std::string legacy = temp + ".part";
  if (!Storage.exists(legacy.c_str())) return;
  if (Storage.remove(legacy.c_str())) {
    LOG_INF("FEXT", "removed the scratch file an older build left: %s", legacy.c_str());
  } else {
    LOG_ERR("FEXT", "could not remove %s left by an older build", legacy.c_str());
  }
}

// Free bytes on the card, or SIZE_MAX when the card cannot say. A full card and a
// refused rename need different words; collapsing them is what made the 0.8.2
// report undiagnosable.
size_t freeSpace() {
  uint64_t total = 0;
  uint64_t free = 0;
  if (!Storage.getSpace(total, free)) return SIZE_MAX;
  return static_cast<size_t>(free);
}

bool downloadAndInstall(const Pack& pack, const ProgressCallback& progress, const bool* cancelFlag,
                        std::string& error, Failure* failure) {
  error.clear();
  g_lastDetail.clear();
  if (failure != nullptr) *failure = Failure::None;
  if (!requireWifi(error)) {
    if (failure != nullptr) *failure = Failure::NoWifi;
    return false;
  }
  const size_t free = freeSpace();
  // 64 KiB of slack: the state file and its .tmp sibling have to fit as well.
  if (free != SIZE_MAX && free < pack.size + 64 * 1024) {
    error = "存储卡空间不足";
    g_lastDetail = "free " + std::to_string(free / 1024) + " KB, need " +
                   std::to_string(static_cast<size_t>(pack.size / 1024)) + " KB";
    LOG_ERR("FEXT", "%s: %s", pack.id, g_lastDetail.c_str());
    if (failure != nullptr) *failure = Failure::SdFull;
    return false;
  }
  TimeSyncPause timeSyncPause;
  if (!ensureStateDir()) {
    error = "无法创建字体扩展目录";
    g_lastDetail = std::string("cannot create ") + kStateDir;
    if (failure != nullptr) *failure = Failure::SdCard;
    return false;
  }
  const std::string target = packPath(pack);
  if (pack.kind == Kind::Fallback) {
    const std::string dir = familyDir(pack.family);
    if (!Storage.ensureDirectoryExists(dir.c_str())) {
      error = "无法创建字体目录";
      g_lastDetail = "cannot create " + dir;
      if (failure != nullptr) *failure = Failure::SdCard;
      return false;
    }
  }
  const std::string temp = target + ".part";
  clearScratch(temp);

  size_t mirrorCount = 0;
  const char* const* mirrors = mirrorList(mirrorCount);
  auto candidates = download_routes::buildGithubShortlist(pack.url, g_preferredMirror, mirrors, mirrorCount, 3,
                                                          kPackAttemptTimeoutMs);
  if (candidates.empty()) candidates.push_back({"direct", pack.url, kPackAttemptTimeoutMs});
  if (pack.size >= kProbeMinBytes && candidates.size() > 1) {
    // Measure before committing. Walking the list in order means the first route
    // that answers wins no matter how slow it is, which is what made a 2.5 MB
    // pack take "a very long download" and then fail. Small packs skip this:
    // three TLS handshakes cost more than the file.
    const std::string chosen =
        download_probe::selectUrl(candidates, pack.url, kProbeSampleBytes, kProbeTimeoutMs, "FEXT");
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (candidates[i].url != chosen) continue;
      const auto it = candidates.begin() + static_cast<ptrdiff_t>(i);
      std::rotate(candidates.begin(), it, it + 1);
      break;
    }
  }

  bool downloaded = false;
  Failure networkFailure = Failure::Download;
  for (const auto& candidate : candidates) {
    LOG_INF("FEXT", "download %s [%s]: %s", pack.id, candidate.label.c_str(), candidate.url.c_str());
    const uint32_t attemptStarted = millis();
    // CallerOwned: `temp` is already this module's scratch name, and it is
    // verified before it is published. Letting the downloader stage it a second
    // time added a rename whose failure looked like a dead card.
    const auto result = HttpDownloader::downloadToFile(candidate.url, temp, progress, cancelFlag, "", "", {}, false,
                                                       candidate.timeoutMs, HttpDownloader::Staging::CallerOwned);
    if (result == HttpDownloader::OK) {
      g_preferredMirror = download_routes::mirrorPrefixForUrl(candidate.url, mirrors, mirrorCount);
      downloaded = true;
      break;
    }
    if (!Storage.remove(temp.c_str())) {
      g_lastDetail = "could not remove " + temp;
    }
    if (result == HttpDownloader::ABORTED) {
      error = "已取消";
      if (failure != nullptr) *failure = Failure::Network;
      return false;
    }
    const bool timedOut = millis() - attemptStarted >= static_cast<uint32_t>(candidate.timeoutMs) - 500;
    networkFailure = classifyHttp(result, timedOut);
    if (networkFailure == Failure::Network || networkFailure == Failure::Download) networkFailure = Failure::Download;
  }
  if (!downloaded) {
    error = "下载失败";
    if (failure != nullptr) *failure = networkFailure;
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("FEXT", temp, file)) {
    error = "无法读取下载文件";
    g_lastDetail = "cannot read back " + temp;
    Storage.remove(temp.c_str());
    if (failure != nullptr) *failure = Failure::SdCard;
    return false;
  }
  const size_t actualSize = file.size();
  file.close();
  if (actualSize != pack.size) {
    LOG_ERR("FEXT", "size mismatch for %s: got %u expected %llu", pack.id, static_cast<unsigned>(actualSize),
            static_cast<unsigned long long>(pack.size));
    error = "文件大小不符";
    g_lastDetail = "size " + std::to_string(static_cast<unsigned long long>(actualSize)) + " of " +
                   std::to_string(pack.size);
    Storage.remove(temp.c_str());
    if (failure != nullptr) *failure = Failure::Verify;
    return false;
  }
  uint8_t actualSha[32] = {};
  if (!sha256File(temp.c_str(), actualSha) ||
      std::memcmp(actualSha, pack.sha256, sizeof(actualSha)) != 0) {
    LOG_ERR("FEXT", "sha256 mismatch for %s", pack.id);
    error = "校验失败";
    g_lastDetail = "sha256 mismatch: " + temp;
    Storage.remove(temp.c_str());
    if (failure != nullptr) *failure = Failure::Verify;
    return false;
  }
  if (!sfntMagicValid(temp.c_str())) {
    LOG_ERR("FEXT", "not an sfnt font: %s", pack.file);
    error = "不是有效的字体文件";
    g_lastDetail = std::string("not sfnt: ") + temp;
    Storage.remove(temp.c_str());
    if (failure != nullptr) *failure = Failure::Verify;
    return false;
  }
  const auto outcome = file_publish::replace("FEXT", temp, target);
  if (!file_publish::succeeded(outcome)) {
    error = "无法安装字体文件";
    g_lastDetail = std::string(file_publish::name(outcome)) + ": " + target;
    Storage.remove(temp.c_str());
    if (failure != nullptr) *failure = Failure::SdCard;
    return false;
  }
  if (outcome == file_publish::Outcome::CopiedOver) {
    // Not an error, but worth a line: it says this card refuses rename-over-existing,
    // which is the fact that made the old code report a storage failure here.
    LOG_INF("FEXT", "installed %s by overwriting the previous face in place", pack.id);
  }

  if (!recordInstalled(pack)) {
    error = "无法保存安装状态";
    if (g_lastDetail.empty()) g_lastDetail = "cannot save " + std::string(kStateFile);
    if (failure != nullptr) *failure = Failure::SdCard;
    return false;
  }
  LOG_INF("FEXT", "installed %s (%s, %llu bytes)", pack.id, pack.kind == Kind::Emoji ? "emoji" : "fallback",
          static_cast<unsigned long long>(pack.size));
  return true;
}

bool removePack(const Pack& pack, std::string& error) {
  error.clear();
  const std::string target = packPath(pack);
  Storage.remove(target.c_str());
  if (pack.kind == Kind::Fallback) {
    // Dedicated family directory. Only remove it when empty: rmdir() is the
    // non-recursive form, so a user file in the same directory is never
    // destroyed by an uninstall.
    Storage.rmdir(familyDir(pack.family).c_str());
  }
  std::vector<InstalledPack> installed;
  loadInstalled(installed);
  installed.erase(std::remove_if(installed.begin(), installed.end(),
                                 [&](const InstalledPack& entry) { return std::strcmp(entry.id, pack.id) == 0; }),
                  installed.end());
  if (!saveInstalled(installed)) {
    error = "无法更新安装状态";
    return false;
  }
  return true;
}

bool installLocalFile(const char* id, const char* kindNameText, const char* family, const char* sourcePath,
                      std::string& error) {
  error.clear();
  Kind kind;
  if (!parseKind(kindNameText, kind)) {
    error = "invalid kind";
    return false;
  }
  if (!isValidId(id)) {
    error = "invalid id";
    return false;
  }
  if (sourcePath == nullptr || sourcePath[0] == '\0') {
    error = "missing source";
    return false;
  }
  const char* slash = std::strrchr(sourcePath, '/');
  const char* file = slash != nullptr ? slash + 1 : sourcePath;
  if (!isValidFileName(file)) {
    error = "invalid font filename";
    return false;
  }
  if (kind == Kind::Fallback && !isValidFamily(family)) {
    error = "invalid family";
    return false;
  }
  if (kind == Kind::Emoji && (family == nullptr || std::strcmp(family, "-") != 0)) {
    error = "emoji family must be '-'";
    return false;
  }
  HalFile source;
  if (!Storage.openFileForRead("FEXT", sourcePath, source)) {
    error = "cannot open source font";
    return false;
  }
  const size_t size = source.size();
  source.close();
  if (size < 512 || size > kMaxPackBytes) {
    error = "source size out of range";
    return false;
  }
  if (!sfntMagicValid(sourcePath)) {
    error = "source is not an sfnt";
    return false;
  }

  Pack pack;
  std::strncpy(pack.id, id, sizeof(pack.id) - 1);
  std::strncpy(pack.family, family != nullptr ? family : "-", sizeof(pack.family) - 1);
  std::strncpy(pack.file, file, sizeof(pack.file) - 1);
  std::strncpy(pack.name, id, sizeof(pack.name) - 1);
  pack.kind = kind;
  pack.size = size;
  if (!sha256File(sourcePath, pack.sha256)) {
    error = "cannot hash source";
    return false;
  }

  if (!ensureStateDir()) {
    error = "cannot create state dir";
    return false;
  }
  if (kind == Kind::Fallback && !Storage.ensureDirectoryExists(familyDir(pack.family).c_str())) {
    error = "cannot create font dir";
    return false;
  }
  const std::string target = packPath(pack);
  const std::string temp = target + ".part";
  Storage.remove(temp.c_str());
  HalFile in;
  HalFile out;
  if (!Storage.openFileForRead("FEXT", sourcePath, in) || !Storage.openFileForWrite("FEXT", temp, out)) {
    Storage.remove(temp.c_str());
    error = "cannot copy source";
    return false;
  }
  uint8_t buffer[4096];
  bool ok = true;
  for (;;) {
    const int read = in.read(buffer, sizeof(buffer));
    if (read < 0) {
      ok = false;
      break;
    }
    if (read == 0) break;
    if (out.write(buffer, static_cast<size_t>(read)) != static_cast<size_t>(read)) {
      ok = false;
      break;
    }
  }
  in.close();
  out.close();
  if (!ok || !Storage.replaceFile(temp.c_str(), target.c_str())) {
    Storage.remove(temp.c_str());
    error = "cannot install local font";
    return false;
  }
  if (!recordInstalled(pack)) {
    error = "cannot save install state";
    return false;
  }
  LOG_INF("FEXT", "local install %s (%s, %u bytes) -> %s", pack.id, kindName(pack.kind),
          static_cast<unsigned>(pack.size), target.c_str());
  return true;
}

bool removeById(const char* id, std::string& error) {
  error.clear();
  if (id == nullptr || id[0] == '\0') {
    error = "missing id";
    return false;
  }
  std::vector<InstalledPack> installed;
  if (!loadInstalled(installed)) {
    error = "installed state unreadable";
    return false;
  }
  for (const auto& entry : installed) {
    if (std::strcmp(entry.id, id) != 0) continue;
    Pack pack;
    std::strncpy(pack.id, entry.id, sizeof(pack.id) - 1);
    std::strncpy(pack.family, entry.family, sizeof(pack.family) - 1);
    std::strncpy(pack.file, entry.file, sizeof(pack.file) - 1);
    pack.kind = entry.kind;
    pack.size = entry.size;
    std::memcpy(pack.sha256, entry.sha256, sizeof(pack.sha256));
    return removePack(pack, error);
  }
  error = "unknown pack id";
  return false;
}

std::string formatSize(const uint64_t bytes) {
  char buffer[32];
  if (bytes >= 1024 * 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
  }
  return buffer;
}

bool debugCheck(std::vector<Pack>& packs, bool& fromCache, std::string& error) {
  return fetchManifest(packs, fromCache, error);
}

bool debugInstallById(const char* id, std::string& error) {
  std::vector<Pack> packs;
  bool fromCache = false;
  if (!fetchManifest(packs, fromCache, error)) return false;
  for (const auto& pack : packs) {
    if (std::strcmp(pack.id, id) == 0) {
      return downloadAndInstall(pack, nullptr, nullptr, error);
    }
  }
  error = "unknown pack id";
  return false;
}

void debugPrintStatus() {
  std::vector<InstalledPack> installed;
  if (!loadInstalled(installed)) {
    LOG_ERR("FEXT", "installed state unreadable");
    return;
  }
  LOG_INF("FEXT", "installed packs: %zu (manifest=%s)", installed.size(), manifestUrl());
  for (const auto& entry : installed) {
    const std::string path = packPath([&] {
      Pack p;
      std::strncpy(p.id, entry.id, sizeof(p.id) - 1);
      std::strncpy(p.family, entry.family, sizeof(p.family) - 1);
      std::strncpy(p.file, entry.file, sizeof(p.file) - 1);
      p.kind = entry.kind;
      return p;
    }());
    LOG_INF("FEXT", "  %s kind=%s family=%s file=%s size=%llu present=%d", entry.id, kindName(entry.kind),
            entry.family, entry.file, static_cast<unsigned long long>(entry.size),
            Storage.exists(path.c_str()) ? 1 : 0);
  }
}

}  // namespace font_extras
