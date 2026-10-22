// SPDX-License-Identifier: GPL-3.0-or-later
#include "pairing.hpp"

#include "net.hpp"
#include "platform.hpp"

#include "alembic/io.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace alembic::engine::pairing {

namespace {

constexpr std::int64_t kSessionSeconds = 120;  // to type the code
constexpr std::int64_t kClockSlack = 120;      // signed requests: this far from our clock
constexpr int kWrongCodes = 3;
constexpr std::size_t kBeginsPerMinute = 6;

std::int64_t now() { return static_cast<std::int64_t>(std::time(nullptr)); }

Bytes bytes_of(const std::string& s) { return Bytes(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }

Result<keys::Key> random_key() {
    keys::Key k{};
    ALEMBIC_TRY(platform::secure_random(k.data(), k.size()));
    return k;
}

bool hex_key(const std::string& hex, keys::Key& out) { return keys::from_hex(hex, MutableBytes(out.data(), out.size())); }

bool all_zero(const keys::Key& k) {
    std::uint8_t d = 0;
    for (std::uint8_t b : k) d |= b;
    return d == 0;
}

keys::Key commitment(const keys::Key& pub, Bytes nonce) {
    std::string m(pub.begin(), pub.end());
    m.append(nonce.begin(), nonce.end());
    return keys::sha256(m);
}

Result<std::string> read_text(const std::string& path) {
    ALEMBIC_ASSIGN(File file, File::open(path, File::Mode::read));
    ALEMBIC_ASSIGN(std::uint64_t size, file.size());
    if (size > 1u << 20) return make_error(Errc::too_large, path + " is too large");
    std::string text(static_cast<std::size_t>(size), '\0');
    ALEMBIC_TRY(file.read_at(0, MutableBytes(reinterpret_cast<std::uint8_t*>(text.data()), text.size())));
    return text;
}

Status write_text(const std::string& path, const std::string& text) {
    ALEMBIC_ASSIGN(AtomicOutput out, AtomicOutput::create(path, true));
    ALEMBIC_TRY(out.file().write_at(0, bytes_of(text)));
    return out.commit();
}

// What a signature covers: one line each, the query sorted by key.
std::string canonical(const std::string& method, const std::string& path, const std::map<std::string, std::string>& query,
                      const std::string& time, const std::string& nonce, const std::string& body) {
    std::string q;
    for (const auto& [k, v] : query) q += k + "=" + v + "\n";
    return "alembic-request-v1\n" + method + "\n" + path + "\n" + q + "\n" + time + "\n" + nonce + "\n" +
           keys::to_hex(keys::sha256(body));
}

// The other engine's JSON reply, or its error message.
Result<JsonValue> post(const std::string& host, std::uint16_t port, const std::string& path, const JsonValue& body,
                       const net::Headers& headers = {}) {
    ALEMBIC_ASSIGN(net::HttpReply r, net::http_request(host, port, "POST", path, body.dump(), 5000, 1u << 20, headers));
    auto v = parse_json(r.body);
    if (r.status != 200) {
        std::string why = v.ok() ? (*v)["error"].str() : "";
        return make_error(Errc::io, why.empty() ? "the other engine answered HTTP " + std::to_string(r.status) : why);
    }
    if (!v.ok()) return make_error(Errc::invalid_data, "the other engine's reply is not JSON");
    return *v;
}

} // namespace

std::string transcript(const std::string& id_a, const std::string& id_b, const keys::Key& pub_a, const keys::Key& pub_b,
                       Bytes nonce_a, Bytes nonce_b) {
    return keys::to_hex(keys::sha256("alembic-pair-v1\n" + id_a + "\n" + id_b + "\n" + keys::to_hex(pub_a) + "\n" +
                                     keys::to_hex(pub_b) + "\n" + keys::to_hex(nonce_a) + "\n" + keys::to_hex(nonce_b)));
}

std::string code_for(const keys::Key& shared, const std::string& t) {
    keys::Key h = keys::hmac_sha256(shared, "code|" + t);
    std::uint32_t n = (std::uint32_t(h[0]) << 24 | std::uint32_t(h[1]) << 16 | std::uint32_t(h[2]) << 8 | h[3]) % 1000000u;
    char text[8];
    std::snprintf(text, sizeof text, "%06u", n);
    return text;
}

keys::Key key_for(const keys::Key& shared, const std::string& t) { return keys::hmac_sha256(shared, "key|" + t); }

Pairing::Pairing(std::string state_dir, peers::Identity self, Hooks hooks)
    : state_dir_(std::move(state_dir)), self_(std::move(self)), hooks_(std::move(hooks)) {
    load();
}

std::string Pairing::t(const std::string& text, const std::map<std::string, std::string>& args) const {
    if (hooks_.translate) return hooks_.translate(text, args);
    std::string out = text;
    for (const auto& [k, v] : args) {
        std::string key = "{" + k + "}";
        for (std::size_t at; (at = out.find(key)) != std::string::npos;) out.replace(at, key.size(), v);
    }
    return out;
}

// ------------------------------------------------------------------ B: started here

Status Pairing::start(const peers::Peer& other) {
    if (other.who.id == self_.id) return make_error(Errc::invalid_argument, "that is this engine");
    ALEMBIC_ASSIGN(keys::Key secret, random_key());
    keys::Key pub = keys::x25519_base(secret);
    std::array<std::uint8_t, 16> nonce{};
    ALEMBIC_TRY(platform::secure_random(nonce.data(), nonce.size()));

    JsonValue begin = JsonValue::object();
    begin.set("from", peers::identity_json(self_));
    begin.set("commit", keys::to_hex(commitment(pub, Bytes(nonce.data(), nonce.size()))));
    ALEMBIC_ASSIGN(JsonValue reply, post(other.address, other.who.port, "/api/pair/begin", begin));
    if (reply["id"].str() != other.who.id) return make_error(Errc::invalid_data, "another engine answered at that address");
    keys::Key their_pub{};
    std::array<std::uint8_t, 16> their_nonce{};
    std::string session = reply["session"].str();
    if (session.size() != 32 || !hex_key(reply["pub"].str(), their_pub) ||
        !keys::from_hex(reply["nonce"].str(), MutableBytes(their_nonce.data(), their_nonce.size())))
        return make_error(Errc::invalid_data, "the other engine's reply is not a pairing reply");

    JsonValue reveal = JsonValue::object();
    reveal.set("session", session);
    reveal.set("pub", keys::to_hex(pub));
    reveal.set("nonce", keys::to_hex(Bytes(nonce.data(), nonce.size())));
    ALEMBIC_TRY(post(other.address, other.who.port, "/api/pair/reveal", reveal));

    keys::Key shared = keys::x25519(secret, their_pub);
    if (all_zero(shared)) return make_error(Errc::invalid_data, "the other engine's key is not usable");
    Outgoing o;
    o.session = session;
    o.address = other.address;
    o.port = other.who.port;
    o.name = other.who.name;
    o.shared = shared;
    // A is the engine that shows the code, B the one it is typed on (here).
    o.transcript = transcript(other.who.id, self_.id, their_pub, pub, Bytes(their_nonce.data(), 16), Bytes(nonce.data(), 16));
    o.code = code_for(shared, o.transcript);
    o.expires = now() + kSessionSeconds;
    std::lock_guard<std::mutex> lock(mutex_);
    outgoing_[other.who.id] = o;
    if (hooks_.log) hooks_.log("pairing with " + other.who.name + " (" + other.address + "): waiting for the code it shows");
    return {};
}

bool Pairing::waiting_for_code(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = outgoing_.find(peer_id);
    return it != outgoing_.end() && it->second.expires > now();
}

Result<Paired> Pairing::confirm(const std::string& peer_id, const std::string& typed) {
    std::string code;
    for (char c : typed)
        if (c >= '0' && c <= '9') code += c;
    Outgoing o;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = outgoing_.find(peer_id);
        if (it == outgoing_.end() || it->second.expires <= now()) {
            if (it != outgoing_.end()) outgoing_.erase(it);
            return make_error(Errc::invalid_argument, "this pairing has expired; start again");
        }
        if (!keys::equal(bytes_of(code), bytes_of(it->second.code))) {
            if (++it->second.wrong < kWrongCodes) return make_error(Errc::invalid_argument, "that is not the code; check it and try again");
            o = it->second;
            outgoing_.erase(it);
            code.clear();
        } else {
            o = it->second;
            outgoing_.erase(it);
        }
    }
    if (code.empty()) { // too many wrong codes: end it there too
        JsonValue c = JsonValue::object();
        c.set("session", o.session);
        (void)post(o.address, o.port, "/api/pair/cancel", c);
        return make_error(Errc::invalid_argument, "wrong code three times; start again");
    }
    keys::Key key = key_for(o.shared, o.transcript);
    JsonValue finish = JsonValue::object();
    finish.set("session", o.session);
    finish.set("proof", keys::to_hex(keys::hmac_sha256(key, "B|" + o.transcript)));
    ALEMBIC_ASSIGN(JsonValue reply, post(o.address, o.port, "/api/pair/finish", finish));
    keys::Key proof{};
    keys::Key expected = keys::hmac_sha256(key, "A|" + o.transcript);
    if (!hex_key(reply["proof"].str(), proof) || !keys::equal(Bytes(proof.data(), 32), Bytes(expected.data(), 32)))
        return make_error(Errc::invalid_data, "the other engine could not prove it has the same key");
    Paired p;
    p.id = peer_id;
    p.name = o.name;
    p.key = key;
    p.since = now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paired_[p.id] = p;
        ALEMBIC_TRY(save_locked());
    }
    if (hooks_.log) hooks_.log("paired with " + o.name + " (" + o.address + ")");
    return p;
}

void Pairing::cancel(const std::string& peer_id) {
    Outgoing o;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = outgoing_.find(peer_id);
        if (it == outgoing_.end()) return;
        o = it->second;
        outgoing_.erase(it);
    }
    JsonValue c = JsonValue::object();
    c.set("session", o.session);
    (void)post(o.address, o.port, "/api/pair/cancel", c);
}

// ------------------------------------------------------------------ A: started there

Result<JsonValue> Pairing::on_begin(const JsonValue& body, const std::string& from) {
    ALEMBIC_ASSIGN(peers::Identity who, peers::identity_from_json(body["from"]));
    if (who.id == self_.id) return make_error(Errc::invalid_argument, "that is this engine");
    keys::Key commit{};
    if (!hex_key(body["commit"].str(), commit)) return make_error(Errc::invalid_argument, "no commitment");
    Session s;
    ALEMBIC_ASSIGN(s.secret, random_key());
    s.pub = keys::x25519_base(s.secret);
    ALEMBIC_TRY(platform::secure_random(s.nonce.data(), s.nonce.size()));
    std::array<std::uint8_t, 16> id{};
    ALEMBIC_TRY(platform::secure_random(id.data(), id.size()));
    s.id = keys::to_hex(Bytes(id.data(), id.size()));
    s.from = who;
    s.address = from;
    s.commit = commit;
    s.expires = now() + kSessionSeconds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::int64_t t = now();
        if (incoming_ && incoming_->expires > t && incoming_->from.id != who.id)
            return make_error(Errc::unsupported, "another engine is pairing with it right now; try again in a minute");
        recent_begins_.erase(std::remove_if(recent_begins_.begin(), recent_begins_.end(), [&](std::int64_t b) { return t - b > 60; }),
                             recent_begins_.end());
        if (recent_begins_.size() >= kBeginsPerMinute) return make_error(Errc::unsupported, "too many pairing attempts; wait a minute");
        recent_begins_.push_back(t);
        incoming_ = s;
    }
    JsonValue reply = JsonValue::object();
    reply.set("session", s.id);
    reply.set("id", self_.id);
    reply.set("name", self_.name);
    reply.set("pub", keys::to_hex(s.pub));
    reply.set("nonce", keys::to_hex(Bytes(s.nonce.data(), s.nonce.size())));
    return reply;
}

Result<JsonValue> Pairing::on_reveal(const JsonValue& body) {
    keys::Key pub{};
    std::array<std::uint8_t, 16> nonce{};
    if (!hex_key(body["pub"].str(), pub) || !keys::from_hex(body["nonce"].str(), MutableBytes(nonce.data(), nonce.size())))
        return make_error(Errc::invalid_argument, "no key");
    std::string name, code;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!incoming_ || incoming_->id != body["session"].str() || incoming_->expires <= now() || incoming_->revealed)
            return make_error(Errc::invalid_argument, "no such pairing");
        Session& s = *incoming_;
        keys::Key c = commitment(pub, Bytes(nonce.data(), nonce.size()));
        if (!keys::equal(Bytes(c.data(), 32), Bytes(s.commit.data(), 32))) {
            incoming_.reset();
            return make_error(Errc::invalid_argument, "the key does not match its commitment");
        }
        s.shared = keys::x25519(s.secret, pub);
        if (all_zero(s.shared)) {
            incoming_.reset();
            return make_error(Errc::invalid_argument, "that key is not usable");
        }
        s.transcript = transcript(self_.id, s.from.id, s.pub, pub, Bytes(s.nonce.data(), 16), Bytes(nonce.data(), 16));
        s.code = code_for(s.shared, s.transcript);
        s.revealed = true;
        name = s.from.name;
        code = s.code.substr(0, 3) + " " + s.code.substr(3);
    }
    if (hooks_.log) hooks_.log("pairing requested by " + name + ": enter " + code + " there");
    if (hooks_.notify) hooks_.notify(t("Pairing with {name}: enter {code} there", {{"name", name}, {"code", code}}));
    return JsonValue::object();
}

Result<JsonValue> Pairing::on_finish(const JsonValue& body) {
    keys::Key proof{};
    if (!hex_key(body["proof"].str(), proof)) return make_error(Errc::invalid_argument, "no proof");
    Session s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!incoming_ || incoming_->id != body["session"].str() || incoming_->expires <= now() || !incoming_->revealed)
            return make_error(Errc::invalid_argument, "no such pairing");
        s = *incoming_;
    }
    keys::Key key = key_for(s.shared, s.transcript);
    keys::Key expected = keys::hmac_sha256(key, "B|" + s.transcript);
    if (!keys::equal(Bytes(proof.data(), 32), Bytes(expected.data(), 32))) {
        std::lock_guard<std::mutex> lock(mutex_);
        incoming_.reset();
        return make_error(Errc::invalid_argument, "the proof does not match; start again");
    }
    Paired p;
    p.id = s.from.id;
    p.name = s.from.name;
    p.key = key;
    p.since = now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        incoming_.reset();
        paired_[p.id] = p;
        ALEMBIC_TRY(save_locked());
    }
    if (hooks_.remember) hooks_.remember(s.from, s.address);
    if (hooks_.log) hooks_.log("paired with " + p.name + " (" + s.address + ")");
    if (hooks_.notify) hooks_.notify(t("Paired with {name}", {{"name", p.name}}));
    JsonValue reply = JsonValue::object();
    reply.set("proof", keys::to_hex(keys::hmac_sha256(key, "A|" + s.transcript)));
    return reply;
}

void Pairing::on_cancel(const JsonValue& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (incoming_ && incoming_->id == body["session"].str()) incoming_.reset();
}

std::optional<Pairing::Incoming> Pairing::incoming() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::int64_t t = now();
    if (!incoming_ || incoming_->expires <= t) return std::nullopt;
    Incoming i;
    i.from_name = incoming_->from.name;
    if (incoming_->revealed) i.code = incoming_->code.substr(0, 3) + " " + incoming_->code.substr(3);
    i.expires_in = incoming_->expires - t;
    return i;
}

void Pairing::cancel_incoming() {
    std::lock_guard<std::mutex> lock(mutex_);
    incoming_.reset();
}

// ------------------------------------------------------------------ paired engines

std::vector<Paired> Pairing::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Paired> out;
    for (const auto& [id, p] : paired_) out.push_back(p);
    return out;
}

bool Pairing::is_paired(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paired_.count(id) != 0;
}

Status Pairing::forget(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!paired_.erase(id)) return make_error(Errc::invalid_argument, "not paired");
    return save_locked();
}

Result<std::vector<std::pair<std::string, std::string>>> Pairing::sign(const std::string& peer_id, const std::string& method,
                                                                       const std::string& path,
                                                                       const std::map<std::string, std::string>& query,
                                                                       const std::string& body) {
    keys::Key key;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = paired_.find(peer_id);
        if (it == paired_.end()) return make_error(Errc::invalid_argument, "not paired with that engine");
        key = it->second.key;
    }
    std::array<std::uint8_t, 16> n{};
    ALEMBIC_TRY(platform::secure_random(n.data(), n.size()));
    std::string time = std::to_string(now()), nonce = keys::to_hex(Bytes(n.data(), n.size()));
    keys::Key mac = keys::hmac_sha256(key, canonical(method, path, query, time, nonce, body));
    return std::vector<std::pair<std::string, std::string>>{
        {kPeerHeader, self_.id}, {kTimeHeader, time}, {kNonceHeader, nonce}, {kSignHeader, keys::to_hex(mac)}};
}

Result<std::string> Pairing::verify(const HttpRequest& r) {
    auto header = [&](const char* name) {
        std::string lower = name;
        for (char& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        auto it = r.headers.find(lower);
        return it == r.headers.end() ? std::string() : it->second;
    };
    std::string id = header(kPeerHeader), time = header(kTimeHeader), nonce = header(kNonceHeader), sign = header(kSignHeader);
    keys::Key mac{};
    if (id.empty() || nonce.size() != 32 || !hex_key(sign, mac)) return make_error(Errc::invalid_argument, "not a signed request");
    std::int64_t when = std::atoll(time.c_str());
    std::int64_t t = now();
    if (when < t - kClockSlack || when > t + kClockSlack)
        return make_error(Errc::invalid_argument, "the clocks of the two engines differ by more than two minutes");
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = paired_.find(id);
    if (it == paired_.end()) return make_error(Errc::invalid_argument, "this engine is not paired with that one");
    keys::Key expected = keys::hmac_sha256(it->second.key, canonical(r.method, r.path, r.query, time, nonce, r.body));
    if (!keys::equal(Bytes(mac.data(), 32), Bytes(expected.data(), 32))) return make_error(Errc::invalid_argument, "bad signature");
    for (auto n = nonces_.begin(); n != nonces_.end();) n = t - n->second > 2 * kClockSlack ? nonces_.erase(n) : std::next(n);
    if (nonces_.count(nonce)) return make_error(Errc::invalid_argument, "a request was sent twice");
    if (nonces_.size() > 100000) nonces_.erase(nonces_.begin());
    nonces_[nonce] = t;
    return id;
}

void Pairing::load() {
    auto text = read_text(state_dir_ + "/paired.json");
    if (!text.ok()) return;
    auto v = parse_json(*text);
    if (!v.ok()) return;
    for (const JsonValue& item : (*v)["engines"].items()) {
        Paired p;
        p.id = item["id"].str();
        p.name = item["name"].str();
        p.since = item["since"].integer(0);
        if (p.id.size() != 32 || !hex_key(item["key"].str(), p.key)) continue;
        paired_[p.id] = p;
    }
}

Status Pairing::save_locked() const {
    JsonValue list = JsonValue::array();
    for (const auto& [id, p] : paired_) {
        JsonValue v = JsonValue::object();
        v.set("id", p.id);
        v.set("name", p.name);
        v.set("key", keys::to_hex(p.key)); // secret: whoever reads this file can act as this engine to that one
        v.set("since", p.since);
        list.push(std::move(v));
    }
    JsonValue root = JsonValue::object();
    root.set("engines", std::move(list));
    std::string path = state_dir_ + "/paired.json";
    (void)make_parent_directories(path);
    return write_text(path, root.dump());
}

} // namespace alembic::engine::pairing
