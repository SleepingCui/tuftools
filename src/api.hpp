// HTTP layer mirroring api.py: request counter, verbose logging and stats.
#pragma once

#include <string>
#include <vector>

#include "pyjson.hpp"

namespace tuf {

extern const std::string BASE_URL;  // "https://api.tuforums.com"

void set_proxies(const std::string& proxy_url);
void set_verbose(bool enabled);
bool verbose_enabled();
std::string active_proxy();

void log(const std::string& msg);

struct FetchResult {
    bool ok = false;
    Json data;
    std::string error;
};

Json fetchapi_sync(const std::string& url);
std::vector<FetchResult> fetchall_sync(const std::vector<std::string>& urls);

void stats();

}  // namespace tuf
