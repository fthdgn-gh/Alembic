// SPDX-License-Identifier: GPL-3.0-or-later
// The Alembic engine: a job queue for long conversions with an HTTP JSON API
// and a web page. On the PS5 it runs as a payload (alembic.elf) and keeps
// running after the app that queued a job is closed.
//
//   alembic-engine [--port N] [--listen ADDR] [--state DIR] [--root DIR]...
//                  [--runtime libSceAmpr.sprx] [--shadowmount HOST:PORT]
//                  [--loader HOST:PORT] [--update-url URL] [--no-update-check] [--take-over]
//
// --loader: the ELF loader an update goes to (the console: 127.0.0.1:9021;
// tests on a PC give one). --update-url: another "latest release" JSON
// (tests). --take-over: stop an engine already on the port first (always on
// the console).
//
// API (JSON):
//   GET  /api/info                    engine, folders, LZ4 runtime state
//   GET  /api/browse?path=P           folder listing (the roots for "/" or no path)
//   GET  /api/inspect?path=P          format, title ID, file count and size of an input
//   POST /api/plan                    a job's resolved target and output, not queued
//   GET  /api/jobs                    every job, oldest first
//   POST /api/jobs                    queue a job (JobSpec as JSON; kind convert, copy, move,
//                                     install, or ampr_emu: add ampr_emu to an existing game)
//   GET  /api/jobs/ID                 one job
//   POST /api/jobs/ID/cancel | /retry | /pause | /resume
//   POST /api/jobs/ID/move    {direction: -1 | 1}   run earlier or later
//   DELETE /api/jobs/ID               remove a finished job
//   POST /api/jobs/clear              remove every finished job
//   GET  /api/deps                    third-party files Alembic can download, and their state
//   POST /api/deps/ID/install | /remove
//   GET  /api/meta?path=P             title, title ID, version of a package, image or game folder
//   GET  /api/icon?path=P             its sce_sys/icon0.png
//   GET  /api/locations               suggested destinations (mounted drives only)
//   GET  /api/fs/space                free and total bytes of each root
//   POST /api/fs/mkdir   {path}       new folder
//   POST /api/fs/rename  {path, name} rename in place
//   POST /api/fs/delete  {path}       delete a file or a folder with its contents
//   POST /api/launcher   {enabled}    add or remove the Media-tab shortcut (PS5)
//   POST /api/cache/clear             delete the cached game info and icons
//   POST /api/lang       {lang}       the language of the engine's notifications
//   POST /api/smp/scan                ask ShadowMount+ to rescan (retry counters reset)
//   GET  /api/ftp                     FTP servers (no passwords); their files are under /ftp/<id>/
//   POST /api/ftp  {name, host, port, user, password}   add one (logs in first)
//   PUT  /api/ftp/ID  {name, host, port, user[, password]}   change one (keeps its id;
//                                     no password: keeps the saved one)
//   DELETE /api/ftp/ID                forget one
//   GET  /api/remote                  files browsers on other computers share
//   POST /api/remote  {name, size, last_modified}   share one: {path} to use as a job input
//   POST /api/remote/ID/want          (long poll) the pieces to send: [{offset, length}]
//   POST /api/remote/ID/data?offset=N one piece, as the raw body
//   POST /api/quit                    stop the engine (a running job with a checkpoint continues
//                                     at the next start)
//   POST /api/restart                 start this version again through the ELF loader (PS5; 409 when
//                                     no copy of its payload is found)
//   GET  /api/update                  this version, the latest release, payload files found
//   POST /api/update/check            ask GitHub again
//   POST /api/update/find {everywhere}  look for the payload files loaders start (PS5)
//   POST /api/update/app              replace only the console app's image (release = this version)
//   POST /api/app/closing             the console app is closing (its image may be swapped)
//   POST /api/update/install {paths, app}  download the latest payload (and the app's image unless
//                                     app is false), replace those files, start it
//                                     through the ELF loader and stop (PS5)
#include "deps.hpp"
#include "l10n.hpp"
#include "fs_ops.hpp"
#include "game_meta.hpp"
#include "http.hpp"
#include "jobs.hpp"
#include "json_value.hpp"
#include "keys.hpp"
#include "net.hpp"
#include "pairing.hpp"
#include "peer_fs.hpp"
#include "peers.hpp"
#include "places.hpp"
#include "platform.hpp"
#include "ftp.hpp"
#include "remote.hpp"
#include "runner.hpp"
#include "update.hpp"

#include "alembic/bytes.hpp"
#include "alembic/convert.hpp"
#include "alembic/crypto.hpp"
#include "alembic/ffpfsc.hpp"
#include "alembic/io.hpp"
#include "alembic/lz4pack.hpp"
#include "alembic/sha256.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <system_error>
#include <thread>

#ifndef ALEMBIC_VERSION
#define ALEMBIC_VERSION "dev"
#endif

// Read by the updater (update.hpp) in payload files to recognise Alembic and
// its version. Used below, so the linker keeps it.
extern "C" const char alembic_payload_version[] = "ALEMBIC_PAYLOAD_VERSION:" ALEMBIC_VERSION;

namespace alembic::engine {

// The web page (src/engine/web, built by its build.mjs), embedded by cmake/embed_text.cmake.
extern const char* const kIndexHtml;
extern const std::size_t kIndexHtmlSize;
// assets/icon/alembic.svg
extern const char* const kI18nJs;
extern const std::size_t kI18nJsSize;
extern const char* const kIconSvg;
extern const std::size_t kIconSvgSize;
// assets/icon/icon0.png, the home-screen shortcut's icon
extern const char* const kIcon0Png;
extern const std::size_t kIcon0PngSize;
// assets/icon/notify_icon.png: alembic.svg with its rounded corners, for notifications
extern const char* const kNotifyIconPng;
extern const std::size_t kNotifyIconPngSize;

namespace {

constexpr std::uint16_t kDefaultPort = 5940;

// This engine's version, as the marker the updater reads says it.
std::string version() { return alembic_payload_version + std::strlen("ALEMBIC_PAYLOAD_VERSION:"); }

std::mutex g_log_mutex;
std::string g_log_path;

void log_line(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::time_t t = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    std::fprintf(stderr, "%s %s\n", stamp, message.c_str());
    if (g_log_path.empty()) return;
    if (std::FILE* f = alembic::open_stdio(g_log_path, "a")) {
        std::fprintf(f, "%s %s\n", stamp, message.c_str());
        std::fclose(f);
    }
}

HttpResponse json(int status, const JsonValue& value) {
    HttpResponse r;
    r.status = status;
    r.body = value.dump();
    r.headers["Access-Control-Allow-Origin"] = "*";
    return r;
}

HttpResponse error(int status, const std::string& message) {
    JsonValue v = JsonValue::object();
    v.set("error", message);
    return json(status, v);
}

// Paths from the page and the command line: '/' between parts (Windows may
// send '\\'), no trailing '/' except for "/" and "C:/".
std::string strip_slashes(std::string p) { return without_trailing_slashes(portable_path(std::move(p))); }

std::string file_sha256(const std::string& path) {
    auto file = File::open(path, File::Mode::read);
    if (!file.ok()) return "";
    auto size = file->size();
    if (!size.ok() || *size > 64u * 1024 * 1024) return "";
    std::vector<std::uint8_t> data(static_cast<std::size_t>(*size));
    if (!file->read_at(0, data).ok()) return "";
    alembic::Sha256 hasher;
    hasher.update(data);
    return alembic::Sha256::hex(hasher.finish());
}

// What a file is, from its name, for the browser.
std::string kind_of(const std::string& name, bool dir) {
    if (dir) return "folder";
    std::string lower = name;
    for (char& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    auto ends = [&](const char* ext) {
        std::size_t n = std::char_traits<char>::length(ext);
        return lower.size() >= n && lower.compare(lower.size() - n, n, ext) == 0;
    };
    if (ends(".pkg")) return "fpkg";
    if (ends(".exfat")) return "exfat";
    if (ends(".ffpfsc")) return "ffpfsc";
    if (ends(".ffpkg")) return "ffpkg";
    if (ends(".ffpfs")) return "ffpfs";
    return "file";
}

class Engine {
public:
    explicit Engine(EngineConfig config)
        : config_(std::move(config)),
          queue_(config_.state_dir, QueueHooks{
                                        [this](const JobSpec& s, const ProgressFn& p) { return run_job(s, config_, p); },
                                        [this](const Job& j) { finished(j); },
                                        [] { platform::keep_awake(); },
                                        [](const std::string& m) { log_line(m); },
                                        [this](const JobSpec& s) { return checkpoint_info(s, config_).has_value(); },
                                        [this](const JobSpec& s) { discard_checkpoint(s, config_); },
                                    }) {
        // Inputs under /remote/ come from a browser on another computer.
        convert::set_file_provider([this](const std::string& path) -> std::shared_ptr<const RandomAccess> {
            if (RemoteFiles::is_remote(path)) return remote_.open(path);
            if (ftp::Servers::is_ftp(path)) return ftp_.open(path);
            return nullptr;
        }, [this](const std::string& path) {
            return RemoteFiles::is_remote(path) ? remote_.open(path) != nullptr : ftp::Servers::is_ftp(path);
        });
        // Outputs under /ftp/ are uploads.
        convert::OutputProvider out;
        out.handles = [](const std::string& path) { return ftp::Servers::is_ftp(path); };
        out.create = [this](const std::string& path, bool overwrite) { return ftp_.create(path, overwrite); };
        out.make_directory = [this](const std::string& path) -> Status {
            return ftp_.exists(path) ? Status{} : ftp_.make_directory(path);
        };
        out.exists = [this](const std::string& path) { return ftp_.exists(path); };
        convert::set_output_provider(std::move(out));
        config_.needed_by_other_jobs = [this](const std::string& path) {
            for (const Job& j : queue_.list())
                if ((j.state == JobState::queued || j.state == JobState::paused) && is_within(strip_slashes(j.spec.input), path))
                    return true;
            return false;
        };
    }

    Status start(const std::string& address, std::uint16_t port, std::uint16_t discovery_port = peers::kDiscoveryPort) {
        // Set before the queue runs jobs; it answers once pairing is up.
        config_.digest_elsewhere = [this](const std::string& path, const std::string& passcode,
                                          const ProgressFn& progress) -> Result<std::vector<convert::FileDigest>> {
            if (!discovery_ || !pairing_) return make_error(Errc::io, "the engine is starting");
            return peer_fs::digest_tree(*discovery_, *pairing_, path, passcode, progress);
        };
        ALEMBIC_TRY(queue_.start());
        listen_address_ = address;
        ALEMBIC_TRY(server_.start(address, port, [this](const HttpRequest& r) { return handle(r); }));
        // Other engines on the network (peers.hpp); it needs the port we got.
        peers::Identity self;
        self.id = id_;
        self.name = std::string(platform::name()) == "ps5" ? "PS5" : net::host_name();
        if (self.name.empty()) self.name = "Alembic";
        self.os = platform::os_description();
        self.platform = platform::name();
        self.version = version();
        self.port = server_.port();
        self.protocol = peers::kPeerProtocol;
        discovery_ = std::make_unique<peers::Discovery>(config_.state_dir, self, discovery_port,
                                                        [](const std::string& m) { log_line(m); });
        discovery_->start();
        pairing::Pairing::Hooks hooks;
        hooks.log = [](const std::string& m) { log_line(m); };
        hooks.notify = [](const std::string& m) { platform::notify(m); };
        hooks.remember = [this](const peers::Identity& who, const std::string& address) { discovery_->remember(who, address); };
        hooks.translate = [](const std::string& text, const std::map<std::string, std::string>& args) {
            return l10n::t(text, l10n::Vars(args.begin(), args.end()));
        };
        pairing_ = std::make_unique<pairing::Pairing>(config_.state_dir, self, std::move(hooks));
        // Paired engines' files: "/peer/<id>/..." (peer_fs.hpp), and a game's
        // digests worked out on the engine that has it.
        alembic::mount(peer_fs::kPrefix, peer_fs::make(*discovery_, *pairing_));
        // Updates from GitHub: checked once now, again from Settings.
        update::Updater::Options u;
        u.state_dir = config_.state_dir;
        u.current_version = version();
        if (!config_.update_url.empty()) u.release_url = config_.update_url;
        u.loader = config_.loader;
        u.get = [](const std::string& url, std::size_t max) { return platform::http_get(url, max); };
        u.log = [](const std::string& m) { log_line(m); };
        u.quit = [this] { request_quit(); };
        u.roots = [this] { return config_.available_roots(); };
        u.smp_source = [this](const std::string& id) { return shadowmount_source(config_, id); };
        u.smp_rescan = [this] { (void)shadowmount_rescan(config_); };
        // ShadowMount+'s default scan folders on the console; on a PC (tests
        // with --loader) <state>/homebrew stands in, as for backports.
        u.scan_dirs = [this] {
            if (std::string(platform::name()) != "ps5") return std::vector<std::string>{config_.state_dir + "/homebrew"};
            std::vector<std::string> dirs = {"/data/homebrew"};
            for (int i = 0; i < 8; ++i) dirs.push_back("/mnt/usb" + std::to_string(i) + "/homebrew");
            return dirs;
        };
        u.game_info = [this](const std::string& path, std::string* id, std::string* ver) {
            auto m = meta_.meta(path);
            if (!m.ok()) return false;
            *id = m->title_id;
            *ver = m->version;
            return true;
        };
        u.app_quiet = [this] { return app_quiet(); };
        updater_ = std::make_unique<update::Updater>(std::move(u));
        if (config_.check_updates) updater_->check();
        return {};
    }

    // Where the page opens: on this computer, and from other devices (empty
    // when it listens on this computer only).
    std::string local_url() const {
        std::string host = listen_address_ == "0.0.0.0" || listen_address_ == "127.0.0.1" ? "localhost" : listen_address_;
        return "http://" + host + ":" + std::to_string(port()) + "/";
    }
    std::vector<std::string> network_urls() const {
        std::vector<std::string> out;
        if (listen_address_ == "0.0.0.0") {
            for (const std::string& ip : net::local_addresses())
                if (std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
        } else if (listen_address_.rfind("127.", 0) != 0) {
            out.push_back(listen_address_);
        }
        for (std::string& ip : out) ip = "http://" + ip + ":" + std::to_string(port()) + "/";
        return out;
    }
    bool open_to_network() const { return listen_address_.rfind("127.", 0) != 0; }
    const EngineConfig& config() const { return config_; }

    void request_quit() {
        {
            std::lock_guard<std::mutex> lock(quit_mutex_);
            quit_ = true;
        }
        quit_cv_.notify_all();
    }

    void wait_for_quit() {
        std::unique_lock<std::mutex> lock(quit_mutex_);
        quit_cv_.wait(lock, [&] { return quit_; });
    }

    void stop() {
        alembic::unmount(peer_fs::kPrefix);
        if (discovery_) discovery_->stop();
        server_.stop();
        queue_.stop();
    }

    std::uint16_t port() const { return server_.port(); }

    // Trims the game info cache (unused for 30 days, or over 100 MB).
    void prune_cache() {
        std::uint64_t dropped = meta_.prune();
        if (dropped) log_line("game info cache: dropped " + std::to_string(dropped) + " old entries");
    }

    // The Media-tab shortcut, unless the user removed it (launcher.off).
    void start_launcher() {
        // Already in place: leave the system installer alone. Registering
        // again can re-register every app (AppInstallAll), which has crashed
        // a game that was starting at the time.
        if (!path_exists(launcher_off_path()) &&
            platform::launcher_current(port(), reinterpret_cast<const std::uint8_t*>(kIcon0Png), kIcon0PngSize)) {
            set_launcher_status("registered earlier");
            return;
        }
        if (!platform::launcher_supported()) {
            set_launcher_status("not available: " + platform::launcher_problem());
            if (std::string(platform::name()) == "ps5")
                platform::notify(l10n::t("Alembic: no Media shortcut: {why}", {{"why", l10n::message(platform::launcher_problem())}}));
            return;
        }
        if (path_exists(launcher_off_path())) {
            set_launcher_status("turned off in Settings");
            return;
        }
        set_launcher_status("registering…");
        std::thread([this] {
            Status s = platform::install_launcher(port(), reinterpret_cast<const std::uint8_t*>(kIcon0Png), kIcon0PngSize);
            launcher_result(s, "at start");
            platform::notify(s.ok() ? l10n::t("Alembic is in Media on the home screen")
                                    : l10n::t("Alembic: Media shortcut failed: {error}", {{"error", l10n::message(s.error().message)}}));
        }).detach();
    }

private:
    // Another engine's paths in a message ("/peer/<id>/data/x") as people
    // read them ("PS5: /data/x"), for notifications.
    std::string with_engine_names(std::string text) const {
        for (std::size_t at = 0; (at = text.find(peer_fs::kPrefix, at)) != std::string::npos;) {
            std::size_t id_end = at + 6 + 32;
            std::string id = text.substr(at + 6, 32);
            if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos) {
                at += 6;
                continue;
            }
            std::string name = "another engine";
            if (auto p = discovery_ ? discovery_->find(id) : std::nullopt) name = p->who.name;
            // "/C:/x" of a Windows engine reads "C:/x".
            bool drive = text.size() > id_end + 3 && text[id_end] == '/' && text[id_end + 2] == ':' &&
                         std::isalpha(static_cast<unsigned char>(text[id_end + 1]));
            std::string head = name + ": " + (id_end >= text.size() || text[id_end] != '/' ? "/" : "");
            text.replace(at, id_end - at + (drive ? 1 : 0), head);
            at += head.size();
        }
        return text;
    }

    void finished(const Job& job) {
        std::string name = job.spec.input.substr(job.spec.input.rfind('/') + 1);
        switch (job.state) {
        case JobState::done:
            platform::notify(l10n::t(job.spec.kind == "copy" ? "Copied {name}" : job.spec.kind == "move" ? "Moved {name}"
                                     : job.spec.kind == "install" ? "Installing {name}"
                                     : job.spec.kind == "ampr_emu" ? "Added the AMPR emulator to {name}" : "Converted {name}",
                                     {{"name", name}}));
            break;
        case JobState::failed:
            platform::notify(l10n::t("Failed: {name}: {error}", {{"name", name}, {"error", l10n::message(with_engine_names(job.error))}}));
            break;
        default: break;
        }
    }

    // Seconds since the console app (User-Agent "Alembic-app...") last asked
    // anything; since this engine started when it hasn't.
    double app_quiet() const {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(app_seen_mutex_);
        return std::chrono::duration<double>(now - app_seen_).count();
    }

    HttpResponse handle(const HttpRequest& r) {
        if (auto ua = r.headers.find("user-agent"); ua != r.headers.end() && ua->second.rfind("Alembic-app", 0) == 0) {
            std::lock_guard<std::mutex> lock(app_seen_mutex_);
            // Closing: count as quiet a few seconds from now, once it has gone.
            app_seen_ = r.path == "/api/app/closing" ? std::chrono::steady_clock::now() - std::chrono::seconds(12)
                                                     : std::chrono::steady_clock::now();
        }
        if (r.method == "OPTIONS") {
            HttpResponse resp = json(204, JsonValue());
            resp.body.clear();
            resp.headers["Access-Control-Allow-Methods"] = "GET, POST, DELETE, OPTIONS";
            resp.headers["Access-Control-Allow-Headers"] = "Content-Type";
            return resp;
        }
        if (r.path == "/" || r.path == "/index.html") {
            HttpResponse resp;
            resp.content_type = "text/html; charset=utf-8";
            resp.body.assign(kIndexHtml, kIndexHtmlSize);
            return resp;
        }
        if (r.path == "/i18n.js") {
            HttpResponse resp;
            resp.content_type = "text/javascript; charset=utf-8";
            resp.body.assign(kI18nJs, kI18nJsSize);
            return resp;
        }
        if (r.path == "/icon.svg" || r.path == "/favicon.ico") {
            HttpResponse resp;
            resp.content_type = "image/svg+xml";
            resp.body.assign(kIconSvg, kIconSvgSize);
            return resp;
        }
        if (r.path == "/api/info" && r.method == "GET") return info();
        if (r.path == "/api/browse" && r.method == "GET") return browse(r);
        if (r.path == "/api/inspect" && r.method == "GET") return inspect(r);
        if (r.path == "/api/plan" && r.method == "POST") return plan(r, false);
        if (r.path == "/api/remote" || r.path.rfind("/api/remote/", 0) == 0) return remote_action(r);
        if (r.path == "/api/ftp" || r.path.rfind("/api/ftp/", 0) == 0) return ftp_action(r);
        if (r.path == "/api/peers" || r.path.rfind("/api/peers/", 0) == 0) return peers_action(r);
        if (r.path == "/api/peer-drives" && r.method == "GET") return peer_drives_json();
        // Other engines' requests: refused when they speak another protocol
        // (peers.hpp). The page's own pairing requests come from a browser.
        if ((r.path.rfind("/api/pair/", 0) == 0 && r.path.rfind("/api/pair/incoming", 0) != 0) || r.path.rfind("/api/peer/", 0) == 0) {
            auto it = r.headers.find("x-alembic-protocol");
            if (std::string why = peers::protocol_problem(it == r.headers.end() ? std::string() : it->second); !why.empty())
                return error(409, why);
        }
        if (r.path.rfind("/api/pair/", 0) == 0) return pair_action(r);
        if (r.path.rfind("/api/peer/fs/", 0) == 0) return peer_fs_action(r);
        if (r.path == "/api/peer/jobs" || r.path.rfind("/api/peer/jobs/", 0) == 0) return peer_jobs_action(r);
        if (r.path == "/api/peer/places" && r.method == "POST") return peer_places_action(r);
        if (r.path == "/api/smp/scan" && r.method == "POST") {
            std::string why;
            if (!shadowmount_rescan(config_, &why)) return error(503, why);
            JsonValue v = JsonValue::object();
            v.set("queued", true);
            return json(200, v);
        }
        if (r.path == "/api/jobs" && r.method == "GET") return json(200, jobs_json(nullptr));
        if (r.path == "/api/jobs" && r.method == "POST") return plan(r, true);
        if (r.path == "/api/jobs/clear" && r.method == "POST") {
            queue_.clear_finished();
            return json(200, JsonValue::object());
        }
        if (r.path == "/api/quit" && r.method == "POST") {
            log_line("quit requested by " + r.peer);
            request_quit();
            return json(200, JsonValue::object());
        }
        if (r.path == "/api/restart" && r.method == "POST") {
            if (!updater_) return error(503, "the engine is starting");
            if (Status s = updater_->restart(); !s.ok()) return error(409, s.error().message);
            log_line("restart requested by " + r.peer);
            JsonValue v = JsonValue::object();
            v.set("restarting", true);
            return json(200, v);
        }
        if (r.path.rfind("/api/update", 0) == 0) return update_action(r);
        if (r.path == "/api/app/closing" && r.method == "POST") return json(200, JsonValue::object());
        if (r.path == "/api/launcher" && r.method == "POST") return launcher(r);
        if (r.path == "/api/lang" && r.method == "POST") {
            auto body = parse_json(r.body);
            if (!body.ok()) return error(400, body.error().message);
            std::string code = (*body)["lang"].str();
            if (!l10n::has_language(code)) return error(400, "unknown language: " + code);
            l10n::set_language(code);
            if (std::FILE* f = alembic::open_stdio(lang_path(), "w")) {
                std::fputs(code.c_str(), f);
                std::fclose(f);
            }
            return json(200, JsonValue::object());
        }
        if (r.path == "/api/cache/clear" && r.method == "POST") {
            Status s = meta_.clear();
            if (!s.ok()) return error(500, s.error().message);
            log_line("game info cache cleared by " + r.peer);
            return json(200, cache_json());
        }
        if (r.path == "/api/fs/space" && r.method == "GET") return fs_space();
        if (r.path == "/api/locations" && r.method == "GET") return locations();
        if (r.path == "/api/places") return places_action(r);
        if ((r.path == "/api/meta" || r.path == "/api/icon") && r.method == "GET") return game_meta(r);
        if (r.path == "/api/deps" && r.method == "GET") {
            JsonValue list = JsonValue::array();
            for (const Dependency& d : dependencies()) list.push(dependency_json(d, config_.state_dir));
            return json(200, list);
        }
        if (r.path.rfind("/api/deps/", 0) == 0 && r.method == "POST") return dep_action(r);
        if (r.path.rfind("/api/fs/", 0) == 0 && r.method == "POST") return fs_action(r);
        if (r.path.rfind("/api/jobs/", 0) == 0) return job_action(r);
        return error(404, "not found");
    }

    JsonValue cache_json() const {
        GameMetaCache::Stats st = meta_.stats();
        JsonValue c = JsonValue::object();
        c.set("games", st.games);
        c.set("bytes", st.bytes);
        return c;
    }
    std::string lang_path() const { return config_.state_dir + "/lang"; }
    std::string launcher_off_path() const { return config_.state_dir + "/launcher.off"; }

    // The last shortcut result, shown in Settings: on the console it is the
    // only place to see why the shortcut is missing.
    void set_launcher_status(const std::string& text) {
        std::lock_guard<std::mutex> lock(launcher_mutex_);
        launcher_status_ = text;
    }
    void launcher_result(const Status& s, const std::string& when) {
        std::string text = s.ok() ? (platform::launcher_recorded() ? "registered"
                                                                   : "registered; check Media (" + platform::launcher_record_note() + ")")
                                  : "failed: " + s.error().message;
        set_launcher_status(text + " (" + when + "; " + platform::launcher_problem() + ")");
        log_line("home-screen shortcut " + when + ": " + text);
    }

    HttpResponse launcher(const HttpRequest& r) {
        auto body = parse_json(r.body);
        if (!body.ok()) return error(400, body.error().message);
        bool enable = (*body)["enabled"].boolean(), again = (*body)["again"].boolean();
        std::lock_guard<std::mutex> lock(deps_mutex_);
        Status s = enable ? platform::install_launcher(port(), reinterpret_cast<const std::uint8_t*>(kIcon0Png), kIcon0PngSize, again)
                          : platform::remove_launcher();
        if (enable) launcher_result(s, std::string(again ? "registered again" : "added") + " by " + r.peer);
        else {
            set_launcher_status(s.ok() ? "removed" : "remove failed: " + s.error().message);
            log_line("remove home-screen shortcut by " + r.peer + (s.ok() ? "" : ": " + s.error().message));
        }
        if (!s.ok()) return error(502, s.error().message);
        if (enable) (void)remove_file(launcher_off_path());
        else if (std::FILE* f = alembic::open_stdio(launcher_off_path(), "w")) std::fclose(f);
        return info();
    }

    HttpResponse update_action(const HttpRequest& r) {
        if (!updater_) return error(503, "the engine is starting");
        if (r.path == "/api/update" && r.method == "GET") return json(200, updater_->status());
        if (r.method != "POST") return error(404, "not found");
        auto body = parse_json(r.body.empty() ? "{}" : r.body);
        if (!body.ok()) return error(400, body.error().message);
        if (r.path == "/api/update/check") {
            updater_->check();
        } else if (r.path == "/api/update/find") {
            updater_->find((*body)["everywhere"].boolean(false));
        } else if (r.path == "/api/update/install") {
            std::vector<std::string> paths;
            for (const JsonValue& p : (*body)["paths"].items()) paths.push_back(p.str());
            if (Status s = updater_->install(paths, (*body)["app"].boolean(true)); !s.ok()) return error(400, s.error().message);
            log_line("update requested by " + r.peer);
        } else if (r.path == "/api/update/app") {
            if (Status s = updater_->install_app(); !s.ok()) return error(400, s.error().message);
            log_line("app update requested by " + r.peer);
        } else {
            return error(404, "not found");
        }
        return json(200, updater_->status());
    }

    HttpResponse info() {
        JsonValue v = JsonValue::object();
        v.set("name", "Alembic");
        v.set("id", id_);
        v.set("version", version());
        v.set("platform", platform::name());
        v.set("os", platform::os_description());
        v.set("cpu_instructions", alembic::crypto::accelerations());
        v.set("host_name", net::host_name());
        v.set("state_dir", config_.state_dir);
        JsonValue urls = JsonValue::array();
        for (const std::string& u : network_urls()) urls.push(u);
        v.set("network_urls", std::move(urls));
        v.set("local_url", local_url());
        v.set("open_to_network", open_to_network());
        JsonValue roots = JsonValue::array();
        for (const std::string& root : config_.available_roots())
            if (is_directory(root)) roots.push(root);
        v.set("roots", std::move(roots));
        JsonValue runtime = JsonValue::object();
        runtime.set("path", config_.runtime_path);
        bool present = !config_.runtime_path.empty() && is_regular_file(config_.runtime_path);
        runtime.set("present", present);
        std::string digest = present ? file_sha256(config_.runtime_path) : "";
        runtime.set("sha256", digest);
        runtime.set("pinned", digest == kPinnedRuntimeSha256);
        v.set("lz4_runtime", std::move(runtime));
        JsonValue profiles = JsonValue::array();
        for (const lz4pack::BundledProfile& p : lz4pack::bundled_profiles()) {
            JsonValue pj = JsonValue::object();
            pj.set("id", p.id);
            pj.set("label", p.label);
            pj.set("tested", p.tested);
            profiles.push(std::move(pj));
        }
        v.set("lz4_profiles", std::move(profiles));
        v.set("busy", queue_.busy());
        JsonValue shortcut = JsonValue::object();
        shortcut.set("supported", platform::launcher_supported());
        shortcut.set("installed", platform::launcher_installed());
        shortcut.set("title_id", platform::kLauncherTitleId);
        {
            std::lock_guard<std::mutex> lock(launcher_mutex_);
            shortcut.set("status", launcher_status_);
        }
        shortcut.set("off", path_exists(launcher_off_path()));
        v.set("launcher", std::move(shortcut));
        v.set("cache", cache_json());
        v.set("port", static_cast<std::uint64_t>(port()));
        v.set("enter_button", platform::enter_button());
        v.set("protocol", static_cast<std::int64_t>(peers::kPeerProtocol)); // how it talks to other engines
        v.set("lang", l10n::language());
        if (updater_) v.set("update", updater_->summary());
        v.set("can_restart", updater_ && !config_.loader.empty());
        v.set("started", started_ms_); // tells a restarted engine from the one before

        return json(200, v);
    }

    HttpResponse browse(const HttpRequest& r) {
        auto it = r.query.find("path");
        std::string path = it == r.query.end() ? "" : strip_slashes(it->second);
        JsonValue v = JsonValue::object();
        JsonValue entries = JsonValue::array();
        if (path.empty() || path == "/") {
            v.set("path", "/");
            v.set("parent", nullptr);
            for (const std::string& root : config_.available_roots()) {
                if (!is_directory(root)) continue;
                JsonValue e = JsonValue::object();
                e.set("name", root);
                e.set("path", root);
                e.set("kind", "folder");
                entries.push(std::move(e));
            }
            for (const ftp::Server& s : ftp_.list()) {
                JsonValue e = JsonValue::object();
                e.set("name", "FTP: " + s.name);
                e.set("path", "/ftp/" + s.id);
                e.set("kind", "folder");
                e.set("ftp", true);
                entries.push(std::move(e));
            }
            for (const PeerDrive& d : peer_drives().first) {
                JsonValue e = JsonValue::object();
                e.set("name", d.name + " · " + d.label);
                e.set("path", d.path);
                e.set("kind", "folder");
                e.set("peer", d.id);
                entries.push(std::move(e));
            }
            v.set("entries", std::move(entries));
            return json(200, v);
        }
        if (ftp::Servers::is_ftp(path)) {
            auto listed = ftp_.list_directory(path);
            if (!listed.ok()) return error(502, listed.error().message);
            std::sort(listed->begin(), listed->end(), [](const ftp::Entry& a, const ftp::Entry& b) {
                if (a.is_directory != b.is_directory) return a.is_directory;
                return a.name < b.name;
            });
            for (const ftp::Entry& f : *listed) {
                JsonValue e = JsonValue::object();
                e.set("name", f.name);
                e.set("path", path + "/" + f.name);
                e.set("kind", kind_of(f.name, f.is_directory));
                if (!f.is_directory) e.set("size", f.size);
                e.set("mtime", f.mtime);
                e.set("ftp", true);
                entries.push(std::move(e));
            }
            v.set("path", path);
            std::size_t slash = path.rfind('/');
            std::string parent = path.substr(0, slash);
            v.set("parent", parent.size() <= 4 || parent == "/ftp" ? std::string("/") : parent);
            v.set("entries", std::move(entries));
            return json(200, v);
        }
        if (!path_allowed(path, config_.available_roots())) return error(403, "outside the allowed folders");
        auto names = list_directory_entries(path); // one request for another engine's folder
        if (!names.ok()) return error(404, "cannot open " + path);
        struct Item {
            std::string name;
            bool dir;
            std::uint64_t size;
            std::int64_t mtime;
            bool game = false;
        };
        std::vector<Item> items;
        const bool elsewhere = alembic::is_mounted(path);
        for (const DirEntry& entry : *names) {
            const std::string& name = entry.name;
            const PathStat& st = entry.stat;
            if (st.kind == PathKind::missing) continue;
            Item item{name, st.is_directory(), st.size, st.mtime};
            // A game folder: sce_sys/param.json inside (not asked of another
            // engine for every folder: one request each).
            item.game = item.dir && !elsewhere && path_exists(join_path(path, name) + "/sce_sys/param.json");
            items.push_back(std::move(item));
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.dir != b.dir) return a.dir;
            return a.name < b.name;
        });
        for (const Item& i : items) {
            JsonValue e = JsonValue::object();
            e.set("name", i.name);
            e.set("path", join_path(path, i.name));
            e.set("kind", kind_of(i.name, i.dir));
            if (i.game) e.set("game", true);
            if (!i.dir) e.set("size", i.size);
            e.set("mtime", i.mtime);
            entries.push(std::move(e));
        }
        v.set("path", path);
        std::string parent = path.substr(0, path.rfind('/'));
        if (parent.size() == 2 && parent[1] == ':') parent += '/'; // "C:/x" -> "C:/"
        bool is_root = std::any_of(config_.roots.begin(), config_.roots.end(),
                                   [&](const std::string& root) { return strip_slashes(root) == path; });
        if (elsewhere) { // another engine's: up to its drive, then to the list of drives
            auto drives = peer_drives().first;
            is_root = std::none_of(drives.begin(), drives.end(), [&](const PeerDrive& d) { return is_within(parent, d.path); });
        }
        if (is_root || parent.empty() || !path_allowed(parent, config_.available_roots()))
            v.set("parent", "/");
        else
            v.set("parent", parent);
        v.set("entries", std::move(entries));
        if (auto s = space(path); s.ok()) {
            v.set("free", s->free);
            v.set("total", s->total);
        }
        return json(200, v);
    }

    // Other engines (peers.hpp): the list, discovery on/off, add one by
    // address (asks it first), remove one.
    HttpResponse peers_action(const HttpRequest& r) {
        if (!discovery_) return error(503, "starting");
        auto state = [this] {
            JsonValue v = JsonValue::object();
            v.set("enabled", discovery_->enabled());
            v.set("listening", discovery_->listening());
            JsonValue list = JsonValue::array();
            std::int64_t t = static_cast<std::int64_t>(std::time(nullptr));
            for (const peers::Peer& p : discovery_->list()) {
                JsonValue e = JsonValue::object();
                e.set("id", p.who.id);
                e.set("name", p.who.name);
                e.set("os", p.who.os);
                e.set("platform", p.who.platform);
                e.set("version", p.who.version);
                e.set("address", p.address);
                e.set("port", static_cast<std::int64_t>(p.who.port));
                e.set("url", "http://" + p.address + ":" + std::to_string(p.who.port) + "/");
                e.set("manual", p.manual);
                e.set("reachable", p.reachable);
                if (!p.problem.empty()) e.set("problem", p.problem);
                e.set("seen_ago", p.last_seen ? t - p.last_seen : -1);
                e.set("paired", pairing_ && pairing_->is_paired(p.who.id));
                e.set("waiting_for_code", pairing_ && pairing_->waiting_for_code(p.who.id));
                list.push(std::move(e));
            }
            v.set("engines", std::move(list));
            return json(200, v);
        };
        if (r.path == "/api/peers" && r.method == "GET") return state();
        if (r.path == "/api/peers/discovery" && r.method == "POST") {
            auto body = parse_json(r.body);
            if (!body.ok()) return error(400, body.error().message);
            Status s = discovery_->set_enabled((*body)["enabled"].boolean(false));
            if (!s.ok()) return error(500, s.error().message);
            log_line(std::string("discovery switched ") + (discovery_->enabled() ? "on" : "off") + " by " + r.peer);
            return state();
        }
        if (r.path == "/api/peers" && r.method == "POST") {
            auto body = parse_json(r.body);
            if (!body.ok()) return error(400, body.error().message);
            std::int64_t port = (*body)["port"].integer(kDefaultPort);
            if (port <= 0 || port > 65535) return error(400, "bad port");
            auto added = discovery_->add((*body)["address"].str(), static_cast<std::uint16_t>(port));
            if (!added.ok()) return error(400, added.error().message);
            return state();
        }
        // Jobs this engine sent to others, for the queue.
        if (r.path == "/api/peers/jobs" && r.method == "GET") {
            JsonValue all = JsonValue::array();
            const std::int64_t now = unix_now();
            for (const pairing::Paired& p : pairing_->list()) {
                auto peer = discovery_->find(p.id);
                if (!peer) continue;
                {
                    // One that did not answer is left alone for a while, so a
                    // page asking every second does not wait on it each time.
                    std::lock_guard<std::mutex> lock(peer_jobs_mutex_);
                    if (auto it = peer_jobs_quiet_until_.find(p.id); it != peer_jobs_quiet_until_.end() && now < it->second) {
                        for (const JsonValue& j : peer_jobs_last_[p.id].items()) all.push(j);
                        continue;
                    }
                }
                auto text = peer_fs::request(*discovery_, *pairing_, p.id, "GET", "/api/peer/jobs", {}, "", 2500, 4u << 20);
                if (!text.ok()) {
                    std::lock_guard<std::mutex> lock(peer_jobs_mutex_);
                    peer_jobs_quiet_until_[p.id] = now + 10;
                    for (const JsonValue& j : peer_jobs_last_[p.id].items()) all.push(j);
                    continue;
                }
                auto list = parse_json(*text);
                if (!list.ok()) continue;
                JsonValue mine = JsonValue::array();
                for (const JsonValue& j : list->items()) {
                    JsonValue v = j;
                    JsonValue engine = JsonValue::object();
                    engine.set("id", p.id);
                    engine.set("name", peer->who.name);
                    v.set("engine", std::move(engine));
                    mine.push(v);
                    all.push(std::move(v));
                }
                std::lock_guard<std::mutex> lock(peer_jobs_mutex_);
                peer_jobs_last_[p.id] = std::move(mine); // shown again while it does not answer
                peer_jobs_quiet_until_.erase(p.id);
            }
            return json(200, all);
        }
        // /api/peers/ID[/pair[/confirm|/session|/check]]
        std::string rest = r.path.size() > 11 ? r.path.substr(11) : "";
        std::size_t slash = rest.find('/');
        std::string id = rest.substr(0, slash), action = slash == std::string::npos ? "" : rest.substr(slash + 1);
        if (action.empty() && r.method == "DELETE") {
            if (pairing_ && pairing_->is_paired(id)) return error(409, "unpair it first");
            Status s = discovery_->remove(id);
            return s.ok() ? state() : error(404, s.error().message);
        }
        if (!pairing_) return error(503, "starting");
        auto peer = discovery_->find(id);
        if (action == "jobs" && r.method == "POST") return send_job(r, id);
        if (action.rfind("jobs/", 0) == 0) {
            // Pause, resume, cancel, retry, remove a job sent there.
            auto reply = peer_fs::request(*discovery_, *pairing_, id, r.method, "/api/peer/" + action, {}, r.body, 15000, 1u << 20);
            if (!reply.ok()) return error(502, reply.error().message);
            HttpResponse out;
            out.body = *reply;
            return out;
        }
        if (action == "pair" && r.method == "POST") {
            if (!peer) return error(404, "no such engine");
            if (pairing_->is_paired(id)) return error(409, "already paired");
            Status s = pairing_->start(*peer);
            if (!s.ok()) return error(502, s.error().message);
            log_line("pairing with " + peer->who.name + " started by " + r.peer);
            return state();
        }
        if (action == "pair/confirm" && r.method == "POST") {
            auto body = parse_json(r.body);
            if (!body.ok()) return error(400, body.error().message);
            auto paired = pairing_->confirm(id, (*body)["code"].str());
            if (!paired.ok()) return error(400, paired.error().message);
            if (peer) discovery_->remember(peer->who, peer->address);
            return state();
        }
        if (action == "pair/session" && r.method == "DELETE") {
            pairing_->cancel(id);
            return state();
        }
        if (action == "pair/check" && r.method == "GET") {
            // A signed request there and back: the pairing works both ways.
            if (!peer) return error(404, "no such engine");
            auto headers = pairing_->sign(id, "GET", "/api/pair/check", {}, "");
            if (!headers.ok()) return error(409, headers.error().message);
            auto reply = net::http_request(peer->address, peer->who.port, "GET", "/api/pair/check", "", 4000, 1u << 16, *headers);
            if (!reply.ok()) return error(502, reply.error().message);
            auto v = parse_json(reply->body);
            if (reply->status != 200 || !v.ok() || (*v)["id"].str() != id)
                return error(502, v.ok() && !(*v)["error"].str().empty() ? (*v)["error"].str() : "the other engine refused the signed request");
            JsonValue ok = JsonValue::object();
            ok.set("ok", true);
            return json(200, ok);
        }
        if (action == "pair" && r.method == "DELETE") {
            if (!pairing_->is_paired(id)) return error(404, "not paired");
            // Tell it first, while this engine can still sign.
            if (peer) {
                auto headers = pairing_->sign(id, "POST", "/api/pair/forget", {}, "{}");
                if (headers.ok()) (void)net::http_request(peer->address, peer->who.port, "POST", "/api/pair/forget", "{}", 3000, 1u << 16, *headers);
            }
            Status s = pairing_->forget(id);
            if (!s.ok()) return error(500, s.error().message);
            log_line("unpaired " + (peer ? peer->who.name : id) + " by " + r.peer);
            return state();
        }
        return error(404, "not found");
    }

    struct Task {
        std::string owner; // the engine that asked
        std::atomic<std::uint64_t> done{0}, total{0};
        std::atomic<bool> cancel{false};
        std::mutex mutex; // the fields below
        bool finished = false;
        Status status;
        JsonValue summary = JsonValue::object();
        std::string blob; // the bulk of the results
        std::int64_t finished_at = 0;
    };

    // A new task for `owner`, run on its own thread by `work`.
    JsonValue start_task(const std::string& owner, std::function<Status(Task&)> work) {
        auto t = std::make_shared<Task>();
        t->owner = owner;
        std::array<std::uint8_t, 16> raw{};
        (void)platform::secure_random(raw.data(), raw.size());
        std::string id = keys::to_hex(Bytes(raw.data(), raw.size()));
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            // Results nobody fetched are dropped after ten minutes.
            std::int64_t now = unix_now();
            for (auto it = tasks_.begin(); it != tasks_.end();) {
                bool old;
                {
                    std::lock_guard<std::mutex> tl(it->second->mutex);
                    old = it->second->finished && now - it->second->finished_at > 600;
                }
                it = old ? tasks_.erase(it) : std::next(it);
            }
            tasks_[id] = t;
        }
        // The thread holds the task; it ends on its own (or when cancelled).
        std::thread([t, work = std::move(work)] {
            Status status = work(*t);
            std::lock_guard<std::mutex> lock(t->mutex);
            t->status = status;
            t->finished = true;
            t->finished_at = unix_now();
        }).detach();
        JsonValue v = JsonValue::object();
        v.set("task", id);
        return v;
    }

    // A package's SHA3 over [0, length) and its per-block CRC32Cs.
    JsonValue start_checksum(const std::string& owner, const std::string& path, std::uint64_t length, std::uint32_t block) {
        return start_task(owner, [path, length, block](Task& t) -> Status {
            t.total = length;
            crypto::Sha3_256 hash;
            std::string crc(static_cast<std::size_t>(div_round_up(length, block) * 4), '\0');
            ALEMBIC_ASSIGN(File file, File::open(path, File::Mode::read));
            crypto::BackgroundSha3 hasher(true, block);
            std::vector<std::uint8_t> buffer(block);
            for (std::uint64_t off = 0, i = 0; off < length; off += block, ++i) {
                if (t.cancel) return make_error(Errc::cancelled, "cancelled");
                auto n = static_cast<std::size_t>(std::min<std::uint64_t>(block, length - off));
                ALEMBIC_TRY(file.read_at(off, MutableBytes(buffer.data(), n)));
                hasher.update(hash, Bytes(buffer.data(), n));
                put_u32(reinterpret_cast<std::uint8_t*>(crc.data()) + i * 4, crypto::crc32c(Bytes(buffer.data(), n)));
                t.done = off + n;
            }
            hasher.wait();
            crypto::Digest d = hash.finish();
            std::lock_guard<std::mutex> lock(t.mutex);
            t.summary.set("sha3", keys::to_hex(Bytes(d.data(), d.size())));
            t.blob = std::move(crc);
            return {};
        });
    }

    // Every file of the game at `path` with its SHA-256 (convert::digest_tree).
    JsonValue start_digest(const std::string& owner, const std::string& path, const std::string& passcode) {
        return start_task(owner, [path, passcode](Task& t) -> Status {
            ProgressFn progress = [&t](const ProgressEvent& e) {
                t.done = e.done;
                t.total = e.total;
                return !t.cancel.load();
            };
            ALEMBIC_ASSIGN(auto list, convert::digest_tree(path, passcode.empty() ? std::string(32, '0') : passcode, progress));
            std::lock_guard<std::mutex> lock(t.mutex);
            t.summary.set("files", static_cast<std::uint64_t>(list.size()));
            t.blob = convert::digests_to_text(list);
            return {};
        });
    }

    // This engine's files for paired engines (peer_fs.hpp is the other side):
    // signed requests only, inside the shared folders, never the state folder
    // (it holds the pairing keys), never another engine's files through this one.
    HttpResponse peer_fs_action(const HttpRequest& r) {
        if (!pairing_) return error(503, "starting");
        auto who = pairing_->verify(r);
        if (!who.ok()) return error(401, who.error().message);
        const std::string op = r.path.substr(std::string("/api/peer/fs/").size());
        // Tests only: a slow network (ALEMBIC_TEST_PEER_DELAY_MS before each read and write).
        if (op == "read" || op == "write") {
            static const int delay = std::atoi(alembic::environment("ALEMBIC_TEST_PEER_DELAY_MS").c_str());
            if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        auto q = [&](const char* k) { auto it = r.query.find(k); return it == r.query.end() ? std::string() : it->second; };
        auto number = [&](const char* k) { return static_cast<std::uint64_t>(std::strtoull(q(k).c_str(), nullptr, 10)); };
        const std::vector<std::string> roots = config_.available_roots();
        // Work done here for another engine (peer_fs.cpp asks): a package's
        // read-back checksums, or a game's file digests. Started, asked about
        // until finished, results fetched in pieces, dropped.
        if (op == "checksum" || op == "digest" || op == "task" || op == "task_blob" || op == "task_drop") {
            if (r.method == "POST" && (op == "checksum" || op == "digest")) {
                std::string path = strip_slashes(q("path"));
                if (!path_allowed(path, roots) || alembic::is_mounted(path) || is_within(path, config_.state_dir))
                    return error(403, "outside the shared folders: " + path);
                if (op == "digest") return json(200, start_digest(*who, path, q("passcode")));
                std::uint64_t block = number("block");
                if (block == 0 || block > (16u << 20)) return error(400, "bad block size");
                return json(200, start_checksum(*who, path, number("length"), static_cast<std::uint32_t>(block)));
            }
            std::shared_ptr<Task> t;
            {
                std::lock_guard<std::mutex> lock(tasks_mutex_);
                auto it = tasks_.find(q("task"));
                if (it == tasks_.end() || it->second->owner != *who) return error(404, "no such task");
                t = it->second;
                if (op == "task_drop") tasks_.erase(it);
            }
            if (op == "task_drop") {
                t->cancel = true;
                return json(200, JsonValue::object());
            }
            std::lock_guard<std::mutex> lock(t->mutex);
            if (op == "task") {
                JsonValue v = JsonValue::object();
                v.set("done", t->done.load());
                v.set("total", t->total.load());
                v.set("finished", t->finished);
                v.set("error", t->status.ok() ? std::string() : t->status.error().message);
                if (t->finished && t->status.ok()) {
                    v.set("summary", t->summary);
                    v.set("blob_bytes", static_cast<std::uint64_t>(t->blob.size()));
                }
                return json(200, v);
            }
            std::uint64_t offset = number("offset"), length = std::min<std::uint64_t>(number("length"), peer_fs::kChunk);
            HttpResponse out;
            out.content_type = "application/octet-stream";
            if (t->finished && offset < t->blob.size())
                out.body = t->blob.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(std::min<std::uint64_t>(length, t->blob.size() - offset)));
            return out;
        }
        if (op == "roots") {
            // The shared folders with their free space, and where outputs
            // usually go (as /api/locations), for the other engine's page.
            JsonValue v = JsonValue::object(), list = JsonValue::array(), places = JsonValue::array();
            for (const std::string& root : roots) {
                if (!is_directory(root)) continue;
                JsonValue d = JsonValue::object();
                d.set("path", root);
                d.set("label", drive_label(root));
                if (auto sp = alembic::disk_space(root); sp.ok()) {
                    d.set("free", sp->free);
                    d.set("total", sp->total);
                }
                list.push(std::move(d));
            }
            for (const Place& p : places_.list()) { // the person's own, shared with paired engines
                if (!path_allowed(p.path, roots) || alembic::is_mounted(p.path) || !is_directory(p.path)) continue;
                JsonValue e = JsonValue::object();
                e.set("label", p.label);
                e.set("path", p.path);
                e.set("custom", true);
                places.push(std::move(e));
            }
            for (const platform::Location& l : platform::suggested_locations()) {
                if (!path_allowed(l.path, roots)) continue;
                JsonValue e = JsonValue::object();
                e.set("label", l.label);
                e.set("path", l.path);
                places.push(std::move(e));
            }
            v.set("roots", std::move(list));
            v.set("locations", std::move(places));
            v.set("platform", platform::name());
            return json(200, v);
        }
        auto allowed = [&](const std::string& p) {
            return path_allowed(p, roots) && !alembic::is_mounted(p) && !is_within(p, config_.state_dir);
        };
        // Changing a folder that holds the state folder, or a shared folder itself, is refused too.
        auto changeable = [&](const std::string& p) {
            if (!allowed(p) || is_within(config_.state_dir, p)) return false;
            return std::none_of(roots.begin(), roots.end(), [&](const std::string& root) { return strip_slashes(root) == p; });
        };
        const std::string path = strip_slashes(q("path"));
        auto stat_json = [](const PathStat& st, JsonValue v) {
            v.set("kind", peer_fs::kind_name(st.kind));
            v.set("size", st.size);
            v.set("mtime", st.mtime);
            v.set("atime", st.atime);
            return v;
        };
        if (op == "stat" || op == "list" || op == "read" || op == "space") {
            // A folder above a shared one is there (so the other engine can walk
            // down to it, as making an output's folders does); nothing more about it.
            if (op == "stat" && !allowed(path) && is_absolute_path(path) &&
                std::any_of(roots.begin(), roots.end(), [&](const std::string& root) { return is_within(strip_slashes(root), path); })) {
                PathStat dir;
                dir.kind = PathKind::directory;
                return json(200, stat_json(dir, JsonValue::object()));
            }
            if (!allowed(path)) return error(403, "outside the shared folders: " + path);
            bool follow = q("follow") != "0";
            if (op == "stat") return json(200, stat_json(stat_path(path, follow), JsonValue::object()));
            if (op == "space") {
                auto sp = alembic::disk_space(path);
                if (!sp.ok()) return error(500, sp.error().message);
                JsonValue v = JsonValue::object();
                v.set("total", sp->total);
                v.set("free", sp->free);
                return json(200, v);
            }
            if (op == "list") {
                auto entries = list_directory_entries(path, follow);
                if (!entries.ok()) return error(404, entries.error().message);
                JsonValue list = JsonValue::array();
                for (const DirEntry& e : *entries) {
                    if (!allowed(join_path(path, e.name))) continue; // the state folder
                    JsonValue item = JsonValue::object();
                    item.set("name", e.name);
                    list.push(stat_json(e.stat, std::move(item)));
                }
                JsonValue v = JsonValue::object();
                v.set("entries", std::move(list));
                return json(200, v);
            }
            // read
            std::uint64_t offset = number("offset"), length = std::min<std::uint64_t>(number("length"), peer_fs::kChunk);
            auto file = files_.get(path, false);
            if (!file.ok()) return error(404, file.error().message);
            auto size = (*file)->size();
            if (!size.ok()) return error(500, size.error().message);
            HttpResponse out;
            out.content_type = "application/octet-stream";
            if (offset < *size) {
                out.body.resize(static_cast<std::size_t>(std::min<std::uint64_t>(length, *size - offset)));
                Status s = (*file)->read_at(offset, MutableBytes(reinterpret_cast<std::uint8_t*>(out.body.data()), out.body.size()));
                if (!s.ok()) return error(500, s.error().message);
            }
            return out;
        }
        if (r.method != "POST") return error(404, "not found");
        if (op == "open") {
            std::string mode = q("mode");
            if (mode == "read" ? !allowed(path) : !changeable(path)) return error(403, "not allowed here: " + path);
            if (mode == "create_new") {
                files_.forget(path);
                auto f = File::open(path, File::Mode::create_new);
                if (!f.ok()) return error(409, f.error().message);
            }
            auto file = files_.get(path, mode != "read");
            if (!file.ok()) return error(404, file.error().message);
            auto size = (*file)->size();
            JsonValue v = JsonValue::object();
            v.set("size", size.ok() ? *size : 0);
            return json(200, v);
        }
        if (!changeable(path)) return error(403, "not allowed here: " + path);
        Status s;
        if (op == "write") {
            auto file = files_.get(path, true);
            if (!file.ok()) return error(404, file.error().message);
            s = (*file)->write_at(number("offset"), Bytes(reinterpret_cast<const std::uint8_t*>(r.body.data()), r.body.size()));
        } else if (op == "truncate") {
            auto file = files_.get(path, true);
            if (!file.ok()) return error(404, file.error().message);
            s = (*file)->truncate(number("length"));
        } else if (op == "sync") {
            auto file = files_.get(path, true);
            if (!file.ok()) return error(404, file.error().message);
            s = (*file)->sync();
        } else if (op == "mkdir") {
            s = alembic::make_directory(path);
        } else if (op == "remove") {
            files_.forget(path);
            s = alembic::remove_file(path);
        } else if (op == "rmdir") {
            s = alembic::remove_directory(path);
        } else if (op == "rename") {
            std::string to = strip_slashes(q("to"));
            if (!changeable(to)) return error(403, "not allowed here: " + to);
            files_.forget(path);
            files_.forget(to);
            s = alembic::rename_path(path, to);
        } else if (op == "times") {
            s = alembic::set_file_times(path, static_cast<std::int64_t>(number("atime")), static_cast<std::int64_t>(number("mtime")));
        } else {
            return error(404, "not found");
        }
        if (!s.ok()) return error(s.error().code == Errc::unsupported ? 501 : 500, s.error().message);
        return json(200, JsonValue::object());
    }

    // Files other engines read and write, kept open between their requests.
    class OpenFiles {
    public:
        Result<std::shared_ptr<File>> get(const std::string& path, bool writable) {
            std::lock_guard<std::mutex> lock(mutex_);
            for (Entry& e : list_)
                if (e.path == path && (e.writable || !writable)) {
                    e.used = ++clock_;
                    return e.file;
                }
            ALEMBIC_ASSIGN(File f, File::open(path, writable ? File::Mode::read_write : File::Mode::read));
            if (list_.size() >= 16) {
                auto oldest = std::min_element(list_.begin(), list_.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
                list_.erase(oldest);
            }
            list_.push_back({path, writable, std::make_shared<File>(std::move(f)), ++clock_});
            return list_.back().file;
        }
        void forget(const std::string& path) {
            std::lock_guard<std::mutex> lock(mutex_);
            list_.erase(std::remove_if(list_.begin(), list_.end(), [&](const Entry& e) { return e.path == path; }), list_.end());
        }

    private:
        struct Entry {
            std::string path;
            bool writable;
            std::shared_ptr<File> file;
            std::uint64_t used;
        };
        std::mutex mutex_;
        std::vector<Entry> list_;
        std::uint64_t clock_ = 0;
    };

    // Pairing requests from other engines (pairing.hpp), and the code shown
    // here, which only a browser on this computer (the console's own) sees.
    HttpResponse pair_action(const HttpRequest& r) {
        if (!pairing_) return error(503, "starting");
        const bool local = r.peer == "127.0.0.1";
        if (r.path == "/api/pair/incoming" && r.method == "GET") {
            JsonValue v = JsonValue::object();
            v.set("local", local);
            if (auto in = pairing_->incoming(); in && local) {
                JsonValue p = JsonValue::object();
                p.set("from", in->from_name);
                p.set("code", in->code);
                p.set("expires_in", in->expires_in);
                v.set("pairing", std::move(p));
            }
            return json(200, v);
        }
        if (r.path == "/api/pair/incoming/cancel" && r.method == "POST") {
            if (!local) return error(403, "only on this engine's own computer");
            pairing_->cancel_incoming();
            log_line("pairing cancelled here");
            return json(200, JsonValue::object());
        }
        if (r.path == "/api/pair/check" && r.method == "GET") {
            auto who = pairing_->verify(r);
            if (!who.ok()) return error(401, who.error().message);
            JsonValue v = JsonValue::object();
            v.set("id", id_);
            return json(200, v);
        }
        if (r.path == "/api/pair/forget" && r.method == "POST") {
            auto who = pairing_->verify(r);
            if (!who.ok()) return error(401, who.error().message);
            (void)pairing_->forget(*who);
            log_line("unpaired by the other engine (" + *who + ")");
            return json(200, JsonValue::object());
        }
        if (r.method != "POST") return error(404, "not found");
        auto body = parse_json(r.body);
        if (!body.ok()) return error(400, body.error().message);
        if (r.path == "/api/pair/begin") {
            auto reply = pairing_->on_begin(*body, r.peer);
            return reply.ok() ? json(200, *reply) : error(reply.error().code == Errc::unsupported ? 429 : 400, reply.error().message);
        }
        if (r.path == "/api/pair/reveal") {
            auto reply = pairing_->on_reveal(*body);
            return reply.ok() ? json(200, *reply) : error(400, reply.error().message);
        }
        if (r.path == "/api/pair/finish") {
            auto reply = pairing_->on_finish(*body);
            return reply.ok() ? json(200, *reply) : error(400, reply.error().message);
        }
        if (r.path == "/api/pair/cancel") {
            pairing_->on_cancel(*body);
            return json(200, JsonValue::object());
        }
        return error(404, "not found");
    }

    // FTP servers (ftp.hpp): the list (no passwords), add one (logs in first), remove one.
    HttpResponse ftp_action(const HttpRequest& r) {
        if (r.path == "/api/ftp" && r.method == "GET") {
            JsonValue list = JsonValue::array();
            for (const ftp::Server& s : ftp_.list()) {
                JsonValue v = JsonValue::object();
                v.set("id", s.id);
                v.set("name", s.name);
                v.set("host", s.host);
                v.set("port", static_cast<std::int64_t>(s.port));
                v.set("user", s.user);
                v.set("path", "/ftp/" + s.id);
                list.push(std::move(v));
            }
            return json(200, list);
        }
        if (r.path == "/api/ftp" && r.method == "POST") {
            auto body = parse_json(r.body);
            if (!body.ok()) return error(400, body.error().message);
            ftp::Server s;
            s.name = (*body)["name"].str();
            s.host = (*body)["host"].str();
            std::int64_t port = (*body)["port"].integer(21);
            if (port <= 0 || port > 65535) return error(400, "bad port");
            s.port = static_cast<std::uint16_t>(port);
            s.user = (*body)["user"].str("anonymous");
            s.password = (*body)["password"].str();
            auto added = ftp_.add(s);
            if (!added.ok()) return error(400, added.error().message);
            log_line("FTP server " + added->host + " added by " + r.peer);
            JsonValue v = JsonValue::object();
            v.set("id", added->id);
            v.set("path", "/ftp/" + added->id);
            return json(200, v);
        }
        if (r.path.rfind("/api/ftp/", 0) == 0 && r.method == "PUT") {
            auto body = parse_json(r.body);
            if (!body.ok()) return error(400, body.error().message);
            ftp::Server s;
            s.name = (*body)["name"].str();
            s.host = (*body)["host"].str();
            std::int64_t port = (*body)["port"].integer(21);
            if (port <= 0 || port > 65535) return error(400, "bad port");
            s.port = static_cast<std::uint16_t>(port);
            s.user = (*body)["user"].str("anonymous");
            // No "password": keep the saved one.
            const bool keep = !(*body).has("password");
            s.password = (*body)["password"].str();
            auto updated = ftp_.update(r.path.substr(std::string("/api/ftp/").size()), s, keep);
            if (!updated.ok()) return error(400, updated.error().message);
            log_line("FTP server " + updated->host + " changed by " + r.peer);
            return json(200, JsonValue::object());
        }
        if (r.path.rfind("/api/ftp/", 0) == 0 && r.method == "DELETE") {
            Status s = ftp_.remove(r.path.substr(std::string("/api/ftp/").size()));
            return s.ok() ? json(200, JsonValue::object()) : error(500, s.error().message);
        }
        return error(404, "not found");
    }

    // Files a browser on another computer shares (remote.hpp).
    HttpResponse remote_action(const HttpRequest& r) {
        if (r.path == "/api/remote") {
            if (r.method == "GET") return json(200, remote_.list());
            if (r.method != "POST") return error(405, "use GET or POST");
            auto body = parse_json(r.body);
            if (!body.ok()) return error(400, body.error().message);
            auto path = remote_.add((*body)["name"].str(), static_cast<std::uint64_t>((*body)["size"].integer()),
                                    (*body)["last_modified"].integer());
            if (!path.ok()) return error(400, path.error().message);
            log_line("sharing " + *path + " from " + r.peer);
            JsonValue v = JsonValue::object();
            v.set("path", *path);
            return json(200, v);
        }
        std::string rest = r.path.substr(std::string("/api/remote/").size());
        std::size_t slash = rest.find('/');
        if (slash == std::string::npos || r.method != "POST") return error(404, "not found");
        std::string id = rest.substr(0, slash), action = rest.substr(slash + 1);
        if (action == "want") return json(200, remote_.want(id, 8000));
        if (action == "data") {
            auto it = r.query.find("offset");
            std::uint64_t offset = 0;
            if (it == r.query.end() ||
                std::from_chars(it->second.data(), it->second.data() + it->second.size(), offset).ec != std::errc())
                return error(400, "offset is missing");
            Status s = remote_.data(id, offset, r.body);
            return s.ok() ? json(200, JsonValue::object()) : error(400, s.error().message);
        }
        return error(404, "not found");
    }

    HttpResponse inspect(const HttpRequest& r) {
        auto it = r.query.find("path");
        std::string path = it == r.query.end() ? "" : strip_slashes(it->second);
        if (!input_allowed(path, config_.available_roots())) return error(403, "outside the allowed folders");
        auto input = convert::open(path, r.query.count("passcode") ? r.query.at("passcode") : std::string(32, '0'));
        if (!input.ok()) return error(400, input.error().message);
        tree::Totals totals = tree::totals(input->tree->root());
        JsonValue v = JsonValue::object();
        v.set("path", path);
        v.set("format", convert::name(input->format));
        if (auto id = ffpfsc::read_title_id(*input->tree, input->tree->root())) v.set("title_id", *id);
        v.set("files", totals.files);
        v.set("bytes", totals.bytes);
        v.set("lz4_packed", lz4pack::is_packed(input->tree->root()));
        lz4pack::ProfileMatch m = lz4pack::match_bundled_profile(
            input->tree->root(), ffpfsc::read_title_id(*input->tree, input->tree->root()).value_or(""));
        v.set("lz4_profile", m.profile ? JsonValue(m.profile->id) : JsonValue("scan"));
        v.set("engine", m.engine);
        return json(200, v);
    }

    HttpResponse fs_space() {
        JsonValue list = JsonValue::array();
        for (const std::string& root : config_.available_roots()) {
            if (!is_directory(root)) continue;
            auto s = space(root);
            if (!s.ok()) continue;
            JsonValue e = JsonValue::object();
            e.set("root", root);
            e.set("total", s->total);
            e.set("free", s->free);
            list.push(std::move(e));
        }
        return json(200, list);
    }

    HttpResponse dep_action(const HttpRequest& r) {
        std::string rest = r.path.substr(std::string("/api/deps/").size());
        std::size_t slash = rest.find('/');
        if (slash == std::string::npos) return error(404, "not found");
        const Dependency* dep = find_dependency(rest.substr(0, slash));
        std::string action = rest.substr(slash + 1);
        if (!dep || (action != "install" && action != "remove")) return error(404, "not found");
        std::lock_guard<std::mutex> lock(deps_mutex_); // one download or removal at a time
        log_line(action + " " + dep->id + " requested by " + r.peer);
        Status s = action == "install" ? install_dependency(*dep, config_.state_dir) : remove_dependency(*dep, config_.state_dir);
        if (!s.ok()) {
            log_line(action + " " + dep->id + " failed: " + s.error().message);
            return error(502, s.error().message);
        }
        return json(200, dependency_json(*dep, config_.state_dir));
    }

    HttpResponse game_meta(const HttpRequest& r) {
        auto it = r.query.find("path");
        std::string path = it == r.query.end() ? "" : strip_slashes(it->second);
        if (!input_allowed(path, config_.available_roots())) return error(403, "outside the allowed folders");
        if (r.path == "/api/icon") {
            auto png = meta_.icon(path);
            if (!png.ok()) return error(404, png.error().message);
            HttpResponse resp;
            resp.content_type = "image/png";
            resp.body.assign(reinterpret_cast<const char*>(png->data()), png->size());
            resp.headers["Cache-Control"] = "max-age=86400"; // the page adds the file's mtime to the URL
            return resp;
        }
        auto m = meta_.meta(path);
        if (!m.ok()) return error(404, m.error().message);
        JsonValue v = JsonValue::object();
        v.set("title", m->title);
        v.set("title_id", m->title_id);
        v.set("content_id", m->content_id);
        v.set("version", m->version);
        v.set("icon", m->has_icon);
        return json(200, v);
    }

    // A drive's name for people: the PS5's own names, else the folder's.
    static std::string drive_label(const std::string& root) {
        if (root == "/data") return "Internal";
        if (root.rfind("/mnt/usb", 0) == 0) return "USB " + root.substr(8);
        if (root.rfind("/mnt/ext", 0) == 0) return "Extended " + root.substr(8);
        std::string name = base_name(root);
        return name.empty() || name == "/" ? root : name;
    }

    // Paired engines' shared folders (from their /api/peer/fs/roots), asked
    // again after a few seconds; one that does not answer is left out.
    struct PeerDrive {
        std::string id, name, platform, path, label;
        std::uint64_t free = 0, total = 0;
        bool has_space = false;
    };
    struct PeerPlace {
        std::string id, name, path, label;
        bool custom = false; // added by the person (Places), not suggested
    };
    std::pair<std::vector<PeerDrive>, std::vector<PeerPlace>> peer_drives() {
        std::lock_guard<std::mutex> lock(peer_drives_mutex_);
        std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        if (now - peer_drives_at_ < 8) return peer_drives_;
        std::vector<PeerDrive> drives;
        std::vector<PeerPlace> places;
        if (pairing_ && discovery_)
            for (const pairing::Paired& p : pairing_->list()) {
                auto peer = discovery_->find(p.id);
                if (!peer) continue;
                auto text = peer_fs::request(*discovery_, *pairing_, p.id, "GET", "/api/peer/fs/roots", {}, "", 2500, 1u << 20);
                if (!text.ok()) continue;
                auto v = parse_json(*text);
                if (!v.ok()) continue;
                for (const JsonValue& d : (*v)["roots"].items()) {
                    PeerDrive drive;
                    drive.id = p.id;
                    drive.name = peer->who.name;
                    drive.platform = (*v)["platform"].str();
                    drive.path = peer_fs::name_for(p.id, d["path"].str());
                    drive.label = d["label"].str();
                    drive.has_space = d.has("free");
                    drive.free = static_cast<std::uint64_t>(d["free"].integer(0));
                    drive.total = static_cast<std::uint64_t>(d["total"].integer(0));
                    drives.push_back(std::move(drive));
                }
                for (const JsonValue& l : (*v)["locations"].items())
                    places.push_back({p.id, peer->who.name, peer_fs::name_for(p.id, l["path"].str()), l["label"].str(), l["custom"].boolean()});
            }
        peer_drives_ = {std::move(drives), std::move(places)};
        peer_drives_at_ = now;
        return peer_drives_;
    }

    HttpResponse peer_drives_json() {
        auto [drives, places] = peer_drives();
        JsonValue list = JsonValue::array();
        for (const PeerDrive& d : drives) {
            JsonValue e = JsonValue::object();
            e.set("id", d.id);
            e.set("engine", d.name);
            e.set("platform", d.platform);
            e.set("root", d.path);
            e.set("label", d.label);
            if (d.has_space) {
                e.set("free", d.free);
                e.set("total", d.total);
            }
            list.push(std::move(e));
        }
        return json(200, list);
    }

    // Why `path` cannot be a place on this engine, or "" when it can.
    std::string place_problem(const std::string& path) {
        if (!path_allowed(path, config_.available_roots()) || alembic::is_mounted(path)) return "outside the allowed folders";
        if (is_within(path, config_.state_dir)) return "the engine keeps its queue there";
        if (!is_directory(path)) return "not a folder: " + path;
        return "";
    }

    // Places (places.hpp): add one or remove one (?remove=1); a folder on
    // another engine goes to that engine's Places, so it shows everywhere.
    HttpResponse places_action(const HttpRequest& r) {
        if (r.method == "GET") return locations();
        if (r.method != "POST") return error(404, "not found");
        auto body = parse_json(r.body);
        if (!body.ok()) return error(400, body.error().message);
        std::string path = strip_slashes((*body)["path"].str()), label = (*body)["label"].str();
        bool remove = (*body)["remove"].boolean();
        if (auto t = peer_fs::parse(path); t && alembic::is_mounted(path)) {
            if (!pairing_ || !discovery_) return error(503, "starting");
            JsonValue fwd = JsonValue::object();
            fwd.set("path", t->path);
            fwd.set("label", label);
            fwd.set("remove", remove);
            auto reply = peer_fs::request(*discovery_, *pairing_, t->id, "POST", "/api/peer/places", {}, fwd.dump(), 10000, 1u << 20);
            if (!reply.ok()) return error(502, reply.error().message);
            std::lock_guard<std::mutex> lock(peer_drives_mutex_);
            peer_drives_at_ = 0; // ask again
        } else {
            Status s;
            if (remove) s = places_.remove(path);
            else if (std::string why = place_problem(path); !why.empty()) return error(400, why);
            else s = places_.add(path, label);
            if (!s.ok()) return error(400, s.error().message);
            log_line(std::string(remove ? "place removed: " : "place added: ") + path + " by " + r.peer);
        }
        return locations();
    }

    // A paired engine adds or removes one of this engine's Places (signed).
    HttpResponse peer_places_action(const HttpRequest& r) {
        if (!pairing_) return error(503, "starting");
        auto who = pairing_->verify(r);
        if (!who.ok()) return error(401, who.error().message);
        auto body = parse_json(r.body);
        if (!body.ok()) return error(400, body.error().message);
        std::string path = strip_slashes((*body)["path"].str());
        Status s;
        if ((*body)["remove"].boolean()) s = places_.remove(path);
        else if (std::string why = place_problem(path); !why.empty()) return error(400, why);
        else s = places_.add(path, (*body)["label"].str());
        if (!s.ok()) return error(400, s.error().message);
        auto peer = discovery_->find(*who);
        log_line(std::string((*body)["remove"].boolean() ? "place removed: " : "place added: ") + path + " by engine " +
                 (peer ? peer->who.name : *who));
        return json(200, JsonValue::object());
    }

    HttpResponse locations() {
        JsonValue list = JsonValue::array();
        std::vector<std::string> roots = config_.available_roots();
        // The ones the person added first (a folder on another engine stays here too).
        for (const Place& p : places_.list()) {
            if (!path_allowed(p.path, roots)) continue;
            JsonValue e = JsonValue::object();
            e.set("label", p.label);
            e.set("path", p.path);
            e.set("exists", is_directory(p.path));
            e.set("custom", true);
            list.push(std::move(e));
        }
        std::vector<platform::Location> suggested = platform::suggested_locations();
        if (suggested.empty())
            for (const std::string& root : roots) suggested.push_back({root.substr(root.rfind('/') + 1), root});
        for (const platform::Location& l : suggested) {
            if (!path_allowed(l.path, roots)) continue;
            JsonValue e = JsonValue::object();
            e.set("label", l.label);
            e.set("path", l.path);
            e.set("exists", is_directory(l.path));
            list.push(std::move(e));
        }
        for (const ftp::Server& s : ftp_.list()) {
            JsonValue e = JsonValue::object();
            e.set("label", "FTP: " + s.name);
            e.set("path", "/ftp/" + s.id);
            e.set("exists", true);
            e.set("ftp", true);
            list.push(std::move(e));
        }
        // Paired engines: their Places and where their outputs usually go,
        // else their drives.
        auto [drives, places] = peer_drives();
        for (const PeerDrive& d : drives) {
            bool has_place = std::any_of(places.begin(), places.end(), [&](const PeerPlace& p) { return p.id == d.id && !p.custom; });
            if (has_place) continue;
            places.push_back({d.id, d.name, d.path, d.label, false});
        }
        for (const PeerPlace& p : places) {
            JsonValue e = JsonValue::object();
            e.set("label", p.name + " · " + p.label);
            e.set("path", p.path);
            e.set("exists", true);
            e.set("peer", p.id);
            e.set("engine", p.name);
            e.set("place", p.label);
            if (p.custom) e.set("custom", true);
            list.push(std::move(e));
        }
        return json(200, list);
    }

    // Why `path` may not be deleted or renamed, or "" when it may.
    std::string protected_reason(const std::string& path) {
        if (!path_allowed(path, config_.available_roots())) return "outside the allowed folders";
        for (const std::string& root : config_.available_roots())
            if (strip_slashes(root) == path) return "a top-level folder cannot be changed";
        if (is_within(config_.state_dir, path)) return "the engine keeps its queue there";
        for (const Job& j : queue_.list()) {
            if (j.state != JobState::queued && j.state != JobState::running) continue;
            if (is_within(strip_slashes(j.spec.input), path) || is_within(path, strip_slashes(j.spec.input)) ||
                is_within(strip_slashes(j.spec.output), path))
                return "a queued or running job uses it";
        }
        return "";
    }

    HttpResponse fs_action(const HttpRequest& r) {
        auto body = parse_json(r.body);
        if (!body.ok()) return error(400, body.error().message);
        std::string path = strip_slashes((*body)["path"].str());
        std::string action = r.path.substr(std::string("/api/fs/").size());
        if (action == "mkdir") {
            if (ftp::Servers::is_ftp(path)) {
                Status s = ftp_.make_directory(path);
                return s.ok() ? json(200, JsonValue::object()) : error(409, s.error().message);
            }
            if (!path_allowed(path, config_.available_roots())) return error(403, "outside the allowed folders");
            Status s = make_folder(path);
            return s.ok() ? json(200, JsonValue::object()) : error(409, s.error().message);
        }
        if (action != "rename" && action != "delete") return error(404, "not found");
        if (!path_exists(path)) return error(404, "no such file or folder: " + path);
        std::string why = protected_reason(path);
        if (!why.empty()) return error(403, why);
        Status s = action == "rename" ? rename_entry(path, (*body)["name"].str()) : remove_tree(path);
        if (s.ok()) log_line(action + " " + path + (action == "rename" ? " -> " + (*body)["name"].str() : "") + " by " + r.peer);
        return s.ok() ? json(200, JsonValue::object()) : error(409, s.error().message);
    }

    // Validates a job; queues it when `queue`.
    // Whether engine `id` has a part in `j`: it sent it, or the job reads or
    // writes its files (/peer/<id>/...), so its page shows the job too.
    static bool involves(const Job& j, const std::string& id) {
        if (j.spec.origin == id) return true;
        const std::string mine = std::string(peer_fs::kPrefix) + id;
        for (const std::string* p : {&j.spec.input, &j.spec.output, &j.spec.output_dir})
            if (!p->empty() && is_within(strip_slashes(*p), mine)) return true;
        return is_within(strip_slashes(j.result["output"].str()), mine);
    }

    // The queue as the page sees it; `origin` keeps only the jobs that engine has a part in.
    JsonValue jobs_json(const std::string* origin) {
        // Times come from the console's clock; "elapsed" saves the page
        // from comparing them with its own.
        std::int64_t now = unix_now();
        JsonValue list = JsonValue::array();
        for (const Job& j : queue_.list()) {
            if (origin && !involves(j, *origin)) continue;
            JsonValue v = j.to_json();
            if (std::int64_t e = j.elapsed(now); e >= 0) v.set("elapsed", e);
            if (j.spec.kind == "convert") v.set("keeps_progress", keeps_progress(j.spec));
            // Where a waiting job continues from.
            if (j.state == JobState::queued || j.state == JobState::paused || j.state == JobState::failed ||
                j.state == JobState::interrupted)
                if (auto cp = checkpoint_info(j.spec, config_)) {
                    JsonValue c = JsonValue::object();
                    c.set("done", cp->done);
                    c.set("total", cp->total);
                    v.set("checkpoint", std::move(c));
                }
            list.push(std::move(v));
        }
        return list;
    }

    // Jobs another engine sends (signed): queue one, list and change the
    // ones it has a part in (sent, or reading or writing its files). Its paths are as this engine sees them (its own files
    // under /peer/<its id>/).
    HttpResponse peer_jobs_action(const HttpRequest& r) {
        if (!pairing_) return error(503, "starting");
        auto who = pairing_->verify(r);
        if (!who.ok()) return error(401, who.error().message);
        auto peer = discovery_->find(*who);
        std::string name = peer ? peer->who.name : *who;
        HttpRequest local = r;
        local.peer = "engine " + name;
        if (r.path == "/api/peer/jobs" && r.method == "GET") return json(200, jobs_json(&*who));
        if (r.path == "/api/peer/jobs" && r.method == "POST") {
            auto body = parse_json(r.body);
            if (!body.ok()) return error(400, body.error().message);
            JsonValue spec = *body;
            spec.set("origin", *who);
            local.body = spec.dump();
            return plan(local, true);
        }
        // /api/peer/jobs/<id>[/<action>]: only its own jobs
        std::string rest = r.path.substr(std::string("/api/peer/jobs/").size());
        std::uint64_t id = std::strtoull(rest.c_str(), nullptr, 10);
        auto job = queue_.get(id);
        if (!job || !involves(*job, *who)) return error(404, "no such job");
        local.path = "/api/jobs/" + rest;
        return job_action(local);
    }

    // Sends a job to engine `id`, its paths rewritten as it sees them: this
    // engine's files become /peer/<this id>/..., its own files plain paths.
    HttpResponse send_job(const HttpRequest& r, const std::string& id) {
        auto body = parse_json(r.body);
        if (!body.ok()) return error(400, body.error().message);
        JsonValue spec = *body;
        auto there = [&](const std::string& p) -> Result<std::string> {
            if (p.empty()) return p;
            std::string path = strip_slashes(p);
            if (auto t = peer_fs::parse(path)) return t->id == id ? (t->path.empty() ? std::string("/") : t->path) : path;
            if (convert::is_provided(path) || convert::is_output_provided(path))
                return make_error(Errc::invalid_argument, "a file shared from a browser or an FTP server here can only be converted by this engine");
            if (!path_allowed(path, config_.available_roots())) return make_error(Errc::invalid_argument, "outside the allowed folders: " + path);
            return peer_fs::name_for(id_, path);
        };
        for (const char* field : {"input", "output", "output_dir"}) {
            auto p = there(spec[field].str());
            if (!p.ok()) return error(400, p.error().message);
            if (!spec[field].str().empty()) spec.set(field, *p);
        }
        spec.set("lz4_runtime", std::string()); // the engine there uses its own
        auto reply = peer_fs::request(*discovery_, *pairing_, id, "POST", "/api/peer/jobs", {}, spec.dump(), 60000, 1u << 20);
        if (!reply.ok()) return error(502, reply.error().message);
        auto v = parse_json(*reply);
        if (!v.ok()) return error(502, "the other engine's reply is not JSON");
        log_line("job sent to " + id + " by " + r.peer + ": " + spec["input"].str());
        return json(201, *v);
    }

    HttpResponse plan(const HttpRequest& r, bool queue) {
        auto body = parse_json(r.body);
        if (!body.ok()) return error(400, body.error().message);
        auto spec = JobSpec::from_json(*body);
        if (!spec.ok()) return error(400, spec.error().message);
        spec->input = strip_slashes(spec->input);
        const bool shared = convert::is_provided(spec->input);
        if (!shared && !path_allowed(spec->input, config_.available_roots())) return error(403, "the input is outside the allowed folders");
        if (!shared && !path_exists(spec->input)) return error(400, "the input does not exist: " + spec->input);
        if (shared && ftp::Servers::is_ftp(spec->input) && !ftp_.exists(spec->input))
            return error(400, "the input does not exist: " + spec->input);
        if (shared) {
            // A file on another computer: converted only, into a folder here.
            if (spec->kind != "convert") return error(400, "a file shared from a computer can only be converted");
            if (spec->delete_source) return error(400, "a file shared from a computer cannot be deleted from here");
            spec->hide_source = false;
            if (spec->output.empty() && spec->output_dir.empty())
                return error(400, "choose where to save it on the console");
        }
        if (spec->kind == "ampr_emu") {
            JsonValue needs = JsonValue::array();
            for (const std::string& id : missing_dependencies(*spec, config_))
                if (const Dependency* d = find_dependency(id)) needs.push(dependency_json(*d, config_.state_dir));
            bool missing = !needs.items().empty();
            if (!queue || missing) {
                JsonValue v = spec->to_json();
                if (auto a = ampr_state(spec->input, spec->passcode.empty() ? std::string(32, '0') : spec->passcode)) {
                    v.set("imports_ampr", a->imports);
                    v.set("ships_emulator", a->ships);
                }
                v.set("needs", std::move(needs));
                if (missing && queue) v.set("error", std::string("this needs the AMPR emulator; get it in Components first"));
                return json(missing && queue ? 409 : 200, v);
            }
            auto job = queue_.add(std::move(*spec));
            if (!job.ok()) return error(500, job.error().message);
            log_line("job " + std::to_string(job->id) + " (ampr_emu) queued by " + r.peer + ": " + job->spec.input);
            return json(201, job->to_json());
        }
        if (spec->kind == "install") {
            if (!queue) return json(200, spec->to_json());
            auto job = queue_.add(std::move(*spec));
            if (!job.ok()) return error(500, job.error().message);
            log_line("job " + std::to_string(job->id) + " (install) queued by " + r.peer + ": " + job->spec.input);
            return json(201, job->to_json());
        }
        if (spec->kind != "convert") {
            spec->output = strip_slashes(spec->output);
            if (!path_allowed(spec->output, config_.available_roots())) return error(403, "the destination is outside the allowed folders");
            if (is_within(spec->output, spec->input)) return error(400, "cannot copy or move a folder into itself");
            if (path_exists(spec->output) && !spec->overwrite) return error(409, "already exists: " + spec->output);
            if (spec->kind == "move") {
                std::string why = protected_reason(spec->input);
                if (!why.empty()) return error(403, why);
            }
            if (!queue) return json(200, spec->to_json());
            auto job = queue_.add(std::move(*spec));
            if (!job.ok()) return error(500, job.error().message);
            log_line("job " + std::to_string(job->id) + " (" + job->spec.kind + ") queued by " + r.peer);
            return json(201, job->to_json());
        }
        auto target = resolve_target(*spec);
        if (!target.ok()) return error(400, target.error().message);
        auto output = resolve_output(*spec);
        if (!output.ok()) return error(400, output.error().message);
        if (!output_allowed(strip_slashes(*output), config_.available_roots())) return error(403, "the output is outside the allowed folders");
        if ((*target == "ffpfsc" || *target == "fpkg") && convert::is_output_provided(*output))
            return error(400, std::string(*target == "ffpfsc" ? "FFPFSC" : "PKG") +
                                  " output cannot go straight to an FTP server; save it on the console, then copy it there");
        if (*target != "folder" && convert::output_exists(*output) && !spec->overwrite)
            return error(409, "the output already exists: " + *output);
        spec->to = *target;
        spec->output = *output;
        // Components to download first; the page asks the user.
        JsonValue needs = JsonValue::array();
        std::string needs_text;
        for (const std::string& id : missing_dependencies(*spec, config_)) {
            const Dependency* d = find_dependency(id);
            if (!d) continue;
            needs.push(dependency_json(*d, config_.state_dir));
            needs_text += (needs_text.empty() ? "" : ", ") + d->label;
        }
        if (!queue) {
            JsonValue v = spec->to_json();
            v.set("needs", std::move(needs));
            // Whether the AMPR emulator goes in (the page says so).
            if (*target != "fpkg" && !spec->lz4)
                if (auto a = ampr_state(spec->input, spec->passcode.empty() ? std::string(32, '0') : spec->passcode)) {
                    v.set("ampr_needed", a->needed());
                    v.set("ships_emulator", a->ships);
                }
            JsonValue warnings = JsonValue::array();
            if (std::string w = mount_name_warning(*output); !w.empty()) warnings.push(w);
            v.set("warnings", std::move(warnings));
            return json(200, v);
        }
        if (!needs.items().empty()) {
            JsonValue v = JsonValue::object();
            v.set("error", "this conversion needs " + needs_text + "; get it in Components first");
            v.set("needs", std::move(needs));
            return json(409, v);
        }
        auto job = queue_.add(std::move(*spec));
        if (!job.ok()) return error(500, job.error().message);
        log_line("job " + std::to_string(job->id) + " queued by " + r.peer + ": " + job->spec.input + " -> " + job->spec.output);
        return json(201, job->to_json());
    }

    HttpResponse job_action(const HttpRequest& r) {
        std::string rest = r.path.substr(std::string("/api/jobs/").size());
        std::string action;
        if (std::size_t slash = rest.find('/'); slash != std::string::npos) {
            action = rest.substr(slash + 1);
            rest = rest.substr(0, slash);
        }
        std::uint64_t id = 0;
        auto res = std::from_chars(rest.data(), rest.data() + rest.size(), id);
        if (res.ec != std::errc() || res.ptr != rest.data() + rest.size()) return error(404, "not found");
        if (action.empty() && r.method == "GET") {
            auto job = queue_.get(id);
            return job ? json(200, job->to_json()) : error(404, "no such job");
        }
        if (action.empty() && r.method == "DELETE") {
            Status s = queue_.remove(id);
            return s.ok() ? json(200, JsonValue::object()) : error(409, s.error().message);
        }
        if (action == "cancel" && r.method == "POST") {
            Status s = queue_.cancel(id);
            return s.ok() ? json(200, JsonValue::object()) : error(409, s.error().message);
        }
        if ((action == "pause" || action == "resume") && r.method == "POST") {
            Status s = action == "pause" ? queue_.pause(id) : queue_.resume(id);
            if (s.ok()) log_line("job " + std::to_string(id) + " " + action + " by " + r.peer);
            return s.ok() ? json(200, JsonValue::object()) : error(409, s.error().message);
        }
        if (action == "move" && r.method == "POST") {
            auto body = parse_json(r.body);
            if (!body.ok()) return error(400, body.error().message);
            Status s = queue_.move(id, (*body)["direction"].integer(1) < 0 ? -1 : 1);
            return s.ok() ? json(200, JsonValue::object()) : error(409, s.error().message);
        }
        if (action == "retry" && r.method == "POST") {
            auto job = queue_.retry(id);
            return job.ok() ? json(201, job->to_json()) : error(409, job.error().message);
        }
        return error(404, "not found");
    }

    EngineConfig config_;
    std::string listen_address_;
    std::string id_ = peers::engine_id(config_.state_dir);
    std::unique_ptr<peers::Discovery> discovery_;
    std::unique_ptr<pairing::Pairing> pairing_;
    OpenFiles files_;
    Places places_{config_.state_dir};
    std::mutex tasks_mutex_;
    std::map<std::string, std::shared_ptr<Task>> tasks_;
    std::mutex peer_jobs_mutex_;
    std::map<std::string, std::int64_t> peer_jobs_quiet_until_;
    std::map<std::string, JsonValue> peer_jobs_last_;
    std::mutex peer_drives_mutex_;
    std::pair<std::vector<PeerDrive>, std::vector<PeerPlace>> peer_drives_;
    std::int64_t peer_drives_at_ = 0;
    RemoteFiles remote_; // before the queue, whose jobs may read from it
    ftp::Servers ftp_{config_.state_dir};
    GameMetaCache meta_{config_.state_dir};
    JobQueue queue_;
    HttpServer server_;
    std::mutex deps_mutex_;
    std::mutex launcher_mutex_;
    std::string launcher_status_ = "not tried";
    std::mutex quit_mutex_;
    std::condition_variable quit_cv_;
    bool quit_ = false;
    mutable std::mutex app_seen_mutex_;
    std::chrono::steady_clock::time_point app_seen_ = std::chrono::steady_clock::now();
    const std::int64_t started_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch()).count();
    std::unique_ptr<update::Updater> updater_; // last: its worker may call request_quit()
};

// Another engine already on our port (the one that started this one through
// the ELF loader, or one loaded earlier): ask it to stop and wait until it has,
// so the two never run the same queue. A newer engine is left alone and this
// one stops instead. Returns false when this engine should not start.
bool take_over_port(std::uint16_t port) {
    auto info = net::http_request("127.0.0.1", port, "GET", "/api/info", "", 1500);
    if (!info.ok() || info->status != 200) return true; // nobody there, or not an engine: start as usual
    auto v = parse_json(info->body);
    if (!v.ok() || (*v)["name"].str() != "Alembic") return true;
    std::string running = (*v)["version"].str();
    if (update::compare_versions(running, version()) > 0) {
        log_line("Alembic " + running + " is already running on port " + std::to_string(port) + "; this older one stops");
        platform::notify(l10n::t("Alembic {version} is already running", {{"version", running}}));
        return false;
    }
    log_line("Alembic " + running + " is running on port " + std::to_string(port) + "; asking it to stop");
    (void)net::http_request("127.0.0.1", port, "POST", "/api/quit", "", 3000);
    // It pauses its job at a checkpoint first; that can take a moment.
    for (int i = 0; i < 180; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto again = net::http_request("127.0.0.1", port, "GET", "/api/info", "", 1000);
        if (!again.ok()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            return true;
        }
    }
    log_line("the engine on port " + std::to_string(port) + " did not stop");
    return true; // starting fails with "port taken", as before
}

} // namespace
} // namespace alembic::engine

int main(int argc, char** argv) {
    using namespace alembic::engine;
    platform::set_process_name("alembic.elf"); // not "payload.elf" in the console's process list
    // UTF-8 arguments on Windows too (main() gets the ANSI code page there).
    alembic::use_utf8_console();
    std::vector<std::string> utf8 = alembic::program_arguments(argc, argv);
    std::vector<char*> pointers;
    for (std::string& a : utf8) pointers.push_back(a.data());
    pointers.push_back(nullptr);
    argc = static_cast<int>(utf8.size());
    argv = pointers.data();
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN); // a client closing early must not end the engine
#endif

    EngineConfig config;
    config.state_dir = platform::default_state_dir();
    std::vector<std::string> roots;
    std::string address = "0.0.0.0";
    std::uint16_t port = kDefaultPort;
    std::string runtime, shadowmount;
    std::uint16_t discovery_port = peers::kDiscoveryPort;
    // On the console the payload replaces an engine already running; on a PC
    // only when asked (--take-over), as two may run on purpose.
    bool take_over = std::string(platform::name()) == "ps5";
    if (take_over) config.loader = "127.0.0.1:9021";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--port") {
            std::string v = next();
            unsigned value = 0;
            auto r = std::from_chars(v.data(), v.data() + v.size(), value);
            if (r.ec != std::errc() || value > 65535) return std::fprintf(stderr, "bad --port\n"), 2;
            port = static_cast<std::uint16_t>(value);
        } else if (a == "--listen") {
            address = next();
        } else if (a == "--state") {
            config.state_dir = strip_slashes(next());
        } else if (a == "--root") {
            roots.push_back(strip_slashes(next()));
        } else if (a == "--runtime") {
            runtime = strip_slashes(next());
        } else if (a == "--shadowmount") {
            shadowmount = next();
        } else if (a == "--discovery-port") {
            std::string v = next();
            unsigned value = 0;
            auto r = std::from_chars(v.data(), v.data() + v.size(), value);
            if (r.ec != std::errc() || value == 0 || value > 65535) return std::fprintf(stderr, "bad --discovery-port\n"), 2;
            discovery_port = static_cast<std::uint16_t>(value);
        } else if (a == "--loader") {
            config.loader = next();
        } else if (a == "--update-url") {
            config.update_url = next();
        } else if (a == "--no-update-check") {
            config.check_updates = false;
        } else if (a == "--take-over") {
            take_over = true;
        } else {
            std::fprintf(stderr, "usage: %s [--port N] [--listen ADDR] [--state DIR] [--root DIR]... [--runtime FILE] [--shadowmount HOST:PORT] [--discovery-port N]\n"
                                 "       [--loader HOST:PORT] [--update-url URL] [--no-update-check] [--take-over]\n",
                         argv[0]);
            return 2;
        }
    }
    config.roots = roots.empty() ? platform::default_roots() : roots;
    config.runtime_path = runtime.empty() ? config.state_dir + "/runtime/libSceAmpr.sprx" : runtime;
    config.backports_root = std::string(platform::name()) == "ps5" ? "/data/homebrew/backports" : config.state_dir + "/backports";
    if (std::string(platform::name()) == "ps5") config.shadowmount_config = "/data/shadowmount/config.ini";
    config.shadowmount_address = shadowmount;

    alembic::Status dirs = alembic::make_parent_directories(config.state_dir + "/engine.log");
    if (dirs.ok()) g_log_path = config.state_dir + "/engine.log";
    // Notifications with Alembic's icon (rounded, like the page's) instead of
    // the download arrow; the home screen rounds the Media shortcut's itself.
    // The name carries a hash of the picture: the console caches icons by
    // path, so a changed icon needs a new one (or the old picture shows once more).
    {
        const auto* png = reinterpret_cast<const std::uint8_t*>(kNotifyIconPng);
        alembic::Sha256 h;
        h.update(alembic::Bytes(png, kNotifyIconPngSize));
        std::string tag = alembic::Sha256::hex(h.finish()).substr(0, 8);
        platform::set_notification_icon(config.state_dir + "/notify_icon-" + tag + ".png", png, kNotifyIconPngSize);
    }
    log_line(std::string("Alembic engine ") + version() + " starting (" + platform::name() + ")");

    // Notifications follow the page's language (saved by /api/lang), else the console's.
    l10n::load(kI18nJs, kI18nJsSize);
    {
        std::string code = platform::system_language();
        if (std::FILE* f = alembic::open_stdio((config.state_dir + "/lang"), "r")) {
            char buf[16] = {};
            if (std::fgets(buf, sizeof buf, f)) code = buf;
            std::fclose(f);
        }
        l10n::set_language(code);
    }
    if (take_over && !take_over_port(port)) return 0;
    Engine engine(config);
    alembic::Status started = engine.start(address, port, discovery_port);
    if (!started.ok()) {
        log_line("cannot start: " + started.error().message);
        platform::notify(l10n::t("Alembic: {error}", {{"error", l10n::message(started.error().message)}}));
        platform::pause_before_exit();
        return 1;
    }
    log_line("listening on " + address + ":" + std::to_string(engine.port()));
    // Where it runs and how to reach it, for whoever started it.
    {
        std::string host = net::host_name();
        log_line("running on " + platform::os_description() + (host.empty() ? "" : ", computer \"" + host + "\"") +
                 "; CPU instructions: " + alembic::crypto::accelerations());
        log_line("open the page: " + engine.local_url());
        for (const std::string& u : engine.network_urls()) log_line("  from other devices: " + u);
        std::string shared;
        for (const std::string& r : engine.config().available_roots()) shared += (shared.empty() ? "" : ", ") + r;
        log_line("shared folders: " + (shared.empty() ? std::string("none available") : shared));
        log_line("state folder: " + engine.config().state_dir);
        if (engine.open_to_network() && std::string(platform::name()) != "ps5")
            log_line("anyone on this network can use the page (there is no password); --listen 127.0.0.1 keeps it to this computer");
        if (std::string(platform::name()) != "ps5") log_line("stop it with Ctrl+C, or in the page: Settings > Stop the engine");
    }
    engine.start_launcher();
    std::thread([&engine] { engine.prune_cache(); }).detach();
    const std::string port_text = std::to_string(engine.port());
    platform::notify(platform::launcher_supported()
                         ? l10n::t("Alembic is ready: open the Alembic app, or the shortcut in Media (port {port})", {{"port", port_text}})
                         : l10n::t("Alembic engine ready on port {port}", {{"port", port_text}}));
    engine.wait_for_quit();
    engine.stop();
    log_line("stopped");
    return 0;
}
