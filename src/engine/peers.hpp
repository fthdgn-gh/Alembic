// SPDX-License-Identifier: GPL-3.0-or-later
// Other Alembic engines on the network. With discovery on, an engine
// announces itself every few seconds with a small UDP broadcast (a JSON
// "beacon" on port 5940) and lists the engines it hears; engines that fall
// silent drop off after a while. Engines can also be added by address, for
// networks that drop broadcasts: those are saved (<state>/peers.json) and
// checked through their /api/info.
//
// Discovery shares only what /api/info already shows to anyone who can open
// the page. Reading or writing another engine's files comes with pairing.
#pragma once

#include "alembic/result.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace alembic::engine::peers {

inline constexpr std::uint16_t kDiscoveryPort = 5940; // UDP (the page is on TCP 5940)

// What an engine says about itself.
struct Identity {
    std::string id;       // 32 hex digits, kept in <state>/engine_id
    std::string name;     // the computer's name, or "PS5"
    std::string os;       // platform::os_description()
    std::string platform; // "ps5" or "host"
    std::string version;
    std::uint16_t port = 0; // its page and API (TCP)
};

struct Peer {
    Identity who;
    std::string address;        // where its beacon came from, or the address it was added with
    std::int64_t last_seen = 0; // Unix seconds
    bool manual = false;        // added by address (kept), else heard
    bool reachable = true;      // manual: whether the last check answered
    std::string problem;        // manual: why not
};

// The engine's id: read from <state>/engine_id, made (random) the first time.
std::string engine_id(const std::string& state_dir);

// A beacon (encode) and its checks (decode fails for anything else).
std::string encode_beacon(const Identity& self, bool query);
struct Beacon {
    Identity who;
    bool query = false; // the sender asks everyone to answer now
};
Result<Beacon> decode_beacon(const std::string& text);

class Discovery {
public:
    using Log = std::function<void(const std::string&)>;
    Discovery(std::string state_dir, Identity self, std::uint16_t udp_port = kDiscoveryPort, Log log = {});
    ~Discovery();
    Discovery(const Discovery&) = delete;
    Discovery& operator=(const Discovery&) = delete;

    // Starts the threads (and broadcasting, if it was left on).
    void start();
    void stop();

    // Broadcasting and listening; remembered in <state>/discovery.on (off by default).
    bool enabled() const { return enabled_; }
    Status set_enabled(bool on);
    bool listening() const { return socket_ok_; } // false when the UDP port could not be opened

    // Heard and added engines, most recently seen first.
    std::vector<Peer> list() const;
    // Asks host:port for its /api/info; adds and saves it when it answers.
    Result<Peer> add(const std::string& host, std::uint16_t port);
    Status remove(const std::string& id);

private:
    void listen_loop();
    void announce_loop();
    void check_manual();
    void heard(const Beacon& b, const std::string& from);
    void load_manual();
    Status save_manual_locked() const;

    std::string state_dir_;
    Identity self_;
    std::uint16_t udp_port_;
    Log log_;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> socket_ok_{false};
    std::atomic<bool> stopping_{false};
    std::intptr_t socket_ = -1; // net::Socket
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::map<std::string, Peer> peers_; // by id
    std::int64_t last_reply_ = 0;       // answers to queries, at most one a second
    std::thread listener_, announcer_;
};

} // namespace alembic::engine::peers
