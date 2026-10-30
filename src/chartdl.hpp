// Non-interactive chart download helpers: URL building, transform presets and
// a silent file transfer core.  menus.cpp drives the same endpoints from its
// interactive menus; this module is what --json mode (jsoncli.cpp) uses.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "http.hpp"
#include "pyjson.hpp"

namespace tuf {

// Ordered query parameters of a transform request; the order is preserved so
// the generated URL stays stable and identical to the interactive flow.
using TransformParams = std::vector<std::pair<std::string, std::string>>;

// One of the five built-in transform modes ("推荐低特效" etc.).
struct TransformPreset {
    std::string key;   // stable machine name, e.g. "low_effects"
    std::string name;  // Chinese label shown by the interactive menu
    TransformParams params;
};

const std::vector<TransformPreset>& builtin_transform_presets();

// URL of a transformed ZIP for one CDN file id.
std::string transform_zip_url(const std::string& file_id, const TransformParams& params);
// URL of the V2 .adofai export of one CDN file id.
std::string level_v2_url(const std::string& file_id);
// "TUF_Level_<id><suffix>" with Windows-hostile characters replaced by '_'.
std::string safe_level_filename(const Json& level, const std::string& suffix);

// /v2/database/levels/<id>/cdnData.  Throws like the rest of the API layer.
Json fetch_cdn_data(const std::string& level_id);

// Custom presets persisted in settings.json under "transformPresets" (at most
// five entries, each {"name": string, "params": {string: string}}).
Json load_custom_presets();
bool save_custom_presets(const Json& presets, std::string& error);

// Validates and appends one custom transform parameter.  Accepted keys:
// keepEvents / dropEvents / dropFilters (comma separated names),
// baseCameraZoom (0.1-10), constantBackgroundColor (rrggbb / rrggbbaa),
// removeForegroundFlash (true/false).  Returns false and fills `error` otherwise.
bool append_transform_param(TransformParams& params, const std::string& key, const std::string& value,
                            std::string& error);

struct ChartDownload {
    bool ok = false;
    std::string path;  // final path, after the _1/_2 collision suffix
    std::uint64_t bytes = 0;
    bool suffixed = false;
    std::string error;  // empty when ok
};

// Downloads `url` into `path` (creating parent directories, suffixing _1, _2...
// when the file already exists).  With `expect_zip` the payload must start with
// "PK".  Transport errors, empty payloads and I/O failures all come back as
// ok=false with a message in `error`; nothing is printed.
ChartDownload download_to_file(const std::string& url, const std::string& path, bool expect_zip,
                               const HttpProgressCallback& progress = HttpProgressCallback());

}  // namespace tuf
