#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
#include <HalStorage.h>
#include <HalSystem.h>
#include <Logging.h>
#include <Memory.h>
#include <ReleaseJsonParser.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>
// clang-format on

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "CrossMuxEndpoints.h"
#include "FirmwareBoardTag.h"
#include "FirmwareFlasher.h"

namespace {
constexpr std::string_view nightlyTagPrefix = "nightly-";
constexpr size_t nightlyShaLength = 7;

constexpr bool isSameNightlyBuild(const std::string_view currentVersion, const std::string_view latestTag) {
  if (!latestTag.starts_with(nightlyTagPrefix) || latestTag.size() != nightlyTagPrefix.size() + nightlyShaLength) {
    return false;
  }

  const std::string_view latestSha = latestTag.substr(nightlyTagPrefix.size());
  const bool validSha = std::all_of(latestSha.begin(), latestSha.end(), [](const char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  });
  if (!validSha) return false;

  const size_t separator = currentVersion.rfind('+');
  return separator != std::string_view::npos && currentVersion.substr(separator + 1) == latestSha;
}

static_assert(isSameNightlyBuild("1.5.2-rc+5064d90", "nightly-5064d90"));
static_assert(isSameNightlyBuild("1.5.2-cn-rc+5064d90", "nightly-5064d90"));
static_assert(!isSameNightlyBuild("1.5.2-rc+5064d90", "nightly-1234567"));
static_assert(!isSameNightlyBuild("1.5.2", "nightly-5064d90"));
static_assert(!isSameNightlyBuild("1.5.2-rc+5064d90", "nightly"));

// The language-selected content profile supplies the OTA host and release asset variant.
// The web proxy re-exposes it as a minimal
// GitHub-release-shaped JSON whose single asset is always named "firmware.bin"
// — that's the literal ReleaseJsonParser matches on.
//
// Going through the web instead of api.github.com directly avoids the
// unauthenticated 60 req/hr/IP rate limit and the unstable mainland-China
// path to api.github.com.
constexpr size_t releaseUrlCapacity = 192;
static_assert(releaseUrlCapacity < 256);

#ifdef CPYOS_OTA_MANIFEST_URL
struct DownloadCandidate {
  const char* label;
  std::string url;
};

bool startsWith(const std::string& value, const char* prefix) {
  const size_t length = std::strlen(prefix);
  return value.size() >= length && value.compare(0, length, prefix) == 0;
}

bool isGithubHosted(const std::string& url) {
  return startsWith(url, "https://github.com/") || startsWith(url, "https://raw.githubusercontent.com/") ||
         startsWith(url, "https://objects.githubusercontent.com/");
}

// The release manifest advertises a mirror-prefixed URL so firmware that
// predates this probe (0.1.1 and earlier) also gets a fast download. Strip the
// prefix to recover the canonical GitHub asset, then measure every route.
std::string selectFastestDownloadUrl(const std::string& advertised) {
  std::string origin = advertised;
  for (size_t i = 0; i < CrossMuxEndpoints::CPYOS_DOWNLOAD_MIRROR_COUNT; ++i) {
    const char* prefix = CrossMuxEndpoints::CPYOS_DOWNLOAD_MIRRORS[i];
    if (startsWith(origin, prefix)) {
      origin = origin.substr(std::strlen(prefix));
      break;
    }
  }

  std::vector<DownloadCandidate> candidates;
  const auto add = [&candidates](const char* label, const std::string& url) {
    for (const auto& existing : candidates) {
      if (existing.url == url) return;
    }
    candidates.push_back({label, url});
  };
  add("direct", origin);
  if (isGithubHosted(origin)) {
    // Keep these in the same order as CPYOS_DOWNLOAD_MIRRORS.
    static constexpr const char* kMirrorLabels[] = {"gh-proxy.org", "git.yylx.win", "ghfast.top",
                                                      "gh-proxy.com", "github.tbap.top", "gh.dpik.top",
                                                      "ghproxy.net"};
    static_assert(sizeof(kMirrorLabels) / sizeof(kMirrorLabels[0]) ==
                  CrossMuxEndpoints::CPYOS_DOWNLOAD_MIRROR_COUNT);
    for (size_t i = 0; i < CrossMuxEndpoints::CPYOS_DOWNLOAD_MIRROR_COUNT; ++i) {
      add(kMirrorLabels[i], std::string(CrossMuxEndpoints::CPYOS_DOWNLOAD_MIRRORS[i]) + origin);
    }
  }
  add("advertised", advertised);

  if (candidates.size() == 1) return candidates.front().url;

  size_t best = SIZE_MAX;
  uint32_t bestBps = 0;
  uint32_t bestFirstByte = UINT32_MAX;
  for (size_t i = 0; i < candidates.size(); ++i) {
    const auto probe = HttpDownloader::probeUrl(candidates[i].url, 32768, 6000);
    if (!probe.ok) {
      LOG_INF("OTA", "probe %s: failed after %u ms", candidates[i].label, static_cast<unsigned>(probe.elapsedMs));
      continue;
    }
    const uint32_t bps = probe.bytesPerSecond();
    LOG_INF("OTA", "probe %s: %u KB in %u ms (%u KB/s, first byte %u ms)", candidates[i].label,
            static_cast<unsigned>(probe.bytes / 1024), static_cast<unsigned>(probe.elapsedMs),
            static_cast<unsigned>(bps / 1024), static_cast<unsigned>(probe.firstByteMs));
    if (best == SIZE_MAX || bps > bestBps || (bps == bestBps && probe.firstByteMs < bestFirstByte)) {
      best = i;
      bestBps = bps;
      bestFirstByte = probe.firstByteMs;
    }
  }
  if (best == SIZE_MAX) {
    LOG_ERR("OTA", "all download routes failed to probe; falling back to %s", advertised.c_str());
    return advertised;
  }
  LOG_INF("OTA", "download route: %s (%u KB/s)", candidates[best].label,
          static_cast<unsigned>(bestBps / 1024));
  return candidates[best].url;
}
#endif

// ---------------------------------------------------------------------------
// Data-pack update support
// ---------------------------------------------------------------------------
// Packs are written through the partition API from a file on SD. The file is
// downloaded and hashed completely before the first erase, so a network failure
// never leaves a half-written language pack; a power loss during the write does,
// and the pack loader rejects it (English fallback) until the user retries.
constexpr uint32_t kPackSectorBytes = 4096;
constexpr uint32_t kPackEraseBlockBytes = 16 * 1024;
constexpr size_t kPackIoBytes = 4096;
constexpr size_t kPackManifestBytes = 4096;
// The spiffs partition has 12 factory bytes at +0x80000 that no code reads, but
// every tool deliberately leaves untouched. No pack may cover them.
constexpr uint32_t kReservedPackOffset = 0x80000;
constexpr uint32_t kReservedPackBytes = 12;

const esp_partition_t* dataPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
}

std::string siblingAssetUrl(const std::string& url, const char* asset) {
  const size_t slash = url.rfind('/');
  if (slash == std::string::npos || asset == nullptr || asset[0] == '\0') return {};
  return url.substr(0, slash + 1) + asset;
}

int hexNibble(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseSha256(const char* text, uint8_t out[32]) {
  if (text == nullptr || std::strlen(text) != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    const int hi = hexNibble(text[i * 2]);
    const int lo = hexNibble(text[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool validPackAssetName(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;
  for (const char* p = name; *p != '\0'; ++p) {
    const char c = *p;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
          c == '_' || c == '-')) {
      return false;
    }
  }
  return true;
}

bool hashPartition(const esp_partition_t* partition, const uint32_t offset, const uint32_t size, uint8_t out[32]) {
  if (partition == nullptr || out == nullptr || offset > partition->size || size > partition->size - offset) {
    return false;
  }
  auto buffer = makeUniqueNoThrow<uint8_t[]>(kPackIoBytes);
  if (!buffer) return false;
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  bool ok = true;
  for (uint32_t at = 0; at < size; at += kPackIoBytes) {
    const uint32_t want = std::min<uint32_t>(kPackIoBytes, size - at);
    if (esp_partition_read(partition, offset + at, buffer.get(), want) != ESP_OK) {
      ok = false;
      break;
    }
    mbedtls_sha256_update(&sha, buffer.get(), want);
  }
  if (ok) mbedtls_sha256_finish(&sha, out);
  mbedtls_sha256_free(&sha);
  return ok;
}

bool hashFile(const char* path, const uint32_t expectedSize, uint8_t out[32]) {
  HalFile file;
  if (!Storage.openFileForRead("OTA", path, file) || !file) return false;
  if (file.fileSize() != expectedSize) {
    file.close();
    return false;
  }
  auto buffer = makeUniqueNoThrow<uint8_t[]>(kPackIoBytes);
  if (!buffer) {
    file.close();
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  bool ok = true;
  uint32_t remaining = expectedSize;
  while (remaining > 0) {
    const uint32_t want = std::min<uint32_t>(kPackIoBytes, remaining);
    const int got = file.read(buffer.get(), want);
    if (got <= 0 || static_cast<uint32_t>(got) != want) {
      ok = false;
      break;
    }
    mbedtls_sha256_update(&sha, buffer.get(), want);
    remaining -= want;
  }
  if (ok) mbedtls_sha256_finish(&sha, out);
  mbedtls_sha256_free(&sha);
  file.close();
  return ok;
}

bool packRangeValid(const esp_partition_t* partition, const uint32_t offset, const uint32_t size, const uint32_t limit) {
  if (partition == nullptr || size == 0 || offset > partition->size || size > partition->size - offset) return false;
  if ((offset & (kPackSectorBytes - 1)) != 0) return false;
  if (limit > partition->size || limit < offset) return false;
  const uint32_t eraseEnd = offset + ((size + kPackSectorBytes - 1) & ~(kPackSectorBytes - 1));
  if (eraseEnd > limit) return false;
  if (offset < kReservedPackOffset + kReservedPackBytes && eraseEnd > kReservedPackOffset) return false;
  return true;
}

bool writePackFromFile(const esp_partition_t* partition, const char* path, const uint32_t offset, const uint32_t size,
                       const uint32_t limit, const std::function<void(size_t)>& onProgress) {
  if (!packRangeValid(partition, offset, size, limit)) {
    LOG_ERR("OTA", "pack range rejected: off=0x%x size=%u limit=0x%x", static_cast<unsigned>(offset),
            static_cast<unsigned>(size), static_cast<unsigned>(limit));
    return false;
  }
  HalFile file;
  if (!Storage.openFileForRead("OTA", path, file) || !file) return false;
  if (file.fileSize() != size) {
    file.close();
    return false;
  }
  auto buffer = makeUniqueNoThrow<uint8_t[]>(kPackIoBytes);
  if (!buffer) {
    file.close();
    return false;
  }

  uint32_t position = 0;
  uint32_t erasedUpTo = 0;
  while (position < size) {
    if (position >= erasedUpTo) {
      uint32_t eraseLen = std::min<uint32_t>(kPackEraseBlockBytes, size - position);
      eraseLen = (eraseLen + kPackSectorBytes - 1) & ~(kPackSectorBytes - 1);
      if (offset + position + eraseLen > limit ||
          esp_partition_erase_range(partition, offset + position, eraseLen) != ESP_OK) {
        LOG_ERR("OTA", "pack erase failed at +0x%x (len %u)", static_cast<unsigned>(position),
                static_cast<unsigned>(eraseLen));
        file.close();
        return false;
      }
      erasedUpTo = position + eraseLen;
    }
    const uint32_t want = std::min<uint32_t>(kPackIoBytes, size - position);
    const int got = file.read(buffer.get(), want);
    if (got <= 0 || static_cast<uint32_t>(got) != want) {
      file.close();
      return false;
    }
    if (esp_partition_write(partition, offset + position, buffer.get(), want) != ESP_OK) {
      LOG_ERR("OTA", "pack write failed at +0x%x", static_cast<unsigned>(position));
      file.close();
      return false;
    }
    position += want;
    if (onProgress) onProgress(position);
  }
  file.close();
  return true;
}
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate(const Channel requestedChannel) {
  channel = requestedChannel;
  packsLoaded = false;
  packCount = 0;
  packUpdates = false;
  phase = Phase::Firmware;
#ifndef CPYOS_OTA_MANIFEST_URL
  const char* channelQuery = channel == Channel::Nightly ? "&channel=nightly" : "";
  const char* const primaryHost = CrossMuxEndpoints::host();
#endif
  LOG_DBG("OTA", "Checking %s channel (current: %s)", channel == Channel::Nightly ? "nightly" : "stable",
          CROSSPOINT_VERSION);

  // Stream the ~32KB release JSON straight into the parser as it arrives.
  // Buffering the whole body in a std::string would add a growing allocation
  // on top of the TLS session's heap during the fetch; with -fno-exceptions an
  // OOM there aborts. fetchUrl handles the configured secure GET transport,
  // redirects, and User-Agent (see HttpDownloader).
  // The shared SAX token is 2 KiB; keep it off the network task stack.
  auto releaseParser = makeUniqueNoThrow<ReleaseJsonParser>(releaseNotes);
  if (!releaseParser) {
    LOG_ERR("OTA", "OOM: release parser (%zu bytes)", sizeof(ReleaseJsonParser));
    return INTERNAL_UPDATE_ERROR;
  }
  // docs/07 F3: one host is a single point of failure, so a transport failure
  // falls back to the other region's host once. Both serve the same manifest
  // shape and the same firmware asset, and the download URL is taken from
  // whichever manifest answered. Only the fetch is retried: a manifest that
  // arrived but does not parse is a server contract problem the other host
  // would answer identically.
  bool ok = false;
#ifdef CPYOS_OTA_MANIFEST_URL
  // One static manifest, served by the project's GitHub Pages site. Both the
  // stable and nightly selectors point at the same CPYOS release; isUpdateNewer
  // compares its semver tag rather than treating it as a nightly SHA.
  char releaseUrl[releaseUrlCapacity];
  const int releaseUrlLength = snprintf(releaseUrl, sizeof(releaseUrl), "%s", CrossMuxEndpoints::CPYOS_OTA_URL);
  if (releaseUrlLength < 0 || static_cast<size_t>(releaseUrlLength) >= sizeof(releaseUrl)) {
    LOG_ERR("OTA", "Release URL exceeds %zu bytes", sizeof(releaseUrl));
    return INTERNAL_UPDATE_ERROR;
  }
  ok = HttpDownloader::fetchUrl(releaseUrl, [&releaseParser](const uint8_t* data, size_t len) {
    releaseParser->feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
#else
  for (size_t attempt = 0; attempt < 2 && !ok; attempt++) {
    const char* const host = attempt == 0 ? primaryHost : CrossMuxEndpoints::alternateHost();
    if (host == nullptr || std::strcmp(host, primaryHost) == 0) break;
    char releaseUrl[releaseUrlCapacity];
    const int releaseUrlLength = snprintf(releaseUrl, sizeof(releaseUrl), CrossMuxEndpoints::OTA_MANIFEST_FORMAT, host,
                                          CrossMuxEndpoints::otaVariant(), channelQuery, HalSystem::getDeviceModel());
    if (releaseUrlLength < 0 || static_cast<size_t>(releaseUrlLength) >= sizeof(releaseUrl)) {
      LOG_ERR("OTA", "Release URL exceeds %zu bytes", sizeof(releaseUrl));
      return INTERNAL_UPDATE_ERROR;
    }
    if (attempt > 0) {
      // The first attempt may have streamed half a document before the transport
      // died; start the parser and the note slots over.
      releaseParser->reset();
      for (auto& note : releaseNotes) note.fill('\0');
      LOG_INF("OTA", "Release check via %s failed; retrying via %s", primaryHost, host);
    }
    ok = HttpDownloader::fetchUrl(releaseUrl, [&releaseParser](const uint8_t* data, size_t len) {
      releaseParser->feed(reinterpret_cast<const char*>(data), len);
      return true;
    });
  }
#endif
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser->foundTag() ? "yes" : "no",
          releaseParser->foundFirmware() ? "yes" : "no");

  if (releaseParser->foundUnsupportedChannel()) {
    LOG_INF("OTA", "Selected update channel is not supported by this device");
    return UNSUPPORTED_CHANNEL;
  }

  if (!releaseParser->foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser->foundFirmware()) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  latestVersion = releaseParser->getTagName();
  otaUrl = releaseParser->getFirmwareUrl();
  otaSize = releaseParser->getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;
  releaseNoteCount = releaseParser->getReleaseNoteCount();

  LOG_DBG("OTA", "Found update: tag=%s size=%zu notes=%zu", latestVersion.c_str(), otaSize, releaseNoteCount);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty()) {
    return false;
  }
  switch (channel) {
    case Channel::Stable:
      break;
    case Channel::Nightly:
#ifdef CPYOS_OTA_MANIFEST_URL
      // CPYOS publishes one semver release manifest for both selectors. Treating
      // its tag as a nightly SHA would make every check look like an update.
      break;
#else
      return !isSameNightlyBuild(CROSSPOINT_VERSION, latestVersion);
#endif
  }
  if (latestVersion == CROSSPOINT_VERSION) return false;

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  const auto currentVersion = CROSSPOINT_VERSION;

  // semantic version check (only match on 3 segments)
  sscanf(latestVersion.c_str(), "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(currentVersion, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

void OtaUpdater::debugProbeDownloadMirrors() {
#ifdef CPYOS_OTA_MANIFEST_URL
  if (otaUrl.empty()) {
    LOG_ERR("OTA", "probe: no firmware URL; run checkForUpdate first");
    return;
  }
  selectFastestDownloadUrl(otaUrl);
#else
  LOG_INF("OTA", "probe: this build has no CPYOS mirror list");
#endif
}

OtaUpdater::OtaUpdaterError OtaUpdater::loadPackManifest() {
  if (packsLoaded) return OK;
  packCount = 0;
  dataTotalSize = 0;
  packUpdates = false;
  if (otaUrl.empty()) {
    LOG_ERR("OTA", "pack manifest: no release URL yet");
    return HTTP_ERROR;
  }
  const std::string packsUrl = siblingAssetUrl(otaUrl, "readpico-packs.txt");
  if (packsUrl.empty()) {
    LOG_ERR("OTA", "pack manifest: cannot derive a sibling URL");
    return HTTP_ERROR;
  }

  auto body = makeUniqueNoThrow<char[]>(kPackManifestBytes);
  if (!body) return OOM_ERROR;
  size_t bodyLen = 0;
  const bool fetchOk = HttpDownloader::fetchUrl(packsUrl, [&](const uint8_t* data, size_t len) {
    if (bodyLen + len >= kPackManifestBytes) return false;
    std::memcpy(body.get() + bodyLen, data, len);
    bodyLen += len;
    return true;
  });
  if (!fetchOk) {
    LOG_ERR("OTA", "pack manifest fetch failed: %s", packsUrl.c_str());
    return HTTP_ERROR;
  }
  body[bodyLen] = '\0';

  const esp_partition_t* partition = dataPartition();
  if (partition == nullptr) {
    LOG_ERR("OTA", "no spiffs partition for data packs");
    return INTERNAL_UPDATE_ERROR;
  }

  size_t cursor = 0;
  while (cursor < bodyLen && packCount < kMaxPacks) {
    const char* line = body.get() + cursor;
    const char* newline = std::strchr(line, '\n');
    const size_t lineLen = newline ? static_cast<size_t>(newline - line) : std::strlen(line);
    cursor += lineLen + (newline ? 1 : 0);
    if (lineLen == 0 || line[0] == '#') continue;

    char asset[32] = {};
    unsigned int offset = 0;
    unsigned int size = 0;
    unsigned int limit = 0;
    char shaText[65] = {};
    if (std::sscanf(line, "%31s %x %u %64s %x", asset, &offset, &size, shaText, &limit) != 5) {
      LOG_ERR("OTA", "malformed pack manifest line: %.*s", static_cast<int>(lineLen), line);
      return INTERNAL_UPDATE_ERROR;
    }
    if (!validPackAssetName(asset)) {
      LOG_ERR("OTA", "pack manifest has an unsafe asset name");
      return INTERNAL_UPDATE_ERROR;
    }
    PackSpec& spec = packs[packCount];
    std::strncpy(spec.asset, asset, sizeof(spec.asset) - 1);
    spec.offset = offset;
    spec.size = size;
    spec.limit = limit;
    if (!parseSha256(shaText, spec.sha256)) {
      LOG_ERR("OTA", "pack %s has an invalid sha256", spec.asset);
      return INTERNAL_UPDATE_ERROR;
    }
    if (!packRangeValid(partition, spec.offset, spec.size, spec.limit)) {
      LOG_ERR("OTA", "pack %s has an invalid range", spec.asset);
      return INTERNAL_UPDATE_ERROR;
    }
    std::snprintf(spec.tempPath, sizeof(spec.tempPath), "/.crosspoint/ota_%s", spec.asset);
    uint8_t current[32] = {};
    if (!hashPartition(partition, spec.offset, spec.size, current)) {
      LOG_ERR("OTA", "cannot read current %s pack", spec.asset);
      return INTERNAL_UPDATE_ERROR;
    }
    spec.changed = std::memcmp(current, spec.sha256, sizeof(current)) != 0;
    if (spec.changed) {
      dataTotalSize += static_cast<size_t>(spec.size) * 2;  // download + write
      packUpdates = true;
    }
    LOG_INF("OTA", "pack %-16s @0x%06x %6u B %s", spec.asset, static_cast<unsigned>(spec.offset),
            static_cast<unsigned>(spec.size), spec.changed ? "update" : "current");
    ++packCount;
  }
  packsLoaded = true;
  LOG_INF("OTA", "pack manifest: %u pack(s), %s", static_cast<unsigned>(packCount),
          packUpdates ? "changes pending" : "all current");
  return OK;
}

bool OtaUpdater::checkPackUpdates() {
  const auto result = loadPackManifest();
  if (result != OK) {
    LOG_ERR("OTA", "pack check failed: %d", static_cast<int>(result));
    return false;
  }
  return packUpdates;
}

OtaUpdater::OtaUpdaterError OtaUpdater::installDataPacks(ProgressCallback onProgress, void* ctx, const bool required) {
  if (!packsLoaded) {
    const auto result = loadPackManifest();
    if (result != OK) {
      if (required) return result;
      // App update remains useful even when a release predates packs (or the
      // manifest is unreachable). The new app can retry from its update page.
      LOG_ERR("OTA", "data packs skipped: manifest unavailable");
      return OK;
    }
  }
  if (packCount == 0 || !packUpdates) return OK;

  phase = Phase::Packs;
  dataProcessedSize = 0;
  if (onProgress) onProgress(ctx);

  // Download and verify every changed pack before erasing anything. A failed
  // network transfer therefore leaves the working packs untouched.
  size_t downloadDone = 0;
  for (size_t i = 0; i < packCount; ++i) {
    PackSpec& spec = packs[i];
    if (!spec.changed) continue;
    const std::string url = siblingAssetUrl(otaUrl, spec.asset);
    if (url.empty()) {
      LOG_ERR("OTA", "pack %s: cannot derive URL", spec.asset);
      return HTTP_ERROR;
    }
    Storage.remove(spec.tempPath);
    const size_t base = downloadDone;
    const auto res = HttpDownloader::downloadToFile(
        url, spec.tempPath,
        [this, base, onProgress, ctx](const size_t got, const size_t) {
          dataProcessedSize = base + got;
          if (onProgress) onProgress(ctx);
        });
    if (res != HttpDownloader::OK) {
      LOG_ERR("OTA", "pack %s download failed (%d)", spec.asset, static_cast<int>(res));
      Storage.remove(spec.tempPath);
      if (!required) {
        // An app update is still useful without its data packs: the new app
        // accepts the old pack as a prefix and can retry from Check updates.
        LOG_ERR("OTA", "data packs skipped for this app update");
        return OK;
      }
      return HTTP_ERROR;
    }
    uint8_t actual[32] = {};
    if (!hashFile(spec.tempPath, spec.size, actual) || std::memcmp(actual, spec.sha256, sizeof(actual)) != 0) {
      LOG_ERR("OTA", "pack %s failed size/sha256 verification", spec.asset);
      Storage.remove(spec.tempPath);
      if (!required) {
        LOG_ERR("OTA", "data packs skipped for this app update");
        return OK;
      }
      return INTERNAL_UPDATE_ERROR;
    }
    downloadDone += spec.size;
    dataProcessedSize = downloadDone;
    if (onProgress) onProgress(ctx);
  }

  const esp_partition_t* partition = dataPartition();
  if (partition == nullptr) return INTERNAL_UPDATE_ERROR;
  size_t writeDone = 0;
  for (size_t i = 0; i < packCount; ++i) {
    PackSpec& spec = packs[i];
    if (!spec.changed) continue;
    const size_t base = downloadDone + writeDone;
    if (!writePackFromFile(partition, spec.tempPath, spec.offset, spec.size, spec.limit,
                           [this, base, onProgress, ctx](const size_t wrote) {
                             dataProcessedSize = base + wrote;
                             if (onProgress) onProgress(ctx);
                           })) {
      LOG_ERR("OTA", "pack %s write failed", spec.asset);
      return INTERNAL_UPDATE_ERROR;
    }
    uint8_t actual[32] = {};
    if (!hashPartition(partition, spec.offset, spec.size, actual) ||
        std::memcmp(actual, spec.sha256, sizeof(actual)) != 0) {
      LOG_ERR("OTA", "pack %s read-back verification failed", spec.asset);
      return INTERNAL_UPDATE_ERROR;
    }
    writeDone += spec.size;
    dataProcessedSize = downloadDone + writeDone;
    if (onProgress) onProgress(ctx);
  }

  for (size_t i = 0; i < packCount; ++i) {
    if (packs[i].changed) Storage.remove(packs[i].tempPath);
  }
  LOG_INF("OTA", "data packs updated: %u blob(s), %u B", static_cast<unsigned>(packCount),
          static_cast<unsigned>(writeDone));
  packUpdates = false;
  packsLoaded = false;
  phase = Phase::Firmware;
  return OK;
}

OtaUpdater::OtaUpdaterError OtaUpdater::installDataPacksOnly(ProgressCallback onProgress, void* ctx) {
  const auto result = loadPackManifest();
  if (result != OK) return result;
  if (!packUpdates) {
    phase = Phase::Firmware;
    return NO_UPDATE;
  }
  return installDataPacks(onProgress, ctx, true);
}

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

#ifdef CPYOS_OTA_MANIFEST_URL
  otaUrl = selectFastestDownloadUrl(otaUrl);
#endif

  // esp_https_ota is hardwired to esp-tls/mbedTLS, whose precompiled build on this
  // package can't negotiate TLS 1.3 (see SecureClient.h). Drive the OTA partition
  // ourselves and stream the firmware through HttpDownloader, which runs over
  // wolfSSL when FREEINK_NET_WOLFSSL is set, reusing its redirect handling for the
  // GitHub -> CDN hop.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (!updatePartition) {
    LOG_ERR("OTA", "No OTA partition available");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  esp_err_t esp_err = esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  processedSize = 0;
  int lastReportedPct = -1;
  bool flashOk = true;
  // The image streams in chunks; only the first bytes carry the header. Buffer
  // the first 14 bytes so we can read chip_id (esp_image_header_t offset 12)
  // and reject a wrong-MCU image before it overwrites the OTA partition.
  uint8_t hdr[14];
  size_t hdrLen = 0;
  bool wrongChip = false;
  board_tag::Scanner boardScanner;
  bool wrongBoard = false;
  const bool fetchOk = HttpDownloader::fetchUrl(otaUrl, [&](const uint8_t* data, size_t len) {
    if (hdrLen < sizeof(hdr)) {
      const size_t take = std::min(len, sizeof(hdr) - hdrLen);
      std::memcpy(hdr + hdrLen, data, take);
      hdrLen += take;
      if (hdrLen == sizeof(hdr)) {
        uint16_t imageChip;
        std::memcpy(&imageChip, hdr + 12, sizeof(imageChip));
        const uint16_t deviceChip = firmware_flash::runningPartitionChipId();
        if (deviceChip != 0xFFFF && imageChip != deviceChip) {
          LOG_ERR("OTA", "wrong chip: image=0x%04X device=0x%04X", imageChip, deviceChip);
          wrongChip = true;
          return false;  // abort the transfer
        }
      }
    }
    boardScanner.feed(data, len);
    if (boardScanner.mismatch()) {
      LOG_ERR("OTA", "wrong board: image=%s device=%.*s", boardScanner.foundName(),
              static_cast<int>(board_tag::boardNameLen()), board_tag::boardName());
      wrongBoard = true;
      return false;  // abort before selecting the incomplete image as bootable
    }
    if (esp_ota_write(otaHandle, data, len) != ESP_OK) {
      flashOk = false;
      return false;  // abort the transfer
    }
    processedSize += len;
    // Fire the callback only on whole-percent change. Per-chunk updates wake the
    // render task, whose framebuffer work contends with TLS on the internal arena,
    // and e-ink can't repaint faster than a percent tick anyway.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
    return true;
  });

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (wrongChip || wrongBoard) {
    LOG_ERR("OTA", "Firmware install aborted: wrong device");
    esp_ota_abort(otaHandle);
    return WRONG_DEVICE_ERROR;
  }

  if (!fetchOk || !flashOk) {
    LOG_ERR("OTA", "Firmware install failed (%s)", flashOk ? "download" : "flash write");
    esp_ota_abort(otaHandle);
    return flashOk ? HTTP_ERROR : INTERNAL_UPDATE_ERROR;
  }

  // esp_ota_end() only verifies the image against its own embedded hash. A
  // complete image of a different build (swapped server, different release)
  // would pass that. The manifest's size is the one external fact we have:
  // refuse a transfer that did not deliver exactly that many bytes.
  if (otaSize > 0 && processedSize != otaSize) {
    LOG_ERR("OTA", "Firmware size mismatch: got %zu, manifest %zu", processedSize, otaSize);
    esp_ota_abort(otaHandle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_end(otaHandle);  // verifies the written image
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  // Data packs are written while the old app is still running, before the new
  // image is selected. A pack failure therefore leaves the old app bootable;
  // the new app's tolerant loader would also fall back per key if the write was
  // interrupted. Pack manifests are optional for releases predating this path.
  const auto packResult = installDataPacks(onProgress, ctx, /*required=*/false);
  if (packResult != OK) {
    LOG_ERR("OTA", "data pack update failed (%d); new app not selected", static_cast<int>(packResult));
    return packResult;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
