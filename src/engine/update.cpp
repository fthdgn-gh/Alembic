// SPDX-License-Identifier: GPL-3.0-or-later
#include "update.hpp"
#include "jobs.hpp" // unix_now
#include "net.hpp"
#include "text_file.hpp"

#include "alembic/sha256.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace alembic::engine::update {

namespace {

constexpr std::size_t kMaxPayload = 64u << 20; // a payload is ~4 MB
constexpr std::size_t kMaxAppImage = 512u << 20; // the app's image is ~50 MB
constexpr double kAppQuietSeconds = 15.0;        // the app polls every few seconds while open
constexpr int kLoaderTimeoutMs = 3000;

// Releases before the marker, by the SHA-256 of their alembic-vX.Y.Z.elf.
struct KnownRelease {
    const char* sha256;
    const char* version;
};
constexpr KnownRelease kKnown[] = {
    {"b4a7e7d7e9b15a05e28d0db62532b9b18bbb73697e644f93aaabf71b70102276", "0.1.0"},
    {"7702e737d3d6b9e57bbb3f511ba806d661136468dd425da2126985b0e4301507", "0.1.1"},
    {"a102fd798fec2137beaf6537b7127f97d7f9719b3d5a6224d3b4ffc8022ac72a", "0.2.0"},
    {"bc8f9dcd7180791a410222fa0314e14df0db2402589d3b11757aab7a44994321", "0.2.1"},
    {"4ead75f7c9cc674046e46f62dbe802141eb4338b3cc8789febe3f5c748e27c54", "0.2.2"},
};

std::string sha256_hex(Bytes data) {
    Sha256 h;
    h.update(data);
    return Sha256::hex(h.finish());
}

Bytes bytes_of(const std::string& s) { return Bytes(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }

Result<std::string> file_sha256(const std::string& path) {
    std::FILE* f = open_stdio(path, "rb");
    if (!f) return make_error(Errc::io, "cannot open " + path + ": " + last_os_error());
    Sha256 h;
    std::vector<std::uint8_t> buffer(1u << 20);
    for (;;) {
        std::size_t n = std::fread(buffer.data(), 1, buffer.size(), f);
        if (n > 0) h.update(Bytes(buffer.data(), n));
        if (n < buffer.size()) break;
    }
    bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed) return make_error(Errc::io, "cannot read " + path);
    return Sha256::hex(h.finish());
}

bool contains(Bytes data, std::string_view needle) {
    if (needle.empty() || data.size() < needle.size()) return false;
    auto it = std::search(data.begin(), data.end(), needle.begin(), needle.end(),
                          [](std::uint8_t a, char b) { return a == static_cast<std::uint8_t>(b); });
    return it != data.end();
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool ends_with(const std::string& s, const char* tail) {
    std::size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

std::string strip_v(const std::string& v) { return !v.empty() && (v[0] == 'v' || v[0] == 'V') ? v.substr(1) : v; }

// The loader a folder belongs to, for the page.
std::string loader_of(const std::string& path) {
    if (path.find("/pldmgr/") != std::string::npos) return "Payload Manager";
    if (path.find("/ps5_autoloader/") != std::string::npos) return "autoloader";
    return "";
}

void walk(const std::string& dir, int depth, bool loader_folder, const std::string& new_version,
          const std::vector<std::string>& skip, std::vector<Found>& out) {
    if (std::find(skip.begin(), skip.end(), dir) != skip.end()) return;
    auto entries = list_directory_entries(dir, false);
    if (!entries.ok()) return;
    for (const DirEntry& e : *entries) {
        if (e.name.empty() || e.name[0] == '.') continue;
        std::string path = join_path(dir, e.name);
        if (e.stat.is_directory()) {
            if (depth > 0) walk(path, depth - 1, loader_folder, new_version, skip, out);
            continue;
        }
        if (!e.stat.is_file() || e.stat.size == 0 || e.stat.size > kMaxPayload) continue;
        std::string name = lower(e.name);
        // Games are full of .bin files; only loader folders hold .bin payloads.
        if (!ends_with(name, ".elf") && !(loader_folder && ends_with(name, ".bin"))) continue;
        auto data = read_text(path, kMaxPayload);
        if (!data.ok()) continue;
        PayloadId id = identify(bytes_of(*data));
        if (!id.alembic || id.version == new_version) continue; // not ours, or already the new one
        Found f;
        f.path = path;
        f.version = id.version;
        f.loader = loader_of(path);
        // Payload Manager finds the file through the slot's meta.json: rename
        // only where that file names it, and fix the name there too.
        std::string to = renamed(e.name, id.version, new_version);
        if (to != e.name && !path_exists(join_path(dir, to))) {
            auto meta = read_text(join_path(dir, "meta.json"), 1u << 20);
            if (meta.ok() && meta->find(e.name) != std::string::npos) f.rename_to = to;
        }
        out.push_back(std::move(f));
    }
}

} // namespace

int compare_versions(const std::string& a, const std::string& b) {
    std::string x = strip_v(a), y = strip_v(b);
    std::size_t i = 0, j = 0;
    while (i < x.size() || j < y.size()) {
        std::uint64_t p = 0, q = 0;
        while (i < x.size() && std::isdigit(static_cast<unsigned char>(x[i]))) p = p * 10 + static_cast<unsigned>(x[i++] - '0');
        while (j < y.size() && std::isdigit(static_cast<unsigned char>(y[j]))) q = q * 10 + static_cast<unsigned>(y[j++] - '0');
        if (p != q) return p < q ? -1 : 1;
        while (i < x.size() && !std::isdigit(static_cast<unsigned char>(x[i]))) ++i;
        while (j < y.size() && !std::isdigit(static_cast<unsigned char>(y[j]))) ++j;
    }
    return 0;
}

Result<Release> parse_release(std::string_view json) {
    auto v = parse_json(json);
    if (!v.ok()) return make_error(Errc::invalid_data, "GitHub's answer is not readable: " + v.error().message);
    Release r;
    r.version = strip_v((*v)["tag_name"].str());
    if (r.version.empty() || !std::isdigit(static_cast<unsigned char>(r.version[0])))
        return make_error(Errc::invalid_data, "the latest release has no version");
    r.notes = (*v)["body"].str();
    r.page_url = (*v)["html_url"].str();
    for (const JsonValue& a : (*v)["assets"].items()) {
        std::string name = a["name"].str();
        if (name == "SHA256SUMS") r.sums_url = a["browser_download_url"].str();
        else if (lower(name).rfind("alembic-app-", 0) == 0 && ends_with(lower(name), ".ffpkg") && r.app_url.empty()) {
            r.app_name = name;
            r.app_url = a["browser_download_url"].str();
            r.app_size = static_cast<std::uint64_t>(a["size"].integer(0));
        } else if (ends_with(lower(name), ".elf") && r.elf_url.empty()) {
            r.elf_name = name;
            r.elf_url = a["browser_download_url"].str();
            r.elf_size = static_cast<std::uint64_t>(a["size"].integer(0));
        }
    }
    if (r.elf_url.empty() || r.sums_url.empty())
        return make_error(Errc::invalid_data, "the latest release has no payload or no SHA256SUMS");
    return r;
}

std::string checksum_for(std::string_view sums, const std::string& name) {
    std::size_t at = 0;
    while (at < sums.size()) {
        std::size_t end = sums.find('\n', at);
        std::string_view line = sums.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at = end == std::string_view::npos ? sums.size() : end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        std::size_t space = line.find(' ');
        if (space != 64) continue;
        std::string_view file = line.substr(space);
        while (!file.empty() && (file.front() == ' ' || file.front() == '*')) file.remove_prefix(1);
        if (file == name) return lower(std::string(line.substr(0, 64)));
    }
    return "";
}

PayloadId identify(Bytes data) {
    PayloadId id;
    if (data.size() < 4 || std::memcmp(data.data(), "\x7f" "ELF", 4) != 0) return id;
    // The marker with a version after it: the bare prefix is in every engine
    // too (the code that looks for it).
    std::string_view marker = kMarker;
    auto same = [](std::uint8_t a, char b) { return a == static_cast<std::uint8_t>(b); };
    for (auto it = std::search(data.begin(), data.end(), marker.begin(), marker.end(), same); it != data.end();
         it = std::search(it, data.end(), marker.begin(), marker.end(), same)) {
        it += static_cast<std::ptrdiff_t>(marker.size());
        if (it == data.end() || !std::isdigit(*it)) continue;
        id.alembic = true;
        for (; it != data.end() && (std::isdigit(*it) || *it == '.'); ++it) id.version += static_cast<char>(*it);
        return id;
    }
    std::string hash = sha256_hex(data);
    for (const KnownRelease& k : kKnown)
        if (hash == k.sha256) return {true, k.version};
    // An older engine built from source: its texts give it away.
    if (contains(data, "Alembic engine ") && contains(data, "Alembic is ready")) id.alembic = true;
    return id;
}

std::string renamed(const std::string& name, const std::string& old_version, const std::string& new_version) {
    if (old_version.empty() || old_version == new_version) return name;
    std::size_t at = name.find(old_version);
    if (at == std::string::npos) return name;
    return name.substr(0, at) + new_version + name.substr(at + old_version.size());
}

std::string replace_all(std::string text, const std::string& from, const std::string& to, int* count) {
    int n = 0;
    if (!from.empty())
        for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
            text.replace(at, from.size(), to);
            ++n;
        }
    if (count) *count = n;
    return text;
}

std::string app_version_text(const std::string& content_version) {
    std::string out, part;
    auto flush = [&] {
        std::size_t i = 0;
        while (i + 1 < part.size() && part[i] == '0') ++i;
        out += (out.empty() ? "" : ".") + part.substr(i);
        part.clear();
    };
    for (char c : content_version) {
        if (c == '.') flush();
        else if (std::isdigit(static_cast<unsigned char>(c))) part += c;
        else return content_version;
    }
    flush();
    return out;
}

std::string app_image_name(const std::string& dir, const std::string& version) {
    std::string name = "alembic-app-" + version + ".ffpkg";
    for (int i = 2; path_exists(join_path(dir, name)); ++i) name = "alembic-app-" + version + "-" + std::to_string(i) + ".ffpkg";
    return name;
}

std::vector<Found> find_payloads(const std::vector<std::string>& dirs, int depth, const std::string& new_version,
                                 const std::vector<std::string>& skip) {
    std::vector<Found> out;
    for (const std::string& d : dirs) {
        bool loader_folder = !loader_of(d + "/").empty();
        walk(d, depth, loader_folder, new_version, skip, out);
    }
    // One entry per file, in a stable order.
    std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) { return a.path < b.path; });
    out.erase(std::unique(out.begin(), out.end(), [](const Found& a, const Found& b) { return a.path == b.path; }), out.end());
    return out;
}

std::vector<std::string> usual_places() {
    std::vector<std::string> bases = {"/data"};
    for (int i = 0; i < 8; ++i) bases.push_back("/mnt/usb" + std::to_string(i));
    std::vector<std::string> out;
    for (const std::string& b : bases) {
        // Payload Manager (itsPLK/ps5-payload-manager): payloads/{local,repo}/<slot>/<file>
        // and older layouts; the autoloaders: ps5_autoloader/<file>.
        for (const char* sub : {"/pldmgr", "/ps5_autoloader"})
            if (is_directory(b + sub)) out.push_back(b + sub);
    }
    if (is_directory("/data/payloads")) out.push_back("/data/payloads");
    return out;
}

Status replace_payload(const Found& found, const std::string& elf) {
    if (found.rename_to.empty()) return write_text(found.path, elf);
    std::string dir = found.path.substr(0, found.path.rfind('/'));
    std::string old_name = base_name(found.path), target = join_path(dir, found.rename_to);
    std::string meta_path = join_path(dir, "meta.json");
    ALEMBIC_ASSIGN(std::string meta, read_text(meta_path, 1u << 20));
    // Payload Manager's schema is not ours: only its file name changes, as text.
    int count = 0;
    std::string fixed = replace_all(meta, old_name, found.rename_to, &count);
    if (count == 0) return write_text(found.path, elf); // it no longer names the file: keep the name
    ALEMBIC_TRY(write_text(target, elf));
    if (Status s = write_text(meta_path, fixed); !s.ok()) {
        (void)remove_file(target);
        return s;
    }
    // The slot now points at the new file; the old one may go.
    if (Status s = remove_file(found.path); !s.ok()) return make_error(s.error().code, "the old payload stays: " + s.error().message);
    return {};
}

Status send_to_loader(const std::string& address, const std::string& elf) {
    std::size_t colon = address.rfind(':');
    if (colon == std::string::npos) return make_error(Errc::invalid_argument, "bad loader address " + address);
    if (!net::startup()) return make_error(Errc::io, "cannot start the network");
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<std::uint16_t>(std::atoi(address.c_str() + colon + 1)));
    if (::inet_pton(AF_INET, address.substr(0, colon).c_str(), &a.sin_addr) != 1)
        return make_error(Errc::invalid_argument, "bad loader address " + address);
    std::string why;
    net::Socket s = net::connect(a, kLoaderTimeoutMs, false, &why);
    if (s == net::kNone) return make_error(Errc::io, "no ELF loader answers on " + address + " (" + why + ")");
    net::set_timeouts(s, 10);
    std::size_t done = 0;
    while (done < elf.size()) {
        long n = net::send(s, elf.data() + done, elf.size() - done);
        if (n < 0 && net::should_retry()) continue;
        if (n <= 0) {
            std::string e = net::error_text();
            net::close(s);
            return make_error(Errc::io, "sending the payload to the ELF loader failed: " + e);
        }
        done += static_cast<std::size_t>(n);
    }
    net::close(s);
    return {};
}

// ---------------------------------------------------------------------------

Updater::Updater(Options options) : o_(std::move(options)) {
    if (o_.loader.empty()) return; // a PC: no app to look after
    watcher_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(stop_mutex_);
        while (!stop_cv_.wait_for(lock, std::chrono::seconds(3), [this] { return stop_; })) {
            lock.unlock();
            tick();
            lock.lock();
        }
    });
}

Updater::~Updater() {
    {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        stop_ = true;
    }
    stop_cv_.notify_all();
    if (watcher_.joinable()) watcher_.join();
    if (thread_.joinable()) thread_.join();
}

// ---- the console app ----

Updater::AppImage Updater::find_app(bool hash) const {
    AppImage a;
    auto ours = [&](const std::string& path, std::string* version) {
        std::string id;
        return o_.game_info && o_.game_info(path, &id, version) && id == kAppTitleId;
    };
    auto take = [&](const std::string& path, const std::string& version) {
        a.path = path;
        a.image = !is_directory(path);
        a.version = app_version_text(version);
    };
    std::string version;
    std::string source = o_.smp_source ? o_.smp_source(kAppTitleId) : "";
    if (!source.empty() && path_exists(source)) {
        take(source, ours(source, &version) ? version : "");
    } else if (o_.scan_dirs) {
        // Not known to ShadowMount+ (or its API is off): the folders it scans,
        // names that look like the app first; only images it mounts as UFS2 and folders.
        for (const std::string& dir : o_.scan_dirs()) {
            auto entries = list_directory_entries(dir, true);
            if (!entries.ok()) continue;
            std::vector<std::string> likely, rest;
            for (const DirEntry& e : *entries) {
                if (e.name.empty() || e.name[0] == '.') continue;
                std::string n = lower(e.name);
                if (!e.stat.is_directory() && !ends_with(n, ".ffpkg")) continue;
                (n.find("alembic") != std::string::npos || n.find(lower(kAppTitleId)) != std::string::npos ? likely : rest)
                    .push_back(join_path(dir, e.name));
            }
            for (auto* list : {&likely, &rest})
                for (const std::string& p : *list)
                    if (a.path.empty() && ours(p, &version)) take(p, version);
            if (!a.path.empty()) break;
        }
    }
    if (hash && a.image) {
        auto h = file_sha256(a.path);
        if (h.ok()) a.sha256 = *h;
    }
    return a;
}

static std::string pending_path(const std::string& state_dir) { return state_dir + "/update/app-pending.json"; }

bool Updater::app_pending() const { return path_exists(pending_path(o_.state_dir)); }

Status Updater::stage_app(const AppImage& app, const Release& release, const std::string& sums) {
    std::string want = checksum_for(sums, release.app_name);
    if (want.empty()) return make_error(Errc::invalid_data, "SHA256SUMS does not list " + release.app_name);
    ALEMBIC_ASSIGN(std::vector<std::uint8_t> data, o_.get(release.app_url, kMaxAppImage));
    std::string got = sha256_hex(data);
    if (got != want)
        return make_error(Errc::invalid_data, "the downloaded app has an unexpected SHA-256 (" + got + "); nothing was changed");
    std::string dir = app.path.substr(0, app.path.rfind('/'));
    // A '.' name: ShadowMount+ does not see it until it is renamed.
    std::string staged = join_path(dir, ".alembic-app-" + release.version + ".ffpkg.new");
    ALEMBIC_TRY(write_text(staged, std::string(data.begin(), data.end())));
    JsonValue p = JsonValue::object();
    p.set("staged", staged);
    p.set("old", app.path);
    p.set("version", release.version);
    p.set("sha256", got);
    if (Status s = write_text(pending_path(o_.state_dir), p.dump()); !s.ok()) {
        (void)remove_file(staged);
        return s;
    }
    if (o_.log) o_.log("staged the app " + release.version + " as " + staged);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        app_.release_sha = got;
    }
    return {};
}

Status Updater::finish_app() {
    std::lock_guard<std::mutex> swap(swap_mutex_);
    std::string path = pending_path(o_.state_dir);
    auto text = read_text(path, 1u << 16);
    if (!text.ok()) return {}; // none
    auto p = parse_json(*text);
    std::string staged = p.ok() ? (*p)["staged"].str() : "", old = p.ok() ? (*p)["old"].str() : "";
    std::string version = p.ok() ? (*p)["version"].str() : "";
    if (staged.empty() || !path_exists(staged)) {
        (void)remove_file(path);
        return make_error(Errc::invalid_data, "the staged app image is gone");
    }
    std::string dir = staged.substr(0, staged.rfind('/'));
    std::string final_path = join_path(dir, app_image_name(dir, version));
    ALEMBIC_TRY(rename_path(staged, final_path));
    // ShadowMount+ keeps the title on the old image while that exists; once
    // it is gone, its next scan mounts the new one.
    if (!old.empty() && old != final_path && is_regular_file(old)) {
        if (Status s = remove_file(old); !s.ok() && o_.log) o_.log("the old app image stays: " + s.error().message);
    }
    (void)remove_file(path);
    if (o_.log) o_.log("the app is now " + final_path);
    if (o_.smp_rescan) o_.smp_rescan();
    std::lock_guard<std::mutex> lock(mutex_);
    app_.path = final_path;
    app_.image = true;
    app_.version = version;
    app_.sha256 = p.ok() ? (*p)["sha256"].str() : "";
    return {};
}

void Updater::tick() {
    if (!app_pending()) return;
    if (o_.app_quiet && o_.app_quiet() < kAppQuietSeconds) return;
    if (Status s = finish_app(); !s.ok() && o_.log) o_.log("app update: " + s.error().message);
}

Status Updater::install_app() {
    if (o_.loader.empty()) return make_error(Errc::unsupported, "updating itself needs the engine running on a PS5");
    AppImage app;
    Release release;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != "idle") return make_error(Errc::invalid_argument, "the updater is busy");
        if (!has_release_ || release_.version != o_.current_version)
            return make_error(Errc::invalid_argument, "the latest release is not this engine's version; update everything instead");
        if (release_.app_url.empty()) return make_error(Errc::invalid_argument, "the latest release has no app image");
        if (app_.path.empty() || !app_.image) return make_error(Errc::invalid_argument, "no app image to replace was found");
        app = app_;
        release = release_;
        state_ = "downloading";
        error_.clear();
    }
    run([this, app, release] {
        auto sums = o_.get(release.sums_url, 1u << 20);
        Status s = sums.ok() ? stage_app(app, release, std::string(sums->begin(), sums->end())) : Status(sums.error());
        if (!s.ok()) {
            if (o_.log) o_.log("app update failed: " + s.error().message);
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = s.error().message;
        }
    });
    return {};
}

void Updater::set_state(const std::string& state) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
}

void Updater::run(std::function<void()> work) {
    // One task at a time; the last one has finished when the state is idle.
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this, work = std::move(work)] {
        work();
        set_state("idle");
    });
}

void Updater::check() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != "idle") return;
        state_ = "checking";
        error_.clear();
    }
    run([this] {
        auto body = o_.get(o_.release_url, 1u << 20);
        Result<Release> r = body.ok() ? parse_release(std::string(body->begin(), body->end())) : Result<Release>(body.error());
        bool newer = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            checked_ = unix_now();
            if (!r.ok()) {
                error_ = r.error().message;
            } else {
                has_release_ = true;
                release_ = *r;
                newer = compare_versions(release_.version, o_.current_version) > 0;
            }
        }
        if (!r.ok() && o_.log) o_.log("update check failed: " + r.error().message);
        if (r.ok() && o_.log) o_.log("latest release: " + r->version + (newer ? " (newer)" : ""));
        // A newer one on a console: where the payload lives, for the page.
        if (newer && !o_.loader.empty()) {
            set_state("searching");
            std::vector<Found> found = find_payloads(usual_places(), 3, r->version);
            std::lock_guard<std::mutex> lock(mutex_);
            found_ = std::move(found);
            searched_ = "usual";
        }
        // The console app, and whether the release's image differs from it.
        if (r.ok() && !o_.loader.empty()) {
            set_state("searching");
            AppImage app = find_app(true);
            if (!r->app_name.empty()) {
                auto sums = o_.get(r->sums_url, 1u << 20);
                if (sums.ok()) app.release_sha = checksum_for(std::string(sums->begin(), sums->end()), r->app_name);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            app_ = std::move(app);
            app_checked_ = true;
        }
    });
}

void Updater::find(bool everywhere) {
    std::string version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != "idle") return;
        state_ = "searching";
        version = has_release_ ? release_.version : o_.current_version;
    }
    run([this, everywhere, version] {
        std::vector<std::string> dirs = usual_places();
        if (everywhere && o_.roots)
            for (const std::string& r : o_.roots()) dirs.push_back(r);
        // Not the engine's own download.
        std::vector<Found> found = find_payloads(dirs, everywhere ? 6 : 3, version, {o_.state_dir + "/update"});
        std::lock_guard<std::mutex> lock(mutex_);
        found_ = std::move(found);
        searched_ = everywhere ? "everywhere" : "usual";
    });
}

Status Updater::install(const std::vector<std::string>& paths, bool app) {
    if (o_.loader.empty()) return make_error(Errc::unsupported, "updating itself needs the engine running on a PS5");
    std::vector<Found> chosen;
    Release release;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != "idle") return make_error(Errc::invalid_argument, "the updater is busy");
        if (!has_release_ || compare_versions(release_.version, o_.current_version) <= 0)
            return make_error(Errc::invalid_argument, "no newer release to install");
        // Only files the search found: the page can't name any other file.
        for (const std::string& p : paths) {
            auto it = std::find_if(found_.begin(), found_.end(), [&](const Found& f) { return f.path == p; });
            if (it == found_.end()) return make_error(Errc::invalid_argument, "not a payload the search found: " + p);
            chosen.push_back(*it);
        }
        release = release_;
        state_ = "downloading";
        error_.clear();
    }
    run([this, chosen, release, app] {
        Status s = do_install(chosen, release, app);
        if (!s.ok()) {
            if (o_.log) o_.log("update failed: " + s.error().message);
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = s.error().message;
        }
    });
    return {};
}

Status Updater::restart() {
    if (o_.loader.empty()) return make_error(Errc::unsupported, "restarting needs the engine running on a PS5");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != "idle") return make_error(Errc::invalid_argument, "the updater is busy");
        state_ = "restarting";
        error_.clear();
    }
    // The console started this engine from a socket, not a file: look for a
    // copy of the same version, its own update download first.
    std::vector<std::string> dirs = {o_.state_dir + "/update"};
    for (const std::string& d : usual_places()) dirs.push_back(d);
    // "" as the new version: the search otherwise leaves out files already at it.
    std::vector<Found> found = find_payloads(dirs, 3, "");
    const std::string own = o_.state_dir + "/update/";
    std::stable_partition(found.begin(), found.end(), [&](const Found& f) { return f.path.rfind(own, 0) == 0; });
    std::string elf, from;
    for (const Found& f : found) {
        if (f.version != o_.current_version) continue;
        auto data = read_text(f.path, kMaxPayload);
        if (!data.ok()) continue;
        PayloadId id = identify(bytes_of(*data));
        if (!id.alembic || id.version != o_.current_version) continue;
        elf = std::move(*data);
        from = f.path;
        break;
    }
    if (elf.empty()) {
        set_state("idle");
        return make_error(Errc::unsupported, "no copy of this engine's payload (Alembic " + o_.current_version +
                                               ") was found; load it again with your payload loader");
    }
    if (o_.log) o_.log("restarting from " + from);
    run([this, elf = std::move(elf)] {
        if (Status s = send_to_loader(o_.loader, elf); !s.ok()) {
            if (o_.log) o_.log("restart failed: " + s.error().message);
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = s.error().message;
            return;
        }
        if (o_.log) o_.log("Alembic " + o_.current_version + " handed to the ELF loader again; stopping");
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        if (o_.quit) o_.quit();
    });
    return {};
}

Status Updater::do_install(const std::vector<Found>& chosen, const Release& release, bool app) {
    // Download and check everything before touching a file.
    ALEMBIC_ASSIGN(std::vector<std::uint8_t> sums, o_.get(release.sums_url, 1u << 20));
    std::string want = checksum_for(std::string(sums.begin(), sums.end()), release.elf_name);
    if (want.empty()) return make_error(Errc::invalid_data, "SHA256SUMS does not list " + release.elf_name);
    ALEMBIC_ASSIGN(std::vector<std::uint8_t> data, o_.get(release.elf_url, kMaxPayload));
    std::string got = sha256_hex(data);
    if (got != want) return make_error(Errc::invalid_data, "the downloaded payload has an unexpected SHA-256 (" + got + "); nothing was changed");
    PayloadId id = identify(data);
    if (!id.alembic || id.version != release.version)
        return make_error(Errc::invalid_data, "the downloaded payload is not Alembic " + release.version + "; nothing was changed");
    std::string elf(data.begin(), data.end());
    ALEMBIC_TRY(write_text(o_.state_dir + "/update/" + release.elf_name, elf));
    if (o_.log) o_.log("downloaded " + release.elf_name + " (" + got + ")");

    // The app's image too, when it differs; it is swapped once the app is
    // quiet (by this engine or the new one). A failure here stops nothing.
    AppImage installed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        installed = app_;
    }
    if (app && installed.image && !release.app_url.empty() && installed.sha256 != checksum_for(std::string(sums.begin(), sums.end()), release.app_name)) {
        if (Status s = stage_app(installed, release, std::string(sums.begin(), sums.end())); !s.ok() && o_.log)
            o_.log("the app was not updated: " + s.error().message);
    }

    set_state("replacing");
    for (const Found& f : chosen) {
        if (Status s = replace_payload(f, elf); !s.ok())
            return make_error(s.error().code, "cannot replace " + f.path + ": " + s.error().message);
        if (o_.log) o_.log("replaced " + f.path + (f.rename_to.empty() ? "" : " (now " + f.rename_to + ")"));
    }

    set_state("starting");
    if (Status s = send_to_loader(o_.loader, elf); !s.ok())
        return make_error(s.error().code, s.error().message + (chosen.empty() ? "" : "; the payload files are already updated"));
    if (o_.log) o_.log("Alembic " + release.version + " handed to the ELF loader; stopping");
    // Let the page see "starting" before this engine goes.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    if (o_.quit) o_.quit();
    return {};
}

JsonValue Updater::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    JsonValue v = JsonValue::object();
    v.set("current", o_.current_version);
    v.set("state", state_);
    v.set("error", error_);
    v.set("checked", checked_);
    v.set("can_install", !o_.loader.empty());
    if (has_release_) {
        JsonValue r = JsonValue::object();
        r.set("version", release_.version);
        r.set("notes", release_.notes);
        r.set("url", release_.page_url);
        v.set("latest", std::move(r));
        v.set("newer", compare_versions(release_.version, o_.current_version) > 0);
    } else {
        v.set("latest", JsonValue());
        v.set("newer", false);
    }
    v.set("searched", searched_);
    JsonValue list = JsonValue::array();
    for (const Found& f : found_) {
        JsonValue e = JsonValue::object();
        e.set("path", f.path);
        e.set("version", f.version);
        e.set("rename_to", f.rename_to);
        e.set("loader", f.loader);
        list.push(std::move(e));
    }
    v.set("payloads", std::move(list));
    // The console app: null until looked for (a console only).
    if (app_checked_) {
        JsonValue a = JsonValue::object();
        a.set("path", app_.path);
        a.set("image", app_.image);
        a.set("version", app_.version);
        a.set("in_release", !app_.release_sha.empty());
        a.set("current", !app_.sha256.empty() && app_.sha256 == app_.release_sha);
        v.set("app", std::move(a));
    } else {
        v.set("app", JsonValue());
    }
    v.set("app_pending", app_pending());
    return v;
}

JsonValue Updater::summary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    JsonValue v = JsonValue::object();
    v.set("state", state_);
    v.set("latest", has_release_ ? JsonValue(release_.version) : JsonValue());
    v.set("newer", has_release_ && compare_versions(release_.version, o_.current_version) > 0);
    v.set("app_pending", app_pending());
    return v;
}

} // namespace alembic::engine::update
