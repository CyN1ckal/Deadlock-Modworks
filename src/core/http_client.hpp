#pragma once

#include "cancellation.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skeleton_repair {

// Received bytes, then the total when the server sent one.
using DownloadProgress = std::function<void(uint64_t, std::optional<uint64_t>)>;

struct HttpsUrl {
    std::string Host;
    // Path and query, starting with '/'.
    std::string Target;
};

// Splits an https:// URL. Anything else, including http:// and URLs with credentials or a port, is refused.
std::optional<HttpsUrl> ParseHttpsUrl(std::string_view url);

// True for `host` itself and its subdomains, compared without case.
bool HostWithin(std::string_view host, std::string_view domain);

struct HttpOptions {
    // Every host the request may end on after redirects. Empty allows any https host.
    std::vector<std::string> AllowedDomains;
    uint64_t MaxBytes = 16ull << 20;
};

// GET over HTTPS through WinHTTP with the system proxy settings. Redirects are followed but never to plain http.
// Throws IoError for a network failure or a status other than 200, InvalidData when the response is too large or
// ends on a host outside AllowedDomains, and OperationCancelled.
std::string HttpGet(std::string_view url, const HttpOptions& options = {}, CancellationToken cancellation = {});

// Streams the response body to `destination`, replacing it. A partial file is removed on failure.
void HttpDownload(std::string_view url, const std::filesystem::path& destination, const HttpOptions& options = {},
    CancellationToken cancellation = {}, const DownloadProgress& progress = {});

}  // namespace skeleton_repair
