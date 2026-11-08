#pragma once

// Firmware identity advertised to SD plugins (docs/sd-plugins.md).
//
// CPYOS is a CrossPoint-family fork. Plugin manifests restrict themselves with
// `compatibility.firmware_family` / `plugin_api_min` / `requires`, and those
// declarations name the CrossPoint plugin family, so FAMILY stays pinned to
// that vocabulary rather than to the CPYOS product name. The firmware *version*
// a plugin sees is CPYOS's own CROSSPOINT_VERSION build macro.
//
// These helpers are header-only and dependency-free so the host tests in
// test/plugin_catalog_stack can exercise them without Arduino/ArduinoJson.

#include <cstddef>
#include <string_view>

namespace firmwareidentity {

inline constexpr const char* FAMILY = "crosspoint";

// Bump only for a plugin-visible behavior change that manifests gate on.
inline constexpr int PLUGIN_API_VERSION = 1;

// Everything this firmware actually implements. Never advertise a capability
// before the behavior ships: catalogs use this list to hide entries the device
// cannot render.
inline constexpr std::string_view CAPABILITIES =
    "catalog.lists,"
    "catalog.bundle_install,"
    "catalog.version_tracking,"
    "catalog.compatibility,"
    "catalog.encoded_item_fields,"
    "catalog.list_index_mapping,"
    "catalog.list_requirements";

// A list's `requires` array holds a handful of names; anything longer is
// treated as malformed instead of being walked.
inline constexpr std::size_t MAX_LIST_REQUIREMENTS = 32;

constexpr std::string_view trimToken(std::string_view token) {
  while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
  while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.remove_suffix(1);
  return token;
}

// Exact, comma-separated token membership. Empty tokens never match.
constexpr bool csvHasToken(std::string_view csv, std::string_view wanted) {
  if (wanted.empty()) return false;
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = csv.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? csv.size() : comma;
    if (trimToken(csv.substr(start, end - start)) == wanted) return true;
    if (comma == std::string_view::npos) return false;
    start = comma + 1;
  }
}

// All-of: every comma-separated capability must be advertised. Empty passes.
constexpr bool matchesCapabilities(std::string_view required) {
  if (required.empty()) return true;
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = required.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? required.size() : comma;
    const std::string_view token = trimToken(required.substr(start, end - start));
    if (token.empty() || !csvHasToken(CAPABILITIES, token)) return false;
    if (comma == std::string_view::npos) return true;
    start = comma + 1;
  }
}

// CSV family restriction; empty matches every family.
constexpr bool matchesFamily(std::string_view restriction) {
  return restriction.empty() || csvHasToken(restriction, FAMILY);
}

// An absent minimum passes; a malformed or newer-than-us minimum fails.
constexpr bool matchesPluginApiMinimum(std::string_view minimum) {
  if (minimum.empty()) return true;
  int value = 0;
  for (const char c : minimum) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + (c - '0');
    if (value > 1000000) return false;  // saturate before int overflow
  }
  return value <= PLUGIN_API_VERSION;
}

// Catalog/list compatibility: family, API floor, then advertised capabilities.
constexpr bool filtersCompatibility(std::string_view family, std::string_view apiMinimum,
                                    std::string_view requiredCapabilities) {
  return matchesFamily(family) && matchesPluginApiMinimum(apiMinimum) &&
         matchesCapabilities(requiredCapabilities);
}

// A browse list's optional `requires` array. Malformed (empty name, too many
// names, unknown capability) hides the list rather than exposing it.
constexpr bool listRequirementsMet(const std::string_view* requirements, std::size_t count) {
  if (count > MAX_LIST_REQUIREMENTS) return false;
  for (std::size_t i = 0; i < count; ++i) {
    if (!csvHasToken(CAPABILITIES, requirements[i])) return false;
  }
  return true;
}

}  // namespace firmwareidentity
