// Locates the data directory used for settings.json and cache.json.
//
// Portable layout: data files live next to the running executable, so tuftools
// can be started from any working directory. If that directory is not writable
// (e.g. installed under /usr/local/bin), it falls back to the per-user data
// directory. TUFTOOLS_DATA_DIR overrides both.
#pragma once

#include <string>

#include "pyjson.hpp"

namespace tuf {

// Directory containing the running executable, or "" when it cannot be found.
std::string executable_dir();

// Directory where data files are read from and written to (created if needed).
std::string data_dir();

// Absolute path of a data file; neither creates nor checks the file.
std::string data_file(const std::string& name);

// All persistent application data except the HTTP cache is stored in settings.json.
bool settings_read(const std::string& section, Json& value, std::string& error);
bool settings_write(const std::string& section, const Json& value, std::string& error);

}  // namespace tuf
