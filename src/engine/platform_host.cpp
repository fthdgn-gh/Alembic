// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform.hpp"

#include "alembic/io.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#define popen _popen
#define pclose _pclose
#endif

namespace alembic::engine::platform {

const char* name() { return "host"; }

void notify(const std::string& message) { std::fprintf(stderr, "[notify] %s\n", message.c_str()); }

void keep_awake() {}
std::string enter_button() { return ""; }
namespace {
// The user's home folder: $HOME, or %USERPROFILE% on Windows ('/' between parts).
std::string home_folder() {
#ifdef _WIN32
    std::string home = portable_path(environment("USERPROFILE"));
    return home.empty() ? "C:/" : home;
#else
    std::string home = environment("HOME");
    return home.empty() ? "/" : home;
#endif
}
} // namespace

std::string system_language() {
#ifdef _WIN32
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) >= 3)
        return std::string{static_cast<char>(name[0]), static_cast<char>(name[1])};
    return "en";
#else
    std::string l = environment("LANG");
    return l.size() >= 2 ? l.substr(0, 2) : "en";
#endif
}

std::vector<std::string> default_roots() { return {home_folder()}; }

bool root_available(const std::string& root) { return is_directory(root); }

std::vector<Location> suggested_locations() { return {}; } // the engine lists its roots instead

Result<std::vector<std::uint8_t>> http_get(const std::string& url, std::size_t max_bytes) {
    // Only https URLs of the fixed dependency list come here; still, refuse quotes.
    if (url.rfind("https://", 0) != 0 || url.find_first_of("'\"\\ ") != std::string::npos)
        return make_error(Errc::invalid_argument, "bad URL");
#ifdef _WIN32
    // curl.exe comes with Windows 10 (1803) and later; "rb": binary data.
    std::string cmd = "curl.exe -sSfL --max-time 120 --max-filesize " + std::to_string(max_bytes) + " \"" + url + "\"";
    std::FILE* p = ::popen(cmd.c_str(), "rb");
#else
    std::string cmd = "curl -sSfL --max-time 120 --max-filesize " + std::to_string(max_bytes) + " '" + url + "'";
    std::FILE* p = ::popen(cmd.c_str(), "r");
#endif
    if (!p) return make_error(Errc::io, "cannot run curl");
    std::vector<std::uint8_t> data;
    std::uint8_t buf[65536];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) {
        data.insert(data.end(), buf, buf + n);
        if (data.size() > max_bytes) {
            ::pclose(p);
            return make_error(Errc::too_large, "the download is larger than expected");
        }
    }
    int rc = ::pclose(p);
    if (rc != 0) return make_error(Errc::io, "download failed (curl exit " + std::to_string(rc) + "): " + url);
    return data;
}

Status install_package(const std::string&) {
    return make_error(Errc::unsupported, "installing packages needs a PS5");
}

bool launcher_supported() { return false; }
bool launcher_installed() { return false; }
bool launcher_recorded() { return false; }
bool launcher_current(std::uint16_t, const std::uint8_t*, std::size_t) { return false; }
std::string launcher_record_note() { return ""; }
std::string launcher_problem() { return "needs a PS5"; }
Status install_launcher(std::uint16_t, const std::uint8_t*, std::size_t, bool) {
    return make_error(Errc::unsupported, "the home-screen shortcut needs a PS5");
}
Status remove_launcher() { return make_error(Errc::unsupported, "the home-screen shortcut needs a PS5"); }

std::string default_state_dir() {
#ifdef _WIN32
    std::string local = portable_path(environment("LOCALAPPDATA"));
    if (!local.empty()) return local + "/Alembic";
#endif
    return home_folder() + "/.alembic";
}

} // namespace alembic::engine::platform
