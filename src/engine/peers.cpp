// SPDX-License-Identifier: GPL-3.0-or-later
#include "peers.hpp"

#include "json_value.hpp"
#include "text_file.hpp"
#include "net.hpp"

#include "alembic/io.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <random>

namespace alembic::engine::peers {

namespace {

constexpr int kAnnounceSeconds = 5;
constexpr int kForgetSeconds = 20;      // a heard engine silent this long drops off
constexpr int kCheckManualSeconds = 15; // added engines: how often /api/info is asked
constexpr std::size_t kMaxText = 80;    // longest name, system or version kept from a beacon
constexpr std::size_t kMaxPeers = 64;

std::int64_t now() { return static_cast<std::int64_t>(std::time(nullptr)); }

bool is_id(const std::string& s) {
    return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// Text from the network: no control characters, not too long.
std::string clean(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; }), s.end());
    if (s.size() > kMaxText) {
        s.resize(kMaxText);
        // Drop a UTF-8 sequence the cut left incomplete.
        std::size_t lead = s.size();
        while (lead > 0 && (static_cast<unsigned char>(s[lead - 1]) & 0xC0) == 0x80) --lead;
        if (lead > 0) {
            unsigned char c = static_cast<unsigned char>(s[lead - 1]);
            std::size_t want = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            if (s.size() - (lead - 1) < want) s.resize(lead - 1);
        }
    }
    return s;
}

} // namespace

Result<Identity> identity_from_json(const JsonValue& v) {
    Identity who;
    who.id = v["id"].str();
    std::int64_t port = v["port"].integer(0);
    if (!is_id(who.id)) return make_error(Errc::invalid_data, "no engine id");
    if (port < 1 || port > 65535) return make_error(Errc::invalid_data, "no port");
    who.port = static_cast<std::uint16_t>(port);
    who.name = clean(v["name"].str());
    who.os = clean(v["os"].str());
    who.platform = clean(v["platform"].str());
    who.version = clean(v["version"].str());
    std::int64_t protocol = v["protocol"].integer(1);
    who.protocol = protocol >= 1 && protocol <= 1000000 ? static_cast<int>(protocol) : 1;
    return who;
}

std::string protocol_problem(const Identity& other) {
    if (other.protocol == kPeerProtocol) return "";
    const std::string numbers = " (engine protocol " + std::to_string(other.protocol) + ", this one " + std::to_string(kPeerProtocol) + ")";
    return other.protocol < kPeerProtocol ? other.name + " has an older Alembic" + numbers + ": update it to use them together"
                                          : other.name + " has a newer Alembic" + numbers + ": update this engine to use them together";
}

std::string protocol_problem(const std::string& header_value) {
    int theirs = header_value.empty() ? 1 : std::atoi(header_value.c_str());
    if (theirs == kPeerProtocol) return "";
    return "the other engine speaks engine protocol " + std::to_string(theirs) + " and this one " + std::to_string(kPeerProtocol) +
           ": update Alembic on the older one";
}

JsonValue identity_json(const Identity& who) {
    JsonValue v = JsonValue::object();
    v.set("id", who.id);
    v.set("name", who.name);
    v.set("os", who.os);
    v.set("platform", who.platform);
    v.set("version", who.version);
    v.set("port", static_cast<std::int64_t>(who.port));
    v.set("protocol", static_cast<std::int64_t>(who.protocol));
    return v;
}

std::string engine_id(const std::string& state_dir) {
    std::string path = state_dir + "/engine_id";
    if (auto text = read_text(path); text.ok()) {
        std::string id = text->substr(0, 32);
        if (is_id(id)) return id;
    }
    std::random_device device;
    static const char hex[] = "0123456789abcdef";
    std::string id;
    for (int i = 0; i < 32; ++i) id += hex[device() & 15];
    (void)make_parent_directories(path);
    (void)write_text(path, id + "\n");
    return id;
}

std::string encode_beacon(const Identity& self, bool query) {
    JsonValue v = identity_json(self);
    v.set("alembic", "engine");
    v.set("v", static_cast<std::int64_t>(1));
    if (query) v.set("query", true);
    return v.dump();
}

Result<Beacon> decode_beacon(const std::string& text) {
    if (text.size() > 2048) return make_error(Errc::invalid_data, "too large");
    ALEMBIC_ASSIGN(JsonValue v, parse_json(text));
    if (!v.is_object() || v["alembic"].str() != "engine" || v["v"].integer(0) != 1)
        return make_error(Errc::invalid_data, "not an Alembic beacon");
    Beacon b;
    ALEMBIC_ASSIGN(b.who, identity_from_json(v));
    b.query = v["query"].boolean(false);
    return b;
}

Discovery::Discovery(std::string state_dir, Identity self, std::uint16_t udp_port, Log log)
    : state_dir_(std::move(state_dir)), self_(std::move(self)), udp_port_(udp_port), log_(std::move(log)) {
    enabled_ = path_exists(state_dir_ + "/discovery.on");
    load_manual();
}

Discovery::~Discovery() { stop(); }

void Discovery::start() {
    stopping_ = false;
    if (enabled_) (void)set_enabled(true);
    listener_ = std::thread([this] { listen_loop(); });
    announcer_ = std::thread([this] { announce_loop(); });
}

void Discovery::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    wake_.notify_all();
    if (listener_.joinable()) listener_.join();
    if (announcer_.joinable()) announcer_.join();
    if (socket_ != net::kNone) net::close(socket_);
    socket_ = net::kNone;
}

Status Discovery::set_enabled(bool on) {
    std::string marker = state_dir_ + "/discovery.on";
    if (on) {
        std::lock_guard<std::mutex> lock(mutex_);
        // The socket opens with the first switch-on and stays until stop():
        // the listener may be waiting on it.
        if (socket_ == net::kNone) {
            socket_ = net::udp_socket(udp_port_);
            socket_ok_ = socket_ != net::kNone;
            if (!socket_ok_ && log_) log_("discovery: cannot open UDP port " + std::to_string(udp_port_) + ": " + net::error_text());
        }
        if (!path_exists(marker)) {
            (void)make_parent_directories(marker);
            ALEMBIC_TRY(write_text(marker, "on\n"));
        }
    } else {
        if (path_exists(marker)) ALEMBIC_TRY(remove_file(marker));
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = peers_.begin(); it != peers_.end();) it = it->second.manual ? std::next(it) : peers_.erase(it);
    }
    bool was = enabled_.exchange(on);
    if (on && !was && log_) log_("discovery on: announcing on UDP " + std::to_string(udp_port_));
    if (!on && was && log_) log_("discovery off");
    wake_.notify_all(); // announce at once
    return {};
}

std::vector<Peer> Discovery::list() const {
    std::vector<Peer> out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, p] : peers_) out.push_back(p);
    }
    std::sort(out.begin(), out.end(), [](const Peer& a, const Peer& b) {
        if (a.last_seen != b.last_seen) return a.last_seen > b.last_seen;
        return a.who.name < b.who.name;
    });
    return out;
}

void Discovery::heard(const Beacon& b, const std::string& from) {
    if (b.who.id == self_.id) return; // our own broadcast
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(b.who.id);
    if (it == peers_.end()) {
        if (peers_.size() >= kMaxPeers) return;
        if (log_) log_("discovery: found " + b.who.name + " (" + b.who.os + ") at " + from + ":" + std::to_string(b.who.port));
        it = peers_.emplace(b.who.id, Peer{}).first;
    }
    Peer& p = it->second;
    p.who = b.who;
    p.address = from; // where it really is, whatever it says
    p.last_seen = now();
    p.reachable = true;
    p.problem.clear();
    if (p.manual) (void)save_manual_locked();
}

void Discovery::listen_loop() {
    char buffer[2048];
    while (!stopping_) {
        net::Socket s;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!enabled_ || socket_ == net::kNone) {
                wake_.wait_for(lock, std::chrono::seconds(1), [&] { return stopping_.load(); });
                continue;
            }
            s = socket_;
        }
        std::string from;
        long n = net::receive_from(s, buffer, sizeof buffer, 500, &from);
        if (n <= 0 || !enabled_) continue;
        auto beacon = decode_beacon(std::string(buffer, static_cast<std::size_t>(n)));
        if (!beacon.ok()) continue;
        heard(*beacon, from);
        // Someone just switched on: answer at once (at most once a second).
        if (beacon->query && beacon->who.id != self_.id) {
            std::int64_t t = now();
            bool reply = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (t != last_reply_) {
                    last_reply_ = t;
                    reply = true;
                }
            }
            // A broadcast, not a reply to `from`: engines sharing the port on one
            // computer would each get only some of the unicast datagrams.
            if (reply) {
                std::string text = encode_beacon(self_, false);
                for (const std::string& to : net::broadcast_addresses()) net::send_to(s, to, udp_port_, text.data(), text.size());
            }
        }
    }
}

void Discovery::announce_loop() {
    bool was_on = false;
    std::int64_t next_check = 0;
    while (!stopping_) {
        bool on = enabled_;
        net::Socket s;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            s = socket_;
            // Heard engines that went quiet drop off; added ones stay.
            std::int64_t t = now();
            for (auto it = peers_.begin(); it != peers_.end();)
                it = !it->second.manual && t - it->second.last_seen > kForgetSeconds ? peers_.erase(it) : std::next(it);
        }
        if (on && s != net::kNone) {
            std::string text = encode_beacon(self_, !was_on); // the first one asks everyone to answer
            for (const std::string& to : net::broadcast_addresses()) net::send_to(s, to, udp_port_, text.data(), text.size());
        }
        was_on = on;
        if (now() >= next_check) {
            check_manual();
            next_check = now() + kCheckManualSeconds;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::seconds(kAnnounceSeconds),
                       [&] { return stopping_.load() || enabled_.load() != was_on; });
    }
}

namespace {
Result<Identity> ask_info(const std::string& host, std::uint16_t port) {
    ALEMBIC_ASSIGN(net::HttpReply r, net::http_request(host, port, "GET", "/api/info", "", 3000, 1u << 20));
    if (r.status != 200) return make_error(Errc::io, host + ":" + std::to_string(port) + " answered HTTP " + std::to_string(r.status));
    auto v = parse_json(r.body);
    if (!v.ok() || (*v)["name"].str() != "Alembic") return make_error(Errc::invalid_data, host + ":" + std::to_string(port) + " is not an Alembic engine");
    JsonValue info = *v;
    // /api/info names the computer "host_name"; the beacon calls it "name".
    JsonValue who = JsonValue::object();
    who.set("id", info["id"].str());
    std::string name = info["host_name"].str();
    who.set("name", name.empty() ? (info["platform"].str() == "ps5" ? std::string("PS5") : host) : name);
    who.set("os", info["os"].str());
    who.set("platform", info["platform"].str());
    who.set("version", info["version"].str());
    who.set("port", static_cast<std::int64_t>(port));
    who.set("protocol", info["protocol"].integer(1));
    auto id = identity_from_json(who);
    if (!id.ok()) return make_error(Errc::invalid_data, host + ":" + std::to_string(port) + " is an older engine (no id); update it");
    return id;
}
} // namespace

void Discovery::check_manual() {
    std::vector<Peer> manual;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, p] : peers_)
            if (p.manual) manual.push_back(p);
    }
    for (const Peer& p : manual) {
        if (stopping_) return;
        auto who = ask_info(p.address, p.who.port);
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = peers_.find(p.who.id);
        if (it == peers_.end()) continue;
        if (who.ok() && who->id == p.who.id) {
            it->second.who = *who;
            it->second.last_seen = now();
            it->second.reachable = true;
            it->second.problem.clear();
        } else {
            it->second.reachable = false;
            it->second.problem = who.ok() ? "another engine answers at this address now" : who.error().message;
        }
    }
}

Result<Peer> Discovery::add(const std::string& host, std::uint16_t port) {
    if (host.empty() || host.size() > 253 || host.find_first_of(" /\\?#@") != std::string::npos)
        return make_error(Errc::invalid_argument, "not an address: " + host);
    ALEMBIC_ASSIGN(Identity who, ask_info(host, port));
    if (who.id == self_.id) return make_error(Errc::invalid_argument, "that is this engine");
    std::lock_guard<std::mutex> lock(mutex_);
    if (peers_.size() >= kMaxPeers && !peers_.count(who.id)) return make_error(Errc::too_large, "too many engines");
    Peer& p = peers_[who.id];
    p.who = who;
    p.address = host;
    p.last_seen = now();
    p.manual = true;
    p.reachable = true;
    p.problem.clear();
    ALEMBIC_TRY(save_manual_locked());
    if (log_) log_("engine " + who.name + " added at " + host + ":" + std::to_string(port));
    return p;
}

void Discovery::remember(const Identity& who, const std::string& address) {
    if (who.id == self_.id) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Peer& p = peers_[who.id];
    p.who = who;
    p.address = address;
    p.last_seen = now();
    p.manual = true;
    p.reachable = true;
    p.problem.clear();
    (void)save_manual_locked();
}

std::optional<Peer> Discovery::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(id);
    if (it == peers_.end()) return std::nullopt;
    return it->second;
}

Status Discovery::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(id);
    if (it == peers_.end()) return make_error(Errc::invalid_argument, "no such engine");
    bool manual = it->second.manual;
    peers_.erase(it);
    return manual ? save_manual_locked() : Status{};
}

void Discovery::load_manual() {
    auto text = read_text(state_dir_ + "/peers.json");
    if (!text.ok()) return;
    auto v = parse_json(*text);
    if (!v.ok()) return;
    for (const JsonValue& item : (*v)["engines"].items()) {
        auto who = identity_from_json(item);
        std::string address = item["address"].str();
        if (!who.ok() || address.empty() || who->id == self_.id) continue;
        Peer p;
        p.who = *who;
        p.address = address;
        p.manual = true;
        p.reachable = false; // until the first check
        p.problem = "not checked yet";
        p.last_seen = item["last_seen"].integer(0);
        peers_[p.who.id] = p;
    }
}

Status Discovery::save_manual_locked() const {
    JsonValue list = JsonValue::array();
    for (const auto& [id, p] : peers_) {
        if (!p.manual) continue;
        JsonValue v = identity_json(p.who);
        v.set("address", p.address);
        v.set("last_seen", p.last_seen);
        list.push(std::move(v));
    }
    JsonValue root = JsonValue::object();
    root.set("engines", std::move(list));
    return write_text(state_dir_ + "/peers.json", root.dump());
}

} // namespace alembic::engine::peers
