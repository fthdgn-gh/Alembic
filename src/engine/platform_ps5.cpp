// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 payload side. Notifications follow ps5-payload-dev/sdk's samples/notify
// (GPL-3.0-or-later); keeping the console awake follows PS5-LZ4-Compression's
// src/lzc_power_guard.c (GPL-3.0-or-later).
#include "platform.hpp"
#include "json_value.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/mount.h>
#include <sys/stat.h>

// sceAppInstUtilInstallByPackage, as in ps5-payload-dev/shsrv's
// bundles/pkg_install (GPL-3.0-or-later).
struct PkgMetadata {
    const char* uri;
    const char* ex_uri;
    const char* playgo_scenario_id;
    const char* content_id;
    const char* content_name;
    const char* icon_url;
};
struct PkgInfo {
    char content_id[48];
    int type;
    int platform;
};
struct PlaygoInfo {
    char lang[8][30];
    char scenario_ids[3][64];
    char content_ids[64];
    long unknown[810];
};

// HTTPS through the system libraries, following drakmor/ShadowMountPlus's
// src/sm_ampr_updater.c (GPL-3.0).
struct SceSslData {
    char* ptr;
    std::size_t size;
};

extern "C" {
int sceNetInit(void) __attribute__((weak));
int sceNetPoolCreate(const char* name, int size, int flags) __attribute__((weak));
int sceNetPoolDestroy(int pool_id) __attribute__((weak));
int sceSslInit(std::size_t pool_size) __attribute__((weak));
int sceSslTerm(int ssl_id) __attribute__((weak));
int sceSslLoadCert(int ssl_id, int ca_count, const SceSslData** ca_list, const SceSslData* cert,
                   const SceSslData* private_key) __attribute__((weak));
int sceSslUnloadCert(int ssl_id) __attribute__((weak));
int sceHttpInit(int net_pool_id, int ssl_id, std::size_t pool_size) __attribute__((weak));
int sceHttpTerm(int http_id) __attribute__((weak));
int sceHttpCreateTemplate(int http_id, const char* user_agent, int http_version, int auto_proxy_config)
    __attribute__((weak));
int sceHttpDeleteTemplate(int template_id) __attribute__((weak));
int sceHttpCreateConnectionWithURL(int template_id, const char* url, int keepalive) __attribute__((weak));
int sceHttpDeleteConnection(int connection_id) __attribute__((weak));
int sceHttpCreateRequestWithURL2(int connection_id, const char* method, const char* url, std::uint64_t length)
    __attribute__((weak));
int sceHttpDeleteRequest(int request_id) __attribute__((weak));
int sceHttpSendRequest(int request_id, const void* data, std::size_t size) __attribute__((weak));
int sceHttpGetStatusCode(int request_id, int* status) __attribute__((weak));
int sceHttpReadData(int request_id, void* buffer, std::size_t size) __attribute__((weak));
int sceHttpSetAutoRedirect(int id, int enabled) __attribute__((weak));
int sceHttpSetResponseHeaderMaxSize(int id, std::size_t size) __attribute__((weak));
int sceHttpSetResolveTimeOut(int id, std::uint32_t us) __attribute__((weak));
int sceHttpSetConnectTimeOut(int id, std::uint32_t us) __attribute__((weak));
int sceHttpSetSendTimeOut(int id, std::uint32_t us) __attribute__((weak));
int sceHttpSetRecvTimeOut(int id, std::uint32_t us) __attribute__((weak));
}

#include "github_ca.inc"

extern "C" {
int sceAppInstUtilInitialize(void) __attribute__((weak));
int sceAppInstUtilInstallByPackage(const PkgMetadata*, PkgInfo*, PlaygoInfo*) __attribute__((weak));
int sceNotificationSend(int user_id, bool is_logged, const char* payload) __attribute__((weak));
int sceSystemServicePowerTick(void) __attribute__((weak));
}

namespace alembic::engine::platform {

namespace {
constexpr int kSystemUser = 0xFE; // SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM
}

const char* name() { return "ps5"; }

void notify(const std::string& message) {
    std::fprintf(stderr, "[notify] %s\n", message.c_str());
    if (!sceNotificationSend) return;
    // A plain toast with the predefined download icon.
    std::string payload = "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\","
                          "\"channelType\":\"Downloads\",\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"No\","
                          "\"isImmediate\":true,\"priority\":100,\"viewData\":{"
                          "\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},"
                          "\"message\":{\"body\":" +
                          json_quote(message) +
                          "},\"subMessage\":{\"body\":\"Alembic\"}}},"
                          "\"createdDateTime\":\"2026-01-01T00:00:00.000Z\",\"localNotificationId\":\"5940\"}";
    sceNotificationSend(kSystemUser, true, payload.c_str());
}

void keep_awake() {
    // At most once every 20 seconds.
    static std::mutex mutex;
    static std::chrono::steady_clock::time_point last{};
    std::lock_guard<std::mutex> lock(mutex);
    auto now = std::chrono::steady_clock::now();
    if (last != std::chrono::steady_clock::time_point{} && now - last < std::chrono::seconds(20)) return;
    last = now;
    if (sceSystemServicePowerTick) sceSystemServicePowerTick();
}

std::vector<std::string> default_roots() {
    std::vector<std::string> roots{"/data"};
    for (int i = 0; i < 8; ++i) roots.push_back("/mnt/usb" + std::to_string(i));
    for (int i = 0; i < 2; ++i) roots.push_back("/mnt/ext" + std::to_string(i));
    return roots;
}

bool root_available(const std::string& root) {
    struct stat st{};
    if (::stat(root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    if (root.rfind("/mnt/", 0) != 0) return true;
    // A drive is mounted exactly on this folder.
    struct statfs fs{};
    return ::statfs(root.c_str(), &fs) == 0 && std::strcmp(fs.f_mntonname, root.c_str()) == 0;
}

std::vector<Location> suggested_locations() {
    // ShadowMount+ scans /data/homebrew and, on drives, <drive>/homebrew.
    std::vector<Location> out{{"Internal · homebrew (ShadowMount+)", "/data/homebrew"}, {"Internal storage", "/data"}};
    auto drive = [&](const std::string& root, const std::string& name) {
        if (!root_available(root)) return;
        out.push_back({name + " · homebrew", root + "/homebrew"});
        out.push_back({name, root});
    };
    for (int i = 0; i < 8; ++i) drive("/mnt/usb" + std::to_string(i), "USB " + std::to_string(i));
    for (int i = 0; i < 2; ++i) drive("/mnt/ext" + std::to_string(i), "Extended storage " + std::to_string(i));
    return out;
}

namespace {

std::string hex_code(int rc) {
    char code[16];
    std::snprintf(code, sizeof code, "0x%08x", static_cast<unsigned>(rc));
    return code;
}

} // namespace

Result<std::vector<std::uint8_t>> http_get(const std::string& url, std::size_t max_bytes) {
    if (!sceHttpInit || !sceSslInit || !sceNetPoolCreate) return make_error(Errc::unsupported, "the system HTTP library is not available");
    // One download at a time keeps the pools small.
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    static bool net_ready = false;
    if (!net_ready) {
        int rc = sceNetInit ? sceNetInit() : 0;
        if (rc != 0) return make_error(Errc::io, "sceNetInit failed: " + hex_code(rc));
        net_ready = true;
    }
    constexpr std::uint32_t kTimeout = 30u * 1000u * 1000u;
    int pool = -1, ssl = -1, http = -1, tmpl = -1, conn = -1, req = -1;
    bool certs = false;
    Result<std::vector<std::uint8_t>> result = make_error(Errc::io, "download failed");
    auto fail = [&](const char* what, int rc) {
        result = make_error(Errc::io, std::string(what) + " failed: " + hex_code(rc) + " (" + url + ")");
    };
    do {
        if ((pool = sceNetPoolCreate("alembic_http", 32 * 1024, 0)) < 0) { fail("sceNetPoolCreate", pool); break; }
        if ((ssl = sceSslInit((256 + 16) * 1024)) < 0) { fail("sceSslInit", ssl); break; }
        SceSslData ca[] = {
            {g_ampr_ca_sectigo_e46_usertrust, sizeof g_ampr_ca_sectigo_e46_usertrust - 1},
            {g_ampr_ca_isrg_root_yr_by_x1, sizeof g_ampr_ca_isrg_root_yr_by_x1 - 1},
            {g_ampr_ca_usertrust_ecc, sizeof g_ampr_ca_usertrust_ecc - 1},
            {g_ampr_ca_isrg_root_x1, sizeof g_ampr_ca_isrg_root_x1 - 1},
        };
        const SceSslData* list[] = {&ca[0], &ca[1], &ca[2], &ca[3]};
        if (int rc = sceSslLoadCert(ssl, 4, list, nullptr, nullptr); rc != 0) { fail("sceSslLoadCert", rc); break; }
        certs = true;
        // Blocking HTTP/1.1 (version 2), as SMP does.
        if ((http = sceHttpInit(pool, ssl, 256 * 1024)) < 0) { fail("sceHttpInit", http); break; }
        if ((tmpl = sceHttpCreateTemplate(http, "Alembic/1.0", 2, 1)) < 0) { fail("sceHttpCreateTemplate", tmpl); break; }
        sceHttpSetAutoRedirect(tmpl, 1);
        sceHttpSetResponseHeaderMaxSize(tmpl, 32 * 1024); // GitHub redirects carry large headers
        sceHttpSetResolveTimeOut(tmpl, kTimeout);
        sceHttpSetConnectTimeOut(tmpl, kTimeout);
        sceHttpSetSendTimeOut(tmpl, kTimeout);
        sceHttpSetRecvTimeOut(tmpl, kTimeout);
        if ((conn = sceHttpCreateConnectionWithURL(tmpl, url.c_str(), 1)) < 0) { fail("sceHttpCreateConnectionWithURL", conn); break; }
        if ((req = sceHttpCreateRequestWithURL2(conn, "GET", url.c_str(), 0)) < 0) { fail("sceHttpCreateRequestWithURL2", req); break; }
        if (int rc = sceHttpSendRequest(req, nullptr, 0); rc != 0) { fail("sceHttpSendRequest", rc); break; }
        int status = 0;
        if (int rc = sceHttpGetStatusCode(req, &status); rc != 0) { fail("sceHttpGetStatusCode", rc); break; }
        if (status != 200) {
            result = make_error(Errc::io, "the server answered HTTP " + std::to_string(status) + " (" + url + ")");
            break;
        }
        std::vector<std::uint8_t> data;
        std::vector<std::uint8_t> buf(64 * 1024);
        bool ok = true;
        for (;;) {
            int n = sceHttpReadData(req, buf.data(), buf.size());
            if (n < 0) { fail("sceHttpReadData", n); ok = false; break; }
            if (n == 0) break;
            if (data.size() + static_cast<std::size_t>(n) > max_bytes) {
                result = make_error(Errc::too_large, "the download is larger than expected");
                ok = false;
                break;
            }
            data.insert(data.end(), buf.begin(), buf.begin() + n);
        }
        if (ok) result = std::move(data);
    } while (false);
    if (req >= 0) sceHttpDeleteRequest(req);
    if (conn >= 0) sceHttpDeleteConnection(conn);
    if (tmpl >= 0) sceHttpDeleteTemplate(tmpl);
    if (http >= 0) sceHttpTerm(http);
    if (certs) sceSslUnloadCert(ssl);
    if (ssl >= 0) sceSslTerm(ssl);
    if (pool >= 0) sceNetPoolDestroy(pool);
    return result;
}

Status install_package(const std::string& path) {
    if (!sceAppInstUtilInitialize || !sceAppInstUtilInstallByPackage)
        return make_error(Errc::unsupported, "the system installer is not available");
    static std::once_flag once;
    static int init = 0;
    std::call_once(once, [] { init = sceAppInstUtilInitialize(); });
    if (init) {
        char code[16];
        std::snprintf(code, sizeof code, "0x%08x", static_cast<unsigned>(init));
        return make_error(Errc::io, std::string("sceAppInstUtilInitialize failed: ") + code);
    }
    PkgMetadata meta{};
    std::string name = path.substr(path.rfind('/') + 1);
    meta.uri = path.c_str();
    meta.ex_uri = meta.playgo_scenario_id = meta.content_id = meta.icon_url = "";
    meta.content_name = name.c_str();
    PkgInfo info{};
    static PlaygoInfo playgo; // ~6.8 KB; one install at a time (the queue runs jobs in order)
    playgo = PlaygoInfo{};
    int rc = sceAppInstUtilInstallByPackage(&meta, &info, &playgo);
    if (rc) {
        char code[16];
        std::snprintf(code, sizeof code, "0x%08x", static_cast<unsigned>(rc));
        return make_error(Errc::io, std::string("the system installer refused the package: ") + code);
    }
    return {};
}

std::string default_state_dir() { return "/data/alembic"; }

} // namespace alembic::engine::platform
