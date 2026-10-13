#include "http_client.hpp"

#include "error.hpp"
#include "strings.hpp"
#include "product_version.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
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
    std::string ContentEncoding;
};

Response Open(std::string_view url, const HttpOptions& options, CancellationToken& cancellation) {
    Response response;
    const auto agent = Widen(std::string(SR_PRODUCT_EXECUTABLE) + "/" + SR_PRODUCT_VERSION);
    response.Session = Handle(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!response.Session.Value) Fail("Could not start a network session.");
    WinHttpSetTimeouts(response.Session.Value, 15000, 15000, 30000, 60000);
    // Decompression stays off. WinHTTP would return the decoded body while Content-Length stayed compressed,
    // and a complete gzip response would look truncated. The body is inflated in ReadBody instead.
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
        WinHttpAddRequestHeaders(response.Request.Value, L"Accept-Encoding: gzip, deflate\r\n", static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
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
        response.ContentEncoding = Narrow(Header(response.Request.Value, WINHTTP_QUERY_CONTENT_ENCODING));
        return response;
    }
    throw IoError("The link redirected too many times.");
}

void RequireFullBody(std::optional<uint64_t> encoded_length, uint64_t encoded_received) {
    if (encoded_length && encoded_received != *encoded_length) throw IoError("The download ended early. Try again.");
}

std::string EncodingName(std::string_view header) {
    const auto end = header.find_first_of(";,");
    auto token = header.substr(0, end);
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) token.remove_prefix(1);
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) token.remove_suffix(1);
    std::string lower(token);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

// Inflates a gzip or zlib-wrapped deflate body. Identity responses do not construct one.
struct Inflate {
    z_stream Stream{};
    bool On = false;
    explicit Inflate(bool gzip) {
        if (inflateInit2(&Stream, gzip ? 16 + MAX_WBITS : MAX_WBITS) != Z_OK) throw IoError("Could not decompress the download.");
        On = true;
    }
    ~Inflate() { if (On) inflateEnd(&Stream); }
    Inflate(const Inflate&) = delete;
    Inflate& operator=(const Inflate&) = delete;
};

struct BodyDecoder {
    std::unique_ptr<Inflate> Compressed;
    explicit BodyDecoder(std::string_view content_encoding) {
        const auto name = EncodingName(content_encoding);
        if (name.empty() || name == "identity") return;
        if (name == "gzip" || name == "x-gzip") Compressed = std::make_unique<Inflate>(true);
        else if (name == "deflate") Compressed = std::make_unique<Inflate>(false);
        else throw InvalidData("The download uses " + name + " compression, which Modworks cannot read.");
    }
    bool Identity() const { return !Compressed; }

    template <typename Sink>
    void Emit(const char* data, size_t size, uint64_t max_decoded, uint64_t& decoded, Sink&& sink) {
        decoded += size;
        if (decoded > max_decoded) throw InvalidData("The download is larger than Modworks accepts.");
        if (size) sink(data, size);
    }

    template <typename Sink>
    void Consume(const char* data, size_t size, uint64_t max_decoded, uint64_t& decoded, Sink&& sink) {
        if (!Compressed) { Emit(data, size, max_decoded, decoded, sink); return; }
        size_t offset = 0;
        std::vector<char> out(1 << 16);
        while (offset < size) {
            const uInt chunk = static_cast<uInt>(std::min(size - offset, static_cast<size_t>(1u << 20)));
            auto& stream = Compressed->Stream;
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data + offset));
            stream.avail_in = chunk;
            offset += chunk;
            while (stream.avail_in > 0) {
                stream.next_out = reinterpret_cast<Bytef*>(out.data());
                stream.avail_out = static_cast<uInt>(out.size());
                const int rc = inflate(&stream, Z_NO_FLUSH);
                if (rc != Z_OK && rc != Z_STREAM_END) throw IoError("The download could not be decompressed.");
                Emit(out.data(), out.size() - stream.avail_out, max_decoded, decoded, sink);
                if (rc == Z_STREAM_END) return;
            }
        }
    }

    template <typename Sink>
    void Finish(uint64_t max_decoded, uint64_t& decoded, Sink&& sink) {
        if (!Compressed) return;
        auto& stream = Compressed->Stream;
        stream.next_in = nullptr;
        stream.avail_in = 0;
        std::vector<char> out(1 << 16);
        for (;;) {
            stream.next_out = reinterpret_cast<Bytef*>(out.data());
            stream.avail_out = static_cast<uInt>(out.size());
            const int rc = inflate(&stream, Z_FINISH);
            Emit(out.data(), out.size() - stream.avail_out, max_decoded, decoded, sink);
            if (rc == Z_STREAM_END) return;
            if (rc != Z_OK || stream.avail_out == out.size()) throw IoError("The download could not be decompressed.");
        }
    }
};

// Reads the body in pieces until the server ends it. Content-Length is compared to the encoded bytes.
template <typename Sink>
void ReadBody(Response& response, const HttpOptions& options, CancellationToken& cancellation, const DownloadProgress& progress, Sink&& sink) {
    BodyDecoder decoder(response.ContentEncoding);
    std::vector<char> buffer(1 << 16);
    uint64_t raw = 0, decoded = 0;
    const auto report = [&] { if (progress) progress(decoded, decoder.Identity() ? response.Length : std::nullopt); };
    for (;;) {
        cancellation.ThrowIfCancelled();
        DWORD read = 0;
        if (!WinHttpReadData(response.Request.Value, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) Fail("The download was interrupted.");
        if (read == 0) break;
        raw += read;
        if (raw > options.MaxBytes) throw InvalidData("The download is larger than Modworks accepts.");
        decoder.Consume(buffer.data(), read, options.MaxBytes, decoded, sink);
        report();
    }
    decoder.Finish(options.MaxBytes, decoded, sink);
    RequireFullBody(response.Length, raw);
    report();
}
}  // namespace

std::string DecodeHttpBody(std::string_view encoded, std::string_view content_encoding,
    std::optional<uint64_t> encoded_length, uint64_t max_bytes) {
    RequireFullBody(encoded_length, encoded.size());
    BodyDecoder decoder(content_encoding);
    std::string out;
    uint64_t decoded = 0;
    const auto sink = [&](const char* data, size_t size) { out.append(data, size); };
    if (!encoded.empty()) decoder.Consume(encoded.data(), encoded.size(), max_bytes, decoded, sink);
    decoder.Finish(max_bytes, decoded, sink);
    return out;
}

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
