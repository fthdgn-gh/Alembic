// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform.hpp"

#include "alembic/io.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#define popen _popen
#define pclose _pclose
#else
#include <sys/utsname.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#if defined(__linux__)
#include <sys/random.h>
#endif

namespace alembic::engine::platform {

const char* name() { return "host"; }

std::string os_description() {
#ifdef _WIN32
    // GetVersionEx answers what the program's manifest asks for; ntdll tells the truth.
    RTL_OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    using Fn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    if (!fn || fn(&v) != 0) return "Windows";
    std::string name = v.dwMajorVersion == 10 ? (v.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10")
                                              : "Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion);
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    const char* arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "ARM64" : "x64";
    return name + " (build " + std::to_string(v.dwBuildNumber) + ", " + arch + ")";
#else
    utsname u{};
    std::string arch = ::uname(&u) == 0 ? u.machine : "";
    std::string name;
#ifdef __APPLE__
    char version[64] = {};
    std::size_t size = sizeof version;
    name = ::sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) == 0 ? std::string("macOS ") + version : "macOS";
#else
    // Linux: the distribution's own name, else the kernel's.
    if (std::FILE* f = std::fopen("/etc/os-release", "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            std::string l = line;
            if (l.rfind("PRETTY_NAME=", 0) != 0) continue;
            l = l.substr(12);
            while (!l.empty() && (l.back() == '\n' || l.back() == '"')) l.pop_back();
            if (!l.empty() && l.front() == '"') l.erase(l.begin());
            name = l;
        }
        std::fclose(f);
    }
    if (name.empty()) name = std::string(u.sysname) + " " + u.release;
#endif
    return arch.empty() ? name : name + " (" + arch + ")";
#endif
}

void notify(const std::string& message) { std::fprintf(stderr, "[notify] %s\n", message.c_str()); }
void set_notification_icon(const std::string&, const std::uint8_t*, std::size_t) {}

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
    // Only https URLs of the fixed dependency list and GitHub's releases come
    // here (plain http to this computer: tests of the updater); still, refuse quotes.
    bool local = url.rfind("http://127.0.0.1:", 0) == 0 || url.rfind("http://localhost:", 0) == 0;
    if ((url.rfind("https://", 0) != 0 && !local) || url.find_first_of("'\"\\ ") != std::string::npos)
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

Status install_package(const std::string&, const std::string&, const std::string&) {
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

Status secure_random(std::uint8_t* out, std::size_t size) {
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return make_error(Errc::io, "the system's random generator failed");
#elif defined(__linux__)
    for (std::size_t done = 0; done < size;) {
        std::size_t n = std::min<std::size_t>(size - done, 256); // getentropy's limit
        if (::getentropy(out + done, n) != 0) return make_error(Errc::io, "the system's random generator failed");
        done += n;
    }
#else
    ::arc4random_buf(out, size); // macOS: never fails
#endif
    return {};
}

void set_process_name(const char*) {}

void pause_before_exit() {
#ifdef _WIN32
    DWORD pids[2];
    if (GetConsoleProcessList(pids, 2) == 1) { // only us: nobody else will show the text
        std::fprintf(stderr, "\nPress Enter to close this window.\n");
        std::getchar();
    }
#endif
}

std::string default_state_dir() {
#ifdef _WIN32
    std::string local = portable_path(environment("LOCALAPPDATA"));
    if (!local.empty()) return local + "/Alembic";
#endif
    return home_folder() + "/.alembic";
}

} // namespace alembic::engine::platform
