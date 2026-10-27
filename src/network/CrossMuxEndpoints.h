#pragma once

#include "CrossPointSettings.h"

namespace CrossMuxEndpoints {

inline constexpr char GLOBAL_HOST[] = "crossmux.com";
inline constexpr char CHINA_HOST[] = "crossmux.cn";
inline constexpr char OTA_MANIFEST_FORMAT[] = "https://%s/api/ota/manifest?variant=%s%s&model=%s";
// CPYOS ships its own Read Pico releases. The release workflow writes this
// static GitHub-Pages manifest on every tag, so no server-side routing or
// query parsing is needed; it uses the same GitHub-release-shaped JSON that
// OtaUpdater already understands. Defined only for Read Pico environments
// (platformio.ini); other upstream targets keep the CrossMux endpoints.
#ifdef CPYOS_OTA_MANIFEST_URL
inline constexpr char CPYOS_OTA_URL[] = CPYOS_OTA_MANIFEST_URL;
#endif
inline constexpr char FONT_MANIFEST_FORMAT[] = "https://%s/api/assets/fonts/m%s-b%s/fonts.json";
inline constexpr char DICTIONARY_MANIFEST_FORMAT[] = "https://%s/api/assets/dictionaries/manifest?version=1&lang=%s";
inline constexpr char AIRPAGE_SUBDOMAIN[] = "airpage.";

constexpr const char* hostFor(const CrossPointSettings::ContentProfile profile) {
  switch (profile) {
    case CrossPointSettings::ContentProfile::Global:
      return GLOBAL_HOST;
    case CrossPointSettings::ContentProfile::China:
      return CHINA_HOST;
  }
  return GLOBAL_HOST;
}

// The other region's host, for the OTA retry in OtaUpdater (docs/07 F3). A
// single host is a single point of failure, and both serve the same manifest
// shape and the same firmware asset, so trying the other one costs one request
// and only happens after the first has failed.
constexpr const char* alternateHostFor(const CrossPointSettings::ContentProfile profile) {
  switch (profile) {
    case CrossPointSettings::ContentProfile::Global:
      return CHINA_HOST;
    case CrossPointSettings::ContentProfile::China:
      return GLOBAL_HOST;
  }
  return nullptr;
}

constexpr const char* otaVariantFor(const CrossPointSettings::ContentProfile profile) {
  switch (profile) {
    case CrossPointSettings::ContentProfile::Global:
      return "global";
    case CrossPointSettings::ContentProfile::China:
      return "cn";
  }
  return "global";
}

inline const char* host() { return hostFor(SETTINGS.contentProfile); }
inline const char* alternateHost() { return alternateHostFor(SETTINGS.contentProfile); }
inline const char* otaVariant() { return otaVariantFor(SETTINGS.contentProfile); }

static_assert(hostFor(CrossPointSettings::ContentProfile::China) == CHINA_HOST);
static_assert(alternateHostFor(CrossPointSettings::ContentProfile::China) == GLOBAL_HOST);
static_assert(alternateHostFor(CrossPointSettings::ContentProfile::Global) == CHINA_HOST);
static_assert(hostFor(CrossPointSettings::ContentProfile::Global) == GLOBAL_HOST);

}  // namespace CrossMuxEndpoints
