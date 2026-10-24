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
        else if (ends_with(lower(name), ".elf") && r.elf_url.empty()) {
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

Updater::Updater(Options options) : o_(std::move(options)) {}

Updater::~Updater() {
    if (thread_.joinable()) thread_.join();
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

Status Updater::install(const std::vector<std::string>& paths) {
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
    run([this, chosen, release] {
        Status s = do_install(chosen, release);
        if (!s.ok()) {
            if (o_.log) o_.log("update failed: " + s.error().message);
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = s.error().message;
        }
    });
    return {};
}

Status Updater::do_install(const std::vector<Found>& chosen, const Release& release) {
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
    return v;
}

JsonValue Updater::summary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    JsonValue v = JsonValue::object();
    v.set("state", state_);
    v.set("latest", has_release_ ? JsonValue(release_.version) : JsonValue());
    v.set("newer", has_release_ && compare_versions(release_.version, o_.current_version) > 0);
    return v;
}

} // namespace alembic::engine::update
