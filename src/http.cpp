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

HttpResponse http_get(const std::string& url, const std::string& proxy, int timeout_sec) {
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
                                    proxy.empty() ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
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

    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        throw std::runtime_error(winhttp_message("发送请求", url));
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        throw std::runtime_error(winhttp_message("接收响应", url));
    }

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

    std::vector<char> buffer(8192);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            throw std::runtime_error(winhttp_message("读取响应", url));
        }
        if (read == 0) break;
        response.body.append(buffer.data(), read);
    }

    if (response.status >= 400) {
        throw HttpError("HTTP " + std::to_string(response.status) + " " + response.reason + " for url: " + url,
                        response.status);
    }
    return response;
}

}  // namespace tuf

#else  // ------------------------------ non-Windows fallback ------------------------------

namespace tuf {
namespace {

std::string shell_quote(const std::string& text) {
    std::string out = "'";
    for (char c : text) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

}  // namespace

std::string http_reason_from_status(int status) {
    if (status == 200) return "OK";
    if (status == 404) return "Not Found";
    if (status == 429) return "Too Many Requests";
    if (status >= 500) return "Server Error";
    if (status >= 400) return "Client Error";
    return std::string();
}

HttpResponse http_get(const std::string& url, const std::string& proxy, int timeout_sec) {
    std::string command = "curl -sS -L --max-time " + std::to_string(timeout_sec);
    if (!proxy.empty()) command += " -x " + shell_quote(proxy);
    command += " -w '\\n%{http_code}' " + shell_quote(url) + " 2>/dev/null";

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) throw std::runtime_error("请求失败: 无法执行 curl: " + url);

    std::string output;
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, read);
    const int rc = pclose(pipe);
    if (rc != 0) throw std::runtime_error("请求失败: curl 退出码 " + std::to_string(rc) + " " + url);

    HttpResponse response;
    const size_t marker = output.rfind('\n');
    if (marker == std::string::npos) {
        response.body = output;
    } else {
        response.body = output.substr(0, marker);
        try {
            response.status = std::stoi(output.substr(marker + 1));
        } catch (const std::exception&) {
            response.status = 0;
        }
    }
    response.reason = http_reason_from_status(response.status);
    if (response.status >= 400) {
        throw HttpError("HTTP " + std::to_string(response.status) + " " + response.reason + " for url: " + url,
                        response.status);
    }
    return response;
}

}  // namespace tuf

#endif
