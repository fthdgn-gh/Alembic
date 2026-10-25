// SPDX-License-Identifier: GPL-3.0-or-later
// Downloads over HTTPS with the console's own libraries (libSceHttp, libSceSsl)
// and the GitHub CA certificates in github_ca.inc. Used by the engine payload
// (updates, components) and by the console app (its engine, when it carries none).
#include "https_ps5.hpp"

#include <cstdio>
#include <mutex>

// HTTPS through the system libraries, following drakmor/ShadowMountPlus's
// src/sm_ampr_updater.c (GPL-3.0).
struct SceSslData {
    char* ptr;
    std::size_t size;
};

// The payload links these weakly (its loader fills them, or leaves them
// empty); the app (ALEMBIC_HTTPS_DYNAMIC) does not import them at all, so a
// title that cannot load them still starts: both can load them when needed.
#ifndef ALEMBIC_HTTPS_DYNAMIC
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
#endif

extern "C" {
int sceKernelLoadStartModule(const char* path, std::size_t args, const void* argp, std::uint32_t flags, void* opt, int* res);
int sceKernelDlsym(int handle, const char* symbol, void** address);
}

#include "github_ca.inc"

namespace alembic::engine {

namespace {

std::string hex_code(int rc) {
    char code[16];
    std::snprintf(code, sizeof code, "0x%08x", static_cast<unsigned>(rc));
    return code;
}

// The functions, linked or looked up at run time.
struct Api {
    using sceNetInit_t = int (*)(void);
    sceNetInit_t sceNetInit = nullptr;
    using sceNetPoolCreate_t = int (*)(const char*, int, int);
    sceNetPoolCreate_t sceNetPoolCreate = nullptr;
    using sceNetPoolDestroy_t = int (*)(int);
    sceNetPoolDestroy_t sceNetPoolDestroy = nullptr;
    using sceSslInit_t = int (*)(std::size_t);
    sceSslInit_t sceSslInit = nullptr;
    using sceSslTerm_t = int (*)(int);
    sceSslTerm_t sceSslTerm = nullptr;
    using sceSslLoadCert_t = int (*)(int, int, const SceSslData**, const SceSslData*, const SceSslData*);
    sceSslLoadCert_t sceSslLoadCert = nullptr;
    using sceSslUnloadCert_t = int (*)(int);
    sceSslUnloadCert_t sceSslUnloadCert = nullptr;
    using sceHttpInit_t = int (*)(int, int, std::size_t);
    sceHttpInit_t sceHttpInit = nullptr;
    using sceHttpTerm_t = int (*)(int);
    sceHttpTerm_t sceHttpTerm = nullptr;
    using sceHttpCreateTemplate_t = int (*)(int, const char*, int, int);
    sceHttpCreateTemplate_t sceHttpCreateTemplate = nullptr;
    using sceHttpDeleteTemplate_t = int (*)(int);
    sceHttpDeleteTemplate_t sceHttpDeleteTemplate = nullptr;
    using sceHttpCreateConnectionWithURL_t = int (*)(int, const char*, int);
    sceHttpCreateConnectionWithURL_t sceHttpCreateConnectionWithURL = nullptr;
    using sceHttpDeleteConnection_t = int (*)(int);
    sceHttpDeleteConnection_t sceHttpDeleteConnection = nullptr;
    using sceHttpCreateRequestWithURL2_t = int (*)(int, const char*, const char*, std::uint64_t);
    sceHttpCreateRequestWithURL2_t sceHttpCreateRequestWithURL2 = nullptr;
    using sceHttpDeleteRequest_t = int (*)(int);
    sceHttpDeleteRequest_t sceHttpDeleteRequest = nullptr;
    using sceHttpSendRequest_t = int (*)(int, const void*, std::size_t);
    sceHttpSendRequest_t sceHttpSendRequest = nullptr;
    using sceHttpGetStatusCode_t = int (*)(int, int*);
    sceHttpGetStatusCode_t sceHttpGetStatusCode = nullptr;
    using sceHttpReadData_t = int (*)(int, void*, std::size_t);
    sceHttpReadData_t sceHttpReadData = nullptr;
    using sceHttpSetAutoRedirect_t = int (*)(int, int);
    sceHttpSetAutoRedirect_t sceHttpSetAutoRedirect = nullptr;
    using sceHttpSetResponseHeaderMaxSize_t = int (*)(int, std::size_t);
    sceHttpSetResponseHeaderMaxSize_t sceHttpSetResponseHeaderMaxSize = nullptr;
    using sceHttpSetResolveTimeOut_t = int (*)(int, std::uint32_t);
    sceHttpSetResolveTimeOut_t sceHttpSetResolveTimeOut = nullptr;
    using sceHttpSetConnectTimeOut_t = int (*)(int, std::uint32_t);
    sceHttpSetConnectTimeOut_t sceHttpSetConnectTimeOut = nullptr;
    using sceHttpSetSendTimeOut_t = int (*)(int, std::uint32_t);
    sceHttpSetSendTimeOut_t sceHttpSetSendTimeOut = nullptr;
    using sceHttpSetRecvTimeOut_t = int (*)(int, std::uint32_t);
    sceHttpSetRecvTimeOut_t sceHttpSetRecvTimeOut = nullptr;
};

// Linked by the payload's loader when it filled them; else the system
// modules are loaded and their functions looked up (once).
const Api* api(std::string* why) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    static Api a;
    static bool tried = false, ok = false;
    static std::string note;
    if (tried) {
        if (!ok) *why = note;
        return ok ? &a : nullptr;
    }
    tried = true;
#ifndef ALEMBIC_HTTPS_DYNAMIC
    if (::sceHttpInit && ::sceSslInit && ::sceNetPoolCreate) {
    a.sceNetInit = ::sceNetInit;
    a.sceNetPoolCreate = ::sceNetPoolCreate;
    a.sceNetPoolDestroy = ::sceNetPoolDestroy;
    a.sceSslInit = ::sceSslInit;
    a.sceSslTerm = ::sceSslTerm;
    a.sceSslLoadCert = ::sceSslLoadCert;
    a.sceSslUnloadCert = ::sceSslUnloadCert;
    a.sceHttpInit = ::sceHttpInit;
    a.sceHttpTerm = ::sceHttpTerm;
    a.sceHttpCreateTemplate = ::sceHttpCreateTemplate;
    a.sceHttpDeleteTemplate = ::sceHttpDeleteTemplate;
    a.sceHttpCreateConnectionWithURL = ::sceHttpCreateConnectionWithURL;
    a.sceHttpDeleteConnection = ::sceHttpDeleteConnection;
    a.sceHttpCreateRequestWithURL2 = ::sceHttpCreateRequestWithURL2;
    a.sceHttpDeleteRequest = ::sceHttpDeleteRequest;
    a.sceHttpSendRequest = ::sceHttpSendRequest;
    a.sceHttpGetStatusCode = ::sceHttpGetStatusCode;
    a.sceHttpReadData = ::sceHttpReadData;
    a.sceHttpSetAutoRedirect = ::sceHttpSetAutoRedirect;
    a.sceHttpSetResponseHeaderMaxSize = ::sceHttpSetResponseHeaderMaxSize;
    a.sceHttpSetResolveTimeOut = ::sceHttpSetResolveTimeOut;
    a.sceHttpSetConnectTimeOut = ::sceHttpSetConnectTimeOut;
    a.sceHttpSetSendTimeOut = ::sceHttpSetSendTimeOut;
    a.sceHttpSetRecvTimeOut = ::sceHttpSetRecvTimeOut;
        ok = true;
        return &a;
    }
#endif
    auto load = [&](const char* name) {
        const std::string path = std::string("/system/common/lib/") + name;
        int h = sceKernelLoadStartModule(path.c_str(), 0, nullptr, 0, nullptr, nullptr);
        if (h < 0) note += std::string(note.empty() ? "" : "; ") + "cannot load " + name + " (" + hex_code(h) + ")";
        return h;
    };
    const int net = load("libSceNet.sprx"), ssl = load("libSceSsl.sprx"), http = load("libSceHttp.sprx");
    bool found = net >= 0 && ssl >= 0 && http >= 0;
    if (found) {
        found = sceKernelDlsym(net, "sceNetInit", reinterpret_cast<void**>(&a.sceNetInit)) == 0 && a.sceNetInit && found;
        found = sceKernelDlsym(net, "sceNetPoolCreate", reinterpret_cast<void**>(&a.sceNetPoolCreate)) == 0 && a.sceNetPoolCreate && found;
        found = sceKernelDlsym(net, "sceNetPoolDestroy", reinterpret_cast<void**>(&a.sceNetPoolDestroy)) == 0 && a.sceNetPoolDestroy && found;
        found = sceKernelDlsym(ssl, "sceSslInit", reinterpret_cast<void**>(&a.sceSslInit)) == 0 && a.sceSslInit && found;
        found = sceKernelDlsym(ssl, "sceSslTerm", reinterpret_cast<void**>(&a.sceSslTerm)) == 0 && a.sceSslTerm && found;
        found = sceKernelDlsym(ssl, "sceSslLoadCert", reinterpret_cast<void**>(&a.sceSslLoadCert)) == 0 && a.sceSslLoadCert && found;
        found = sceKernelDlsym(ssl, "sceSslUnloadCert", reinterpret_cast<void**>(&a.sceSslUnloadCert)) == 0 && a.sceSslUnloadCert && found;
        found = sceKernelDlsym(http, "sceHttpInit", reinterpret_cast<void**>(&a.sceHttpInit)) == 0 && a.sceHttpInit && found;
        found = sceKernelDlsym(http, "sceHttpTerm", reinterpret_cast<void**>(&a.sceHttpTerm)) == 0 && a.sceHttpTerm && found;
        found = sceKernelDlsym(http, "sceHttpCreateTemplate", reinterpret_cast<void**>(&a.sceHttpCreateTemplate)) == 0 && a.sceHttpCreateTemplate && found;
        found = sceKernelDlsym(http, "sceHttpDeleteTemplate", reinterpret_cast<void**>(&a.sceHttpDeleteTemplate)) == 0 && a.sceHttpDeleteTemplate && found;
        found = sceKernelDlsym(http, "sceHttpCreateConnectionWithURL", reinterpret_cast<void**>(&a.sceHttpCreateConnectionWithURL)) == 0 && a.sceHttpCreateConnectionWithURL && found;
        found = sceKernelDlsym(http, "sceHttpDeleteConnection", reinterpret_cast<void**>(&a.sceHttpDeleteConnection)) == 0 && a.sceHttpDeleteConnection && found;
        found = sceKernelDlsym(http, "sceHttpCreateRequestWithURL2", reinterpret_cast<void**>(&a.sceHttpCreateRequestWithURL2)) == 0 && a.sceHttpCreateRequestWithURL2 && found;
        found = sceKernelDlsym(http, "sceHttpDeleteRequest", reinterpret_cast<void**>(&a.sceHttpDeleteRequest)) == 0 && a.sceHttpDeleteRequest && found;
        found = sceKernelDlsym(http, "sceHttpSendRequest", reinterpret_cast<void**>(&a.sceHttpSendRequest)) == 0 && a.sceHttpSendRequest && found;
        found = sceKernelDlsym(http, "sceHttpGetStatusCode", reinterpret_cast<void**>(&a.sceHttpGetStatusCode)) == 0 && a.sceHttpGetStatusCode && found;
        found = sceKernelDlsym(http, "sceHttpReadData", reinterpret_cast<void**>(&a.sceHttpReadData)) == 0 && a.sceHttpReadData && found;
        found = sceKernelDlsym(http, "sceHttpSetAutoRedirect", reinterpret_cast<void**>(&a.sceHttpSetAutoRedirect)) == 0 && a.sceHttpSetAutoRedirect && found;
        found = sceKernelDlsym(http, "sceHttpSetResponseHeaderMaxSize", reinterpret_cast<void**>(&a.sceHttpSetResponseHeaderMaxSize)) == 0 && a.sceHttpSetResponseHeaderMaxSize && found;
        found = sceKernelDlsym(http, "sceHttpSetResolveTimeOut", reinterpret_cast<void**>(&a.sceHttpSetResolveTimeOut)) == 0 && a.sceHttpSetResolveTimeOut && found;
        found = sceKernelDlsym(http, "sceHttpSetConnectTimeOut", reinterpret_cast<void**>(&a.sceHttpSetConnectTimeOut)) == 0 && a.sceHttpSetConnectTimeOut && found;
        found = sceKernelDlsym(http, "sceHttpSetSendTimeOut", reinterpret_cast<void**>(&a.sceHttpSetSendTimeOut)) == 0 && a.sceHttpSetSendTimeOut && found;
        found = sceKernelDlsym(http, "sceHttpSetRecvTimeOut", reinterpret_cast<void**>(&a.sceHttpSetRecvTimeOut)) == 0 && a.sceHttpSetRecvTimeOut && found;
        if (!found) note = "the system HTTP library lacks a function it should have";
    }
    ok = found;
    if (!ok) *why = note.empty() ? "the system HTTP library is not available" : note;
    return ok ? &a : nullptr;
}

} // namespace

Result<std::vector<std::uint8_t>> https_get(const std::string& url, std::size_t max_bytes) {
    std::string why;
    const Api* f = api(&why);
    if (!f) return make_error(Errc::unsupported, why);
    // One download at a time keeps the pools small.
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    static bool net_ready = false;
    if (!net_ready) {
        int rc = f->sceNetInit ? f->sceNetInit() : 0;
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
        if ((pool = f->sceNetPoolCreate("alembic_http", 32 * 1024, 0)) < 0) { fail("sceNetPoolCreate", pool); break; }
        if ((ssl = f->sceSslInit((256 + 16) * 1024)) < 0) { fail("sceSslInit", ssl); break; }
        SceSslData ca[] = {
            {g_ampr_ca_sectigo_e46_usertrust, sizeof g_ampr_ca_sectigo_e46_usertrust - 1},
            {g_ampr_ca_isrg_root_yr_by_x1, sizeof g_ampr_ca_isrg_root_yr_by_x1 - 1},
            {g_ampr_ca_usertrust_ecc, sizeof g_ampr_ca_usertrust_ecc - 1},
            {g_ampr_ca_isrg_root_x1, sizeof g_ampr_ca_isrg_root_x1 - 1},
        };
        const SceSslData* list[] = {&ca[0], &ca[1], &ca[2], &ca[3]};
        if (int rc = f->sceSslLoadCert(ssl, 4, list, nullptr, nullptr); rc != 0) { fail("sceSslLoadCert", rc); break; }
        certs = true;
        // Blocking HTTP/1.1 (version 2), as SMP does.
        if ((http = f->sceHttpInit(pool, ssl, 256 * 1024)) < 0) { fail("sceHttpInit", http); break; }
        if ((tmpl = f->sceHttpCreateTemplate(http, "Alembic/1.0", 2, 1)) < 0) { fail("sceHttpCreateTemplate", tmpl); break; }
        f->sceHttpSetAutoRedirect(tmpl, 1);
        f->sceHttpSetResponseHeaderMaxSize(tmpl, 32 * 1024); // GitHub redirects carry large headers
        f->sceHttpSetResolveTimeOut(tmpl, kTimeout);
        f->sceHttpSetConnectTimeOut(tmpl, kTimeout);
        f->sceHttpSetSendTimeOut(tmpl, kTimeout);
        f->sceHttpSetRecvTimeOut(tmpl, kTimeout);
        if ((conn = f->sceHttpCreateConnectionWithURL(tmpl, url.c_str(), 1)) < 0) { fail("sceHttpCreateConnectionWithURL", conn); break; }
        if ((req = f->sceHttpCreateRequestWithURL2(conn, "GET", url.c_str(), 0)) < 0) { fail("sceHttpCreateRequestWithURL2", req); break; }
        if (int rc = f->sceHttpSendRequest(req, nullptr, 0); rc != 0) { fail("sceHttpSendRequest", rc); break; }
        int status = 0;
        if (int rc = f->sceHttpGetStatusCode(req, &status); rc != 0) { fail("sceHttpGetStatusCode", rc); break; }
        if (status != 200) {
            result = make_error(Errc::io, "the server answered HTTP " + std::to_string(status) + " (" + url + ")");
            break;
        }
        std::vector<std::uint8_t> data;
        std::vector<std::uint8_t> buf(64 * 1024);
        bool ok = true;
        for (;;) {
            int n = f->sceHttpReadData(req, buf.data(), buf.size());
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
    if (req >= 0) f->sceHttpDeleteRequest(req);
    if (conn >= 0) f->sceHttpDeleteConnection(conn);
    if (tmpl >= 0) f->sceHttpDeleteTemplate(tmpl);
    if (http >= 0) f->sceHttpTerm(http);
    if (certs) f->sceSslUnloadCert(ssl);
    if (ssl >= 0) f->sceSslTerm(ssl);
    if (pool >= 0) f->sceNetPoolDestroy(pool);
    return result;
}

} // namespace alembic::engine
