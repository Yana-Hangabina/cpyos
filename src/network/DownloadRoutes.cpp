#include "DownloadRoutes.h"

#include <algorithm>
#include <cstring>

namespace download_routes {
namespace {

bool startsWith(const std::string& text, const char* prefix) {
  return prefix != nullptr && std::strncmp(text.c_str(), prefix, std::strlen(prefix)) == 0;
}

void addUnique(std::vector<Candidate>& candidates, const char* label, std::string url, const int timeoutMs,
               const size_t maxCandidates) {
  if (url.empty()) return;
  if (maxCandidates != 0 && candidates.size() >= maxCandidates) return;
  for (const auto& existing : candidates) {
    if (existing.url == url) return;
  }
  candidates.push_back({label != nullptr ? label : "", std::move(url), timeoutMs});
}

std::string mirrorUrl(const char* mirror, const std::string& origin) {
  if (mirror == nullptr || *mirror == '\0') return {};
  std::string url(mirror);
  if (url.back() != '/') url.push_back('/');
  url += origin;
  return url;
}

}  // namespace

bool isGithubHosted(const std::string& url) {
  return startsWith(url, "https://github.com/") || startsWith(url, "https://raw.githubusercontent.com/") ||
         startsWith(url, "https://objects.githubusercontent.com/");
}

std::string stripMirrorPrefix(const std::string& url, const char* const* mirrors, const size_t mirrorCount) {
  if (mirrors == nullptr) return url;
  for (size_t i = 0; i < mirrorCount; ++i) {
    if (startsWith(url, mirrors[i])) return url.substr(std::strlen(mirrors[i]));
  }
  return url;
}

std::string mirrorPrefixForUrl(const std::string& url, const char* const* mirrors, const size_t mirrorCount) {
  if (mirrors == nullptr) return {};
  for (size_t i = 0; i < mirrorCount; ++i) {
    if (startsWith(url, mirrors[i])) return mirrors[i];
  }
  return {};
}

std::vector<Candidate> buildGithubShortlist(const std::string& advertised, const std::string& preferredMirror,
                                            const char* const* mirrors, const size_t mirrorCount,
                                            const size_t maxCandidates, const int timeoutMs,
                                            const bool includeAllMirrors) {
  std::vector<Candidate> out;
  if (advertised.empty()) return out;
  const std::string origin = stripMirrorPrefix(advertised, mirrors, mirrorCount);
  if (!isGithubHosted(origin)) {
    addUnique(out, "direct", advertised, timeoutMs, maxCandidates);
    return out;
  }

  // The manifest's route is known to work on this network; prefer it first.
  if (!preferredMirror.empty()) {
    addUnique(out, "preferred", mirrorUrl(preferredMirror.c_str(), origin), timeoutMs, maxCandidates);
  }
  addUnique(out, "github", origin, timeoutMs, maxCandidates);

  if (includeAllMirrors && mirrors != nullptr) {
    for (size_t i = 0; i < mirrorCount; ++i) {
      addUnique(out, "mirror", mirrorUrl(mirrors[i], origin), timeoutMs, maxCandidates);
    }
  } else if (mirrors != nullptr) {
    // One ordinary fallback is enough for a normal download; the full list is
    // reserved for the explicit OTA diagnostic.
    for (size_t i = 0; i < mirrorCount; ++i) {
      if (mirrors[i] != nullptr && preferredMirror == mirrors[i]) continue;
      addUnique(out, "mirror", mirrorUrl(mirrors[i], origin), timeoutMs, maxCandidates);
      break;
    }
  }

  addUnique(out, "advertised", advertised, timeoutMs, maxCandidates);
  return out;
}

std::vector<Candidate> buildManifestShortlist(const std::string& primary, const std::string& githubRaw,
                                              const char* const* mirrors, const size_t mirrorCount,
                                              const int timeoutMs, const size_t maxCandidates) {
  std::vector<Candidate> out;
  addUnique(out, "pages", primary, timeoutMs, maxCandidates);
  addUnique(out, "raw", githubRaw, timeoutMs, maxCandidates);
  if (!githubRaw.empty() && isGithubHosted(githubRaw) && mirrors != nullptr) {
    // Two mirrors are enough; a dead Pages/raw path is already two attempts.
    for (size_t i = 0; i < mirrorCount && i < 2; ++i) {
      addUnique(out, "mirror", mirrorUrl(mirrors[i], githubRaw), timeoutMs, maxCandidates);
    }
  }
  return out;
}

}  // namespace download_routes
