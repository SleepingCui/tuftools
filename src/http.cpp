#include "http.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

namespace tuf {
namespace {

std::wstring to_wide(const std::string& text) {
    if (text.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], need);
    return out;
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], need, nullptr, nullptr);
    return out;
}

class HandleGuard {
public:
    HandleGuard() = default;
    explicit HandleGuard(HINTERNET handle) : handle_(handle) {}
    ~HandleGuard() { reset(); }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;

    void reset(HINTERNET handle = nullptr) {
        if (handle_) WinHttpCloseHandle(handle_);
        handle_ = handle;
    }
    HINTERNET get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    HINTERNET handle_ = nullptr;
};

std::string winhttp_message(const std::string& stage, const std::string& url) {
    return stage + "失败: " + url + " (WinHTTP error " + std::to_string(GetLastError()) + ")";
}

std::string trim_ascii(const std::string& text) {
    size_t first = 0;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r')) ++first;
    size_t last = text.size();
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r')) --last;
    return text.substr(first, last - first);
}

// Reads one arbitrary response header by name.  WINHTTP_QUERY_CUSTOM accepts a
// caller-supplied header name, which is how ETag / Last-Modified are fetched
// without depending on the version-specific WINHTTP_QUERY_* aliases.
std::string query_custom_header(HINTERNET request, const wchar_t* name) {
    DWORD size = 0;
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, WINHTTP_NO_OUTPUT_BUFFER, &size,
                            WINHTTP_NO_HEADER_INDEX)) {
        return std::string();
    }
    if (size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return std::string();
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, &value[0], &size, WINHTTP_NO_HEADER_INDEX)) {
        return std::string();
    }
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return trim_ascii(to_utf8(value));
}

}  // namespace

std::string http_reason_from_status(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "";
    }
}

HttpResponse http_get(const std::string& url, const std::string& proxy, int timeout_sec,
                      const HttpProgressCallback& progress, const HttpHeaders& request_headers) {
    HttpResponse response;

    const std::wstring wide_url = to_wide(url);
    if (wide_url.empty()) throw std::runtime_error("请求失败: 无效的 URL");

    URL_COMPONENTS components;
    std::memset(&components, 0, sizeof(components));
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &components)) {
        throw std::runtime_error("请求失败: 无法解析 URL " + url);
    }

    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;

    const std::wstring wide_proxy = to_wide(proxy);
    HandleGuard session(WinHttpOpen(L"tuftools/1.0 (cpp)",
                                    proxy.empty() ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                                    proxy.empty() ? WINHTTP_NO_PROXY_NAME : wide_proxy.c_str(),
                                    WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) throw std::runtime_error(winhttp_message("打开会话", url));

    const int timeout_ms = timeout_sec * 1000;
    WinHttpSetTimeouts(session.get(), timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HandleGuard connection(WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
    if (!connection) throw std::runtime_error(winhttp_message("连接", url));

    HandleGuard request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) throw std::runtime_error(winhttp_message("创建请求", url));

#ifdef WINHTTP_OPTION_DECOMPRESSION
    {
        DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(request.get(), WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
    }
#endif

    if (!request_headers.empty()) {
        std::wstring block;
        for (const std::pair<std::string, std::string>& header : request_headers) {
            block += to_wide(header.first);
            block += L": ";
            block += to_wide(header.second);
            block += L"\r\n";
        }
        if (!WinHttpAddRequestHeaders(request.get(), block.c_str(), static_cast<DWORD>(block.size()),
                                      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            throw std::runtime_error(winhttp_message("设置请求头", url));
        }
    }

    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        throw std::runtime_error(winhttp_message("发送请求", url));
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        throw std::runtime_error(winhttp_message("接收响应", url));
    }

    // Validators must be read after the response headers arrived and before the
    // body is drained; a 304 carries them without a body.
    response.etag = query_custom_header(request.get(), L"ETag");
    response.last_modified = query_custom_header(request.get(), L"Last-Modified");

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        response.status = static_cast<int>(status);
    }

    DWORD reason_size = 0;
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_TEXT, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &reason_size,
                        WINHTTP_NO_HEADER_INDEX);
    if (reason_size > 0) {
        std::wstring reason(reason_size / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_TEXT, WINHTTP_HEADER_NAME_BY_INDEX, &reason[0],
                                &reason_size, WINHTTP_NO_HEADER_INDEX)) {
            while (!reason.empty() && reason.back() == L'\0') reason.pop_back();
            response.reason = to_utf8(reason);
        }
    }
    if (response.reason.empty()) response.reason = http_reason_from_status(response.status);

    std::uint64_t content_length = 0;
    DWORD content_length_value = 0;
    DWORD content_length_size = sizeof(content_length_value);
    if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &content_length_value, &content_length_size,
                            WINHTTP_NO_HEADER_INDEX)) {
        content_length = content_length_value;
    }
    if (progress) {
        try {
            progress(0, content_length);
        } catch (...) {
        }
    }

    std::vector<char> buffer(8192);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            throw std::runtime_error(winhttp_message("读取响应", url));
        }
        if (read == 0) break;
        response.body.append(buffer.data(), read);
        if (progress) {
            try {
                progress(static_cast<std::uint64_t>(response.body.size()), content_length);
            } catch (...) {
            }
        }
    }

    if (response.status >= 400) {
        throw HttpError("HTTP " + std::to_string(response.status) + " " + response.reason + " for url: " + url,
                        response.status, response.body);
    }
    return response;
}

}  // namespace tuf

#else  // ------------------------------ non-Windows (libcurl) ------------------------------

#include <curl/curl.h>

#include <mutex>

namespace tuf {
namespace {

std::once_flag g_curl_once;
CURLcode g_curl_init_code = CURLE_OK;

void ensure_curl_global_init() {
    std::call_once(g_curl_once, []() { g_curl_init_code = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (g_curl_init_code != CURLE_OK) {
        throw std::runtime_error(std::string("请求失败: libcurl 初始化失败: ") +
                                 curl_easy_strerror(g_curl_init_code));
    }
}

size_t write_body(char* data, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(data, size * nmemb);
    return size * nmemb;
}

int report_progress(void* userdata, curl_off_t download_total, curl_off_t downloaded, curl_off_t, curl_off_t) {
    const auto* callback = static_cast<const HttpProgressCallback*>(userdata);
    if (callback == nullptr || !*callback) return 0;
    try {
        (*callback)(downloaded > 0 ? static_cast<std::uint64_t>(downloaded) : 0,
                    download_total > 0 ? static_cast<std::uint64_t>(download_total) : 0);
    } catch (...) {
        // Never allow a progress display exception to cross libcurl's C callback.
    }
    return 0;
}

// Collects the response validators while the body callback fetches the payload.
// A redirect chain produces one header block per hop, so the stored values are
// reset on every status line and end up describing the final response.
struct HeaderCapture {
    std::string etag;
    std::string last_modified;
};

std::string trim_ascii(const std::string& text) {
    size_t first = 0;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r')) ++first;
    size_t last = text.size();
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r')) --last;
    return text.substr(first, last - first);
}

std::string lowercase_ascii(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

size_t capture_header(char* data, size_t size, size_t nmemb, void* userdata) {
    const size_t length = size * nmemb;
    auto* capture = static_cast<HeaderCapture*>(userdata);
    if (capture == nullptr || length == 0) return length;
    const std::string line(data, length);
    if (line.rfind("HTTP/", 0) == 0) {
        capture->etag.clear();
        capture->last_modified.clear();
        return length;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos) return length;
    const std::string name = lowercase_ascii(trim_ascii(line.substr(0, colon)));
    const std::string value = trim_ascii(line.substr(colon + 1));
    if (name == "etag") {
        capture->etag = value;
    } else if (name == "last-modified") {
        capture->last_modified = value;
    }
    return length;
}

}  // namespace

std::string http_reason_from_status(int status) {
    if (status == 200) return "OK";
    if (status == 204) return "No Content";
    if (status == 304) return "Not Modified";
    if (status == 404) return "Not Found";
    if (status == 429) return "Too Many Requests";
    if (status >= 500) return "Server Error";
    if (status >= 400) return "Client Error";
    return std::string();
}

HttpResponse http_get(const std::string& url, const std::string& proxy, int timeout_sec,
                      const HttpProgressCallback& progress, const HttpHeaders& request_headers) {
    ensure_curl_global_init();

    CURL* handle = curl_easy_init();
    if (!handle) throw std::runtime_error("请求失败: 无法创建 curl 句柄: " + url);
    struct CurlGuard {
        CURL* handle;
        ~CurlGuard() { curl_easy_cleanup(handle); }
    } guard{handle};

    HttpResponse response;
    HeaderCapture capture;
    char error_buffer[CURL_ERROR_SIZE] = {0};

    // Conditional-GET headers built by the cache layer; curl keeps its own copy,
    // so the list only has to outlive curl_easy_perform().
    curl_slist* header_list = nullptr;
    for (const std::pair<std::string, std::string>& header : request_headers) {
        const std::string line = header.first + ": " + header.second;
        curl_slist* grown = curl_slist_append(header_list, line.c_str());
        if (grown == nullptr) {
            curl_slist_free_all(header_list);
            throw std::runtime_error("请求失败: 无法设置请求头 " + header.first + " " + url);
        }
        header_list = grown;
    }
    struct SlistGuard {
        curl_slist* list;
        ~SlistGuard() { curl_slist_free_all(list); }
    } header_guard{header_list};

    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "tuftools/1.0 (cpp)");
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");  // ask libcurl to request/decompress gzip etc.
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, capture_header);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &capture);
    if (header_list != nullptr) curl_easy_setopt(handle, CURLOPT_HTTPHEADER, header_list);
    if (progress) {
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, report_progress);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &progress);
    }
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec));
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_buffer);
    if (!proxy.empty()) curl_easy_setopt(handle, CURLOPT_PROXY, proxy.c_str());

#if LIBCURL_VERSION_NUM >= 0x075500  // 7.85.0: string protocol lists
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#endif

    const CURLcode code = curl_easy_perform(handle);
    if (code != CURLE_OK) {
        const char* detail = error_buffer[0] ? error_buffer : curl_easy_strerror(code);
        throw std::runtime_error("请求失败: " + std::string(detail) + " " + url);
    }

    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    response.status = static_cast<int>(status);
    response.reason = http_reason_from_status(response.status);
    response.etag = capture.etag;
    response.last_modified = capture.last_modified;

    if (response.status >= 400) {
        throw HttpError("HTTP " + std::to_string(response.status) + " " + response.reason + " for url: " + url,
                        response.status, response.body);
    }
    return response;
}

}  // namespace tuf

#endif
