#include "apppaths.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace tuf {
namespace {

constexpr const char* kSettingsFile = "settings.json";

struct LegacyDataFile {
    const char* filename;
    const char* section;
};

constexpr LegacyDataFile kLegacyDataFiles[] = {
    {"xacc.json", "xacc"},
    {"difficulties.json", "difficulties"},
    {"transform_presets.json", "transformPresets"},
    {"costs.json", "legacyCosts"},
    {"weights.json", nullptr},
};

void retire_legacy_file(const std::string& path) {
    std::string backup = path + ".v1.bak";
    for (unsigned int suffix = 1; std::filesystem::exists(backup); ++suffix) {
        backup = path + ".v1.bak." + std::to_string(suffix);
    }
    std::error_code ec;
    std::filesystem::rename(path, backup, ec);
}

bool write_settings_file(const Json& root, std::string& error) {
    std::ofstream output(data_file(kSettingsFile), std::ios::binary | std::ios::trunc);
    if (!output.good()) {
        error = std::string("无法写入 ") + kSettingsFile;
        return false;
    }
    output << root.dump(2);
    if (!output.good()) {
        error = std::string("写入 ") + kSettingsFile + " 失败";
        return false;
    }
    return true;
}

bool load_settings_root(Json& root, std::string& error) {
    const std::string path = data_file(kSettingsFile);
    std::ifstream input(path, std::ios::binary);
    if (input.good()) {
        std::ostringstream buffer;
        buffer << input.rdbuf();
        try {
            root = Json::parse(buffer.str());
        } catch (const std::exception& e) {
            error = std::string(kSettingsFile) + " 无法解析: " + e.what();
            return false;
        }
        if (!root.is_object()) {
            error = std::string(kSettingsFile) + " 顶层必须是 JSON 对象";
            return false;
        }
    } else {
        root = Json::object();
    }

    bool imported = false;
    std::vector<std::string> obsolete_files;
    for (const LegacyDataFile& legacy : kLegacyDataFiles) {
        const std::string legacy_path = data_file(legacy.filename);
        std::ifstream old_input(legacy_path, std::ios::binary);
        if (!old_input.good()) continue;

        if (legacy.section != nullptr && !root.has(legacy.section)) {
            std::ostringstream buffer;
            buffer << old_input.rdbuf();
            try {
                root.set(legacy.section, Json::parse(buffer.str()));
                imported = true;
                obsolete_files.push_back(legacy_path);
            } catch (const std::exception&) {
                // Keep malformed legacy files so their contents can be recovered.
            }
        } else {
            obsolete_files.push_back(legacy_path);
        }
    }

    if (imported && !write_settings_file(root, error)) return false;
    // Keep a recoverable copy while removing the old .json names from active use.
    for (const std::string& obsolete : obsolete_files) retire_legacy_file(obsolete);
    return true;
}

std::string dir_of(const std::string& path) {
    const size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return std::string();
    if (pos == 0) return path.substr(0, 1);  // filesystem root
    return path.substr(0, pos);
}

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

bool is_writable_dir(const std::string& dir) {
    if (dir.empty()) return false;
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesA(dir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
#endif
    const std::filesystem::path probe = std::filesystem::path(dir) / ".tuftools-write-test";
    std::ofstream test(probe, std::ios::binary | std::ios::trunc);
    if (!test.good()) return false;
    test.close();
    std::error_code ec;
    std::filesystem::remove(probe, ec);
    return true;
}

#ifdef _WIN32
std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], need, nullptr, nullptr);
    return out;
}
#endif

std::string user_data_dir() {
#ifdef _WIN32
    std::string base = env_or_empty("LOCALAPPDATA");
    if (base.empty()) base = env_or_empty("APPDATA");
    if (base.empty()) return std::string();
    return base + "\\tuftools";
#elif defined(__APPLE__)
    const std::string home = env_or_empty("HOME");
    if (home.empty()) return std::string();
    return home + "/Library/Application Support/tuftools";
#else
    std::string base = env_or_empty("XDG_DATA_HOME");
    if (base.empty()) {
        const std::string home = env_or_empty("HOME");
        if (home.empty()) return std::string();
        base = home + "/.local/share";
    }
    return base + "/tuftools";
#endif
}

std::string compute_data_dir() {
    const std::string override_dir = env_or_empty("TUFTOOLS_DATA_DIR");
    if (!override_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(override_dir, ec);
        return override_dir;
    }

    const std::string exe_dir = executable_dir();
    if (is_writable_dir(exe_dir)) return exe_dir;

    std::string fallback = user_data_dir();
    if (fallback.empty()) return exe_dir;
    std::error_code ec;
    std::filesystem::create_directories(fallback, ec);
    return fallback;
}

}  // namespace

std::string executable_dir() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
        if (len == 0) return std::string();
        if (len < buffer.size()) {
            buffer.resize(len);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return dir_of(to_utf8(buffer));
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(&buffer[0], &size) != 0) return std::string();
    buffer.resize(std::strlen(buffer.c_str()));
    char resolved[PATH_MAX];
    if (realpath(buffer.c_str(), resolved)) return dir_of(resolved);
    return dir_of(buffer);
#else
    char buffer[PATH_MAX];
    const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len <= 0) return std::string();
    buffer[len] = '\0';
    return dir_of(buffer);
#endif
}

std::string data_dir() {
    static const std::string dir = compute_data_dir();
    return dir;
}

std::string data_file(const std::string& name) {
    const std::string dir = data_dir();
    if (dir.empty()) return name;
#ifdef _WIN32
    return dir + "\\" + name;
#else
    return dir + "/" + name;
#endif
}

bool settings_read(const std::string& section, Json& value, std::string& error) {
    Json root;
    if (!load_settings_root(root, error)) return false;
    value = root.get(section);
    return true;
}

bool settings_write(const std::string& section, const Json& value, std::string& error) {
    Json root;
    if (!load_settings_root(root, error)) return false;
    root.set(section, value);
    return write_settings_file(root, error);
}

}  // namespace tuf
