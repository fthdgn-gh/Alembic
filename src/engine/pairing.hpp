// SPDX-License-Identifier: GPL-3.0-or-later
// Pairing two engines, so they can trust each other's requests later
// (another engine's files as inputs and outputs, jobs queued from afar).
//
// The person starts it on engine B ("Pair" next to engine A); A shows a
// six-digit code where only someone at A can see it (a PS5 notification, the
// engine's window, its page opened on that computer); they type it on B.
//
//   B -> A  begin   {from: B's identity, commit = SHA-256(pubB || nB)}
//   A -> B          {session, pubA, nA}
//   B -> A  reveal  {session, pubB, nB}     A checks the commitment
//   both:   shared = X25519, code = from HMAC(shared, transcript)
//   person types A's code on B; B compares
//   B -> A  finish  {session, HMAC(K, "B" | transcript)}
//   A -> B          {HMAC(K, "A" | transcript)}   both keep K
//
// X25519 keeps K from anyone who only watches. The code stops anyone in the
// middle: B commits to its values before it sees A's, and A shows its code
// before B reveals, so an attacker cannot search for values that give both
// sides the same code (one chance in a million). K = HMAC(shared, "key" |
// transcript); the transcript binds both ids, keys and nonces.
//
// Paired engines sign their requests to each other (sign / verify): HMAC(K,
// method, path, query, time, nonce, SHA-256(body)), within 2 minutes, each
// nonce once.
#pragma once

#include "http.hpp"
#include "json_value.hpp"
#include "keys.hpp"
#include "peers.hpp"

#include "alembic/result.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace alembic::engine::pairing {

struct Paired {
    std::string id;
    std::string name;
    keys::Key key{};
    std::int64_t since = 0; // Unix seconds
};

// Signed-request headers.
inline constexpr const char* kPeerHeader = "X-Alembic-Peer";
inline constexpr const char* kTimeHeader = "X-Alembic-Time";
inline constexpr const char* kNonceHeader = "X-Alembic-Nonce";
inline constexpr const char* kSignHeader = "X-Alembic-Sign";

class Pairing {
public:
    struct Hooks {
        std::function<void(const std::string&)> log;
        // A message for the person at this engine (a PS5 notification).
        std::function<void(const std::string&)> notify;
        // Pairing finished: keep the other engine's address (peers::Discovery::remember).
        std::function<void(const peers::Identity&, const std::string& address)> remember;
        std::function<std::string(const std::string&, const std::map<std::string, std::string>&)> translate;
    };
    Pairing(std::string state_dir, peers::Identity self, Hooks hooks);

    // ---- started here (B): the code is typed here
    // Exchanges keys with `other`; then the person types the code it shows.
    Status start(const peers::Peer& other);
    // The code shown on the other engine. Three wrong tries end the attempt.
    Result<Paired> confirm(const std::string& peer_id, const std::string& code);
    void cancel(const std::string& peer_id);
    bool waiting_for_code(const std::string& peer_id) const;

    // ---- started there (A): the code is shown here. `from` is the caller's address.
    Result<JsonValue> on_begin(const JsonValue& body, const std::string& from);
    Result<JsonValue> on_reveal(const JsonValue& body);
    Result<JsonValue> on_finish(const JsonValue& body);
    void on_cancel(const JsonValue& body);
    struct Incoming {
        std::string from_name;
        std::string code; // "" until the other side has revealed
        std::int64_t expires_in = 0;
    };
    std::optional<Incoming> incoming() const;
    void cancel_incoming();

    // ---- paired engines
    std::vector<Paired> list() const;
    bool is_paired(const std::string& id) const;
    // Forgets it here (the caller tells the other engine first, while it can still sign).
    Status forget(const std::string& id);

    // Headers that let `peer_id` check a request came from this engine.
    Result<std::vector<std::pair<std::string, std::string>>> sign(const std::string& peer_id, const std::string& method,
                                                                  const std::string& path,
                                                                  const std::map<std::string, std::string>& query,
                                                                  const std::string& body);
    // The paired engine that signed `r`, or why it is refused.
    Result<std::string> verify(const HttpRequest& r);

private:
    struct Outgoing {
        std::string session, address, name;
        std::uint16_t port = 0;
        keys::Key shared{};
        std::string transcript; // hex digest
        std::string code;
        int wrong = 0;
        std::int64_t expires = 0;
    };
    struct Session {
        std::string id;
        peers::Identity from;
        std::string address;
        keys::Key secret{}, pub{};
        std::array<std::uint8_t, 16> nonce{};
        keys::Key commit{};
        bool revealed = false;
        keys::Key shared{};
        std::string transcript;
        std::string code;
        int failures = 0;
        std::int64_t expires = 0;
    };

    Status save_locked() const;
    void load();
    std::string t(const std::string& text, const std::map<std::string, std::string>& args = {}) const;

    std::string state_dir_;
    peers::Identity self_;
    Hooks hooks_;
    mutable std::mutex mutex_;
    std::map<std::string, Paired> paired_;    // by id
    std::map<std::string, Outgoing> outgoing_; // by the other engine's id
    std::optional<Session> incoming_;
    std::vector<std::int64_t> recent_begins_;  // a few attempts a minute at most
    std::map<std::string, std::int64_t> nonces_; // seen in signed requests, by when
};

// The code and key both sides compute (also for tests).
std::string transcript(const std::string& id_a, const std::string& id_b, const keys::Key& pub_a, const keys::Key& pub_b,
                       Bytes nonce_a, Bytes nonce_b);
std::string code_for(const keys::Key& shared, const std::string& transcript);
keys::Key key_for(const keys::Key& shared, const std::string& transcript);

} // namespace alembic::engine::pairing
