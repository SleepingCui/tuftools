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

// Response cache.  Every GET goes through here, so a second lookup of the same
// player or difficulty list costs no request at all.  Entries live in
// cache.json inside the data directory; a corrupt or unreadable file is simply
// ignored (the cache degrades to a no-op, it never breaks a lookup).
void set_cache_enabled(bool enabled);
bool cache_enabled();

// Number of requests served from the cache instead of the network.
long long cache_hits();

// Empties the cache both in memory and on disk.  Returns false when the file
// could not be rewritten.
bool cache_clear(std::string& error);

// Persists the cache; called before the process exits so entries survive restarts.
void cache_flush();

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
