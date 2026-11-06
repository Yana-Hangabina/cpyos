#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Shared route planning for GitHub-hosted downloads (OTA releases, font packs).
// The mirror list is passed in so this module stays pure and host-testable;
// device callers pass CrossMuxEndpoints::CPYOS_DOWNLOAD_MIRRORS.
namespace download_routes {

struct Candidate {
  std::string label;
  std::string url;
  int timeoutMs = 0;
};

bool isGithubHosted(const std::string& url);
std::string stripMirrorPrefix(const std::string& url, const char* const* mirrors, size_t mirrorCount);
std::string mirrorPrefixForUrl(const std::string& url, const char* const* mirrors, size_t mirrorCount);

// Ordered, deduplicated shortlist for a single GitHub asset:
//   preferred route (the mirror that served the manifest, or direct GitHub)
//   -> GitHub direct
//   -> one ordinary mirror (all mirrors when includeAllMirrors)
//   -> the advertised URL
// At most `maxCandidates` entries (0 = unlimited).
std::vector<Candidate> buildGithubShortlist(const std::string& advertised, const std::string& preferredMirror,
                                            const char* const* mirrors, size_t mirrorCount, size_t maxCandidates,
                                            int timeoutMs, bool includeAllMirrors = false);

// Ordered shortlist for the FEXT1 manifest: the configured URL (Pages), the
// raw GitHub copy, then one or two pass-through mirrors of the raw copy.
std::vector<Candidate> buildManifestShortlist(const std::string& primary, const std::string& githubRaw,
                                              const char* const* mirrors, size_t mirrorCount, int timeoutMs,
                                              size_t maxCandidates = 4);

}  // namespace download_routes
