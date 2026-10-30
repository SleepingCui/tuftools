// Synchronous HTTPS GET, implemented with WinHTTP on Windows and with libcurl
// elsewhere (macOS / Linux).
#pragma once

#include <stdexcept>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace tuf {

struct HttpResponse {
    int status = 0;
    std::string reason;
    std::string body;
    // Response validators, kept verbatim (ETag retains its quotes) so they can be
    // replayed as If-None-Match / If-Modified-Since on the next request.  Either
    // may be empty when the server does not supply it.
    std::string etag;
    std::string last_modified;
};

// Extra request headers, sent in order.  Used by the response cache to issue
// conditional GETs; values are passed through untouched.
using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

class HttpError : public std::runtime_error {
public:
    HttpError(const std::string& message, int status, std::string body = std::string())
        : std::runtime_error(message), status_(status), body_(std::move(body)) {}
    int status() const { return status_; }
    // Error payload as returned by the server; useful for diagnostics (verbose log).
    const std::string& body() const { return body_; }

private:
    int status_;
    std::string body_;
};

// Throws std::runtime_error on transport failure, HttpError on status >= 400.
using HttpProgressCallback = std::function<void(std::uint64_t downloaded, std::uint64_t total)>;

HttpResponse http_get(const std::string& url, const std::string& proxy = std::string(), int timeout_sec = 30,
                      const HttpProgressCallback& progress = HttpProgressCallback(),
                      const HttpHeaders& request_headers = HttpHeaders());

std::string http_reason_from_status(int status);

}  // namespace tuf
