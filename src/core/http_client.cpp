#include "http_client.hpp"

#include "error.hpp"
#include "strings.hpp"
#include "product_version.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <utility>
#include <vector>

namespace skeleton_repair {
namespace {
namespace fs = std::filesystem;

constexpr int kMaxRedirects = 8;

struct Handle {
    HINTERNET Value = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET value) : Value(value) {}
    ~Handle() { if (Value) WinHttpCloseHandle(Value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : Value(std::exchange(other.Value, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { if (Value) WinHttpCloseHandle(Value); Value = std::exchange(other.Value, nullptr); }
        return *this;
    }
};

[[noreturn]] void Fail(const std::string& what) {
    const DWORD code = GetLastError();
    std::string reason;
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT: reason = "The connection timed out."; break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: reason = "The server name could not be resolved. Check your internet connection."; break;
    case ERROR_WINHTTP_CANNOT_CONNECT: reason = "Could not connect to the server."; break;
    case ERROR_WINHTTP_CONNECTION_ERROR: reason = "The connection was reset."; break;
    case ERROR_WINHTTP_SECURE_FAILURE: reason = "The secure connection could not be verified."; break;
    default: reason = "Windows error " + std::to_string(code) + ".";
    }
    throw IoError(what + " " + reason);
}

std::wstring Header(HINTERNET request, DWORD query) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return {};
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
    value.resize(size / sizeof(wchar_t));
    return value;
}

bool Allowed(const HttpOptions& options, std::string_view host) {
    return options.AllowedDomains.empty()
        || std::any_of(options.AllowedDomains.begin(), options.AllowedDomains.end(), [&](const auto& domain) { return HostWithin(host, domain); });
}

// Sends GET requests, following redirects by hand so every hop is checked, and returns the final request
// with its response headers received.
struct Response {
    Handle Session, Connection, Request;
    std::optional<uint64_t> Length;
};

Response Open(std::string_view url, const HttpOptions& options, CancellationToken& cancellation) {
    Response response;
    const auto agent = Widen(std::string(SR_PRODUCT_EXECUTABLE) + "/" + SR_PRODUCT_VERSION);
    response.Session = Handle(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!response.Session.Value) Fail("Could not start a network session.");
    WinHttpSetTimeouts(response.Session.Value, 15000, 15000, 30000, 60000);
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(response.Session.Value, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));

    std::string current(url);
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        cancellation.ThrowIfCancelled();
        const auto parsed = ParseHttpsUrl(current);
        if (!parsed) throw InvalidData("Only secure https links can be downloaded.");
        if (!Allowed(options, parsed->Host)) throw InvalidData("The link leads to " + parsed->Host + ", which Modworks does not download from.");
        const auto host = Widen(parsed->Host), target = Widen(parsed->Target);
        response.Connection = Handle(WinHttpConnect(response.Session.Value, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        if (!response.Connection.Value) Fail("Could not reach " + parsed->Host + ".");
        response.Request = Handle(WinHttpOpenRequest(response.Connection.Value, L"GET", target.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (!response.Request.Value) Fail("Could not request " + parsed->Host + ".");
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(response.Request.Value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        if (!WinHttpSendRequest(response.Request.Value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
            Fail("Could not contact " + parsed->Host + ".");
        if (!WinHttpReceiveResponse(response.Request.Value, nullptr)) Fail("No response from " + parsed->Host + ".");

        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(response.Request.Value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            auto location = Narrow(Header(response.Request.Value, WINHTTP_QUERY_LOCATION));
            if (location.empty()) throw IoError(parsed->Host + " redirected without a destination.");
            if (location.starts_with("/") && !location.starts_with("//")) location = "https://" + parsed->Host + location;
            current = location;
            continue;
        }
        if (status != 200) throw IoError(parsed->Host + " answered with HTTP status " + std::to_string(status) + ".");
        const auto length = Narrow(Header(response.Request.Value, WINHTTP_QUERY_CONTENT_LENGTH));
        if (!length.empty() && std::all_of(length.begin(), length.end(), [](unsigned char c) { return std::isdigit(c); }))
            response.Length = std::stoull(length);
        if (response.Length && *response.Length > options.MaxBytes) throw InvalidData("The download is larger than Modworks accepts.");
        return response;
    }
    throw IoError("The link redirected too many times.");
}

// Reads the body in pieces until the server ends it.
template <typename Sink>
void ReadBody(Response& response, const HttpOptions& options, CancellationToken& cancellation, const DownloadProgress& progress, Sink&& sink) {
    std::vector<char> buffer(1 << 16);
    uint64_t received = 0;
    for (;;) {
        cancellation.ThrowIfCancelled();
        DWORD read = 0;
        if (!WinHttpReadData(response.Request.Value, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) Fail("The download was interrupted.");
        if (read == 0) break;
        received += read;
        if (received > options.MaxBytes) throw InvalidData("The download is larger than Modworks accepts.");
        sink(buffer.data(), read);
        if (progress) progress(received, response.Length);
    }
    if (response.Length && received != *response.Length) throw IoError("The download ended early. Try again.");
}
}  // namespace

std::optional<HttpsUrl> ParseHttpsUrl(std::string_view url) {
    constexpr std::string_view scheme = "https://";
    if (url.size() <= scheme.size() || !EqualsIgnoreCase(url.substr(0, scheme.size()), scheme)) return std::nullopt;
    const auto rest = url.substr(scheme.size());
    const auto end = rest.find_first_of("/?#");
    const auto host = rest.substr(0, end);
    if (host.empty() || host.find_first_of("@:\\ ") != std::string_view::npos) return std::nullopt;
    if (!std::all_of(host.begin(), host.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '-'; })) return std::nullopt;
    std::string target = end == std::string_view::npos ? "/" : std::string(rest.substr(end));
    if (const auto fragment = target.find('#'); fragment != std::string::npos) target.erase(fragment);
    if (target.empty() || target[0] == '?') target.insert(target.begin(), '/');
    if (target.find_first_of(" \r\n") != std::string::npos) return std::nullopt;
    std::string lower(host);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return HttpsUrl{lower, target};
}

bool HostWithin(std::string_view host, std::string_view domain) {
    if (host.size() == domain.size()) return EqualsIgnoreCase(host, domain);
    return host.size() > domain.size() && host[host.size() - domain.size() - 1] == '.'
        && EqualsIgnoreCase(host.substr(host.size() - domain.size()), domain);
}

std::string HttpGet(std::string_view url, const HttpOptions& options, CancellationToken cancellation) {
    auto response = Open(url, options, cancellation);
    std::string body;
    if (response.Length) body.reserve(static_cast<size_t>(*response.Length));
    ReadBody(response, options, cancellation, {}, [&](const char* data, size_t size) { body.append(data, size); });
    return body;
}

void HttpDownload(std::string_view url, const fs::path& destination, const HttpOptions& options, CancellationToken cancellation,
    const DownloadProgress& progress) {
    try {
        auto response = Open(url, options, cancellation);
        std::ofstream out(destination, std::ios::binary | std::ios::trunc);
        if (!out) throw IoError("Could not write " + Narrow(destination.wstring()) + ".");
        ReadBody(response, options, cancellation, progress, [&](const char* data, size_t size) {
            out.write(data, static_cast<std::streamsize>(size));
            if (!out) throw IoError("Could not write " + Narrow(destination.wstring()) + ". The disk may be full.");
        });
    } catch (...) {
        std::error_code error;
        fs::remove(destination, error);
        throw;
    }
}

}  // namespace skeleton_repair
