// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 payload side. Notifications follow ps5-payload-dev/sdk's samples/notify
// (GPL-3.0-or-later); keeping the console awake follows PS5-LZ4-Compression's
// src/lzc_power_guard.c (GPL-3.0-or-later).
#include "platform.hpp"

#include <algorithm>
#include "json_value.hpp"

#include <chrono>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <ps5/kernel.h>

// sceAppInstUtilInstallByPackage, as in ps5-payload-dev/shsrv's
// bundles/pkg_install (GPL-3.0-or-later).
// Layouts checked against ps5-web-file-manager's src/pkg_installer.c
// (GPL-3.0-or-later), which asserts the same sizes: the system writes the
// whole PlaygoInfo, so a short one overflows.
struct PkgMetadata {
    const char* uri;
    const char* ex_uri;
    const char* playgo_scenario_id;
    const char* content_id;
    const char* content_name;
    const char* icon_url;
    std::uint32_t slot;
    std::uint32_t is_playgo_enabled;
};
static_assert(sizeof(PkgMetadata) == 0x38, "sceAppInstUtil metadata ABI");
struct PkgInfo {
    char content_id[48];
    int type;
    int platform;
};
struct PlaygoInfo {
    char languages[30][8];
    char scenario_ids[64][3];
    char content_ids[64][48];
    long unknown[810];
};
static_assert(sizeof(PlaygoInfo) == 0x2700, "sceAppInstUtil PlayGoInfo ABI");

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
int sceAppInstUtilAppInstallTitleDir(const char* title_id, const char* dir, void*) __attribute__((weak));
int sceAppInstUtilAppUnInstall(const char* title_id) __attribute__((weak));
int sceNotificationSend(int user_id, bool is_logged, const char* payload) __attribute__((weak));
int sceSystemServicePowerTick(void) __attribute__((weak));
int sceSystemServiceParamGetInt(int param_id, int* value) __attribute__((weak));
int sceUserServiceInitialize(const int* priority) __attribute__((weak));
}

namespace alembic::engine::platform {

namespace {
constexpr int kSystemUser = 0xFE; // SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM
}

const char* name() { return "ps5"; }

std::string os_description() { return "PS5"; }

void pause_before_exit() {}

// The loader names a payload with thr_set_name on its main thread
// (ps5-payload-dev/elfldr, elfldr_set_procname); process lists show that
// thread's name. Called from the main thread, so -1 (this thread) is it.
void set_process_name(const char* name) { syscall(SYS_thr_set_name, -1, name); }



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

namespace {



// The system installer's functions. The loader may leave the weak imports
// empty (seen on a console: the shortcut never ran), so they are also looked
// up at run time, loading the library first when needed, as
// PS5-LZ4-Compression's src/lzc_app_installer.c (GPL-3.0-or-later) does.
struct AppInstApi {
    int (*initialize)(void) = nullptr;
    int (*install_by_package)(const PkgMetadata*, PkgInfo*, PlaygoInfo*) = nullptr;
    int (*install_title_dir)(const char*, const char*, void*) = nullptr;
    int (*install_all)(void*) = nullptr;
    int (*uninstall)(const char*) = nullptr;
    int (*user_service_initialize)(const int*) = nullptr;
    std::string note; // how they were found, or why not
};

extern "C" int sceKernelLoadStartModule(const char*, std::size_t, const void*, std::uint32_t, void*, int*);

bool module_handle(const char* name, const char* path, std::uint32_t& handle, std::string& note) {
    if (kernel_dynlib_handle(-1, name, &handle) == 0) return true;
    int rc = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
    if (rc > 0 && kernel_dynlib_handle(-1, name, &handle) == 0) {
        note += std::string(name) + " loaded; ";
        return true;
    }
    note += std::string("cannot load ") + path + " (" + hex_code(rc) + "); ";
    return false;
}

template <typename F>
void pick(F& out, F linked, std::uint32_t handle, bool have, const char* symbol, const char* nid) {
    if (linked) { out = linked; return; }
    if (!have) return;
    intptr_t a = kernel_dynlib_dlsym(-1, handle, symbol);
    if (!a && nid) a = kernel_dynlib_resolve(-1, handle, nid);
    out = reinterpret_cast<F>(a);
}

const AppInstApi& appinst() {
    static AppInstApi api;
    static std::once_flag once;
    std::call_once(once, [] {
        std::uint32_t h = 0, uh = 0;
        bool have = module_handle("libSceAppInstUtil.sprx", "/system/common/lib/libSceAppInstUtil.sprx", h, api.note);
        bool uhave = module_handle("libSceUserService.sprx", "/system/common/lib/libSceUserService.sprx", uh, api.note);
        bool linked = sceAppInstUtilAppInstallTitleDir != nullptr;
        pick(api.initialize, sceAppInstUtilInitialize ? &sceAppInstUtilInitialize : nullptr, h, have, "sceAppInstUtilInitialize", nullptr);
        pick(api.install_by_package, sceAppInstUtilInstallByPackage ? &sceAppInstUtilInstallByPackage : nullptr, h, have, "sceAppInstUtilInstallByPackage", nullptr);
        pick(api.install_title_dir, sceAppInstUtilAppInstallTitleDir ? &sceAppInstUtilAppInstallTitleDir : nullptr, h, have, "sceAppInstUtilAppInstallTitleDir", "Wudg3Xe3heE");
        pick(api.install_all, decltype(api.install_all)(nullptr), h, have, "sceAppInstUtilAppInstallAll", nullptr);
        pick(api.uninstall, sceAppInstUtilAppUnInstall ? &sceAppInstUtilAppUnInstall : nullptr, h, have, "sceAppInstUtilAppUnInstall", nullptr);
        pick(api.user_service_initialize, sceUserServiceInitialize ? &sceUserServiceInitialize : nullptr, uh, uhave, "sceUserServiceInitialize", nullptr);
        api.note += linked ? "imports linked" : "imports looked up at run time";
        const std::pair<const char*, bool> found[] = {
            {"Initialize", api.initialize != nullptr}, {"AppInstallTitleDir", api.install_title_dir != nullptr},
            {"AppInstallAll", api.install_all != nullptr}, {"AppUnInstall", api.uninstall != nullptr},
            {"InstallByPackage", api.install_by_package != nullptr}};
        std::string missing;
        for (const auto& f : found)
            if (!f.second) missing += std::string(missing.empty() ? "" : ", ") + f.first;
        if (!missing.empty()) api.note += "; missing: " + missing;
    });
    return api;
}

int appinst_init() {
    static std::once_flag once;
    static int rc = 0;
    std::call_once(once, [] {
        // websrv, ps5-web-file-manager and Payload Manager all start the user
        // service before registering their shortcut.
        const AppInstApi& api = appinst();
        if (api.user_service_initialize) {
            int priority = 256;
            api.user_service_initialize(&priority);
        }
        rc = api.initialize ? api.initialize() : -1;
    });
    return rc;
}

} // namespace

Status install_package(const std::string& path, const std::string& content_id, const std::string& title) {
    if (!appinst().initialize || !appinst().install_by_package)
        return make_error(Errc::unsupported, "the system installer is not available");
    int init = appinst_init();
    if (init) {
        char code[16];
        std::snprintf(code, sizeof code, "0x%08x", static_cast<unsigned>(init));
        return make_error(Errc::io, std::string("sceAppInstUtilInitialize failed: ") + code);
    }
    PkgMetadata meta{};
    std::string name = title.empty() ? path.substr(path.rfind('/') + 1) : title;
    // The installer runs in another process that sees the internal drive as
    // /user/data, not /data: ps5-web-file-manager's src/pkg_installer.c
    // (GPL-3.0-or-later) passes it that way. (Given /data/..., it refused a
    // package with 0x80b2150f; whether this is why is not confirmed yet.)
    std::string uri = path.rfind("/data/", 0) == 0 ? "/user" + path : path;
    meta.uri = uri.c_str();
    meta.ex_uri = meta.playgo_scenario_id = meta.icon_url = "";
    meta.content_id = content_id.c_str();
    meta.content_name = name.c_str();
    PkgInfo info{};
    static PlaygoInfo playgo; // ~10 KB; one install at a time (the queue runs jobs in order)
    playgo = PlaygoInfo{};
    int rc = appinst().install_by_package(&meta, &info, &playgo);
    if (rc) {
        char code[16];
        std::snprintf(code, sizeof code, "0x%08x", static_cast<unsigned>(rc));
        return make_error(Errc::io, std::string("the system installer refused the package: ") + code);
    }
    return {};
}

// SCE_SYSTEM_SERVICE_PARAM_ID_LANG (1), as in the public PS4 headers.
std::string system_language() {
    int value = -1;
    if (!sceSystemServiceParamGetInt || sceSystemServiceParamGetInt(1, &value) != 0) return "en";
    switch (value) {
    case 0: return "ja";
    case 2: return "fr";
    case 3: case 20: return "es"; // Spain, Latin America
    case 4: return "de";
    case 19: return "tr";
    default: return "en";
    }
}

// SCE_SYSTEM_SERVICE_PARAM_ID_ENTER_BUTTON_ASSIGN: 0 is ○, 1 is ✕.
std::string enter_button() {
    int value = -1;
    if (!sceSystemServiceParamGetInt || sceSystemServiceParamGetInt(1000, &value) != 0) return "";
    return value == 0 ? "circle" : value == 1 ? "cross" : "";
}

std::string default_state_dir() { return "/data/alembic"; }

// The Media-tab shortcut, as ps5-payload-dev/websrv (src/ps5/sys.c) and
// ps5-web-file-manager (src/app_installer.c) install theirs (both
// GPL-3.0-or-later): a title folder in /user/app with only sce_sys/param.json
// and icon0.png, registered with sceAppInstUtilAppInstallTitleDir.
// applicationCategoryType 65536 ("native media app") puts it in Media instead
// of Games; deeplinkUri makes the console open that URL in its browser.
namespace {

const std::string kAppDir = std::string("/user/app/") + kLauncherTitleId;
const std::string kMetaParam = std::string("/user/appmeta/") + kLauncherTitleId + "/param.json";

std::string launcher_param(std::uint16_t port) {
    return std::string("{\n  \"applicationCategoryType\": 65536,\n  \"titleId\": \"") + kLauncherTitleId +
           "\",\n  \"deeplinkUri\": \"http://127.0.0.1:" + std::to_string(port) +
           "/\",\n  \"localizedParameters\": {\n    \"defaultLanguage\": \"en-US\",\n"
           "    \"en-US\": {\n      \"titleName\": \"Alembic\"\n    }\n  }\n}\n";
}

bool read_small_file(const std::string& path, std::string& out, std::size_t max = 1 << 20) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    std::size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && out.size() <= max) out.append(buf, n);
    std::fclose(f);
    return true;
}

bool write_whole_file(const std::string& path, const void* data, std::size_t size) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data, 1, size, f) == size;
    return std::fclose(f) == 0 && ok;
}


std::mutex g_launcher_mutex;

// websrv sets its auth ID to this one before registering its shortcut
// (src/ps5/sys.c). Here only for the call, then the old one is restored, as
// ShadowMountPlus's src/sm_shellcore_remote.c does around its own calls.
constexpr std::uint64_t kShellCoreAuthId = 0x4801000000000013ULL;
class AuthIdScope {
public:
    AuthIdScope() : saved_(kernel_get_ucred_authid(-1)) { changed_ = kernel_set_ucred_authid(-1, kShellCoreAuthId) == 0; }
    ~AuthIdScope() { if (changed_) kernel_set_ucred_authid(-1, saved_); }
    std::uint64_t saved() const { return saved_; }
    bool changed() const { return changed_; }

private:
    std::uint64_t saved_;
    bool changed_ = false;
};

std::string hex64(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%016llx", static_cast<unsigned long long>(v));
    return b;
}

} // namespace

// websrv, ps5-web-file-manager and Payload Manager register their shortcut
// with AppInstallTitleDir and, where a firmware lacks it, AppInstallAll (which
// registers every title folder in /user/app).
bool launcher_supported() { return appinst().initialize && (appinst().install_title_dir || appinst().install_all); }
std::string launcher_problem() { return appinst().note; }

bool launcher_installed() {
    struct stat st{};
    return ::stat((kAppDir + "/sce_sys/param.json").c_str(), &st) == 0;
}

bool launcher_recorded() {
    struct stat st{};
    return ::stat(kMetaParam.c_str(), &st) == 0;
}

bool launcher_current(std::uint16_t port, const std::uint8_t* icon_png, std::size_t icon_size) {
    std::string param, icon;
    return read_small_file(kAppDir + "/sce_sys/param.json", param) && param == launcher_param(port) &&
           read_small_file(kAppDir + "/sce_sys/icon0.png", icon) &&
           icon == std::string(reinterpret_cast<const char*>(icon_png), icon_size);
}

std::string launcher_record_note() {
    std::string dir = std::string("/user/appmeta/") + kLauncherTitleId;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return "no " + dir + " (" + std::strerror(errno) + ")";
    std::string names;
    int n = 0;
    while (dirent* e = ::readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        if (n++ < 8) names += (names.empty() ? "" : ", ") + std::string(e->d_name);
    }
    ::closedir(d);
    return dir + " holds " + (n ? names + (n > 8 ? ", ..." : "") : std::string("nothing"));
}

Status install_launcher(std::uint16_t port, const std::uint8_t* icon_png, std::size_t icon_size, bool again) {
    std::lock_guard<std::mutex> lock(g_launcher_mutex);
    if (!launcher_supported()) return make_error(Errc::unsupported, "the system installer is not available");
    // /user/appmeta/<id> is no sign of success: some firmwares never write it.
    if (!again && launcher_current(port, icon_png, icon_size)) return {};
    std::string param = launcher_param(port);
    std::string param_path = kAppDir + "/sce_sys/param.json";
    if (int rc = appinst_init(); rc != 0) return make_error(Errc::io, "sceAppInstUtilInitialize failed: " + hex_code(rc));
    // Registering again: drop the old registration first, as
    // PS5-LZ4-Compression's src/lzc_app_installer.c does.
    if (again && (launcher_recorded() || launcher_installed()) && appinst().uninstall) {
        AuthIdScope auth;
        appinst().uninstall(kLauncherTitleId);
    }
    for (const std::string& d : {std::string("/user/app"), kAppDir, kAppDir + "/sce_sys"})
        if (::mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) return make_error(Errc::io, "cannot create " + d);
    if (!write_whole_file(param_path, param.data(), param.size()) ||
        !write_whole_file(kAppDir + "/sce_sys/icon0.png", icon_png, icon_size))
        return make_error(Errc::io, "cannot write the shortcut's files in " + kAppDir);
    AuthIdScope auth;
    const char* call = appinst().install_title_dir ? "sceAppInstUtilAppInstallTitleDir" : "sceAppInstUtilAppInstallAll";
    int rc = appinst().install_title_dir ? appinst().install_title_dir(kLauncherTitleId, "/user/app/", nullptr)
                                         : appinst().install_all(nullptr);
    std::string how = " (auth ID " + hex64(auth.saved()) + (auth.changed() ? ", raised for the call)" : ", could not be raised)");
    if (rc != 0) {
        ::unlink(param_path.c_str()); // so the next start tries again
        return make_error(Errc::io, std::string(call) + " failed: " + hex_code(rc) + how);
    }
    // The system may write its copy a moment later, or (on some firmwares)
    // elsewhere: the shortcut can show in Media without it, so this is no error.
    for (int i = 0; i < 50 && !launcher_recorded(); ++i) ::usleep(100 * 1000);
    return {};
}

Status remove_launcher() {
    std::lock_guard<std::mutex> lock(g_launcher_mutex);
    if (!appinst().uninstall) return make_error(Errc::unsupported, "the system installer is not available");
    if (int rc = appinst_init(); rc != 0) return make_error(Errc::io, "sceAppInstUtilInitialize failed: " + hex_code(rc));
    AuthIdScope auth;
    int rc = launcher_recorded() || launcher_installed() ? appinst().uninstall(kLauncherTitleId) : 0;
    for (const char* f : {"/sce_sys/param.json", "/sce_sys/icon0.png"}) ::unlink((kAppDir + f).c_str());
    ::rmdir((kAppDir + "/sce_sys").c_str());
    ::rmdir(kAppDir.c_str());
    if (rc != 0) return make_error(Errc::io, "sceAppInstUtilAppUnInstall failed: " + hex_code(rc));
    return {};
}

// The system's random generator (libSceRandom), found like the installer
// above: the weak import, else the module loaded now; failing both, libc's
// arc4random_buf (the kernel's generator).
extern "C" int sceRandomGetRandomNumber(void* buffer, std::size_t size) __attribute__((weak));
extern "C" void arc4random_buf(void* buffer, std::size_t size);

Status secure_random(std::uint8_t* out, std::size_t size) {
    using Fn = int (*)(void*, std::size_t);
    static Fn fn = [] {
        Fn f = sceRandomGetRandomNumber ? &sceRandomGetRandomNumber : nullptr;
        std::uint32_t handle = 0;
        std::string note;
        if (!f && module_handle("libSceRandom.sprx", "/system/common/lib/libSceRandom.sprx", handle, note))
            f = reinterpret_cast<Fn>(kernel_dynlib_dlsym(-1, handle, "sceRandomGetRandomNumber"));
        return f;
    }();
    if (!fn) {
        arc4random_buf(out, size);
        return {};
    }
    for (std::size_t done = 0; done < size;) {
        std::size_t n = std::min<std::size_t>(size - done, 64); // the most it gives per call
        if (fn(out + done, n) != 0) return make_error(Errc::io, "the system's random generator failed");
        done += n;
    }
    return {};
}

} // namespace alembic::engine::platform
