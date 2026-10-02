// Synchronous HTTPS GET, implemented with WinHTTP on Windows and with libcurl
// elsewhere (macOS / Linux).
#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace tuf {

struct HttpResponse {
    int status = 0;
    std::string reason;
    std::string body;
};

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
HttpResponse http_get(const std::string& url, const std::string& proxy = std::string(), int timeout_sec = 30);

std::string http_reason_from_status(int status);

}  // namespace tuf
