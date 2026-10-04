#include "api.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include "apppaths.hpp"
#include "console.hpp"
#include "http.hpp"
#include "numfmt.hpp"

namespace tuf {

// Overridable so the tool can be pointed at a mirror, a stub, or a local test
// server (TUFTOOLS_API_BASE=https://host[:port]).  Read once at startup; an
// empty value keeps the official endpoint.
const std::string BASE_URL = [] {
    const char* override_url = std::getenv("TUFTOOLS_API_BASE");
    if (override_url != nullptr && *override_url != '\0') return std::string(override_url);
    return std::string("https://api.tuforums.com");
}();

namespace {

using Clock = std::chrono::steady_clock;

const char* const kCacheFile = "cache.json";

std::string g_proxy;
bool g_verbose = false;
std::atomic<int> g_count{0};
std::atomic<long long> g_api_us{0};
Clock::time_point g_span_start{};
Clock::time_point g_span_end{};
std::mutex g_print_mutex;

// ------------------------------------------------------------- response cache
//
// Keyed by the full request URL, valued by the raw response body.  The body is
// kept as text rather than a parsed tree so a cache hit replays byte-for-byte
// what the network would have returned, and so the cache stays valid when the
// parser changes.
bool g_cache_enabled = true;
bool g_cache_loaded = false;
bool g_cache_dirty = false;
std::atomic<long long> g_cache_hits{0};
std::map<std::string, std::string> g_cache;
std::mutex g_cache_mutex;

double elapsed_ms(Clock::time_point t0, Clock::time_point t1) {
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

void print_locked(const std::string& line) {
    std::lock_guard<std::mutex> guard(g_print_mutex);
    std::cout << line << std::endl;
}

// Reads cache.json once, on first use.  Every failure path leaves the cache
// empty and usable instead of reporting an error: a broken cache file must never
// stop a lookup from reaching the network.
void cache_load_locked() {
    if (g_cache_loaded) return;
    g_cache_loaded = true;
    std::ifstream file(data_file(kCacheFile), std::ios::binary);
    if (!file.good()) return;
    try {
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const Json parsed = Json::parse(buffer.str());
        if (!parsed.is_object()) return;
        for (const std::pair<std::string, Json>& entry : parsed.items()) {
            if (!entry.second.is_string()) continue;
            g_cache[entry.first] = entry.second.as_string();
        }
        if (g_verbose) {
            print_locked("[cache] 载入 " + std::to_string(g_cache.size()) + " 条缓存 (" + data_file(kCacheFile) + ")");
        }
    } catch (const std::exception&) {
        // Corrupt file: start empty and let the next write replace it.
        g_cache.clear();
        g_cache_dirty = true;
    }
}

bool cache_write_locked(std::string& error) {
    try {
        Json root = Json::object();
        for (const std::pair<const std::string, std::string>& entry : g_cache) {
            root.set(entry.first, entry.second);
        }
        std::ofstream file(data_file(kCacheFile), std::ios::binary | std::ios::trunc);
        if (!file.good()) {
            error = std::string("无法写入 ") + kCacheFile;
            return false;
        }
        file << root.dump(2);
        g_cache_dirty = false;
        return true;
    } catch (const std::exception& e) {
        error = std::string("保存 ") + kCacheFile + " 失败: " + e.what();
        return false;
    }
}

int start_request(const std::string& url) {
    const int index = g_count.fetch_add(1) + 1;
    if (index == 1) {
        g_span_start = Clock::now();
        if (!g_verbose) print_locked("查询中...");
    }
    if (g_verbose) {
        std::string line = "#" + std::to_string(index) + " GET " + url;
        if (!g_proxy.empty()) line += "  (proxy: " + g_proxy + ")";
        print_locked(line);
    }
    return index;
}

// Bodies can be large (a single passes page carries whole level objects), so keep
// the verbose dump bounded; the real body size stays visible in the header line.
constexpr std::size_t kMaxVerboseBody = 256 * 1024;

// Renders a response body as an indented, line-numbered-free block that stays
// readable inside the verbose stream: JSON is pretty-printed when it parses,
// anything else (HTML error pages, plain text) is printed verbatim.
std::string format_body_block(const std::string& body) {
    if (body.empty()) return std::string();
    std::string text;
    bool pretty = false;
    try {
        Json parsed = Json::parse(body);
        text = parsed.dump(2);
        pretty = true;
    } catch (const std::exception&) {
        text = body;
    }
    bool truncated = false;
    if (text.size() > kMaxVerboseBody) {
        text.resize(kMaxVerboseBody);
        truncated = true;
    }

    std::string out = "    | (" + std::string(pretty ? "JSON" : "原始文本") + ")\n";
    std::size_t start = 0;
    while (true) {
        const std::size_t nl = text.find('\n', start);
        const std::string line =
            (nl == std::string::npos) ? text.substr(start) : text.substr(start, nl - start);
        out += "    | " + line;
        if (!line.empty() && line.back() == '\r') out.pop_back();
        out += '\n';
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (truncated) {
        out += "    | ... 已截断 (响应 " + std::to_string(body.size()) + " 字节)\n";
    }
    out.pop_back();  // print_locked() adds the final newline
    return out;
}

Json get_once(const std::string& url) {
    // A hit never counts as a request, so the "查询中..." banner and the request
    // total seen by stats() both reflect real network traffic only.
    if (g_cache_enabled) {
        std::lock_guard<std::mutex> guard(g_cache_mutex);
        cache_load_locked();
        auto it = g_cache.find(url);
        if (it != g_cache.end()) {
            g_cache_hits.fetch_add(1);
            if (g_verbose) {
                print_locked("    <- #cached " + url + "  (" + std::to_string(it->second.size()) + " B)");
            }
            if (it->second.empty()) return Json();
            return Json::parse(it->second);
        }
    }

    const int index = start_request(url);
    const Clock::time_point t0 = Clock::now();
    HttpResponse response;
    try {
        response = http_get(url, g_proxy, 30);
    } catch (const HttpError& e) {
        const Clock::time_point t1 = Clock::now();
        g_span_end = t1;
        g_api_us.fetch_add(static_cast<long long>(elapsed_ms(t0, t1) * 1000.0));
        if (g_verbose) {
            std::string line = "    <- #" + std::to_string(index) + " HTTP " + std::to_string(e.status()) +
                               " 请求失败 " + e.what() + "  " + format_fixed(elapsed_ms(t0, t1), 2) + " ms";
            const std::string block = format_body_block(e.body());
            if (!block.empty()) line += "\n" + block;
            print_locked(line);
        }
        throw;
    } catch (const std::exception& e) {
        const Clock::time_point t1 = Clock::now();
        g_span_end = t1;
        g_api_us.fetch_add(static_cast<long long>(elapsed_ms(t0, t1) * 1000.0));
        log("    <- #" + std::to_string(index) + " 请求失败 " + e.what() + "  " +
            format_fixed(elapsed_ms(t0, t1), 2) + " ms");
        throw;
    }

    const Clock::time_point t1 = Clock::now();
    g_span_end = t1;
    g_api_us.fetch_add(static_cast<long long>(elapsed_ms(t0, t1) * 1000.0));
    if (g_verbose) {
        std::string line = "    <- #" + std::to_string(index) + " " + std::to_string(response.status) + " " +
                           response.reason + "  " + format_fixed(elapsed_ms(t0, t1), 2) + " ms  " +
                           std::to_string(response.body.size()) + " B";
        const std::string block = format_body_block(response.body);
        if (!block.empty()) line += "\n" + block;
        print_locked(line);
    }

    if (response.body.empty()) return Json();
    if (g_cache_enabled) {
        std::lock_guard<std::mutex> guard(g_cache_mutex);
        cache_load_locked();
        g_cache[url] = response.body;
        g_cache_dirty = true;
    }
    return Json::parse(response.body);
}

}  // namespace

void set_proxies(const std::string& proxy_url) {
    if (!proxy_url.empty()) g_proxy = proxy_url;
}

void set_verbose(bool enabled) { g_verbose = enabled; }

bool verbose_enabled() { return g_verbose; }

std::string active_proxy() { return g_proxy; }

void set_cache_enabled(bool enabled) {
    std::lock_guard<std::mutex> guard(g_cache_mutex);
    g_cache_enabled = enabled;
}

bool cache_enabled() { return g_cache_enabled; }

long long cache_hits() { return g_cache_hits.load(); }

bool cache_clear(std::string& error) {
    std::lock_guard<std::mutex> guard(g_cache_mutex);
    // Load first so "clear" is applied to what is really on disk; marking the
    // cache loaded without reading it would make a later clear() a no-op on a
    // map that was never populated.
    cache_load_locked();
    g_cache.clear();
    g_cache_dirty = false;
    const bool written = cache_write_locked(error);
    if (written) g_cache_dirty = false;
    return written;
}

void cache_flush() {
    std::lock_guard<std::mutex> guard(g_cache_mutex);
    if (g_cache_enabled && g_cache_dirty) {
        std::string error;
        if (!cache_write_locked(error)) log(std::string("[cache] ") + error);
    }
}

void log(const std::string& msg) {
    if (g_verbose) print_locked(msg);
}

Json fetchapi_sync(const std::string& url) { return get_once(url); }

std::vector<FetchResult> fetchall_sync(const std::vector<std::string>& urls) {
    std::vector<FetchResult> results(urls.size());
    std::vector<std::thread> workers;
    workers.reserve(urls.size());
    for (size_t i = 0; i < urls.size(); ++i) {
        workers.emplace_back([&urls, &results, i]() {
            try {
                results[i].data = get_once(urls[i]);
                results[i].ok = true;
            } catch (const std::exception& e) {
                results[i].ok = false;
                results[i].error = e.what();
            }
        });
    }
    for (std::thread& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    return results;
}

void stats() {
    const int count = g_count.load();
    const long long hits = g_cache_hits.load();
    if (count || hits) {
        const double span = elapsed_ms(g_span_start, g_span_end);
        const double api_ms = static_cast<double>(g_api_us.load()) / 1000.0;
        const std::string hit_text = hits ? ("，命中缓存 " + std::to_string(hits) + " 次") : std::string();
        if (g_verbose) {
            print_locked("used " + std::to_string(count) + " requests, span " + format_fixed(span, 2) +
                         " ms (请求累计 " + format_fixed(api_ms, 2) + " ms)" + hit_text);
        } else {
            print_locked("总耗时: " + format_fixed(span, 2) + " ms" + hit_text);
        }
    }
    print_locked("\n" + std::string(70, '='));
    g_count.store(0);
    g_api_us.store(0);
    g_cache_hits.store(0);
    g_span_start = Clock::time_point{};
    g_span_end = Clock::time_point{};
}

}  // namespace tuf
