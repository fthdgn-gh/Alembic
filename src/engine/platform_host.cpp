// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace alembic::engine::platform {

const char* name() { return "host"; }

void notify(const std::string& message) { std::fprintf(stderr, "[notify] %s\n", message.c_str()); }

void keep_awake() {}

std::vector<std::string> default_roots() {
    const char* home = std::getenv("HOME");
    return {home && *home ? home : "/"};
}

bool root_available(const std::string& root) {
    struct stat st{};
    return ::stat(root.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<Location> suggested_locations() { return {}; } // the engine lists its roots instead

Result<std::vector<std::uint8_t>> http_get(const std::string& url, std::size_t max_bytes) {
    // Only https URLs of the fixed dependency list come here; still, refuse quotes.
    if (url.rfind("https://", 0) != 0 || url.find_first_of("'\"\\ ") != std::string::npos)
        return make_error(Errc::invalid_argument, "bad URL");
    std::string cmd = "curl -sSfL --max-time 120 --max-filesize " + std::to_string(max_bytes) + " '" + url + "'";
    std::FILE* p = ::popen(cmd.c_str(), "r");
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
std::string launcher_problem() { return "needs a PS5"; }
Status install_launcher(std::uint16_t, const std::uint8_t*, std::size_t, bool) {
    return make_error(Errc::unsupported, "the home-screen shortcut needs a PS5");
}
Status remove_launcher() { return make_error(Errc::unsupported, "the home-screen shortcut needs a PS5"); }

std::string default_state_dir() {
    const char* home = std::getenv("HOME");
    return std::string(home && *home ? home : ".") + "/.alembic";
}

} // namespace alembic::engine::platform
